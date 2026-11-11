#pragma once

/**
 * Stutter: a slice of what was just played, caught and repeated in time.
 *
 * The signal is recorded as it is played. On each step of the grid (a Division of the beat at the
 * host's tempo) the effect may decide, by Chance, to stop playing the input and repeat the slice
 * that has just gone by: Repeats times, each a Division long, sometimes backwards (Reverse).
 * Then it lets the input through again. With Chance at 1 it stutters on every step it is free to;
 * at 0 it never does and the input passes untouched.
 *
 *     input --> [ record ] --------------------------+--> output
 *                    |                               |
 *                    +--> slice player (when a stutter is on, replaces the input)
 *
 * Each repeat is faded in and out over a couple of milliseconds, and so is the move between the
 * input and the player, so nothing clicks however the slice was cut. The choices come from a
 * fixed generator: the same settings on the same performance stutter in the same places, so a
 * take can be repeated.
 *
 * Both channels stutter together, so the effect never widens a mono signal.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
namespace stutter
{
inline constexpr int kMaxRepeats = 16;
/// The longest slice kept: a whole note at the slowest tempo would be 8 s, far more than a
/// stutter is, so a slice is held to this and a longer Division is played as this much.
inline constexpr double kMaxSliceSeconds = 2.0;
inline constexpr double kEdgeFadeMs = 2.0;
/// A quarter note down to a thirty-second: index 7 in tempo_sync's table is the eighth note.
inline constexpr int kDefaultDivision = 7;

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        b.Defaults("now-and-then", "Now and Then"),
        b.Make("eighth-note-stammer", "Eighth Note Stammer", {{"chance", 0.5}, {"repeats", 2.0}}),
        b.Make("sixteenth-machine", "Sixteenth Machine", {{"division", 10.0}, {"chance", 0.6}, {"repeats", 4.0}}),
        b.Make("tape-rewind", "Tape Rewind", {{"division", 4.0}, {"chance", 0.3}, {"repeats", 1.0}, {"reverse", 1.0}}),
        b.Make("broken-cd", "Broken CD", {{"division", 13.0}, {"chance", 0.35}, {"repeats", 8.0}, {"reverse", 0.15}}),
    };
}
} // namespace stutter

class StutterEffect : public EffectProcessor
{
  public:
    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mCapacity = static_cast<int>(std::ceil(stutter::kMaxSliceSeconds * sampleRate)) + 8;

        for (auto& buffer : mBuffers)
        {
            buffer.assign(static_cast<std::size_t>(mCapacity), 0.0f);
        }

        mFade = std::max(2, static_cast<int>(stutter::kEdgeFadeMs * 0.001 * sampleRate));
        Reset();
    }

    void Reset() override
    {
        for (auto& buffer : mBuffers)
        {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
        }

        mRng.Seed(0x57a77e21u);
        mWrite = 0;
        mStepPosition = 0;
        mStepLength = 0;
        mRepeatsLeft = 0;
        mSliceStart = 0;
        mSliceLength = 0;
        mPlayPosition = 0;
        mReversed = false;
        mWetShare = 0.0f;
        mRecorded = 0;
        mLiveSteps = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* in[2] = {inputs[0], inputs[1]};
        float* out[2] = {outputs[0], outputs[1]};
        Run(in, out, 2, numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        const float* in[2] = {input, nullptr};
        float* out[2] = {output, nullptr};
        Run(in, out, 1, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "bpm")
        {
            mBpm.store(tempo_sync::ClampBpm(value), std::memory_order_relaxed);
        }
        else if (key == "division")
        {
            mDivision.store(tempo_sync::ClampDivision(value), std::memory_order_relaxed);
        }
        else if (key == "chance")
        {
            mChance.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "repeats")
        {
            mRepeats.store(static_cast<int>(std::lround(std::clamp(value, 1.0, double{stutter::kMaxRepeats}))),
                           std::memory_order_relaxed);
        }
        else if (key == "reverse")
        {
            mReverse.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bpm")
        {
            return mBpm.load(std::memory_order_relaxed);
        }

        if (key == "division")
        {
            return mDivision.load(std::memory_order_relaxed);
        }

        if (key == "chance")
        {
            return mChance.load(std::memory_order_relaxed);
        }

        if (key == "repeats")
        {
            return mRepeats.load(std::memory_order_relaxed);
        }

        if (key == "reverse")
        {
            return mReverse.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "stutter";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "delay";
    }

  private:
    /// A step of the grid in samples, held to what the buffer keeps.
    [[nodiscard]] int StepSamples() const noexcept
    {
        const double seconds = tempo_sync::DivisionDelayMs(mBpm.load(std::memory_order_relaxed),
                                                           mDivision.load(std::memory_order_relaxed)) *
                               0.001;
        const double limited = std::min(seconds, stutter::kMaxSliceSeconds);
        return std::max(4 * mFade, static_cast<int>(std::lround(limited * mSampleRate)));
    }

    /// A raised-cosine window over the slice's two ends: 0 at each edge, 1 in between.
    [[nodiscard]] float EdgeGain(int position, int length) const noexcept
    {
        const int fromEdge = std::min(position, length - 1 - position);

        if (fromEdge >= mFade)
        {
            return 1.0f;
        }

        return 0.5f - 0.5f * std::cos(3.14159265f * static_cast<float>(fromEdge) / static_cast<float>(mFade));
    }

    void Run(const float* const* in, float* const* out, int channels, int numSamples) noexcept
    {
        const float chance = mChance.load(std::memory_order_relaxed);
        const float reverseChance = mReverse.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const int repeats = mRepeats.load(std::memory_order_relaxed);
        const float shareStep = 1.0f / static_cast<float>(mFade);

        if (mStepLength <= 0)
        {
            mStepLength = StepSamples();
        }

        for (int i = 0; i < numSamples; ++i)
        {
            if (mStepPosition >= mStepLength)
            {
                // A step of the grid. The length is read here, so a tempo or Division change
                // takes hold at a step and never in the middle of a slice.
                mStepPosition = 0;
                const int finished = mStepLength;
                mStepLength = StepSamples();

                const bool wasStuttering = mRepeatsLeft > 0;

                if (wasStuttering)
                {
                    --mRepeatsLeft;
                    mPlayPosition = 0;
                }
                else
                {
                    ++mLiveSteps;
                }

                // One draw each for whether and which way, taken on every step so the sequence
                // of choices does not depend on how the last stutter fell.
                const float draw = 0.5f * (mRng.NextBipolar() + 1.0f);
                const float direction = 0.5f * (mRng.NextBipolar() + 1.0f);

                // A stutter repeats the step that has just been played live, so there has to
                // have been one: straight after a stutter the newest recording is still the
                // slice it repeated.
                if (!wasStuttering && mLiveSteps >= 1 && chance > 0.0f && draw < chance && mRecorded >= finished)
                {
                    mRepeatsLeft = repeats;
                    mSliceLength = finished;
                    mSliceStart = (mWrite - finished + mCapacity) % mCapacity;
                    mReversed = direction < reverseChance;
                    mPlayPosition = 0;
                    mLiveSteps = 0;
                }

                // A slice's repeats are as long as the slice, whatever the grid does meanwhile.
                if (mRepeatsLeft > 0)
                {
                    mStepLength = mSliceLength;
                }
            }

            const bool stuttering = mRepeatsLeft > 0;
            mWetShare = stuttering ? std::min(1.0f, mWetShare + shareStep) : std::max(0.0f, mWetShare - shareStep);
            int readIndex = 0;
            float edge = 0.0f;

            if (mSliceLength > 0 && (stuttering || mWetShare > 0.0f))
            {
                const int position = std::min(mPlayPosition, mSliceLength - 1);
                const int offset = mReversed ? (mSliceLength - 1 - position) : position;
                readIndex = (mSliceStart + offset) % mCapacity;
                edge = EdgeGain(position, mSliceLength);
            }

            for (int c = 0; c < channels; ++c)
            {
                const float sample = in[c] ? in[c][i] : 0.0f;
                const float dry = IsFinite(sample) ? sample : 0.0f;
                auto& buffer = mBuffers[static_cast<std::size_t>(c)];
                float wet = dry;

                if (mWetShare > 0.0f && mSliceLength > 0)
                {
                    const float slice = buffer[static_cast<std::size_t>(readIndex)] * edge;
                    wet = dry + mWetShare * (slice - dry);
                }

                // Recording stops while a stutter runs, so the slice it repeats is never
                // overwritten however many repeats there are.
                if (!stuttering)
                {
                    buffer[static_cast<std::size_t>(mWrite)] = dry;
                }

                if (out[c])
                {
                    out[c][i] = dry + mix * (wet - dry);
                }
            }

            if (!stuttering)
            {
                mWrite = (mWrite + 1) % mCapacity;
                mRecorded = std::min(mRecorded + 1, mCapacity);
            }

            if (stuttering || mWetShare > 0.0f)
            {
                ++mPlayPosition;
            }

            ++mStepPosition;
        }
    }

    std::atomic<double> mBpm{tempo_sync::kDefaultBpm};
    std::atomic<int> mDivision{stutter::kDefaultDivision};
    std::atomic<float> mChance{0.25f};
    std::atomic<int> mRepeats{3};
    std::atomic<float> mReverse{0.0f};
    std::atomic<float> mMix{1.0f};

    std::array<std::vector<float>, 2> mBuffers;
    delay_line::Rng mRng;
    int mCapacity = 0;
    int mFade = 2;
    int mWrite = 0;
    int mRecorded = 0;
    int mStepPosition = 0;
    int mStepLength = 0;
    int mRepeatsLeft = 0;
    int mSliceStart = 0;
    int mSliceLength = 0;
    int mPlayPosition = 0;
    int mLiveSteps = 0;
    float mWetShare = 0.0f;
    bool mReversed = false;
};

inline void RegisterStutterEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kStutter;
    info.aliases = {"stutter"};
    info.displayName = "Stutter";
    info.category = "delay";
    info.description = "Catches a slice of what was just played and repeats it in time, sometimes "
                       "backwards";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.parameters = {
        {"division", "Division", double{stutter::kDefaultDivision}, 0.0, 14.0, "enum", "", false, 1.0,
         tempo_sync::DivisionLabels()},
        {"chance", "Chance", 0.25, 0.0, 1.0, "amount"},
        {"repeats", "Repeats", 3.0, 1.0, double{stutter::kMaxRepeats}, "", "", false, 1.0},
        {"reverse", "Reverse", 0.0, 0.0, 1.0, "amount"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
    };
    info.presets = stutter::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<StutterEffect>(); });
}
} // namespace guitarfx
