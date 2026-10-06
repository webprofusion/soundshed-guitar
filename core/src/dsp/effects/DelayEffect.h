#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/TempoSync.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Digital delay with high/low cut filtering, stereo spread, ping-pong,
 * LFO modulation, drive saturation, and ducking.
 *
 * Direction Reverse plays each slice of the input backwards: every Time, the last Time of
 * input is played from its end to its start. Two read heads half a slice apart, each faded in
 * and out with a sin^2 window, cover each other's ends, so the windows sum to one and no slice
 * starts with a click. The repeats fed back through Feedback are reversed again, so they
 * alternate between backwards and forwards, as most reverse pedals' do.
 */
class DelayEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        // A reverse head reaches back two slices, and a slice is up to 2 s plus 50 ms of Spread
        // and 20 ms of modulation. Forward needs only half of this.
        const size_t maxSamples = static_cast<size_t>(sampleRate * 4.2);
        mBufferL.assign(maxSamples, 0.0f);
        mBufferR.assign(maxSamples, 0.0f);

        UpdateDelaySamples();
        UpdateFilters();
        Reset();
    }

    void Reset() override
    {
        std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
        std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
        mWritePos = 0;
        mLfoPhase = 0.0f;
        mEnvelopeL = 0.0f;
        mEnvelopeR = 0.0f;
        mReversePosL = 0.0;
        mReversePosR = 0.0;
        mLpStateL = mLpStateR = 0.0f;
        mHpStateL = mHpStateR = 0.0f;
        mHpPrevInL = mHpPrevInR = 0.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (mBufferL.empty())
        {
            return;
        }

        const float feedback = static_cast<float>(mFeedback);
        const float wet = static_cast<float>(mMix);
        const float dry = 1.0f - wet;
        const float drive = static_cast<float>(mDrive);
        const float ducking = static_cast<float>(mDucking);
        const float lfoStep = static_cast<float>(mModRate / mSampleRate);
        const float modAmp = static_cast<float>(mModDepth * 0.001 * mSampleRate);
        const bool pingPong = (mStereoMode == 1);
        const bool reverse = (mDirection == 1);
        const bool hasRightInput = (inputs[1] != nullptr);
        bool dualMonoInput = false;

        if (hasRightInput && inputs[0] && inputs[1])
        {
            dualMonoInput = true;
            constexpr float kDualMonoEpsilon = 1.0e-6f;

            for (int i = 0; i < numSamples; ++i)
            {
                if (std::abs(inputs[0][i] - inputs[1][i]) > kDualMonoEpsilon)
                {
                    dualMonoInput = false;
                    break;
                }
            }
        }

        const bool monoSource = !hasRightInput || dualMonoInput;
        const size_t bufSize = mBufferL.size();
        const double maxDelay = static_cast<double>(bufSize - 2);

        for (int i = 0; i < numSamples; ++i)
        {
            // Per sample: modulate delay time, read interpolated taps, filter/drive
            // the feedback path, then mix ducked wet signal with dry input.
            const float lfoVal = std::sin(mLfoPhase * 6.28318530f);
            mLfoPhase += lfoStep;

            if (mLfoPhase >= 1.0f)
            {
                mLfoPhase -= 1.0f;
            }

            const double delayL = std::clamp(mDelaySamples + lfoVal * modAmp, 1.0, maxDelay);
            const double delayR = std::clamp(mDelaySamples + mSpreadSamples + lfoVal * modAmp, 1.0, maxDelay);
            const double sliceL = std::max(kMinReverseSlice, mDelaySamples);
            const double sliceR = std::max(kMinReverseSlice, mDelaySamples + mSpreadSamples);

            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = hasRightInput ? inputs[1][i] : inL;

            // In ping-pong mode with mono/dual-mono input, keep source energy
            // centered but add a small side bias so repeats can alternate L/R.
            float delayInL = inL;
            float delayInR = inR;

            if (pingPong && monoSource)
            {
                constexpr float kPingPongSkew = 0.3f;
                const float monoIn = inL;
                const float center = monoIn * 0.5f;
                const float skew = monoIn * kPingPongSkew;
                delayInL = center + skew;
                delayInR = center - skew;
            }

            if (std::abs(inL) > mEnvelopeL)
            {
                mEnvelopeL += 0.001f * (std::abs(inL) - mEnvelopeL);
            }
            else
            {
                mEnvelopeL += 0.0001f * (std::abs(inL) - mEnvelopeL);
            }

            if (std::abs(inR) > mEnvelopeR)
            {
                mEnvelopeR += 0.001f * (std::abs(inR) - mEnvelopeR);
            }
            else
            {
                mEnvelopeR += 0.0001f * (std::abs(inR) - mEnvelopeR);
            }

            const float duckGainL = 1.0f - ducking * std::min(mEnvelopeL * 4.0f, 1.0f);
            const float duckGainR = 1.0f - ducking * std::min(mEnvelopeR * 4.0f, 1.0f);

            // Read with linear interpolation
            float delayedL = reverse ? ReadReverse(mBufferL, bufSize, mReversePosL, sliceL, lfoVal * modAmp, maxDelay)
                                     : ReadInterp(mBufferL, bufSize, delayL);
            float delayedR = reverse ? ReadReverse(mBufferR, bufSize, mReversePosR, sliceR, lfoVal * modAmp, maxDelay)
                                     : ReadInterp(mBufferR, bufSize, delayR);

            // Tone shaping on delayed signal (shapes feedback colour on each repeat)
            delayedL = ApplyLP(mLpStateL, mLpCoeff, delayedL);
            delayedR = ApplyLP(mLpStateR, mLpCoeff, delayedR);
            delayedL = ApplyHP(mHpStateL, mHpPrevInL, mHpCoeff, delayedL);
            delayedR = ApplyHP(mHpStateR, mHpPrevInR, mHpCoeff, delayedR);

            // Ping-pong: swap which channel feeds back into which buffer
            float fbL = (pingPong ? delayedR : delayedL) * feedback;
            float fbR = (pingPong ? delayedL : delayedR) * feedback;

            // Soft saturation in feedback path
            if (drive > 0.0f)
            {
                const float g = 1.0f + drive * 4.0f;
                fbL = std::tanh(fbL * g) / g;
                fbR = std::tanh(fbR * g) / g;
            }

            mBufferL[mWritePos] = delayInL + fbL;
            mBufferR[mWritePos] = delayInR + fbR;

            if (outputs[0])
            {
                outputs[0][i] = inL * dry + delayedL * wet * duckGainL;
            }

            if (outputs[1])
            {
                outputs[1][i] = inR * dry + delayedR * wet * duckGainR;
            }

            mWritePos = (mWritePos + 1) % bufSize;
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "bpm")
        {
            mBpm = tempo_sync::ClampBpm(value);

            if (mSyncMode == tempo_sync::kSyncModeTempo)
            {
                UpdateDelaySamples();
            }
        }
        else if (key == "syncMode")
        {
            mSyncMode = tempo_sync::ClampSyncMode(value);
            UpdateDelaySamples();
        }
        else if (key == "syncDivision")
        {
            mSyncDivision = tempo_sync::ClampDivision(value);

            if (mSyncMode == tempo_sync::kSyncModeTempo)
            {
                UpdateDelaySamples();
            }
        }
        else if (key == "time" || key == "timeMs")
        {
            mDelayMs = std::clamp(value, 1.0, 2000.0);

            if (mSyncMode != tempo_sync::kSyncModeTempo)
            {
                UpdateDelaySamples();
            }
        }
        else if (key == "feedback")
        {
            mFeedback = std::clamp(value, 0.0, 0.95);
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "highCut")
        {
            mHighCutHz = std::clamp(value, 200.0, 20000.0);
            UpdateFilters();
        }
        else if (key == "lowCut")
        {
            mLowCutHz = std::clamp(value, 20.0, 5000.0);
            UpdateFilters();
        }
        else if (key == "stereoMode")
        {
            mStereoMode = static_cast<int>(std::round(std::clamp(value, 0.0, 1.0)));
        }
        else if (key == "spread")
        {
            mSpreadMs = std::clamp(value, 0.0, 50.0);
            UpdateDelaySamples();
        }
        else if (key == "modRate")
        {
            mModRate = std::clamp(value, 0.0, 10.0);
        }
        else if (key == "modDepth")
        {
            mModDepth = std::clamp(value, 0.0, 20.0);
        }
        else if (key == "drive")
        {
            mDrive = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "ducking")
        {
            mDucking = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "direction")
        {
            mDirection = static_cast<int>(std::round(std::clamp(value, 0.0, 1.0)));
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bpm")
        {
            return mBpm;
        }

        if (key == "syncMode")
        {
            return mSyncMode;
        }

        if (key == "syncDivision")
        {
            return mSyncDivision;
        }

        if (key == "effectiveTimeMs")
        {
            return GetEffectiveDelayMs();
        }

        if (key == "time" || key == "timeMs")
        {
            return mDelayMs;
        }

        if (key == "feedback")
        {
            return mFeedback;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "highCut")
        {
            return mHighCutHz;
        }

        if (key == "lowCut")
        {
            return mLowCutHz;
        }

        if (key == "stereoMode")
        {
            return mStereoMode;
        }

        if (key == "spread")
        {
            return mSpreadMs;
        }

        if (key == "modRate")
        {
            return mModRate;
        }

        if (key == "modDepth")
        {
            return mModDepth;
        }

        if (key == "drive")
        {
            return mDrive;
        }

        if (key == "ducking")
        {
            return mDucking;
        }

        if (key == "direction")
        {
            return mDirection;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "delay_digital";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "delay";
    }

    [[nodiscard]] bool ProducesStereoOutput() const override
    {
        return mStereoMode == 1;
    }

  private:
    [[nodiscard]] double GetEffectiveDelayMs() const
    {
        if (mSyncMode != tempo_sync::kSyncModeTempo)
        {
            return mDelayMs;
        }

        return std::clamp(tempo_sync::DivisionDelayMs(mBpm, mSyncDivision), 1.0, 2000.0);
    }

    // Read from circular buffer with linear interpolation.
    [[nodiscard]] float ReadInterp(const std::vector<float>& buf, size_t bufSize, double delay) const
    {
        const size_t intD = static_cast<size_t>(delay);
        const float frac = static_cast<float>(delay - static_cast<double>(intD));
        const size_t posA = (mWritePos + bufSize - intD) % bufSize;
        const size_t posB = (mWritePos + bufSize - intD - 1) % bufSize;
        return buf[posA] * (1.0f - frac) + buf[posB] * frac;
    }

    /// Reverse slices shorter than this (about 10 ms at 48 kHz) are a buzz, not a reversal.
    static constexpr double kMinReverseSlice = 480.0;

    /// One sample from a reverse head pair. `position` counts samples into the current slice
    /// and is advanced here. A head that is `k` samples into its slice reads `2k` back, so it
    /// moves backwards through the input at normal speed, from now to a slice ago; the second
    /// head runs half a slice behind, and each is faded by sin^2 of its place in the slice.
    [[nodiscard]] float ReadReverse(const std::vector<float>& buf, size_t bufSize, double& position, double slice,
                                    double modulation, double maxDelay) const
    {
        if (position >= slice)
        {
            position = std::fmod(position, slice);
        }

        const double second = (position + 0.5 * slice >= slice) ? position - 0.5 * slice : position + 0.5 * slice;
        const auto window = static_cast<float>(std::sin(3.14159265358979323846 * position / slice));
        const float firstGain = window * window;

        const float first = ReadInterp(buf, bufSize, std::clamp(2.0 * position + modulation, 1.0, maxDelay));
        const float other = ReadInterp(buf, bufSize, std::clamp(2.0 * second + modulation, 1.0, maxDelay));
        position += 1.0;
        return first * firstGain + other * (1.0f - firstGain);
    }

    // One-pole low-pass
    static float ApplyLP(float& state, float coeff, float in)
    {
        state = coeff * in + (1.0f - coeff) * state;
        return state;
    }

    // One-pole high-pass
    static float ApplyHP(float& state, float& prevIn, float coeff, float in)
    {
        state = coeff * (state + in - prevIn);
        prevIn = in;
        return state;
    }

    void UpdateDelaySamples()
    {
        const double delayMs = GetEffectiveDelayMs();
        mDelaySamples = mSampleRate * delayMs * 0.001;
        mSpreadSamples = mSampleRate * mSpreadMs * 0.001;

        if (!mBufferL.empty())
        {
            const double maxD = static_cast<double>(mBufferL.size() - 2);
            mDelaySamples = std::min(mDelaySamples, maxD);
            mSpreadSamples = std::min(mSpreadSamples, maxD - mDelaySamples);
        }
    }

    void UpdateFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        constexpr double pi2 = 6.28318530717958647;
        mLpCoeff = static_cast<float>(1.0 - std::exp(-pi2 * mHighCutHz / mSampleRate));
        mHpCoeff = static_cast<float>(std::exp(-pi2 * mLowCutHz / mSampleRate));
    }

    // Buffers
    std::vector<float> mBufferL, mBufferR;
    size_t mWritePos = 0;

    // Derived state
    double mDelaySamples = 0.0;
    double mSpreadSamples = 0.0;

    // Parameters
    double mDelayMs = 300.0;
    double mFeedback = 0.3;
    double mMix = 0.3;
    double mHighCutHz = 8000.0;
    double mLowCutHz = 20.0;
    double mBpm = tempo_sync::kDefaultBpm;
    int mStereoMode = 0;
    int mDirection = 0;
    int mSyncMode = tempo_sync::kSyncModeOff;
    int mSyncDivision = 4;
    double mSpreadMs = 0.0;
    double mModRate = 0.0;
    double mModDepth = 0.0;
    double mDrive = 0.0;
    double mDucking = 0.0;

    // Filter coefficients & state
    float mLpCoeff = 1.0f;
    float mHpCoeff = 1.0f;
    float mLpStateL = 0.0f, mLpStateR = 0.0f;
    float mHpStateL = 0.0f, mHpStateR = 0.0f;
    float mHpPrevInL = 0.0f, mHpPrevInR = 0.0f;

    // LFO & envelope
    float mLfoPhase = 0.0f;
    float mEnvelopeL = 0.0f;
    float mEnvelopeR = 0.0f;
    // Samples into the current reverse slice, per channel.
    double mReversePosL = 0.0;
    double mReversePosR = 0.0;
};

namespace digital_delay
{
/// Division is left out of a preset with Sync off, so the player's own survives; Dotted Eighth
/// names its division, and its Time is the same at 120 bpm, so turning Sync off keeps the
/// feel. Every advanced control is set too: hidden, a stale value would be heard and not seen.
/// Keep ids stable once shipped; the UIs list them.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"syncDivision"});

    return {
        b.Defaults("dd3", "DD-3"),
        b.Make("slapback", "Slapback",
               {{"time", 100.0}, {"feedback", 0.1}, {"mix", 0.35}, {"highCut", 6000.0}, {"lowCut", 100.0}}),
        b.Make("dotted-eighth", "Dotted Eighth",
               {{"time", 375.0},
                {"syncMode", 1.0},
                {"syncDivision", 8.0},
                {"feedback", 0.35},
                {"mix", 0.4},
                {"highCut", 7000.0},
                {"lowCut", 150.0}}),
        b.Make("ping-pong", "Ping-Pong",
               {{"time", 400.0},
                {"feedback", 0.45},
                {"mix", 0.35},
                {"highCut", 6000.0},
                {"lowCut", 120.0},
                {"stereoMode", 1.0}}),
        // The echoes stay back while you play and bloom in the gaps.
        b.Make("ducked-lead", "Ducked Lead",
               {{"time", 450.0},
                {"feedback", 0.45},
                {"mix", 0.35},
                {"highCut", 5500.0},
                {"lowCut", 180.0},
                {"spread", 12.0},
                {"modRate", 0.6},
                {"modDepth", 1.2},
                {"ducking", 0.8}}),
        b.Make("ambient-wash", "Ambient Wash",
               {{"time", 650.0},
                {"feedback", 0.65},
                {"mix", 0.3},
                {"highCut", 3500.0},
                {"lowCut", 250.0},
                {"spread", 30.0},
                {"modRate", 0.35},
                {"modDepth", 3.0},
                {"ducking", 0.3}}),
        b.Make("lo-fi-echo", "Lo-Fi Echo",
               {{"time", 280.0},
                {"feedback", 0.5},
                {"mix", 0.3},
                {"highCut", 2200.0},
                {"lowCut", 500.0},
                {"drive", 1.0},
                {"modRate", 1.2},
                {"modDepth", 0.4}}),
        // Each slice played backwards, swelling in behind the note.
        b.Make("reverse", "Reverse",
               {{"time", 550.0}, {"feedback", 0.25}, {"mix", 0.45}, {"highCut", 7000.0}, {"direction", 1.0}}),
        b.Make("reverse-wash", "Reverse Wash",
               {{"time", 800.0},
                {"feedback", 0.55},
                {"mix", 0.4},
                {"highCut", 4500.0},
                {"lowCut", 150.0},
                {"spread", 20.0},
                {"modRate", 0.4},
                {"modDepth", 1.5},
                {"direction", 1.0}}),
    };
}
} // namespace digital_delay

inline void RegisterDelayEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDelayDigital;
    info.aliases = {"delay_digital"};
    info.displayName = "Digital Delay";
    info.category = "delay";
    info.description = "Stereo digital delay with tone shaping, modulation, drive, and ducking";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.parameters = {
        {"time", "Time", 300.0, 1.0, 2000.0, "ms"},
        {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "timing", false, 1.0, tempo_sync::SyncModeLabels()},
        {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "timing", false, 1.0, tempo_sync::DivisionLabels()},
        {"feedback", "Feedback", 0.4, 0.0, 0.95, "amount"},
        {"mix", "Mix", 0.3, 0.0, 1.0, "amount"},
        WithLogTaper({"highCut", "High Cut", 8000.0, 200.0, 20000.0, "Hz"}),
        WithLogTaper({"lowCut", "Low Cut", 20.0, 20.0, 5000.0, "Hz"}),
        {"drive", "Drive", 0.0, 0.0, 1.0, "amount"},
        {"stereoMode", "Stereo", 0.0, 0.0, 1.0, "enum", "", false, 1.0, {"Normal", "Ping-Pong"}},
        {"spread", "Spread", 0.0, 0.0, 50.0, "ms", "", true},
        {"modRate", "Mod Rate", 0.0, 0.0, 10.0, "Hz", "", true},
        {"modDepth", "Mod Depth", 0.0, 0.0, 20.0, "ms", "", true},
        {"ducking", "Ducking", 0.0, 0.0, 1.0, "amount", "", true},
        {"direction", "Direction", 0.0, 0.0, 1.0, "enum", "", false, 1.0, {"Forward", "Reverse"}}};
    info.presets = digital_delay::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<DelayEffect>(); });
}
} // namespace guitarfx
