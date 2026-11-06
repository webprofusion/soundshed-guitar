#pragma once

/**
 * Verbal: a multi-machine reverb. Twelve machines — Room, Hall, Plate, Spring, Swell, Bloom,
 * Cloud, Choir, Shimmer, Tape Echo, Nonlinear and Reflections — behind one set of common
 * controls (Decay as a calibrated RT60 in seconds, Pre-Delay to 1.5 s, Tone, Mod, Low End,
 * Mix) plus each machine's own, shown only under that machine.
 *
 * Behaviours worth knowing:
 *   - Mix adds reverb to an untouched dry: the dry stays at unity to nine tenths of the knob
 *     and fades out over the last tenth, so the top of the knob is 100% wet.
 *   - Hold sustains the tail. Infinite keeps adding what is played; Freeze closes the input,
 *     so the held tail sits under whatever comes next. Both engage and release on ramps.
 *   - Changing machine lets the old tail ring out on a second engine while the new one starts.
 *     There are two engines, so a change while a tail still rings fades that tail first,
 *     unless the running machine has heard nothing, when it simply becomes the new one.
 *   - Swell in Dry mode runs 10 ms late, the one case with latency: that is what lets the gain
 *     be down before the pick it reacted to comes out.
 *
 * See docs/fx-library.md, "Verbal".
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/reverb/VerbalEngine.h"
#include "dsp/reverb/VerbalPresets.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace guitarfx
{
class VerbalEffect : public EffectProcessor
{
  public:
    VerbalEffect()
    {
        for (std::size_t i = 0; i < verbal::kParamCount; ++i)
        {
            mValues[i] = verbal::kParams[i].defaultValue;
        }
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        for (auto& engine : mEngines)
        {
            engine.Prepare(sampleRate);
        }

        mSwell.Prepare(sampleRate);
        mLookSamples = kSwellLookSeconds * sampleRate;
        mLookL.Resize(static_cast<std::size_t>(mLookSamples) + 16);
        mLookR.Resize(static_cast<std::size_t>(mLookSamples) + 16);
        mTailFadeStep = static_cast<float>(1.0 / (kTailFadeSeconds * sampleRate));
        mTailQuietLimit = static_cast<int>(kTailQuietSeconds * sampleRate);
        mMix.SetTime(0.02, sampleRate);
        mOutput.SetTime(0.02, sampleRate);
        mHold.SetTime(0.03, sampleRate);
        mIntake.SetTime(0.02, sampleRate);
        mDryL.SetTime(0.03, sampleRate);
        mDryR.SetTime(0.03, sampleRate);
        mSwellWet.SetTime(0.02, sampleRate);
        mSwellDry.SetTime(0.02, sampleRate);
        mHandover.SetTime(0.005, sampleRate);
        mTailHold.SetTime(0.03, sampleRate);
        mPrepared = true;
        ApplySettings(true);
        Reset();
    }

    void Reset() override
    {
        for (auto& engine : mEngines)
        {
            engine.Reset();
        }

        mSwell.Reset();
        mLookL.Clear();
        mLookR.Clear();
        mTailActive = false;
        mTailFading = false;
        // A queued machine change has nothing to wait for now; ApplySettings makes it.
        mDirty = mDirty || mChangeQueued;
        mChangeQueued = false;
        SnapSmoothers();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (!mEnabled || !mPrepared)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (mDirty)
        {
            mDirty = false;
            ApplySettings(false);
        }

        if (mTailActive && mTailFade <= 0.0f)
        {
            mTailActive = false;
            mTailFading = false;
        }

        if (mChangeQueued && !mTailActive)
        {
            mChangeQueued = false;
            HandOver(BuildSettings());
        }

        verbal::Engine& active = mEngines[mActive];
        verbal::Engine& tail = mEngines[1 - mActive];
        active.BeginBlock(numSamples);

        if (mTailActive)
        {
            tail.BeginBlock(numSamples);
        }

        UpdateSwellMode();
        float activePeak = 0.0f;
        float tailPeak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float rawL = reverb::ScrubNonFinite(inputs && inputs[0] ? inputs[0][i] : 0.0f);
            const float rawR = reverb::ScrubNonFinite(inputs && inputs[1] ? inputs[1][i] : rawL);
            const float mix = mMix.Next();
            const float hold = mHold.Next();
            const float intake = mIntake.Next();
            const float swellGain = mSwell.Process(0.5f * (rawL + rawR)) - 1.0f;
            const float swellDry = mSwellDry.Next();
            const float swellWet = mSwellWet.Next();
            mLookL.Write(rawL);
            mLookR.Write(rawR);
            const float inL = rawL + (mLookL.ReadLinear(mLookSamples) - rawL) * swellDry;
            const float inR = rawR + (mLookR.ReadLinear(mLookSamples) - rawR) * swellDry;
            // Swell Dry rocks a volume pedal ahead of everything; Swell Wet only behind the tank,
            // and not over a held tail, which is there to sit under what is played.
            const float feedGain = 1.0f + swellGain * swellDry;
            const float wetSwell = 1.0f + swellGain * swellWet * (1.0f - hold);
            const float handover = mHandover.Next();

            float wetL = 0.0f;
            float wetR = 0.0f;
            active.Process(inL * feedGain, inR * feedGain, intake * (1.0f - handover), hold, wetL, wetR);
            activePeak = std::max(activePeak, std::fabs(rawL) + std::fabs(rawR) + std::fabs(wetL) + std::fabs(wetR));
            wetL *= wetSwell;
            wetR *= wetSwell;

            if (mTailActive)
            {
                float tl = 0.0f;
                float tr = 0.0f;
                // The outgoing machine's input closes on a short ramp, and it keeps its own decay.
                tail.Process(inL, inR, intake * handover, mTailHold.Next(), tl, tr);
                const float gain = TailGain();
                wetL += tl * gain;
                wetR += tr * gain;
                tailPeak = std::max(tailPeak, std::fabs(tl) + std::fabs(tr));
            }

            const float dryGain = DryGain(mix) * feedGain;
            const float wetGain = WetGain(mix) * mOutput.Next();
            const float dryL = mDryL.Next();
            const float dryR = mDryR.Next();

            if (outputs[0])
            {
                outputs[0][i] = inL * dryGain * dryL + wetL * wetGain;
            }

            if (outputs[1])
            {
                outputs[1][i] = inR * dryGain * dryR + wetR * wetGain;
            }
        }

        // The spilled-over tail retires once it is inaudible, and is then free for the next change.
        // Silent isn't finished until the longest delay has had time to come round.
        mTailQuiet = tailPeak < kSilence ? mTailQuiet + numSamples : 0;
        mActiveQuiet = activePeak < kSilence ? std::min(mActiveQuiet + numSamples, mTailQuietLimit) : 0;

        if (mTailActive && mTailQuiet >= mTailQuietLimit)
        {
            mTailActive = false;
            mTailFading = false;
        }

        // One non-finite value inside a loop would circulate for ever; clear it instead.
        if (!active.Finite() || (mTailActive && !tail.Finite()))
        {
            Reset();
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        const std::size_t index = verbal::FindParam(key);

        if (index == verbal::kParamCount || !IsFinite(value))
        {
            return;
        }

        const double normalised = NormaliseParamValue(verbal::kParams[index], value);

        if (normalised == mValues[index])
        {
            return;
        }

        mValues[index] = normalised;
        mDirty = true;
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        const std::size_t index = verbal::FindParam(key);
        return index == verbal::kParamCount ? 0.0 : mValues[index];
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "reverb_verbal";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "reverb";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        const bool swellDry = mEnabled && Machine() == verbal::Machine::Swell && mValues[verbal::kSwellMode] >= 0.5;
        return swellDry ? static_cast<int>(std::lround(mLookSamples)) : 0;
    }

    /// The wet level the Mix law gives at `mix`: silent at 0, -24 dB at a tenth, unity from nine
    /// tenths. Between, it follows a curve through -15 dB at 0.3 and -10 dB at a half, so small
    /// settings are usable and the middle of the knob sits the reverb behind the dry.
    [[nodiscard]] static float WetGain(float mix) noexcept
    {
        static constexpr std::array<float, 6> kAt = {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f};
        static constexpr std::array<float, 6> kDb = {-60.0f, -24.0f, -15.0f, -10.0f, -4.7f, 0.0f};

        if (mix <= 0.0f)
        {
            return 0.0f;
        }

        if (mix >= 0.9f)
        {
            return 1.0f;
        }

        if (mix < 0.1f)
        {
            // Linear in gain from silence, so the very bottom of the knob fades out cleanly.
            return mix * 10.0f * std::pow(10.0f, kDb[1] / 20.0f);
        }

        std::size_t k = 1;

        while (k + 1 < kAt.size() && mix > kAt[k + 1])
        {
            ++k;
        }

        const float t = (mix - kAt[k]) / (kAt[k + 1] - kAt[k]);
        return std::pow(10.0f, (kDb[k] + t * (kDb[k + 1] - kDb[k])) / 20.0f);
    }

    /// The dry level at `mix`: unity to nine tenths, then out over the last tenth.
    [[nodiscard]] static float DryGain(float mix) noexcept
    {
        return mix <= 0.9f ? 1.0f : std::max(0.0f, (1.0f - mix) * 10.0f);
    }

  private:
    /// How far Swell Dry runs behind its detector, and how fast each mode's gain falls. The
    /// detector takes up to about 5 ms, so Dry's fall ends before the pick comes out.
    static constexpr double kSwellLookSeconds = 0.01;
    static constexpr double kSwellFallWetSeconds = 0.012;
    static constexpr double kSwellFallDrySeconds = 0.004;
    /// A tail that has to make way for a newer change fades over this.
    static constexpr double kTailFadeSeconds = 0.05;
    /// Longer than anything can sit in a line unheard: Pre-Delay plus its early pattern, Echo
    /// Time, the Nonlinear's Time.
    static constexpr double kTailQuietSeconds = 2.5;
    static constexpr float kSilence = 1.0e-6f;

    [[nodiscard]] verbal::Machine Machine() const noexcept
    {
        return static_cast<verbal::Machine>(static_cast<int>(mValues[verbal::kMachine]));
    }

    [[nodiscard]] verbal::Settings BuildSettings() const noexcept
    {
        const auto& v = mValues;
        const auto i = [&v](std::size_t p) { return static_cast<int>(std::lround(v[p])); };
        verbal::Settings s;
        s.machine = Machine();
        s.decay = v[verbal::kDecay];
        s.preDelayMs = v[verbal::kPreDelay];
        s.tone = v[verbal::kTone];
        s.mod = v[verbal::kMod];
        s.lowEnd = v[verbal::kLowEnd];
        s.roomSize = i(verbal::kRoomSize);
        s.hallSize = i(verbal::kHallSize);
        s.plateSize = i(verbal::kPlateSize);
        s.dwell = i(verbal::kDwell);
        s.springs = i(verbal::kSprings);
        s.midEq = v[verbal::kMidEq];
        s.bloomLength = v[verbal::kBloomLength];
        s.bloomFeedback = v[verbal::kBloomFeedback];
        s.diffusion = v[verbal::kDiffusion];
        s.vowel = i(verbal::kVowel);
        s.resonance = i(verbal::kResonance);
        s.shimmerMode = i(verbal::kShimmerMode);
        s.shift1 = i(verbal::kShift1);
        s.shift2 = i(verbal::kShift2);
        s.shimmerAmount = v[verbal::kShimmerAmount];
        s.echoTimeMs = v[verbal::kEchoTime];
        s.echoFeedback = v[verbal::kEchoFeedback];
        s.heads = i(verbal::kHeads);
        s.spacing = i(verbal::kSpacing);
        s.shape = i(verbal::kShape);
        s.shapeTimeMs = v[verbal::kShapeTime];
        s.shapeFeedback = v[verbal::kShapeFeedback];
        s.lateLevel = v[verbal::kLateLevel];
        s.modSpeed = v[verbal::kModSpeed];
        s.size = v[verbal::kSize];
        s.roomShape = i(verbal::kRoomShape);
        s.listenerX = v[verbal::kListenerX];
        s.listenerY = v[verbal::kListenerY];
        return s;
    }

    /// The tail's gain this sample: one until it is told to make way, then a short fade.
    [[nodiscard]] float TailGain() noexcept
    {
        if (!mTailFading)
        {
            return 1.0f;
        }

        mTailFade = std::max(0.0f, mTailFade - mTailFadeStep);
        return mTailFade * mTailFade * (3.0f - 2.0f * mTailFade);
    }

    /// The Swell follows the machine actually running, which a queued change hasn't reached yet.
    void UpdateSwellMode() noexcept
    {
        const bool swell = mEngines[mActive].CurrentSettings().machine == verbal::Machine::Swell;
        const bool swellDry = swell && mValues[verbal::kSwellMode] >= 0.5;
        mSwellWet.SetTarget(swell && !swellDry ? 1.0f : 0.0f);
        mSwellDry.SetTarget(swellDry ? 1.0f : 0.0f);
        mSwell.SetFallSeconds(swellDry ? kSwellFallDrySeconds : kSwellFallWetSeconds);
    }

    /// The running machine rings out where it is and the other engine, whose contents are
    /// finished with, starts the new one.
    void HandOver(const verbal::Settings& settings) noexcept
    {
        mTailHold.Snap(mHold.Value());
        mTailHold.SetTarget(0.0f);
        mHandover.Snap(1.0f);
        mHandover.SetTarget(0.0f);
        mActive = 1 - mActive;
        mTailActive = true;
        mTailFading = false;
        mTailFade = 1.0f;
        mTailQuiet = 0;
        mActiveQuiet = mTailQuietLimit; // a cleared engine has nothing in it to lose
        mEngines[mActive].Reset();
        mEngines[mActive].Configure(settings, true);
    }

    /// Hands every control to the engines. On the audio thread when automation drives it, so
    /// nothing here allocates; a machine change resets the incoming engine's buffers, which
    /// is the one costly step and only happens on a deliberate change of machine.
    void ApplySettings(bool initial)
    {
        if (!mPrepared)
        {
            return;
        }

        const verbal::Settings settings = BuildSettings();
        verbal::Engine& active = mEngines[mActive];

        const bool change = !initial && settings.machine != active.CurrentSettings().machine;
        const bool idle = mActiveQuiet >= mTailQuietLimit;
        mChangeQueued = change && mTailActive && !idle;

        if (mChangeQueued)
        {
            // Both engines are busy, and neither can be retuned where it stands: a machine that
            // runs apart from the tank would be cut, and a tank left idle would wake with its
            // old contents. The older tail fades, then Process makes the change.
            mTailFading = true;
        }
        else if (change && mTailActive)
        {
            // Nothing has reached the running machine, so the tail can ring on undisturbed.
            active.Reset();
            active.Configure(settings, true);
        }
        else if (change)
        {
            HandOver(settings);
        }
        else
        {
            active.Configure(settings, initial);

            if (initial)
            {
                mEngines[1 - mActive].Configure(settings, true);
            }
        }

        mSwell.SetRiseSeconds(mValues[verbal::kRiseTime]);
        const bool hold = mValues[verbal::kHold] >= 0.5;
        const bool freeze = hold && mValues[verbal::kHoldMode] >= 0.5;
        mHold.SetTarget(hold ? 1.0f : 0.0f);
        mIntake.SetTarget(freeze ? 0.0f : 1.0f);
        mMix.SetTarget(static_cast<float>(mValues[verbal::kMix]));
        mOutput.SetTarget(static_cast<float>(std::pow(10.0, mValues[verbal::kOutputGain] / 20.0)));

        // Reflections places the player in the room: X pans the dry, and further back (Y) the
        // dry is quieter and more central against the reflections.
        float dryL = 1.0f;
        float dryR = 1.0f;

        if (settings.machine == verbal::Machine::Reflections)
        {
            const auto x = static_cast<float>(settings.listenerX);
            const auto y = static_cast<float>(settings.listenerY);
            const float panL = 1.0f - 0.6f * x;
            const float panR = 0.4f + 0.6f * x;
            const float back = 1.0f - 0.25f * y;
            dryL = back * (panL + (0.7f - panL) * 0.4f * y);
            dryR = back * (panR + (0.7f - panR) * 0.4f * y);
        }

        mDryL.SetTarget(dryL);
        mDryR.SetTarget(dryR);
        UpdateSwellMode();

        if (initial)
        {
            SnapSmoothers();
        }
    }

    void SnapSmoothers() noexcept
    {
        mMix.Snap(mMix.Target());
        mOutput.Snap(mOutput.Target());
        mHold.Snap(mHold.Target());
        mIntake.Snap(mIntake.Target());
        mDryL.Snap(mDryL.Target());
        mDryR.Snap(mDryR.Target());
        mSwellWet.Snap(mSwellWet.Target());
        mSwellDry.Snap(mSwellDry.Target());
        mHandover.Snap(0.0f);
    }

    std::array<double, verbal::kParamCount> mValues{};
    std::array<verbal::Engine, 2> mEngines;
    std::size_t mActive = 0;
    bool mTailActive = false;
    bool mTailFading = false;
    bool mChangeQueued = false;
    bool mDirty = true;
    bool mPrepared = false;
    float mTailFade = 1.0f;
    float mTailFadeStep = 0.001f;
    int mTailQuiet = 0;
    int mActiveQuiet = 0;
    int mTailQuietLimit = 1;
    reverb::SwellEnvelope mSwell;
    delay_line::FractionalDelayLine mLookL, mLookR;
    double mLookSamples = 0.0;
    reverb::Smoothed mMix, mOutput, mHold, mIntake, mDryL, mDryR;
    reverb::Smoothed mSwellWet, mSwellDry, mHandover, mTailHold;
};

inline void RegisterVerbalEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbVerbal;
    info.aliases = {"reverb_verbal"};
    info.displayName = "Verbal";
    info.category = "reverb";
    info.description = "Twelve reverb machines in one: rooms, halls, plates, springs, swells, blooms, clouds, a choir, "
                       "shimmer, tape echo, nonlinear shapes and small-room reflections, with Hold and Freeze.";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(verbal::kParams);

    auto& machine = info.parameters[verbal::kMachine];
    machine.control = "picker";

    for (std::size_t i = 0; i < verbal::kMachineCount; ++i)
    {
        machine.labelInfo.push_back({"", "", "", verbal::kMachineDetails[i]});
    }

    info.presets = verbal::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<VerbalEffect>(); });
}
} // namespace guitarfx
