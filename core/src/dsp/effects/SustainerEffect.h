#pragma once

/**
 * Sustainer: a held note that blooms into feedback instead of dying away.
 *
 * A loud amp in the room feeds a ringing string back into itself, so the note swells and holds,
 * often turning over into a harmonic. Through headphones nothing does that, and however much gain
 * follows, a note ends when the string stops. This stands in for the room: it follows the note
 * being played, and once it has been held for a moment it brings in a tone at that pitch (or a
 * harmonic of it) that carries on as the string fades.
 *
 * It belongs in front of the amp, where the amp's own distortion turns its plain sine into the
 * same harmonics the guitar's signal gets, so the two are heard as one note.
 *
 *     input --+-------------------------------------------+--> output
 *             |                                           |
 *             +--> pitch tracker --> oscillator --> gain --+
 *             +--> envelopes --> note state ------^
 *
 * The note state, in order:
 *   - A pick (the fast envelope jumping clear of the slow one) starts a note: whatever tone was
 *     sounding is let go, and the note's level is taken from the pick's peak.
 *   - While the note is playing and has a single pitch, a timer runs. When it passes Bloom, the
 *     tone fades in at Amount times the pick's level, an octave or more up if Harmonic says so.
 *   - When the string has faded under Sensitivity the tone carries on for Hold, then lets go.
 *
 * Notes shorter than Bloom are left alone, so fast playing is untouched, and a chord has no single
 * pitch to follow, so it gets none either. The pitch is glided, so vibrato and bends carry over.
 * The tracker is monophonic and listens to the input only, never to its own tone.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
namespace sustainer
{
/// The harmonic of the played note the tone sits on. Index order is stored in presets: append,
/// never reorder.
inline constexpr double kHarmonicRatios[] = {1.0, 2.0, 3.0, 4.0};

inline const std::vector<std::string>& HarmonicLabels()
{
    static const std::vector<std::string> labels = {"Unison", "Octave", "Octave + Fifth", "Two Octaves"};
    return labels;
}

inline constexpr double kMinBloomSeconds = 0.05;
inline constexpr double kMaxBloomSeconds = 3.0;
inline constexpr double kMaxHoldSeconds = 20.0;
inline constexpr double kMinSensitivityDb = -70.0;
inline constexpr double kMaxSensitivityDb = -30.0;

/// How the tone comes in and goes: slow enough in to sound like a string being driven, quick
/// enough out that a new note is not smeared by the last one.
inline constexpr double kSwellMs = 250.0;
inline constexpr double kLetGoMs = 45.0;
/// Vibrato and bends are followed this fast; any faster and the tracker's own jitter is heard.
inline constexpr double kPitchGlideMs = 18.0;
/// A pick is the fast envelope standing this far clear of the slow one.
inline constexpr float kPickRatio = 2.5f;
/// Picks closer together than this are one pick: the envelopes need that long to settle.
inline constexpr double kPickGuardMs = 80.0;
/// How long after a pick its peak is still being taken.
inline constexpr double kPeakWindowMs = 60.0;

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        b.Defaults("endless-note", "Endless Note"),
        b.Make("octave-bloom", "Octave Bloom", {{"harmonic", 1.0}, {"bloom", 1.1}, {"amount", 0.45}}),
        b.Make("harmonic-squeal", "Harmonic Squeal", {{"harmonic", 2.0}, {"bloom", 0.5}, {"amount", 0.4}}),
        b.Make("slow-rise", "Slow Rise", {{"bloom", 2.0}, {"hold", 8.0}, {"amount", 0.6}}),
        b.Make("quick-catch", "Quick Catch", {{"bloom", 0.25}, {"hold", 2.0}, {"amount", 0.5}}),
    };
}
} // namespace sustainer

class SustainerEffect : public EffectProcessor
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
        mTracker.Prepare(sampleRate);
        mFast.Prepare(sampleRate);
        mFast.SetTimes(1.0, 30.0);
        mSlow.Prepare(sampleRate);
        mSlow.SetTimes(30.0, 300.0);
        mSwell = Coefficient(sustainer::kSwellMs);
        mLetGo = Coefficient(sustainer::kLetGoMs);
        mGlide = Coefficient(sustainer::kPitchGlideMs);
        mPickGuard = static_cast<int>(sustainer::kPickGuardMs * 0.001 * sampleRate);
        mPeakWindow = static_cast<int>(sustainer::kPeakWindowMs * 0.001 * sampleRate);
        Reset();
    }

    void Reset() override
    {
        mTracker.Reset();
        mFast.Reset();
        mSlow.Reset();
        mPhase = 0.0;
        mFrequency = 0.0;
        mGain = 0.0f;
        mNotePeak = 0.0f;
        mHeldSamples = 0.0;
        mHoldLeft = 0.0;
        mSincePick = 1 << 30;
        mBloomed = false;
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

        if (key == "amount")
        {
            mAmount.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "bloom")
        {
            mBloom.store(
                static_cast<float>(std::clamp(value, sustainer::kMinBloomSeconds, sustainer::kMaxBloomSeconds)),
                std::memory_order_relaxed);
        }
        else if (key == "hold")
        {
            mHold.store(static_cast<float>(std::clamp(value, 0.0, sustainer::kMaxHoldSeconds)),
                        std::memory_order_relaxed);
        }
        else if (key == "harmonic")
        {
            mHarmonic.store(static_cast<int>(std::lround(std::clamp(value, 0.0, 3.0))), std::memory_order_relaxed);
        }
        else if (key == "sensitivity")
        {
            mSensitivityDb.store(
                static_cast<float>(std::clamp(value, sustainer::kMinSensitivityDb, sustainer::kMaxSensitivityDb)),
                std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "amount")
        {
            return mAmount.load(std::memory_order_relaxed);
        }

        if (key == "bloom")
        {
            return mBloom.load(std::memory_order_relaxed);
        }

        if (key == "hold")
        {
            return mHold.load(std::memory_order_relaxed);
        }

        if (key == "harmonic")
        {
            return mHarmonic.load(std::memory_order_relaxed);
        }

        if (key == "sensitivity")
        {
            return mSensitivityDb.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "sustainer";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

  private:
    static constexpr double kTwoPi = 6.28318530717958648;

    [[nodiscard]] float Coefficient(double milliseconds) const noexcept
    {
        return static_cast<float>(1.0 - std::exp(-1000.0 / (milliseconds * std::max(1.0, mSampleRate))));
    }

    void LetGo() noexcept
    {
        mBloomed = false;
        mHeldSamples = 0.0;
        mHoldLeft = 0.0;
    }

    void Run(const float* const* in, float* const* out, int channels, int numSamples) noexcept
    {
        const float amount = mAmount.load(std::memory_order_relaxed);
        const double bloomSamples = mBloom.load(std::memory_order_relaxed) * mSampleRate;
        const double holdSamples = mHold.load(std::memory_order_relaxed) * mSampleRate;
        const float threshold = std::pow(10.0f, mSensitivityDb.load(std::memory_order_relaxed) / 20.0f);
        const double ratio = sustainer::kHarmonicRatios[std::clamp(mHarmonic.load(std::memory_order_relaxed), 0, 3)];
        // Kept clear of Nyquist at low sample rates, where the top harmonic of a high note is not.
        const double highestHz = 0.45 * mSampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            float dry[2] = {0.0f, 0.0f};
            float sum = 0.0f;

            for (int c = 0; c < channels; ++c)
            {
                const float sample = in[c] ? in[c][i] : 0.0f;
                dry[c] = IsFinite(sample) ? std::clamp(sample, -8.0f, 8.0f) : 0.0f;
                sum += dry[c];
            }

            const float detect = sum / static_cast<float>(channels);
            mTracker.Push(detect);
            const float fast = mFast.Process(detect);
            const float slow = mSlow.Process(detect);
            mSincePick = std::min(mSincePick + 1, 1 << 30);

            if (fast > threshold && fast > sustainer::kPickRatio * slow && mSincePick > mPickGuard)
            {
                // A new note: the old tone goes, and this one's level is taken afresh.
                LetGo();
                mSincePick = 0;
                mNotePeak = fast;
            }
            else if (mSincePick < mPeakWindow)
            {
                mNotePeak = std::max(mNotePeak, fast);
            }

            const bool playing = slow > threshold;
            const bool pitched = mTracker.HasPitch() && mTracker.FrequencyHz() > 0.0;

            if (playing)
            {
                mHoldLeft = holdSamples;

                if (pitched)
                {
                    mHeldSamples += 1.0;
                }
                else if (!mBloomed)
                {
                    // Noise or a chord: wind the timer back rather than stop it dead, so one
                    // missed detection on a real note costs little.
                    mHeldSamples = std::max(0.0, mHeldSamples - 2.0);
                }

                mBloomed = mBloomed || mHeldSamples >= bloomSamples;
            }
            else if (mBloomed)
            {
                mHoldLeft -= 1.0;

                if (mHoldLeft <= 0.0)
                {
                    LetGo();
                }
            }
            else
            {
                mHeldSamples = 0.0;
            }

            if (pitched)
            {
                const double target = std::min(mTracker.FrequencyHz() * ratio, highestHz);
                // The first pitch of a note is taken as it is; after that it glides.
                mFrequency =
                    (mGain < 1.0e-4f || mFrequency <= 0.0) ? target : mFrequency + mGlide * (target - mFrequency);
            }

            const float gainTarget = (mBloomed && mFrequency > 0.0) ? 1.0f : 0.0f;
            mGain += (gainTarget > mGain ? mSwell : mLetGo) * (gainTarget - mGain);

            if (mGain < 1.0e-6f && gainTarget == 0.0f)
            {
                mGain = 0.0f;
            }

            float tone = 0.0f;

            if (mGain > 0.0f)
            {
                tone = amount * std::min(mNotePeak, 1.0f) * mGain * static_cast<float>(std::sin(mPhase));
                mPhase += kTwoPi * mFrequency / mSampleRate;

                if (mPhase >= kTwoPi)
                {
                    mPhase -= kTwoPi;
                }
            }

            for (int c = 0; c < channels; ++c)
            {
                if (out[c])
                {
                    out[c][i] = dry[c] + tone;
                }
            }
        }

        if (!IsFinite(mPhase) || !IsFinite(mFrequency) || !IsFinite(mGain) || !IsFinite(mNotePeak))
        {
            Reset();
        }
    }

    std::atomic<float> mAmount{0.5f};
    std::atomic<float> mBloom{0.7f};
    std::atomic<float> mHold{4.0f};
    std::atomic<int> mHarmonic{0};
    std::atomic<float> mSensitivityDb{-50.0f};

    PitchTracker mTracker;
    delay_line::EnvelopeFollower mFast;
    delay_line::EnvelopeFollower mSlow;
    double mPhase = 0.0;
    double mFrequency = 0.0;
    double mHeldSamples = 0.0;
    double mHoldLeft = 0.0;
    float mGain = 0.0f;
    float mNotePeak = 0.0f;
    float mSwell = 1.0f;
    float mLetGo = 1.0f;
    float mGlide = 1.0f;
    int mSincePick = 1 << 30;
    int mPickGuard = 0;
    int mPeakWindow = 0;
    bool mBloomed = false;
};

inline void RegisterSustainerEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kSustainer;
    info.aliases = {"sustainer"};
    info.displayName = "Sustainer";
    info.category = "dynamics";
    info.description = "A held note blooms into feedback and carries on after the string fades. Put it "
                       "before the amp";
    info.requiresResource = false;
    info.parameters = {
        {"amount", "Amount", 0.5, 0.0, 1.0, "amount"},
        WithLogTaper({"bloom", "Bloom", 0.7, sustainer::kMinBloomSeconds, sustainer::kMaxBloomSeconds, "s"}),
        {"hold", "Hold", 4.0, 0.0, sustainer::kMaxHoldSeconds, "s"},
        {"harmonic", "Harmonic", 0.0, 0.0, 3.0, "enum", "", false, 1.0, sustainer::HarmonicLabels()},
        {"sensitivity", "Sensitivity", -50.0, sustainer::kMinSensitivityDb, sustainer::kMaxSensitivityDb, "dB", "",
         true},
    };
    info.presets = sustainer::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<SustainerEffect>(); });
}
} // namespace guitarfx
