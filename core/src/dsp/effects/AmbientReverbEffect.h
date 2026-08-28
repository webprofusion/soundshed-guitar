#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace guitarfx
{
class AmbientReverbEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        constexpr double maxSizeScale = 2.8;
        constexpr double extraModulationMs = 8.0;

        const size_t maxPreDelaySamples = DelayMsToSamples(kMaxPreDelayMs + kMaxEarlyTapMs + 4.0);
        mPreDelayL.assign(maxPreDelaySamples, 0.0f);
        mPreDelayR.assign(maxPreDelaySamples, 0.0f);
        mPreDelayWrite = 0;

        for (size_t index = 0; index < kCombCount; ++index)
        {
            const size_t lenL = DelayMsToSamples(kCombMsL[index] * maxSizeScale + extraModulationMs);
            const size_t lenR = DelayMsToSamples(kCombMsR[index] * maxSizeScale + extraModulationMs);
            mCombBufferL[index].assign(lenL, 0.0f);
            mCombBufferR[index].assign(lenR, 0.0f);
            mCombWriteL[index] = 0;
            mCombWriteR[index] = 0;
            mCombFilterStateL[index] = 0.0f;
            mCombFilterStateR[index] = 0.0f;
        }

        for (size_t index = 0; index < kAllpassCount; ++index)
        {
            const size_t lenL = DelayMsToSamples(kAllpassMsL[index] * maxSizeScale + 4.0);
            const size_t lenR = DelayMsToSamples(kAllpassMsR[index] * maxSizeScale + 4.0);
            mAllpassBufferL[index].assign(lenL, 0.0f);
            mAllpassBufferR[index].assign(lenR, 0.0f);
            mAllpassWriteL[index] = 0;
            mAllpassWriteR[index] = 0;
        }

        for (size_t tap = 0; tap < kEarlyTapCount; ++tap)
        {
            mEarlyTapSamples[tap] = DelayMsToSamples(kEarlyTapMs[tap]);
            mEarlyTapMirrorSamples[tap] = DelayMsToSamples(kEarlyTapMs[kEarlyTapCount - 1 - tap] + 3.0);
        }

        mSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.015)));
        mSizeSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.18)));

        for (size_t index = 0; index < kCombCount; ++index)
        {
            mCombPrecompSinOffsets[index] = static_cast<float>(std::sin(kCombModPhaseOffsets[index]));
            mCombPrecompCosOffsets[index] = static_cast<float>(std::cos(kCombModPhaseOffsets[index]));
        }

        UpdateParameters();
        mFeedback = mFeedbackTarget;
        mDamp = mDampTarget;
        mDiffusion = mDiffusionTarget;
        mToneCoeff = mToneCoeffTarget;
        mSizeScale = mSizeScaleTarget;
        mMixSmoothed = static_cast<float>(mMix);
        mWidthSmoothed = static_cast<float>(mWidth);
        mOutputGainSmoothed = mOutputGainTarget;

        Reset();
    }

    void Reset() override
    {
        std::fill(mPreDelayL.begin(), mPreDelayL.end(), 0.0f);
        std::fill(mPreDelayR.begin(), mPreDelayR.end(), 0.0f);
        mPreDelayWrite = 0;

        for (size_t index = 0; index < kCombCount; ++index)
        {
            std::fill(mCombBufferL[index].begin(), mCombBufferL[index].end(), 0.0f);
            std::fill(mCombBufferR[index].begin(), mCombBufferR[index].end(), 0.0f);
            mCombWriteL[index] = 0;
            mCombWriteR[index] = 0;
            mCombFilterStateL[index] = 0.0f;
            mCombFilterStateR[index] = 0.0f;
        }

        for (size_t index = 0; index < kAllpassCount; ++index)
        {
            std::fill(mAllpassBufferL[index].begin(), mAllpassBufferL[index].end(), 0.0f);
            std::fill(mAllpassBufferR[index].begin(), mAllpassBufferR[index].end(), 0.0f);
            mAllpassWriteL[index] = 0;
            mAllpassWriteR[index] = 0;
        }

        mWetToneStateL = 0.0f;
        mWetToneStateR = 0.0f;
        mModPhase = 0.0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (mPreDelayL.empty() || mCombBufferL[0].empty() || mAllpassBufferL[0].empty())
        {
            CopyInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (!mEnabled)
        {
            CopyInputToOutput(inputs, outputs, numSamples);
            return;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const float inL = inputs && inputs[0] ? inputs[0][sampleIndex] : 0.0f;
            const float inR = inputs && inputs[1] ? inputs[1][sampleIndex] : inL;

            mFeedback += (mFeedbackTarget - mFeedback) * mSmoothCoeff;
            mDamp += (mDampTarget - mDamp) * mSmoothCoeff;
            mDiffusion += (mDiffusionTarget - mDiffusion) * mSmoothCoeff;
            mToneCoeff += (mToneCoeffTarget - mToneCoeff) * mSmoothCoeff;
            mSizeScale += (mSizeScaleTarget - mSizeScale) * mSizeSmoothCoeff;
            mMixSmoothed += (static_cast<float>(mMix) - mMixSmoothed) * mSmoothCoeff;
            mWidthSmoothed += (static_cast<float>(mWidth) - mWidthSmoothed) * mSmoothCoeff;
            mOutputGainSmoothed += (mOutputGainTarget - mOutputGainSmoothed) * mSmoothCoeff;

            const float wetMix = mMixSmoothed;
            const float dryMix = 1.0f - wetMix;

            const float monoIn = 0.5f * (inL + inR);
            const float sideIn = 0.5f * (inL - inR);
            const float wetInL = monoIn + sideIn * 0.3f;
            const float wetInR = monoIn - sideIn * 0.3f;

            mPreDelayL[mPreDelayWrite] = wetInL;
            mPreDelayR[mPreDelayWrite] = wetInR;

            const float preL = ReadFromDelay(mPreDelayL, mPreDelayWrite, mPreDelaySamples);
            const float preR = ReadFromDelay(mPreDelayR, mPreDelayWrite, mPreDelaySamples);

            float earlyL = preL * 0.38f;
            float earlyR = preR * 0.38f;

            for (size_t tap = 0; tap < kEarlyTapCount; ++tap)
            {
                earlyL += ReadFromDelay(mPreDelayL, mPreDelayWrite, mPreDelaySamples + mEarlyTapSamples[tap]) *
                          kEarlyTapGains[tap];
                earlyR += ReadFromDelay(mPreDelayR, mPreDelayWrite, mPreDelaySamples + mEarlyTapMirrorSamples[tap]) *
                          kEarlyTapGains[tap];
            }

            if (++mPreDelayWrite >= mPreDelayL.size())
            {
                mPreDelayWrite = 0;
            }

            // Compute LFO sin/cos once per sample via fast polynomial; then derive per-comb
            // sin(phase+offset) using the angle-addition identity instead of N separate sin() calls.
            const float phaseF = static_cast<float>(mModPhase > kPi ? mModPhase - kTwoPi : mModPhase);
            const float sinPhi = FastSin(phaseF);
            const float cosPhi = FastCos(phaseF);
            mModPhase += mModPhaseInc;

            if (mModPhase >= kTwoPi)
            {
                mModPhase -= kTwoPi;
            }

            const float feedL = preL + earlyL * 0.24f + preR * 0.08f;
            const float feedR = preR + earlyR * 0.24f + preL * 0.08f;

            float combSumL = 0.0f;
            float combSumR = 0.0f;

            for (size_t combIndex = 0; combIndex < kCombCount; ++combIndex)
            {
                // Additive modulation: sin(φ) + sin(φ+offset) via angle-addition identity.
                // Previously `sinPhi * sin(phase+offset)` caused ring-modulation artifact at 2× LFO.
                const float sinPhiOffset =
                    sinPhi * mCombPrecompCosOffsets[combIndex] + cosPhi * mCombPrecompSinOffsets[combIndex];
                const float phase = sinPhi + sinPhiOffset;
                const float modSamples = mModDepthSamples * phase * (0.6f + 0.07f * static_cast<float>(combIndex));

                const float delayL = std::clamp(DelayMsToSamplesFloat(kCombMsL[combIndex] * mSizeScale) + modSamples,
                                                1.0f, static_cast<float>(mCombBufferL[combIndex].size() - 2));
                const float delayR = std::clamp(DelayMsToSamplesFloat(kCombMsR[combIndex] * mSizeScale) - modSamples,
                                                1.0f, static_cast<float>(mCombBufferR[combIndex].size() - 2));

                const float delayedL = ReadFromDelayFractional(mCombBufferL[combIndex], mCombWriteL[combIndex], delayL);
                const float delayedR = ReadFromDelayFractional(mCombBufferR[combIndex], mCombWriteR[combIndex], delayR);

                mCombFilterStateL[combIndex] = FlushNearZero(
                    mCombFilterStateL[combIndex] + (delayedL - mCombFilterStateL[combIndex]) * (1.0f - mDamp));
                mCombFilterStateR[combIndex] = FlushNearZero(
                    mCombFilterStateR[combIndex] + (delayedR - mCombFilterStateR[combIndex]) * (1.0f - mDamp));

                mCombBufferL[combIndex][mCombWriteL[combIndex]] =
                    FlushNearZero(feedL + mCombFilterStateL[combIndex] * mFeedback);
                mCombBufferR[combIndex][mCombWriteR[combIndex]] =
                    FlushNearZero(feedR + mCombFilterStateR[combIndex] * mFeedback);

                if (++mCombWriteL[combIndex] >= mCombBufferL[combIndex].size())
                {
                    mCombWriteL[combIndex] = 0;
                }

                if (++mCombWriteR[combIndex] >= mCombBufferR[combIndex].size())
                {
                    mCombWriteR[combIndex] = 0;
                }

                combSumL += delayedL;
                combSumR += delayedR;
            }

            float lateL = combSumL / static_cast<float>(kCombCount);
            float lateR = combSumR / static_cast<float>(kCombCount);

            const float crossFeed = 0.05f + static_cast<float>(mSpace) * 0.12f;
            const float lateCrossL = lateL + lateR * crossFeed;
            const float lateCrossR = lateR + lateL * crossFeed;
            lateL = lateCrossL;
            lateR = lateCrossR;

            for (size_t allpassIndex = 0; allpassIndex < kAllpassCount; ++allpassIndex)
            {
                const float delayL = std::clamp(DelayMsToSamplesFloat(kAllpassMsL[allpassIndex] * mSizeScale), 1.0f,
                                                static_cast<float>(mAllpassBufferL[allpassIndex].size() - 2));
                const float delayR = std::clamp(DelayMsToSamplesFloat(kAllpassMsR[allpassIndex] * mSizeScale), 1.0f,
                                                static_cast<float>(mAllpassBufferR[allpassIndex].size() - 2));
                lateL = ProcessAllpass(mAllpassBufferL[allpassIndex], mAllpassWriteL[allpassIndex], delayL, lateL,
                                       mDiffusion);
                lateR = ProcessAllpass(mAllpassBufferR[allpassIndex], mAllpassWriteR[allpassIndex], delayR, lateR,
                                       mDiffusion);
            }

            float wetL = earlyL * 0.22f + lateL * 0.78f;
            float wetR = earlyR * 0.22f + lateR * 0.78f;

            mWetToneStateL = FlushNearZero(mWetToneStateL + (wetL - mWetToneStateL) * mToneCoeff);
            mWetToneStateR = FlushNearZero(mWetToneStateR + (wetR - mWetToneStateR) * mToneCoeff);
            wetL = mWetToneStateL;
            wetR = mWetToneStateR;

            const float wetMid = 0.5f * (wetL + wetR);
            const float wetSide = 0.5f * (wetL - wetR);
            const float width = mWidthSmoothed;
            wetL = (wetMid + wetSide * width) * mOutputGainSmoothed;
            wetR = (wetMid - wetSide * width) * mOutputGainSmoothed;

            if (outputs[0])
            {
                outputs[0][sampleIndex] = inL * dryMix + wetL * wetMix;
            }

            if (outputs[1])
            {
                outputs[1][sampleIndex] = inR * dryMix + wetR * wetMix;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "decay")
        {
            mDecay = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "space")
        {
            mSpace = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "diffusion")
        {
            mDiffusionAmount = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "preDelay")
        {
            mPreDelayMs = std::clamp(value, 0.0, kMaxPreDelayMs);
        }
        else if (key == "tone")
        {
            mTone = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "width")
        {
            mWidth = std::clamp(value, 0.0, 1.25);
        }
        else if (key == "modRate")
        {
            mModRateHz = std::clamp(value, 0.02, 2.0);
        }
        else if (key == "modDepth")
        {
            mModDepth = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "outputGain")
        {
            mOutputGainDb = std::clamp(value, -18.0, 12.0);
        }
        else
        {
            return;
        }

        UpdateParameters();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "decay")
        {
            return mDecay;
        }

        if (key == "space")
        {
            return mSpace;
        }

        if (key == "diffusion")
        {
            return mDiffusionAmount;
        }

        if (key == "preDelay")
        {
            return mPreDelayMs;
        }

        if (key == "tone")
        {
            return mTone;
        }

        if (key == "width")
        {
            return mWidth;
        }

        if (key == "modRate")
        {
            return mModRateHz;
        }

        if (key == "modDepth")
        {
            return mModDepth;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "outputGain")
        {
            return mOutputGainDb;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "reverb_ambient";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "reverb";
    }

  private:
    static constexpr size_t kCombCount = 6;
    static constexpr size_t kAllpassCount = 4;
    static constexpr size_t kEarlyTapCount = 5;
    static constexpr double kTwoPi = 6.2831853071795864769;
    static constexpr double kMaxPreDelayMs = 200.0;
    static constexpr double kMaxEarlyTapMs = 52.0;

    static constexpr std::array<double, kCombCount> kCombMsL = {41.2, 48.9, 56.7, 64.1, 72.4, 81.8};
    static constexpr std::array<double, kCombCount> kCombMsR = {42.8, 50.4, 58.1, 65.8, 74.0, 83.1};
    static constexpr std::array<double, kAllpassCount> kAllpassMsL = {8.1, 12.7, 17.8, 24.9};
    static constexpr std::array<double, kAllpassCount> kAllpassMsR = {8.9, 13.6, 18.7, 26.1};
    static constexpr std::array<double, kEarlyTapCount> kEarlyTapMs = {7.0, 13.0, 21.0, 34.0, 49.0};
    static constexpr std::array<float, kEarlyTapCount> kEarlyTapGains = {0.24f, 0.18f, 0.14f, 0.10f, 0.07f};
    static constexpr std::array<double, kCombCount> kCombModPhaseOffsets = {0.0, 0.9, 1.7, 2.6, 3.8, 4.9};
    static constexpr double kPi = kTwoPi * 0.5;

    size_t DelayMsToSamples(double ms) const
    {
        return std::max<size_t>(1, static_cast<size_t>(ms * mSampleRate * 0.001));
    }

    float DelayMsToSamplesFloat(double ms) const
    {
        return static_cast<float>(std::max(1.0, ms * mSampleRate * 0.001));
    }

    static float ReadFromDelay(const std::vector<float>& buffer, size_t writePos, size_t delaySamples)
    {
        if (buffer.empty())
        {
            return 0.0f;
        }

        const size_t back = std::min(delaySamples, buffer.size() - 1);
        const size_t readPos = (writePos + buffer.size() - back) % buffer.size();
        return buffer[readPos];
    }

    float ReadFromDelayFractional(const std::vector<float>& buffer, size_t writePos, float delaySamples) const
    {
        if (buffer.empty())
        {
            return 0.0f;
        }

        const size_t delayFloor = static_cast<size_t>(delaySamples);
        const float frac = delaySamples - static_cast<float>(delayFloor);
        const float sample0 = ReadFromDelay(buffer, writePos, delayFloor);
        const float sample1 = ReadFromDelay(buffer, writePos, delayFloor + 1);
        return sample0 + frac * (sample1 - sample0);
    }

    float ProcessAllpass(std::vector<float>& buffer, size_t& writePos, float delaySamples, float input, float gain)
    {
        const float delayed = ReadFromDelayFractional(buffer, writePos, delaySamples);
        const float output = delayed - input * gain;
        buffer[writePos] = input + delayed * gain;

        if (++writePos >= buffer.size())
        {
            writePos = 0;
        }

        return output;
    }

    static void CopyInputToOutput(float** inputs, float** outputs, int numSamples)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            if (!outputs[channel])
            {
                continue;
            }

            if (inputs && inputs[channel])
            {
                std::copy_n(inputs[channel], numSamples, outputs[channel]);
            }
            else
            {
                std::fill_n(outputs[channel], numSamples, 0.0f);
            }
        }
    }

    // 5th-order minimax polynomial sin approximation for x in [-π, π], max error ≈ 1.6e-5.
    static float FastSin(float x) noexcept
    {
        const float x2 = x * x;
        return x * (1.0f + x2 * (-0.16666667f + x2 * 0.00833333f));
    }

    // 4th-order polynomial cos approximation for x in [-π, π], max error ≈ 5e-5.
    static float FastCos(float x) noexcept
    {
        const float x2 = x * x;
        return 1.0f + x2 * (-0.5f + x2 * 0.04166667f);
    }

    static float FlushNearZero(float x) noexcept
    {
        return std::fabs(x) < 1.0e-9f ? 0.0f : x;
    }

    void UpdateParameters()
    {
        mPreDelaySamples = DelayMsToSamples(mPreDelayMs);
        mFeedbackTarget = static_cast<float>(std::clamp(0.72 + mDecay * 0.20 + mSpace * 0.03, 0.68, 0.95));
        mDampTarget = static_cast<float>(std::clamp(0.86 - mTone * 0.64, 0.16, 0.88));
        mDiffusionTarget = static_cast<float>(std::clamp(0.48 + mDiffusionAmount * 0.40, 0.38, 0.92));
        mToneCoeffTarget = static_cast<float>(std::clamp(0.05 + mTone * 0.28, 0.05, 0.33));
        mSizeScaleTarget = static_cast<float>(0.95 + mSpace * 1.55);
        mModPhaseInc = kTwoPi * std::clamp(mModRateHz, 0.02, 2.0) / std::max(1.0, mSampleRate);
        mModDepthSamples = DelayMsToSamplesFloat(0.08 + mModDepth * (1.2 + mSpace * 1.8));
        mOutputGainTarget = static_cast<float>(std::pow(10.0, mOutputGainDb / 20.0));
    }

    std::vector<float> mPreDelayL;
    std::vector<float> mPreDelayR;
    size_t mPreDelayWrite = 0;
    size_t mPreDelaySamples = 1;

    std::array<std::vector<float>, kCombCount> mCombBufferL;
    std::array<std::vector<float>, kCombCount> mCombBufferR;
    std::array<size_t, kCombCount> mCombWriteL{};
    std::array<size_t, kCombCount> mCombWriteR{};
    std::array<float, kCombCount> mCombFilterStateL{};
    std::array<float, kCombCount> mCombFilterStateR{};

    std::array<std::vector<float>, kAllpassCount> mAllpassBufferL;
    std::array<std::vector<float>, kAllpassCount> mAllpassBufferR;
    std::array<size_t, kAllpassCount> mAllpassWriteL{};
    std::array<size_t, kAllpassCount> mAllpassWriteR{};

    std::array<size_t, kEarlyTapCount> mEarlyTapSamples{};
    std::array<size_t, kEarlyTapCount> mEarlyTapMirrorSamples{};

    double mDecay = 0.70;
    double mSpace = 0.72;
    double mDiffusionAmount = 0.84;
    double mPreDelayMs = 26.0;
    double mTone = 0.42;
    double mWidth = 1.08;
    double mModRateHz = 0.18;
    double mModDepth = 0.38;
    double mMix = 0.28;
    double mOutputGainDb = 0.0;

    float mFeedback = 0.82f;
    float mFeedbackTarget = 0.82f;
    float mDamp = 0.46f;
    float mDampTarget = 0.46f;
    float mDiffusion = 0.72f;
    float mDiffusionTarget = 0.72f;
    float mToneCoeff = 0.16f;
    float mToneCoeffTarget = 0.16f;
    float mSizeScale = 1.8f;
    float mSizeScaleTarget = 1.8f;
    float mMixSmoothed = 0.28f;
    float mWidthSmoothed = 1.08f;
    float mOutputGainSmoothed = 1.0f;
    float mOutputGainTarget = 1.0f;
    float mModDepthSamples = 6.0f;
    float mSmoothCoeff = 0.0f;
    float mSizeSmoothCoeff = 0.0f;
    float mWetToneStateL = 0.0f;
    float mWetToneStateR = 0.0f;
    double mModPhase = 0.0;
    double mModPhaseInc = 0.0;

    // Precomputed sin/cos of per-comb LFO phase offsets — avoids kCombCount sin() calls per sample.
    std::array<float, kCombCount> mCombPrecompSinOffsets{};
    std::array<float, kCombCount> mCombPrecompCosOffsets{};
};

inline void RegisterAmbientReverbEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbAmbient;
    info.aliases = {"reverb_ambient"};
    info.displayName = "Ambient Reverb";
    info.category = "reverb";
    info.description = "Long, diffuse reverb with slow modulation and wide stereo bloom";
    info.requiresResource = false;
    info.parameters = {{"decay", "Decay", 0.70, 0.0, 1.0, "", "space"},
                       {"space", "Space", 0.72, 0.0, 1.0, "", "space"},
                       {"diffusion", "Diffusion", 0.4, 0.0, 1.0, "", "space"},
                       {"preDelay", "Pre-Delay", 26.0, 0.0, 200.0, "ms", "space"},
                       {"tone", "Tone", 0.42, 0.0, 1.0, "", "tone"},
                       {"width", "Width", 1.08, 0.0, 1.25, "", "tone"},
                       {"modRate", "Mod Rate", 0.18, 0.02, 2.0, "Hz", "modulation"},
                       {"modDepth", "Mod Depth", 0.38, 0.0, 1.0, "", "modulation"},
                       {"mix", "Mix", 0.28, 0.0, 1.0, "", "tone"},
                       {"outputGain", "Output", 0.0, -18.0, 12.0, "dB", "tone", true}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<AmbientReverbEffect>(); });
}
} // namespace guitarfx