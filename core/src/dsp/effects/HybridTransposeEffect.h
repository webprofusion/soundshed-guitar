#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/StftTransposeEffect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace guitarfx
{
namespace detail
{
inline constexpr int kHybridTransposeQualityModeThresholdSemitones = 4;

[[nodiscard]] inline int GetHybridTransposePitchMode(int semitones)
{
    return std::abs(std::clamp(semitones, -15, 0)) >= kHybridTransposeQualityModeThresholdSemitones ? 1 : 0;
}

struct HybridTransposeConfig
{
    int semitones = 0;
    int latencySamples = 0;
    int pitchMode = 0;
    int transientHoldSamples = 0;
    double shiftDepth = 0.0;
};

class HybridTransposeChannel
{
  public:
    void Prepare(double sampleRate, int maxBlockSize, int semitones, double transientHoldMs, double transientAssist,
                 double brightness)
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mAttackCoeff = ComputeEnvelopeCoeff(0.5);
        mReleaseCoeff = ComputeEnvelopeCoeff(20.0);
        mToneAlpha = ComputeToneAlpha(2200.0);
        mSplitAlpha = ComputeToneAlpha(kSplitCutoffHz);
        Configure(semitones, transientHoldMs, transientAssist, brightness);
        Reset();
    }

    void Configure(int semitones, double transientHoldMs, double transientAssist, double brightness)
    {
        const HybridTransposeConfig previousConfig = mConfig;
        const bool wasPitchPrepared = mPitchPrepared;
        mTransientAssist = std::clamp(transientAssist, 0.0, 1.0);
        mBrightness = std::clamp(brightness, 0.0, 1.0);
        mConfig = BuildConfig(mSampleRate, mMaxBlockSize, semitones, transientHoldMs);

        const StftTransposeOptions lowOptions = BuildPitchOptions(false);
        const StftTransposeOptions highOptions = BuildPitchOptions(true);
        if (!mPitchPrepared)
        {
            mLowPitch.Prepare(mSampleRate, mMaxBlockSize, mConfig.semitones, lowOptions);
            mHighPitch.Prepare(mSampleRate, mMaxBlockSize, mConfig.semitones, highOptions);
            mPitchPrepared = true;
        }
        else
        {
            mLowPitch.Configure(mConfig.semitones, lowOptions);
            mHighPitch.Configure(mConfig.semitones, highOptions);
        }

        EnsureBuffers();

        if (wasPitchPrepared &&
            (previousConfig.semitones != mConfig.semitones || previousConfig.latencySamples != mConfig.latencySamples ||
             previousConfig.pitchMode != mConfig.pitchMode))
        {
            mReconfigurationFadeSamples = std::max(1, static_cast<int>(std::lround(mSampleRate * 0.006)));
            mReconfigurationFadeRemaining = mReconfigurationFadeSamples;
        }
    }

    void Reset()
    {
        std::fill(mDelayBuffer.begin(), mDelayBuffer.end(), 0.0f);
        std::fill(mLowInputBuffer.begin(), mLowInputBuffer.end(), 0.0f);
        std::fill(mHighInputBuffer.begin(), mHighInputBuffer.end(), 0.0f);
        std::fill(mLowPitchBuffer.begin(), mLowPitchBuffer.end(), 0.0f);
        std::fill(mHighPitchBuffer.begin(), mHighPitchBuffer.end(), 0.0f);
        mWritePos = 0;
        mEnvelope = 0.0f;
        mTransientBlend = 0.0f;
        mTransientCountdown = 0;
        mToneLowpass = 0.0f;
        mInputLowState = 0.0f;
        mDryLowState = 0.0f;
        mReconfigurationFadeRemaining = 0;
        if (mPitchPrepared)
        {
            mLowPitch.Reset();
            mHighPitch.Reset();
        }
    }

    void Process(const float* input, float* wet, float* dryAligned, int numSamples)
    {
        if (!input || !wet || !dryAligned || mDelayBuffer.empty() || numSamples <= 0)
        {
            return;
        }

        EnsureBlockBuffers(numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = input[i];
            const float low = SplitLow(in, mInputLowState);
            mLowInputBuffer[static_cast<size_t>(i)] = low;
            mHighInputBuffer[static_cast<size_t>(i)] = in - low;
        }

        if (mConfig.semitones != 0 && mPitchPrepared)
        {
            mLowPitch.Process(mLowInputBuffer.data(), mLowPitchBuffer.data(), numSamples);
            mHighPitch.Process(mHighInputBuffer.data(), mHighPitchBuffer.data(), numSamples);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = input[i];
            mDelayBuffer[mWritePos] = in;

            const float alignedDry = ReadDelay(static_cast<double>(mConfig.latencySamples));
            dryAligned[i] = alignedDry;

            if (mConfig.semitones == 0)
            {
                wet[i] = alignedDry;
                AdvanceWrite();
                continue;
            }

            const float dryLow = SplitLow(alignedDry, mDryLowState);
            const float dryHigh = alignedDry - dryLow;
            const float lowPitched = mLowPitchBuffer[static_cast<size_t>(i)];
            const float highPitched = mHighPitchBuffer[static_cast<size_t>(i)];
            UpdateTransientState(std::max(std::abs(alignedDry), std::abs(dryHigh) * 1.5f));

            mToneLowpass += mToneAlpha * (highPitched - mToneLowpass);
            const float compensatedHigh = highPitched + (highPitched - mToneLowpass) *
                                                            static_cast<float>(mBrightness * 0.38 * mConfig.shiftDepth);

            const float assist = static_cast<float>(mTransientAssist * mTransientBlend);
            const float lowAssist = assist * static_cast<float>(0.18 + 0.10 * mConfig.shiftDepth);
            wet[i] = lowPitched * (1.0f - lowAssist) + compensatedHigh * (1.0f - assist) + dryHigh * assist +
                     dryLow * lowAssist;

            if (mReconfigurationFadeRemaining > 0)
            {
                const float wetWeight = 1.0f - static_cast<float>(mReconfigurationFadeRemaining) /
                                                   static_cast<float>(std::max(1, mReconfigurationFadeSamples));
                wet[i] = alignedDry * (1.0f - wetWeight) + wet[i] * wetWeight;
                --mReconfigurationFadeRemaining;
            }

            AdvanceWrite();
        }
    }

    [[nodiscard]] int GetLatencySamples() const
    {
        return mConfig.semitones == 0 ? 0 : mConfig.latencySamples;
    }

  private:
    [[nodiscard]] static HybridTransposeConfig BuildConfig(double sampleRate, int maxBlockSize, int semitones,
                                                           double transientHoldMs)
    {
        HybridTransposeConfig config;
        config.semitones = std::clamp(semitones, -15, 0);
        if (sampleRate <= 0.0 || maxBlockSize <= 0 || config.semitones == 0)
        {
            return config;
        }

        config.shiftDepth = std::clamp(-static_cast<double>(config.semitones) / 15.0, 0.0, 1.0);
        config.pitchMode = GetHybridTransposePitchMode(config.semitones);
        config.latencySamples = StftTransposeChannel::GetExpectedLatencySamples(config.semitones, config.pitchMode);
        config.transientHoldSamples =
            std::max(1, static_cast<int>(std::lround(sampleRate * std::clamp(transientHoldMs, 2.0, 40.0) * 0.001)));
        return config;
    }

    [[nodiscard]] StftTransposeOptions BuildPitchOptions(bool highBand) const
    {
        StftTransposeOptions options;
        options.mode = mConfig.pitchMode;
        options.quefrencySeconds = highBand ? 0.0 : 0.0005;
        options.timbre = highBand ? 1.08 : 0.96;
        options.normalization = true;
        return options;
    }

    void EnsureBuffers()
    {
        const size_t bufferSize = static_cast<size_t>(std::max(mConfig.latencySamples + mMaxBlockSize + 8, 4096));
        if (mDelayBuffer.size() < bufferSize)
        {
            mDelayBuffer.resize(bufferSize, 0.0f);
        }

        const size_t blockSize = static_cast<size_t>(std::max(1, mMaxBlockSize));
        if (mLowInputBuffer.size() < blockSize)
        {
            mLowInputBuffer.resize(blockSize, 0.0f);
        }
        if (mHighInputBuffer.size() < blockSize)
        {
            mHighInputBuffer.resize(blockSize, 0.0f);
        }
        if (mLowPitchBuffer.size() < blockSize)
        {
            mLowPitchBuffer.resize(blockSize, 0.0f);
        }
        if (mHighPitchBuffer.size() < blockSize)
        {
            mHighPitchBuffer.resize(blockSize, 0.0f);
        }
    }

    void EnsureBlockBuffers(int numSamples)
    {
        const size_t blockSize = static_cast<size_t>(std::max(1, numSamples));
        if (blockSize <= mLowInputBuffer.size())
        {
            return;
        }

        mLowInputBuffer.resize(blockSize, 0.0f);
        mHighInputBuffer.resize(blockSize, 0.0f);
        mLowPitchBuffer.resize(blockSize, 0.0f);
        mHighPitchBuffer.resize(blockSize, 0.0f);
    }

    [[nodiscard]] float ReadDelay(double delaySamples) const
    {
        return ReadPosition(static_cast<double>(mWritePos) - delaySamples);
    }

    [[nodiscard]] float ReadPosition(double readPos) const
    {
        if (mDelayBuffer.empty())
        {
            return 0.0f;
        }

        const double wrapped = WrapBufferPosition(readPos);
        const size_t index0 = static_cast<size_t>(wrapped);
        const size_t index1 = (index0 + 1) % mDelayBuffer.size();
        const float frac = static_cast<float>(wrapped - static_cast<double>(index0));
        const float sample0 = mDelayBuffer[index0];
        const float sample1 = mDelayBuffer[index1];
        return sample0 + (sample1 - sample0) * frac;
    }

    void UpdateTransientState(float absInput)
    {
        const float previousEnvelope = mEnvelope;
        const float coeff = absInput > previousEnvelope ? mAttackCoeff : mReleaseCoeff;
        mEnvelope = absInput + coeff * (previousEnvelope - absInput);

        if (mEnvelope > kTransientFloor && mEnvelope > previousEnvelope * kTransientTriggerRatio)
        {
            mTransientCountdown = mConfig.transientHoldSamples;
        }

        const float target = mTransientCountdown > 0 ? 1.0f : 0.0f;
        const float slew = target > mTransientBlend ? 0.30f : 0.05f;
        mTransientBlend += (target - mTransientBlend) * slew;

        if (mTransientCountdown > 0)
        {
            --mTransientCountdown;
        }
    }

    void AdvanceWrite()
    {
        ++mWritePos;
        if (mWritePos >= mDelayBuffer.size())
        {
            mWritePos = 0;
        }
    }

    [[nodiscard]] double WrapBufferPosition(double position) const
    {
        if (mDelayBuffer.empty())
        {
            return 0.0;
        }

        const double size = static_cast<double>(mDelayBuffer.size());
        while (position < 0.0)
        {
            position += size;
        }
        while (position >= size)
        {
            position -= size;
        }
        return position;
    }

    [[nodiscard]] float ComputeEnvelopeCoeff(double timeMs) const
    {
        const double seconds = std::max(1.0e-6, timeMs * 0.001);
        return static_cast<float>(std::exp(-1.0 / (seconds * std::max(1.0, mSampleRate))));
    }

    [[nodiscard]] float ComputeToneAlpha(double cutoffHz) const
    {
        const double clampedCutoff = std::clamp(cutoffHz, 20.0, 0.45 * mSampleRate);
        return static_cast<float>(1.0 -
                                  std::exp(-2.0 * 3.14159265358979323846 * clampedCutoff / std::max(1.0, mSampleRate)));
    }

    [[nodiscard]] float SplitLow(float sample, float& state) const
    {
        state += mSplitAlpha * (sample - state);
        return state;
    }

    static constexpr double kSplitCutoffHz = 900.0;
    static constexpr float kTransientFloor = 0.01f;
    static constexpr float kTransientTriggerRatio = 1.6f;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    double mTransientAssist = 0.65;
    double mBrightness = 0.35;
    HybridTransposeConfig mConfig;

    StftTransposeChannel mLowPitch;
    StftTransposeChannel mHighPitch;
    bool mPitchPrepared = false;
    std::vector<float> mDelayBuffer;
    std::vector<float> mLowInputBuffer;
    std::vector<float> mHighInputBuffer;
    std::vector<float> mLowPitchBuffer;
    std::vector<float> mHighPitchBuffer;
    size_t mWritePos = 0;
    float mEnvelope = 0.0f;
    float mTransientBlend = 0.0f;
    int mTransientCountdown = 0;
    float mToneLowpass = 0.0f;
    float mInputLowState = 0.0f;
    float mDryLowState = 0.0f;
    int mReconfigurationFadeSamples = 1;
    int mReconfigurationFadeRemaining = 0;

    float mAttackCoeff = 0.0f;
    float mReleaseCoeff = 0.0f;
    float mToneAlpha = 0.0f;
    float mSplitAlpha = 0.0f;
};
} // namespace detail

class HybridTransposeEffect : public EffectProcessor
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
        mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mDryL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mDryR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

        mActiveSemitones =
            static_cast<int>(std::round(std::clamp(mRequestedSemitones.load(std::memory_order_relaxed), -15.0, 0.0)));
        mActiveMix = std::clamp(mRequestedMix.load(std::memory_order_relaxed), 0.0, 1.0);
        mActiveTransientAssist = std::clamp(mRequestedTransientAssist.load(std::memory_order_relaxed), 0.0, 1.0);
        mActiveTransientHoldMs = std::clamp(mRequestedTransientHoldMs.load(std::memory_order_relaxed), 2.0, 40.0);
        mActiveBrightness = std::clamp(mRequestedBrightness.load(std::memory_order_relaxed), 0.0, 1.0);
        mParamChangePending.store(false, std::memory_order_relaxed);

        mLeft.Prepare(sampleRate, maxBlockSize, mActiveSemitones, mActiveTransientHoldMs, mActiveTransientAssist,
                      mActiveBrightness);
        mRight.Prepare(sampleRate, maxBlockSize, mActiveSemitones, mActiveTransientHoldMs, mActiveTransientAssist,
                       mActiveBrightness);
        mReportedLatencySamples.store(mLeft.GetLatencySamples(), std::memory_order_relaxed);
        mConfigured = true;
        Reset();
    }

    void Reset() override
    {
        if (!mConfigured)
        {
            return;
        }

        mLeft.Reset();
        mRight.Reset();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        if (!mConfigured)
        {
            return;
        }

        ApplyPendingRealtimeParams();

        if (mActiveSemitones == 0)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (outputs[ch])
                {
                    const float* source = inputs[ch] ? inputs[ch] : mZero.data();
                    std::copy(source, source + numSamples, outputs[ch]);
                }
            }
            return;
        }

        if (static_cast<size_t>(numSamples) > mWetL.size())
        {
            mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
            mDryL.resize(static_cast<size_t>(numSamples), 0.0f);
            mDryR.resize(static_cast<size_t>(numSamples), 0.0f);
            mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        const float* leftInput = inputs[0] ? inputs[0] : mZero.data();
        const float* rightInput = inputs[1] ? inputs[1] : mZero.data();

        mLeft.Process(leftInput, mWetL.data(), mDryL.data(), numSamples);
        mRight.Process(rightInput, mWetR.data(), mDryR.data(), numSamples);

        const float dryMix = static_cast<float>(1.0 - mActiveMix);
        const float wetMix = static_cast<float>(mActiveMix);
        for (int i = 0; i < numSamples; ++i)
        {
            if (outputs[0])
            {
                outputs[0][i] = mDryL[static_cast<size_t>(i)] * dryMix + mWetL[static_cast<size_t>(i)] * wetMix;
            }
            if (outputs[1])
            {
                outputs[1][i] = mDryR[static_cast<size_t>(i)] * dryMix + mWetR[static_cast<size_t>(i)] * wetMix;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "semitones")
        {
            mRequestedSemitones.store(std::clamp(value, -15.0, 0.0), std::memory_order_relaxed);
            mReportedLatencySamples.store(
                detail::StftTransposeChannel::GetExpectedLatencySamples(
                    static_cast<int>(std::round(std::clamp(value, -15.0, 0.0))),
                    detail::GetHybridTransposePitchMode(static_cast<int>(std::round(std::clamp(value, -15.0, 0.0))))),
                std::memory_order_relaxed);
            mParamChangePending.store(true, std::memory_order_release);
        }
        else if (key == "mix")
        {
            mRequestedMix.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "transientAssist")
        {
            mRequestedTransientAssist.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
            mParamChangePending.store(true, std::memory_order_release);
        }
        else if (key == "transientHoldMs")
        {
            mRequestedTransientHoldMs.store(std::clamp(value, 2.0, 40.0), std::memory_order_relaxed);
            mParamChangePending.store(true, std::memory_order_release);
        }
        else if (key == "brightness")
        {
            mRequestedBrightness.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
            mParamChangePending.store(true, std::memory_order_release);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "semitones")
        {
            return mRequestedSemitones.load(std::memory_order_relaxed);
        }
        if (key == "mix")
        {
            return mRequestedMix.load(std::memory_order_relaxed);
        }
        if (key == "transientAssist")
        {
            return mRequestedTransientAssist.load(std::memory_order_relaxed);
        }
        if (key == "transientHoldMs")
        {
            return mRequestedTransientHoldMs.load(std::memory_order_relaxed);
        }
        if (key == "brightness")
        {
            return mRequestedBrightness.load(std::memory_order_relaxed);
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "transpose_hybrid";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "pitch";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mConfigured ? mReportedLatencySamples.load(std::memory_order_relaxed) : 0;
    }

  private:
    void ApplyPendingRealtimeParams()
    {
        if (!mConfigured)
        {
            return;
        }

        mActiveMix = std::clamp(mRequestedMix.load(std::memory_order_relaxed), 0.0, 1.0);

        const bool paramChanged = mParamChangePending.exchange(false, std::memory_order_acq_rel);
        if (!paramChanged)
        {
            return;
        }

        const int requestedSemitones =
            static_cast<int>(std::round(std::clamp(mRequestedSemitones.load(std::memory_order_acquire), -15.0, 0.0)));
        const double requestedTransientAssist =
            std::clamp(mRequestedTransientAssist.load(std::memory_order_acquire), 0.0, 1.0);
        const double requestedTransientHoldMs =
            std::clamp(mRequestedTransientHoldMs.load(std::memory_order_acquire), 2.0, 40.0);
        const double requestedBrightness = std::clamp(mRequestedBrightness.load(std::memory_order_acquire), 0.0, 1.0);

        if (requestedSemitones == mActiveSemitones &&
            std::abs(requestedTransientAssist - mActiveTransientAssist) < 1.0e-9 &&
            std::abs(requestedTransientHoldMs - mActiveTransientHoldMs) < 1.0e-9 &&
            std::abs(requestedBrightness - mActiveBrightness) < 1.0e-9)
        {
            return;
        }

        mActiveSemitones = requestedSemitones;
        mActiveTransientAssist = requestedTransientAssist;
        mActiveTransientHoldMs = requestedTransientHoldMs;
        mActiveBrightness = requestedBrightness;

        mLeft.Configure(mActiveSemitones, mActiveTransientHoldMs, mActiveTransientAssist, mActiveBrightness);
        mRight.Configure(mActiveSemitones, mActiveTransientHoldMs, mActiveTransientAssist, mActiveBrightness);
        mReportedLatencySamples.store(mLeft.GetLatencySamples(), std::memory_order_relaxed);
    }

    std::atomic<double> mRequestedSemitones{0.0};
    std::atomic<double> mRequestedMix{1.0};
    std::atomic<double> mRequestedTransientAssist{0.65};
    std::atomic<double> mRequestedTransientHoldMs{12.0};
    std::atomic<double> mRequestedBrightness{0.35};
    std::atomic<bool> mParamChangePending{false};
    std::atomic<int> mReportedLatencySamples{0};
    bool mConfigured = false;

    int mActiveSemitones = 0;
    double mActiveMix = 1.0;
    double mActiveTransientAssist = 0.65;
    double mActiveTransientHoldMs = 12.0;
    double mActiveBrightness = 0.35;

    detail::HybridTransposeChannel mLeft;
    detail::HybridTransposeChannel mRight;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mDryL;
    std::vector<float> mDryR;
    std::vector<float> mZero;
};

inline void RegisterHybridTransposeEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTransposeHybrid;
    info.aliases = {"transpose_hybrid"};
    info.displayName = "Transpose (Hybrid)";
    info.category = "pitch";
    info.description =
        "Dual-band downshift transpose with adaptive polyphonic sustain shifting and transient-stable attack assist";
    info.requiresResource = false;
    info.parameters = {{"semitones", "Semitones", -5.0, -15.0, 0.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                       {"transientAssist", "Transient Assist", 0.65, 0.0, 1.0, "amount", "", true, 0.01},
                       {"transientHoldMs", "Transient Hold", 12.0, 2.0, 40.0, "ms", "", true, 0.5},
                       {"brightness", "Brightness", 0.35, 0.0, 1.0, "amount", "", true, 0.01}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<HybridTransposeEffect>(); });
}

} // namespace guitarfx