#include "dsp/PresetVoicePool.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace guitarfx
{
PresetVoicePool::ReadScope::ReadScope(const PresetVoicePool& pool) : mPool(pool)
{
    mPool.mReaders.fetch_add(1, std::memory_order_seq_cst);

    // An erase that began before the audio thread could see this reader is let finish.
    while (mPool.mErasing.load(std::memory_order_seq_cst))
    {
        std::this_thread::yield();
    }
}

PresetVoicePool::ReadScope::~ReadScope()
{
    mPool.mReaders.fetch_sub(1, std::memory_order_release);
}

void PresetVoicePool::Prepare(double sampleRate)
{
    mSampleRate = sampleRate;
    // Tail spill windows follow the sample rate; the hold itself is resolved per swap from
    // whatever length the caller last set.
    mTailReleaseSamples = std::max(1, static_cast<int>(std::lround(kTailReleaseSeconds * sampleRate)));
}

PresetInstance* PresetVoicePool::Find(const std::string& id)
{
    for (auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && inst->cfg.id == id)
        {
            return inst.get();
        }
    }

    return nullptr;
}

const PresetInstance* PresetVoicePool::Find(const std::string& id) const
{
    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && inst->cfg.id == id)
        {
            return inst.get();
        }
    }

    return nullptr;
}

void PresetVoicePool::Install(std::unique_ptr<PresetInstance> inst)
{
    mInstances.push_back(std::move(inst));
}

void PresetVoicePool::Remove(const std::string& id)
{
    for (auto it = mInstances.begin(); it != mInstances.end(); ++it)
    {
        if (!(*it)->IsRetiring() && (*it)->cfg.id == id)
        {
            mReaper.Retire(std::move(*it));
            mInstances.erase(it);
            break;
        }
    }
}

bool PresetVoicePool::Rename(const std::string& oldId, const std::string& newId, const std::string& name)
{
    if (oldId.empty() || newId.empty())
    {
        return false;
    }

    if (oldId == newId)
    {
        if (auto* inst = Find(newId))
        {
            inst->cfg.name = name;
            return true;
        }

        return false;
    }

    // Refuse rather than produce two slots answering to the same id: Find returns the first
    // match, so a duplicate would silently route half the updates to the wrong chain.
    // Retiring instances are excluded from both lookups, so an outgoing instance still
    // carrying oldId does not block the rename.
    if (Find(newId) != nullptr)
    {
        return false;
    }

    auto* inst = Find(oldId);

    if (inst == nullptr)
    {
        return false;
    }

    inst->cfg.id = newId;
    inst->cfg.name = name;
    return true;
}

void PresetVoicePool::Stage(std::unique_ptr<PresetInstance> inst)
{
    mPending = std::move(inst);
}

bool PresetVoicePool::CommitReplacement(const std::string& id)
{
    auto existing =
        std::find_if(mInstances.begin(), mInstances.end(), [&](const std::unique_ptr<PresetInstance>& candidate) {
            return !candidate->IsRetiring() && candidate->cfg.id == id;
        });

    if (!mPending || mPending->cfg.id != id || existing == mInstances.end())
    {
        return false;
    }

    auto inst = std::move(mPending);
    auto name = std::move(inst->cfg.name);
    inst->cfg = (*existing)->cfg;
    inst->cfg.name = std::move(name);

    // Crossfade rather than cutting: leave the old slot in place, ramping down, and add
    // the replacement alongside it ramping up. Lookups skip retiring instances, so the
    // shared preset ID still resolves to the new slot from here on.
    const bool outgoingMuted = (*existing)->cfg.mute;
    const SwapTiming timing = TimeSwap(*inst, outgoingMuted ? 0 : (*existing)->executor.GetTotalLatencySamples());
    inst->BeginFadeIn(timing.fadeSamples, outgoingMuted ? 0 : timing.warmupSamples);

    if (outgoingMuted)
    {
        // Nothing to fade; retire it outright rather than running a silent chain.
        mReaper.Retire(std::move(*existing));
        *existing = std::move(inst);
    }
    else
    {
        auto& outgoing = **existing;
        RetireSuperseded(outgoing, timing);
        mInstances.push_back(std::move(inst));
        LimitTailing();
        LimitRetiring();
    }

    return true;
}

bool PresetVoicePool::CommitAddition(const std::string& id)
{
    if (!mPending || mPending->cfg.id != id || Find(id) != nullptr)
    {
        return false;
    }

    // No fade, as AddActivePreset: the slot joins a mix that is already playing.
    mInstances.push_back(std::move(mPending));
    return true;
}

bool PresetVoicePool::CommitSwap()
{
    // Fast swap: install the pre-built instance and start fading the old ones out.
    // Must be called while holding the DSP lock. Everything here is O(instances) pointer
    // work — no allocation beyond the vector push, no resource loading, no destruction.
    if (!mPending)
    {
        return false;
    }

    // What is heard now is the live, unmuted chains; the swap lines up with the latest of them.
    int outgoingLatency = 0;

    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && !inst->cfg.mute)
        {
            outgoingLatency = std::max(outgoingLatency, inst->executor.GetTotalLatencySamples());
        }
    }

    const SwapTiming timing = TimeSwap(*mPending, outgoingLatency);

    // Retire everything currently live: ringing out if the tail spill is on, fading out
    // otherwise. An instance already on its way out from an earlier switch is left alone
    // so it carries on from where it is rather than jumping back to full gain, and a tail
    // already ringing keeps the budget it started with.
    for (auto it = mInstances.begin(); it != mInstances.end();)
    {
        auto& inst = **it;

        if (inst.cfg.mute)
        {
            // Contributes nothing to fade out; retire it outright.
            mReaper.Retire(std::move(*it));
            it = mInstances.erase(it);
            continue;
        }

        if (!inst.IsRetiring())
        {
            RetireSuperseded(inst, timing);
        }

        ++it;
    }

    // Hold the tail budget first: a third tail starts its release rather than being cut.
    LimitTailing();

    // Backstop on simultaneous retirees: switching faster than they can drain drops the
    // oldest rather than stacking a full chain's CPU cost per switch. Oldest is also the
    // one furthest through its ramp, or the tail that has had longest to decay, so it is
    // the least likely to be heard going.
    LimitRetiring();

    // With nothing audible going out there is nothing to hold, and no reason to wait.
    const bool anythingHeard = std::any_of(mInstances.begin(), mInstances.end(), [](const auto& inst) {
        return inst->phase == InstancePhase::Tailing ||
               (inst->phase == InstancePhase::FadingOut && inst->fadeSamplesRemaining > 0);
    });
    mPending->BeginFadeIn(timing.fadeSamples, anythingHeard ? timing.warmupSamples : 0);
    mInstances.push_back(std::move(mPending));
    mPending.reset();
    return true;
}

PresetVoicePool::SwapTiming PresetVoicePool::TimeSwap(const PresetInstance& incoming, int outgoingLatency) const
{
    const int limit = std::max(kFadeSamples, static_cast<int>(std::lround(kMaxSwapAlignSeconds * mSampleRate)));
    const int incomingLatency = std::max(0, incoming.executor.GetTotalLatencySamples());

    // A chain with latency has engines that work from a history (a pitch shifter's splice
    // search, an STFT window), and those settle a little after the latency itself has filled:
    // Pitch Shift's Low Latency engine clicked about 36 ms in from cold, past its 16 ms.
    const int settle = incomingLatency > 0 ? static_cast<int>(std::lround(kSwapSettleSeconds * mSampleRate)) : 0;

    SwapTiming timing;
    timing.warmupSamples = std::min(incomingLatency + settle, limit);
    timing.fadeSamples = std::clamp(std::abs(std::max(0, outgoingLatency) - incomingLatency), kFadeSamples, limit);
    return timing;
}

void PresetVoicePool::SetTailSeconds(double seconds)
{
    mTailSeconds = IsFinite(seconds) ? std::clamp(seconds, 0.0, kMaxTailSeconds) : 0.0;
}

int PresetVoicePool::TailHoldSamples() const
{
    if (mTailSeconds <= 0.0)
    {
        return 0;
    }

    return static_cast<int>(std::lround(mTailSeconds * mSampleRate));
}

void PresetVoicePool::RetireSuperseded(PresetInstance& inst, const SwapTiming& timing)
{
    // A retiring instance must not influence the live solo decision either way.
    inst.cfg.solo = false;

    const int holdSamples = TailHoldSamples();

    if (holdSamples > 0 && inst.canRingOut)
    {
        // The hold counts from when the input starts to ramp away.
        inst.BeginTail(timing.fadeSamples, holdSamples, timing.warmupSamples);
        return;
    }

    inst.BeginFadeOut(timing.fadeSamples, timing.warmupSamples);
}

void PresetVoicePool::LimitTailing()
{
    std::size_t tailing = 0;

    for (const auto& inst : mInstances)
    {
        if (inst->phase == InstancePhase::Tailing)
        {
            ++tailing;
        }
    }

    // Oldest first — mInstances is in install order — so the tail that has had the longest
    // to decay is the one asked to finish.
    for (auto& inst : mInstances)
    {
        if (tailing <= kMaxTailingInstances)
        {
            break;
        }

        if (inst->phase == InstancePhase::Tailing)
        {
            inst->BeginFadeOut(mTailReleaseSamples);
            --tailing;
        }
    }
}

void PresetVoicePool::LimitRetiring()
{
    auto retiring = RetiringCount();

    for (auto it = mInstances.begin(); it != mInstances.end() && retiring > kMaxRetiringInstances;)
    {
        if ((*it)->IsRetiring())
        {
            mReaper.Retire(std::move(*it));
            it = mInstances.erase(it);
            --retiring;
        }
        else
        {
            ++it;
        }
    }
}

void PresetVoicePool::AdvanceRamps(int numSamples)
{
    for (auto& inst : mInstances)
    {
        inst->AdvanceRamps(numSamples, mTailReleaseSamples);
    }
}

void PresetVoicePool::CollectFinishedFadeOuts()
{
    const auto isFinished = [](const std::unique_ptr<PresetInstance>& inst) { return inst->IsFinishedFadingOut(); };

    // Nearly every block has nothing to drop, and then there is nobody to hand-shake with.
    if (std::none_of(mInstances.begin(), mInstances.end(), isFinished))
    {
        return;
    }

    // The other half of ReadScope: announce the erase, then look for a reader.
    mErasing.store(true, std::memory_order_seq_cst);

    if (mReaders.load(std::memory_order_seq_cst) == 0)
    {
        for (auto it = mInstances.begin(); it != mInstances.end();)
        {
            if (isFinished(*it) && mReaper.TryRetireRealtime(*it))
            {
                it = mInstances.erase(it); // unique_ptr moves only — no executor moves, no joins
            }
            else
            {
                ++it;
            }
        }
    }

    mErasing.store(false, std::memory_order_release);
}

std::size_t PresetVoicePool::LiveCount() const
{
    const ReadScope readScope(*this);
    std::size_t count = 0;

    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring())
        {
            ++count;
        }
    }

    return count;
}

std::size_t PresetVoicePool::RetiringCount() const
{
    const ReadScope readScope(*this);
    std::size_t count = 0;

    for (const auto& inst : mInstances)
    {
        if (inst->IsRetiring())
        {
            ++count;
        }
    }

    return count;
}
} // namespace guitarfx
