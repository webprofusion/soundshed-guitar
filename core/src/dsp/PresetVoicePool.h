#pragma once

#include "dsp/DspReaper.h"
#include "dsp/PresetInstance.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
/// The mixer's preset instances over their whole lifetime: installed as a slot, crossfaded
/// in on a swap, rung out or faded out once superseded, and handed to the reaper when done.
///
/// Building an instance is the mixer's job — it knows the resource library, the node-type
/// defaults and the stream format. This class decides only what happens to an instance
/// once it exists, which is where the swap rules live: the declick ramp, the tail spill,
/// and the caps on how many outgoing chains may still be running.
///
/// Threads: the list is changed by the message thread under the DSP lock, and by the audio
/// thread's CollectFinishedFadeOuts() at the end of each block. A walk needs the DSP lock,
/// or, on the message thread, a ReadScope.
class PresetVoicePool
{
  public:
    explicit PresetVoicePool(DspReaper& reaper) : mReaper(reaper)
    {
    }

    PresetVoicePool(const PresetVoicePool&) = delete;
    PresetVoicePool& operator=(const PresetVoicePool&) = delete;

    /// Resolves the sample-based windows for this sample rate.
    void Prepare(double sampleRate);

    // ── Slots ──────────────────────────────────────────────────────────────────────
    /// The live instance answering to `id`, or null. Retiring instances are invisible: their
    /// id often matches the incoming one (a scene switch reuses the preset id), so returning
    /// one would route parameter updates into the chain that is on its way out.
    [[nodiscard]] PresetInstance* Find(const std::string& id);
    [[nodiscard]] const PresetInstance* Find(const std::string& id) const;

    /// Installs a built instance as one more slot at full gain, as AddActivePreset does.
    /// The caller has already checked no live slot answers to its id.
    void Install(std::unique_ptr<PresetInstance> inst);
    /// Retires the live slot answering to `id`, if there is one, without a fade.
    void Remove(const std::string& id);
    /// Re-keys a live slot. See MultiPresetMixer::RenameActivePreset.
    bool Rename(const std::string& oldId, const std::string& newId, const std::string& name);

    // ── Swaps ──────────────────────────────────────────────────────────────────────
    /// Holds an instance built off the DSP lock until one of the commits below installs it.
    void Stage(std::unique_ptr<PresetInstance> inst);
    /// Installs the staged instance, fading it in, and retires everything live. False, with
    /// nothing changed, when nothing was staged.
    bool CommitSwap();
    /// Installs the staged instance in place of the live slot with the same id, keeping that
    /// slot's settings. False, with the instance left staged, unless one was staged for `id`
    /// and a live slot answers to it.
    bool CommitReplacement(const std::string& id);
    /// Installs the staged instance as one more slot at full gain. False, with the instance
    /// left staged, unless one was staged for `id` and no live slot already answers to it.
    bool CommitAddition(const std::string& id);

    // ── Tail spill ─────────────────────────────────────────────────────────────────
    /// How long a superseded instance may ring out, in seconds. See
    /// MultiPresetMixer::SetPresetSwapTailSeconds.
    void SetTailSeconds(double seconds);

    [[nodiscard]] double GetTailSeconds() const noexcept
    {
        return mTailSeconds;
    }

    static constexpr double kMaxTailSeconds = 20.0;

    // ── Audio thread ───────────────────────────────────────────────────────────────
    /// Moves every instance's ramps on by one block. Once per block, after every mix site —
    /// instances left out of the mix still advance, so a muted fade-out cannot get stuck
    /// holding its resources forever.
    void AdvanceRamps(int numSamples);
    /// Hands every finished fade-out to the reaper. End of the block. Leaves them for the
    /// next block while a ReadScope is held or the reaper cannot take them.
    void CollectFinishedFadeOuts();

    // ── Walks ──────────────────────────────────────────────────────────────────────
    /// Every instance, retiring ones included, in install order. See the class comment for
    /// what a walk needs.
    [[nodiscard]] std::vector<std::unique_ptr<PresetInstance>>& Instances() noexcept
    {
        return mInstances;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<PresetInstance>>& Instances() const noexcept
    {
        return mInstances;
    }

    /// Live instance count. Takes its own ReadScope.
    [[nodiscard]] std::size_t LiveCount() const;
    /// Instances ringing out or fading out. Takes its own ReadScope.
    [[nodiscard]] std::size_t RetiringCount() const;

    /// Holds the audio thread's CollectFinishedFadeOuts() off the list, without the DSP lock,
    /// for as long as it lives. The audio thread keeps processing, and leaves a finished
    /// fade-out in place until the next block, which costs nothing since it is no longer
    /// processed. Nests. Waits only while an erase is already under way, which is a few
    /// pointer moves.
    ///
    /// The two sides are a store-then-load handshake (mReaders here, mErasing in
    /// CollectFinishedFadeOuts), both sequentially consistent, so at least one always sees
    /// the other.
    class ReadScope
    {
      public:
        explicit ReadScope(const PresetVoicePool& pool);
        ~ReadScope();
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;

      private:
        const PresetVoicePool& mPool;
    };

    /// Length of the declick ramp for a swap, in samples (~21 ms at 48 kHz). A declick, not
    /// a musical crossfade.
    static constexpr int kFadeSamples = 1024;

    /// The longest a swap's warm-up or latency-spanning crossfade may run.
    static constexpr double kMaxSwapAlignSeconds = 0.25;
    /// How much longer than its latency an incoming chain with latency is given to settle.
    static constexpr double kSwapSettleSeconds = 0.025;

  private:
    /// When a swap's ramps start and how long they run.
    struct SwapTiming
    {
        int warmupSamples = 0;
        int fadeSamples = kFadeSamples;
    };

    /// Lines a swap up with the latency on both sides. A freshly built chain is silent for its
    /// own latency (a pitch shifter's aligned dry path starts empty), so the incoming one stays
    /// muted, and the outgoing one at its gain, until that has filled; fading in at once left a
    /// gap and then a step where the signal began. When the two chains' latencies differ, the
    /// crossfade runs at least that difference, so the stretch of playing one chain has not
    /// reached yet and the other has already passed is still heard rather than skipped.
    [[nodiscard]] SwapTiming TimeSwap(const PresetInstance& incoming, int outgoingLatency) const;

    /// Retires a superseded instance the way the current settings say to: ringing out when
    /// the tail spill is on and its graph can ring, plain declick fade otherwise.
    void RetireSuperseded(PresetInstance& inst, const SwapTiming& timing);
    /// Pushes the oldest tails into their release once more than kMaxTailingInstances are
    /// ringing at once, so a run of fast switches cannot stack full chains without bound.
    void LimitTailing();
    /// Hands the oldest retirees straight to the reaper once more than kMaxRetiringInstances
    /// are on their way out.
    void LimitRetiring();
    /// The hold budget a new tail starts with, in samples.
    [[nodiscard]] int TailHoldSamples() const;

    // Upper bound on simultaneously retiring instances. Rapid successive switches hard-drop
    // the oldest rather than stacking unbounded CPU cost.
    static constexpr std::size_t kMaxRetiringInstances = 3;
    // How many outgoing presets may ring at once. A ringing instance is a whole chain still
    // being processed on the audio thread, so this is a CPU budget before it is a musical
    // choice. One tail holds; older tails release, with at most three retirees in total.
    static constexpr std::size_t kMaxTailingInstances = 1;
    // Release once the hold runs out. Long enough that cutting a still-loud feedback delay
    // reads as an ending rather than a chop — the 21 ms declick would be audible there.
    static constexpr double kTailReleaseSeconds = 0.25;

    DspReaper& mReaper;
    // Held by pointer so the audio thread can drop a finished fade-out with a pointer move:
    // moving a PresetInstance by value moves its SignalGraphExecutor, whose move-assignment
    // joins worker threads — not something that can happen on the realtime path.
    std::vector<std::unique_ptr<PresetInstance>> mInstances;
    // ReadScope's side of the handshake, and the audio thread's.
    mutable std::atomic<int> mReaders{0};
    std::atomic<bool> mErasing{false};
    // Built off the DSP lock by the mixer; installed by one of the commits.
    std::unique_ptr<PresetInstance> mPending;

    double mSampleRate = 44100.0;
    // Off until somebody sets a length. Prepare() resolves the release sample count from
    // kTailReleaseSeconds, so it follows the sample rate.
    double mTailSeconds = 0.0;
    int mTailReleaseSamples = kFadeSamples;
};
} // namespace guitarfx
