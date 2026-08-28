#pragma once

#include "NAM/dsp.h"

// ResamplingContainer pulls in WDL, which includes <windows.h> on Win32. That
// defines UpdateResource as UpdateResourceA, which then mangles unrelated member
// calls such as ResourceLibrary::UpdateResource in every translation unit that
// includes this header. NOMINMAX is already set for the core target; drop the
// remaining macros that collide with our own API names.
#include "dsp/ResamplingContainer/ResamplingContainer.h"

#ifdef UpdateResource
    #undef UpdateResource
#endif
#ifdef RemoveResource
    #undef RemoveResource
#endif

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <memory>
#include <vector>

namespace guitarfx
{
inline constexpr int kNamOversamplingMaxIndex = 5;
inline constexpr int kNamAntiAliasPhaseMaxIndex = 2;
inline constexpr int kNamOversamplingIndexDefault = 0;
inline constexpr int kNamAntiAliasPhaseIndexDefault = 0;

[[nodiscard]] inline int NamOversamplingFactorFromIndex(double value)
{
    const int index = std::clamp(static_cast<int>(std::llround(value)), 0, kNamOversamplingMaxIndex);
    return 1 << index;
}

[[nodiscard]] inline int NamOversamplingIndexFromFactor(int factor)
{
    int index = 0;
    int normalized = std::max(1, factor);
    while (normalized > 1 && index < kNamOversamplingMaxIndex)
    {
        normalized >>= 1;
        ++index;
    }
    return index;
}

[[nodiscard]] inline int SanitizeNamOversamplingIndex(double value)
{
    if (!std::isfinite(value))
    {
        return kNamOversamplingIndexDefault;
    }
    return std::clamp(static_cast<int>(std::llround(value)), 0, kNamOversamplingMaxIndex);
}

[[nodiscard]] inline int SanitizeNamAntiAliasPhaseIndex(double value)
{
    if (!std::isfinite(value))
    {
        return kNamAntiAliasPhaseIndexDefault;
    }
    return std::clamp(static_cast<int>(std::llround(value)), 0, kNamAntiAliasPhaseMaxIndex);
}

// Oversampling and its anti-alias filter phase are owned per NAM node, not by a
// process-wide global: a DAW loads every plugin instance into one process, so a
// shared static would force one quality tier on every instance in the project.
// PluginController pushes each instance's values down as node config, and
// SignalGraphExecutor keeps them as type defaults so nodes built later inherit them.
[[nodiscard]] inline dsp::EAntiAliasFilterPhase NamAntiAliasPhaseFromIndex(double value)
{
    switch (std::clamp(static_cast<int>(std::llround(value)), 0, kNamAntiAliasPhaseMaxIndex))
    {
    case 1:
        return dsp::EAntiAliasFilterPhase::LinearCascadedFIRShort;
    case 2:
        return dsp::EAntiAliasFilterPhase::LinearCascadedFIRLong;
    default:
        return dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR;
    }
}

[[nodiscard]] inline double ResolveNamOversampledRenderingRate(double modelSampleRate, double hostSampleRate,
                                                               int factor)
{
    if (modelSampleRate <= 0.0 || hostSampleRate <= 0.0)
    {
        return modelSampleRate;
    }
    if (factor <= 1)
    {
        return modelSampleRate;
    }

    const double requestedRate = hostSampleRate * static_cast<double>(factor);
    const double timeScale = std::max(1.0, std::round(requestedRate / modelSampleRate));
    return modelSampleRate * timeScale;
}

/**
 * One mono NAM lane using the NAM-Oversampler processing model:
 *
 *   host audio -> stateful AA upsampler -> time-scaled NAM -> AA downsampler
 *
 * The model's temporal dilation is multiplied by the same integer scale used
 * for its rendering rate, preserving the physical convolution timing.
 */
class NamOversamplingProcessor
{
  public:
    void Prepare(::nam::DSP& model, double hostSampleRate, double modelSampleRate, int maxBlockSize, int factor,
                 dsp::EAntiAliasFilterPhase filterPhase)
    {
        mHostSampleRate = hostSampleRate;
        mModelSampleRate = modelSampleRate;
        mMaxBlockSize = std::max(1, maxBlockSize);
        const int oversamplingFactor = std::clamp(factor, 1, 32);
        mFilterPhase = filterPhase;

        mRenderingSampleRate =
            ResolveNamOversampledRenderingRate(mModelSampleRate, mHostSampleRate, oversamplingFactor);
        if (mRenderingSampleRate <= 0.0)
        {
            mRenderingSampleRate = mHostSampleRate;
        }

        mTimeScale =
            static_cast<int>(std::max(1.0, std::round(mRenderingSampleRate / std::max(mModelSampleRate, 1.0))));
        mMaxRenderingBlockSize =
            std::max(1, static_cast<int>(std::ceil(static_cast<double>(mMaxBlockSize) * mRenderingSampleRate /
                                                   std::max(mHostSampleRate, 1.0))) +
                            1);

        mResamplingActive = std::abs(mRenderingSampleRate - mHostSampleRate) > 1.0e-6;
        if (mResamplingActive)
        {
            mResampler = std::make_unique<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>>(mRenderingSampleRate,
                                                                                       mFilterPhase, mModelSampleRate);
            mResampler->Reset(mHostSampleRate, mMaxBlockSize);
            if (mFilterPhase == dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR &&
                mResampler->HasSingleProcessCallbackPerBlock())
            {
                PrimeResampler();
            }
        }
        else
        {
            mResampler.reset();
        }

        model.SetTimeScale(mTimeScale);
        model.Reset(mRenderingSampleRate, mMaxRenderingBlockSize);
        mPrepared = true;
    }

    void Reset(::nam::DSP& model)
    {
        if (!mPrepared)
        {
            return;
        }

        if (mResampler)
        {
            mResampler->SetAntiAliasFilterPhase(mFilterPhase);
            mResampler->Reset(mHostSampleRate, mMaxBlockSize);
        }
        model.SetTimeScale(mTimeScale);
        model.Reset(mRenderingSampleRate, mMaxRenderingBlockSize);
    }

    void Process(::nam::DSP& model, NAM_SAMPLE* input, NAM_SAMPLE* output, int numFrames)
    {
        if (!input || !output || numFrames <= 0)
        {
            return;
        }

        NAM_SAMPLE* inputPointers[1] = {input};
        NAM_SAMPLE* outputPointers[1] = {output};

        if (!mResampler)
        {
            model.process(inputPointers, outputPointers, numFrames);
            return;
        }

        mResampler->ProcessBlock(
            inputPointers, outputPointers, numFrames,
            [&model](NAM_SAMPLE** renderingInput, NAM_SAMPLE** renderingOutput, int renderingFrames) {
                model.process(renderingInput, renderingOutput, renderingFrames);
            });
    }

    [[nodiscard]] int GetLatencySamples() const noexcept
    {
        return mResampler ? mResampler->GetLatency() : 0;
    }

    [[nodiscard]] double GetRenderingSampleRate() const noexcept
    {
        return mRenderingSampleRate;
    }

    [[nodiscard]] int GetMaxRenderingBlockSize() const noexcept
    {
        return mMaxRenderingBlockSize;
    }

    [[nodiscard]] int GetTimeScale() const noexcept
    {
        return mTimeScale;
    }

    [[nodiscard]] bool IsResamplingActive() const noexcept
    {
        return mResamplingActive;
    }

  private:
    void PrimeResampler()
    {
        if (!mResampler)
        {
            return;
        }

        // The minimum-phase backend lazily creates one decimator state array on
        // its first block. Exercise that path during Prepare(), then clear its
        // state, so the audio-thread call is allocation-free from block one.
        std::vector<NAM_SAMPLE> silence(static_cast<std::size_t>(mMaxBlockSize), static_cast<NAM_SAMPLE>(0.0));
        std::vector<NAM_SAMPLE> discarded(static_cast<std::size_t>(mMaxBlockSize), static_cast<NAM_SAMPLE>(0.0));
        NAM_SAMPLE* inputPointers[1] = {silence.data()};
        NAM_SAMPLE* outputPointers[1] = {discarded.data()};
        mResampler->ProcessBlock(inputPointers, outputPointers, mMaxBlockSize,
                                 [](NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) {
                                     std::copy_n(input[0], numFrames, output[0]);
                                 });
        mResampler->Reset(mHostSampleRate, mMaxBlockSize);
    }

    std::unique_ptr<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>> mResampler;
    double mHostSampleRate = 48000.0;
    double mModelSampleRate = 48000.0;
    double mRenderingSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    int mMaxRenderingBlockSize = 512;
    int mTimeScale = 1;
    dsp::EAntiAliasFilterPhase mFilterPhase = dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR;
    bool mResamplingActive = false;
    bool mPrepared = false;
};

/** Allocation-free fixed delay used to align an effect's dry signal with a
 * linear-phase oversampled wet path. Prepare() is called off the audio thread. */
class NamDryDelay
{
  public:
    void Prepare(int latencySamples, int maxBlockSize)
    {
        mLatencySamples = std::max(0, latencySamples);
        const std::size_t capacity = static_cast<std::size_t>(mLatencySamples + std::max(1, maxBlockSize) + 1);
        mBuffer.assign(capacity, 0.0f);
        mWritePosition = 0;
    }

    void Reset()
    {
        std::fill(mBuffer.begin(), mBuffer.end(), 0.0f);
        mWritePosition = 0;
    }

    void Process(float* samples, int numSamples)
    {
        if (!samples || numSamples <= 0 || mLatencySamples <= 0 || mBuffer.empty())
        {
            return;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            std::size_t readPosition = mWritePosition + mBuffer.size() - static_cast<std::size_t>(mLatencySamples);
            if (readPosition >= mBuffer.size())
            {
                readPosition -= mBuffer.size();
            }
            const float delayed = mBuffer[readPosition];
            mBuffer[mWritePosition] = samples[sampleIndex];
            samples[sampleIndex] = delayed;
            ++mWritePosition;
            if (mWritePosition == mBuffer.size())
            {
                mWritePosition = 0;
            }
        }
    }

  private:
    std::vector<float> mBuffer;
    std::size_t mWritePosition = 0;
    int mLatencySamples = 0;
};
} // namespace guitarfx
