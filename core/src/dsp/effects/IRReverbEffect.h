#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/IRTypes.h"
#include "dsp/IRWavLoader.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/RealtimeConvolver.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

namespace guitarfx
{
/**
 * Impulse Response reverb effect.
 * Supports mono, stereo, and true-stereo (4-channel) IR WAV files.
 */
class IRReverbEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        mInputBufferL.resize(static_cast<std::size_t>(maxBlockSize));
        mInputBufferR.resize(static_cast<std::size_t>(maxBlockSize));
        mOutputBufferLL.resize(static_cast<std::size_t>(maxBlockSize));
        mOutputBufferRR.resize(static_cast<std::size_t>(maxBlockSize));
        mOutputBufferLR.resize(static_cast<std::size_t>(maxBlockSize));
        mOutputBufferRL.resize(static_cast<std::size_t>(maxBlockSize));

        // CRITICAL: Clear filter state BEFORE updating filter coefficients to prevent transients
        // when stale filter state is processed with new sample-rate-dependent coefficients.
        mToneStateL = 0.0f;
        mToneStateR = 0.0f;

        ApplyPendingQuality();
        UpdateToneFilter();

        if (!mImpulseLL.empty())
        {
            InitializeConvolvers();
        }
    }

    void Reset() override
    {
        mConvolverLL.Reset();
        mConvolverRR.Reset();
        mConvolverLR.Reset();
        mConvolverRL.Reset();
        mToneStateL = 0.0f;
        mToneStateR = 0.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        // A host may hand over a block larger than the size we were prepared for. Clamping
        // alone would leave the tail of the output buffer holding whatever was in it before,
        // which is audible as a click every time it happens, so run the excess through as
        // further chunks instead.
        if (mMaxBlockSize > 0 && numSamples > mMaxBlockSize)
        {
            int offset = 0;
            while (offset < numSamples)
            {
                const int chunk = std::min(mMaxBlockSize, numSamples - offset);
                float* chunkIn[2] = {inputs && inputs[0] ? inputs[0] + offset : nullptr,
                                     inputs && inputs[1] ? inputs[1] + offset : nullptr};
                float* chunkOut[2] = {outputs[0] ? outputs[0] + offset : nullptr,
                                      outputs[1] ? outputs[1] + offset : nullptr};
                ProcessChunk(chunkIn, chunkOut, chunk);
                offset += chunk;
            }
            return;
        }

        ProcessChunk(inputs, outputs, numSamples);
    }

  private:
    void ProcessChunk(float** inputs, float** outputs, int numSamples)
    {
        if (!mEnabled || !mConvolverLL.IsInitialized() || !mConvolverRR.IsInitialized() ||
            mRebuilding.load(std::memory_order_acquire))
        {
            // Bypass: copy input to output, falling back L→R if R is null
            if (outputs[0])
            {
                if (inputs[0])
                {
                    std::copy_n(inputs[0], numSamples, outputs[0]);
                }
                else
                {
                    std::fill_n(outputs[0], numSamples, 0.0f);
                }
            }
            if (outputs[1])
            {
                if (inputs[1])
                {
                    std::copy_n(inputs[1], numSamples, outputs[1]);
                }
                else if (inputs[0])
                {
                    std::copy_n(inputs[0], numSamples, outputs[1]);
                }
                else
                {
                    std::fill_n(outputs[1], numSamples, 0.0f);
                }
            }
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0f;
            mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0f);
        }

        const bool allowParallel = rtparallel::ShouldParallelizeStereoWork(numSamples);
        bool ranParallel = false;
        if (allowParallel)
        {
            ranParallel = rtparallel::DualLaneExecutor::Instance().Run(
                [&]() { mConvolverRR.Process(mInputBufferR.data(), mOutputBufferRR.data(), numSamples); },
                [&]() { mConvolverLL.Process(mInputBufferL.data(), mOutputBufferLL.data(), numSamples); });
        }
        if (!ranParallel)
        {
            mConvolverLL.Process(mInputBufferL.data(), mOutputBufferLL.data(), numSamples);
            mConvolverRR.Process(mInputBufferR.data(), mOutputBufferRR.data(), numSamples);
        }

        if (mHasTrueStereo)
        {
            bool ranTrueStereoParallel = false;
            if (allowParallel)
            {
                ranTrueStereoParallel = rtparallel::DualLaneExecutor::Instance().Run(
                    [&]() { mConvolverRL.Process(mInputBufferL.data(), mOutputBufferRL.data(), numSamples); },
                    [&]() { mConvolverLR.Process(mInputBufferR.data(), mOutputBufferLR.data(), numSamples); });
            }
            if (!ranTrueStereoParallel)
            {
                mConvolverLR.Process(mInputBufferR.data(), mOutputBufferLR.data(), numSamples);
                mConvolverRL.Process(mInputBufferL.data(), mOutputBufferRL.data(), numSamples);
            }
        }

        const double mix = mMix.load(std::memory_order_relaxed);
        const float wetGain = static_cast<float>(mix * mOutputGain.load(std::memory_order_relaxed));
        const float dryGain = static_cast<float>(1.0 - mix);
        const float toneCoef = mToneCoef.load(std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
            const float dryR = inputs[1] ? inputs[1][i] : dryL;

            float wetL = static_cast<float>(mOutputBufferLL[i] + (mHasTrueStereo ? mOutputBufferLR[i] : 0.0f));
            float wetR = static_cast<float>(mOutputBufferRR[i] + (mHasTrueStereo ? mOutputBufferRL[i] : 0.0f));

            if (toneCoef < 1.0f)
            {
                mToneStateL += toneCoef * (wetL - mToneStateL);
                mToneStateR += toneCoef * (wetR - mToneStateR);
                wetL = mToneStateL;
                wetR = mToneStateR;
            }

            if (outputs[0])
            {
                outputs[0][i] = wetL * wetGain + dryL * dryGain;
            }
            if (outputs[1])
            {
                outputs[1][i] = wetR * wetGain + dryR * dryGain;
            }
        }
    }

  public:
    void SetParam(const std::string& key, double value) override
    {
        if (key == "mix")
        {
            mMix.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "outputGain")
        {
            mOutputGain.store(std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0), std::memory_order_relaxed);
        }
        else if (key == "enabled")
        {
            mEnabled = value > 0.5;
        }
        else if (key == "tone")
        {
            mTone.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
            UpdateToneFilter();
        }
        else if (key == "quality")
        {
            const int q = static_cast<int>(std::clamp(value, 0.0, 3.0));
            const int pending = mPendingQuality.load(std::memory_order_acquire);
            const int effective = pending >= 0 ? pending : static_cast<int>(mQuality);
            // Rebuilding drops the effect to dry until the new convolvers are ready, so a
            // redundant write (preset re-apply, automation resending the same value) would
            // be audible as a click for no reason. Match the lowLatency handler and ignore it.
            if (q == effective)
            {
                return;
            }

            mPendingQuality.store(q, std::memory_order_release);
            // Reinitialise immediately so quality changes take effect without requiring
            // a prepareToPlay() call. Only safe when called from non-audio thread (UI interaction).
            if (HasResource())
            {
                ApplyPendingQuality();
                InitializeConvolvers();
            }
        }
        else if (key == "lowLatency")
        {
            const bool nv = value > 0.5;
            if (nv != mLowLatency)
            {
                mLowLatency = nv;
                // Rebuild immediately so the latency change takes effect without a reload.
                // Safe from the non-audio (UI/controller) thread; InitializeConvolvers raises
                // mRebuilding so the audio thread bypasses while convolvers are swapped.
                if (HasResource())
                {
                    InitializeConvolvers();
                }
            }
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }
        if (key == "tone")
        {
            return mTone.load(std::memory_order_relaxed);
        }
        if (key == "outputGain")
        {
            return 20.0 * std::log10(mOutputGain.load(std::memory_order_relaxed));
        }
        if (key == "enabled")
        {
            return mEnabled ? 1.0 : 0.0;
        }
        if (key == "quality")
        {
            const int pending = mPendingQuality.load(std::memory_order_acquire);
            return pending >= 0 ? static_cast<double>(pending) : static_cast<double>(mQuality);
        }
        if (key == "lowLatency")
        {
            return mLowLatency ? 1.0 : 0.0;
        }
        return 0.0;
    }

    bool LoadResource(const std::filesystem::path& resourcePath) override
    {
        if (!std::filesystem::exists(resourcePath))
        {
            std::cerr << "[IRReverbEffect] ERROR: IR file not found: " << resourcePath << "\n";
            return false;
        }

        if (!LoadWavFile(resourcePath))
        {
            std::cerr << "[IRReverbEffect] ERROR: Failed to load/parse IR WAV file: " << resourcePath << "\n";
            return false;
        }

        mIRPath = resourcePath;
        ApplyPendingQuality();

        if (!InitializeConvolvers())
        {
            std::cerr << "[IRReverbEffect] ERROR: Failed to initialize convolvers for: " << resourcePath << "\n";
            return false;
        }

        return true;
    }

    [[nodiscard]] bool HasResource() const override
    {
        return mConvolverLL.IsInitialized() && mConvolverRR.IsInitialized();
    }

    [[nodiscard]] std::filesystem::path GetResourcePath() const override
    {
        return mIRPath;
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mConvolverLL.IsInitialized() ? mConvolverLL.GetLatency() : 0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "reverb_ir";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "reverb";
    }

  private:
    // Host rate the IR normalisation gain is anchored to (see ComputeL2NormGain).
    static constexpr double kNormalizationReferenceRate = 48000.0;

    static size_t FindEnergyTruncationPoint(const std::vector<float>& a, const std::vector<float>& b,
                                            const std::vector<float>* c, const std::vector<float>* d,
                                            float threshold = 0.001f)
    {
        if (a.empty() || b.empty())
        {
            return 0;
        }

        const std::size_t frames = std::min(a.size(), b.size());
        double totalEnergy = 0.0;

        for (std::size_t i = 0; i < frames; ++i)
        {
            totalEnergy += static_cast<double>(a[i]) * static_cast<double>(a[i]);
            totalEnergy += static_cast<double>(b[i]) * static_cast<double>(b[i]);
            if (c && d)
            {
                totalEnergy += static_cast<double>((*c)[i]) * static_cast<double>((*c)[i]);
                totalEnergy += static_cast<double>((*d)[i]) * static_cast<double>((*d)[i]);
            }
        }

        if (totalEnergy < 1e-10)
        {
            return frames;
        }

        const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
        double cumulativeEnergy = 0.0;

        for (std::size_t i = 0; i < frames; ++i)
        {
            cumulativeEnergy += static_cast<double>(a[i]) * static_cast<double>(a[i]);
            cumulativeEnergy += static_cast<double>(b[i]) * static_cast<double>(b[i]);
            if (c && d)
            {
                cumulativeEnergy += static_cast<double>((*c)[i]) * static_cast<double>((*c)[i]);
                cumulativeEnergy += static_cast<double>((*d)[i]) * static_cast<double>((*d)[i]);
            }
            if (cumulativeEnergy >= targetEnergy)
            {
                return std::min(i + 256, frames);
            }
        }

        return frames;
    }

    static std::vector<float> TruncateAndFade(const std::vector<float>& input, std::size_t length,
                                              std::size_t fadeLen = 2048)
    {
        if (input.empty() || length == 0)
        {
            return {};
        }

        const std::size_t truncLength = std::min(length, input.size());
        std::vector<float> truncated(input.begin(), input.begin() + truncLength);

        // Use a long fade (~42ms at 48kHz) to prevent Gibbs-phenomenon ringing
        // when the reverb tail is still active at the truncation point.
        const std::size_t effectiveFade = std::min(fadeLen, truncLength);
        if (effectiveFade > 1)
        {
            for (std::size_t i = 0; i < effectiveFade; ++i)
            {
                const float fadeGain =
                    static_cast<float>(effectiveFade - 1 - i) / static_cast<float>(effectiveFade - 1);
                truncated[truncLength - effectiveFade + i] *= fadeGain;
            }
        }

        return truncated;
    }

    // Energy-preserving (unity-gain) normalisation factor based on the combined L2 norm,
    // matching the IR cab path so reverb levels stay consistent across IR files.
    // playbackRate is the rate the (already resampled) impulse runs at. Area-preserving
    // resampling leaves ||h||_2 proportional to 1/sqrt(rate), so a bare 1/||h||_2 gain would
    // make the same IR louder at higher host rates (+6 dB at 192 kHz versus 48 kHz).
    // Anchoring to a reference rate removes that; 48 kHz is the anchor so the normalisation
    // there is unchanged. Matches IRCabEffect::ComputeL2NormGain.
    static float ComputeL2NormGain(double playbackRate, const std::vector<float>& a, const std::vector<float>& b,
                                   const std::vector<float>* c = nullptr, const std::vector<float>* d = nullptr)
    {
        double sumSq = 0.0;
        for (const float s : a)
        {
            sumSq += static_cast<double>(s) * s;
        }
        for (const float s : b)
        {
            sumSq += static_cast<double>(s) * s;
        }
        if (c && d)
        {
            for (const float s : *c)
            {
                sumSq += static_cast<double>(s) * s;
            }
            for (const float s : *d)
            {
                sumSq += static_cast<double>(s) * s;
            }
        }
        // Average across the two output channels so mono/stereo/true-stereo IRs all
        // normalise to the same perceived level.
        sumSq *= 0.5;
        if (sumSq <= 1e-12)
        {
            return 1.0f;
        }

        const double rateCompensation =
            playbackRate > 0.0 ? std::sqrt(kNormalizationReferenceRate / playbackRate) : 1.0;
        return static_cast<float>(rateCompensation / std::sqrt(sumSq));
    }

    static double Sinc(double x)
    {
        if (std::fabs(x) < 1e-9)
        {
            return 1.0;
        }
        const double px = 3.14159265358979323846 * x;
        return std::sin(px) / px;
    }

    static double BlackmanWindow(double distance)
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr int kHalfTaps = 64;
        const double normalizedDistance = std::abs(distance) / static_cast<double>(kHalfTaps);
        if (normalizedDistance >= 1.0)
        {
            return 0.0;
        }

        return 0.42 + 0.5 * std::cos(kPi * normalizedDistance) + 0.08 * std::cos(2.0 * kPi * normalizedDistance);
    }

    static void ResampleImpulseForConvolution(std::vector<float>& samples, double sourceRate, double targetRate)
    {
        if (samples.empty() || sourceRate <= 0.0 || targetRate <= 0.0 || std::abs(sourceRate - targetRate) <= 1.0)
        {
            return;
        }

        constexpr int kHalfTaps = 64;
        const double ratio = targetRate / sourceRate;
        const double cutoff = std::min(ratio, 1.0);
        const std::size_t newSize = static_cast<std::size_t>(std::ceil(static_cast<double>(samples.size()) * ratio));
        if (newSize == 0)
        {
            samples.clear();
            return;
        }

        std::vector<float> resampled(newSize, 0.0f);
        const int sourceLength = static_cast<int>(samples.size());
        const double coefficientScale = sourceRate / targetRate;

        for (std::size_t outputIndex = 0; outputIndex < newSize; ++outputIndex)
        {
            const double sourcePosition = static_cast<double>(outputIndex) / ratio;
            const int center = static_cast<int>(std::floor(sourcePosition));
            double sum = 0.0;

            for (int tapOffset = -kHalfTaps; tapOffset <= kHalfTaps; ++tapOffset)
            {
                const int sourceIndex = center + tapOffset;
                if (sourceIndex < 0 || sourceIndex >= sourceLength)
                {
                    continue;
                }

                const double distance = sourcePosition - static_cast<double>(sourceIndex);
                const double tap = cutoff * Sinc(distance * cutoff) * BlackmanWindow(distance);
                sum += static_cast<double>(samples[static_cast<std::size_t>(sourceIndex)]) * tap;
            }

            resampled[outputIndex] = static_cast<float>(sum * coefficientScale);
        }

        samples = std::move(resampled);
    }

    // Resampling a long reverb IR costs ~130 sinc taps per output sample per channel --
    // seconds of work for a multi-second true-stereo IR. Quality only changes where the
    // tail is truncated, not the impulse itself, so the resampled copy is built once per
    // (IR, playback rate) and reused. Without this, every quality change re-resampled the
    // whole IR while holding the DSP lock, which silences the audio thread for the duration.
    void EnsurePlaybackImpulses()
    {
        const bool needsResample = std::abs(mIRSampleRate - mSampleRate) > 1.0;
        if (mPlaybackCacheValid && mPlaybackCacheRate == mSampleRate && mPlaybackCacheIRRate == mIRSampleRate)
        {
            return;
        }

        mPlaybackLL.clear();
        mPlaybackRR.clear();
        mPlaybackLR.clear();
        mPlaybackRL.clear();
        mPlaybackCacheResampled = needsResample;

        if (needsResample)
        {
            mPlaybackLL = mImpulseLL;
            mPlaybackRR = mImpulseRR;
            ResampleImpulseForConvolution(mPlaybackLL, mIRSampleRate, mSampleRate);
            ResampleImpulseForConvolution(mPlaybackRR, mIRSampleRate, mSampleRate);
            if (mHasTrueStereo)
            {
                mPlaybackLR = mImpulseLR;
                mPlaybackRL = mImpulseRL;
                ResampleImpulseForConvolution(mPlaybackLR, mIRSampleRate, mSampleRate);
                ResampleImpulseForConvolution(mPlaybackRL, mIRSampleRate, mSampleRate);
            }
        }

        mPlaybackCacheValid = true;
        mPlaybackCacheRate = mSampleRate;
        mPlaybackCacheIRRate = mIRSampleRate;
    }

    void InvalidatePlaybackImpulses()
    {
        mPlaybackCacheValid = false;
        mPlaybackCacheResampled = false;
        mPlaybackLL.clear();
        mPlaybackRR.clear();
        mPlaybackLR.clear();
        mPlaybackRL.clear();
    }

    // The impulses at playback rate. When no resampling is needed these are the raw
    // impulses, so a matched-rate IR costs no extra memory.
    const std::vector<float>& PlaybackLL() const
    {
        return mPlaybackCacheResampled ? mPlaybackLL : mImpulseLL;
    }

    const std::vector<float>& PlaybackRR() const
    {
        return mPlaybackCacheResampled ? mPlaybackRR : mImpulseRR;
    }

    const std::vector<float>& PlaybackLR() const
    {
        return mPlaybackCacheResampled ? mPlaybackLR : mImpulseLR;
    }

    const std::vector<float>& PlaybackRL() const
    {
        return mPlaybackCacheResampled ? mPlaybackRL : mImpulseRL;
    }

    std::size_t GetMinimumImpulseLength() const
    {
        if (mHasTrueStereo)
        {
            return std::min({PlaybackLL().size(), PlaybackLR().size(), PlaybackRL().size(), PlaybackRR().size()});
        }
        return std::min(PlaybackLL().size(), PlaybackRR().size());
    }

    // Call only after EnsurePlaybackImpulses(): lengths are in playback-rate samples.
    std::size_t GetTruncationLength() const
    {
        const std::size_t minLength = GetMinimumImpulseLength();
        if (minLength == 0)
        {
            return 0;
        }

        if (mQuality == IRQuality::Full)
        {
            return minLength;
        }

        // GetMaxReverbIRSamples returns a limit expressed in playback-rate samples, which is
        // the domain the cached impulses are already in.
        const size_t maxSamples = GetMaxReverbIRSamples(mQuality, mSampleRate);
        if (maxSamples == 0 || minLength <= maxSamples)
        {
            return minLength;
        }

        const std::size_t energyTrunc =
            FindEnergyTruncationPoint(PlaybackLL(), PlaybackRR(), mHasTrueStereo ? &PlaybackLR() : nullptr,
                                      mHasTrueStereo ? &PlaybackRL() : nullptr, 0.001f);

        return std::min({minLength, maxSamples, energyTrunc});
    }

    bool LoadWavFile(const std::filesystem::path& path)
    {
        IRWavData data;
        if (!irwav::LoadAudioFile(path, data))
        {
            std::cerr << "[IRReverbEffect] Failed to parse audio data from: " << path << "\n";
            return false;
        }

        if (data.channels < 1)
        {
            std::cerr << "[IRReverbEffect] ERROR: IR file has no audio channels: " << path << "\n";
            return false;
        }

        mIRSampleRate = data.sampleRate;
        mIRChannels = data.channels;
        InvalidatePlaybackImpulses(); // new impulse data; the cached playback copy is stale

        if (data.channels >= 4)
        {
            irwav::SplitToQuad(data, mImpulseLL, mImpulseLR, mImpulseRL, mImpulseRR);
            mHasTrueStereo = !mImpulseLL.empty() && !mImpulseLR.empty() && !mImpulseRL.empty() && !mImpulseRR.empty();

            if (!mHasTrueStereo)
            {
                std::cerr << "[IRReverbEffect] WARNING: 4-channel IR file has empty channels: " << path << "\n";
            }
        }
        else
        {
            irwav::SplitToStereo(data, mImpulseLL, mImpulseRR);
            mImpulseLR.clear();
            mImpulseRL.clear();
            mHasTrueStereo = false;
        }

        if (mImpulseLL.empty() || mImpulseRR.empty())
        {
            std::cerr << "[IRReverbEffect] ERROR: IR file missing required stereo channels: " << path << "\n";
            return false;
        }

        return true;
    }

    bool InitializeConvolvers()
    {
        if (mImpulseLL.empty() || mImpulseRR.empty() || mMaxBlockSize == 0)
        {
            return false;
        }

        // Resample once per (IR, playback rate); a quality change only re-truncates.
        // Done before raising mRebuilding so a cache rebuild cannot leave the effect
        // bypassed if anything below bails out.
        EnsurePlaybackImpulses();

        // Signal the audio thread to bypass (dry copy) while convolvers are being rebuilt.
        mRebuilding.store(true, std::memory_order_release);

        const std::size_t truncLength = GetTruncationLength();
        if (truncLength == 0)
        {
            mRebuilding.store(false, std::memory_order_release);
            return false;
        }

        std::vector<float> processedLL = TruncateAndFade(PlaybackLL(), truncLength);
        std::vector<float> processedRR = TruncateAndFade(PlaybackRR(), truncLength);
        std::vector<float> processedLR;
        std::vector<float> processedRL;

        if (mHasTrueStereo)
        {
            processedLR = TruncateAndFade(PlaybackLR(), truncLength);
            processedRL = TruncateAndFade(PlaybackRL(), truncLength);
        }

        // Energy (L2-norm) normalisation for unity-gain convolution, mirroring the IR cab path.
        // Reverb IRs contain thousands of significant samples, so peak normalisation (scaling so
        // the loudest sample == 1.0) leaves the summed convolution energy proportional to the
        // tail density/length. Dense, bright IRs (e.g. EMT-140 style plates) then come out far
        // louder than sparse ones. Normalising by the combined L2 norm makes the wet output
        // level consistent across IR files and equal-energy to the input. Computed on the
        // playback-rate impulse so the level is independent of the source IR sample rate.
        {
            const float normGain =
                ComputeL2NormGain(mSampleRate, processedLL, processedRR, mHasTrueStereo ? &processedLR : nullptr,
                                  mHasTrueStereo ? &processedRL : nullptr);
            for (float& s : processedLL)
            {
                s *= normGain;
            }
            for (float& s : processedRR)
            {
                s *= normGain;
            }
            if (mHasTrueStereo)
            {
                for (float& s : processedLR)
                {
                    s *= normGain;
                }
                for (float& s : processedRL)
                {
                    s *= normGain;
                }
            }
        }

        mConvolverLL.SetLowLatencyMode(mLowLatency);
        mConvolverRR.SetLowLatencyMode(mLowLatency);
        mConvolverLR.SetLowLatencyMode(mLowLatency);
        mConvolverRL.SetLowLatencyMode(mLowLatency);

        if (!mConvolverLL.SetImpulse(processedLL, mMaxBlockSize))
        {
            mRebuilding.store(false, std::memory_order_release);
            return false;
        }
        if (!mConvolverRR.SetImpulse(processedRR, mMaxBlockSize))
        {
            mRebuilding.store(false, std::memory_order_release);
            return false;
        }

        if (mHasTrueStereo)
        {
            if (!mConvolverLR.SetImpulse(processedLR, mMaxBlockSize))
            {
                mRebuilding.store(false, std::memory_order_release);
                return false;
            }
            if (!mConvolverRL.SetImpulse(processedRL, mMaxBlockSize))
            {
                mRebuilding.store(false, std::memory_order_release);
                return false;
            }
        }

        mRebuilding.store(false, std::memory_order_release);
        return true;
    }

    void ApplyPendingQuality()
    {
        const int pending = mPendingQuality.exchange(-1, std::memory_order_acq_rel);
        if (pending >= 0)
        {
            mQuality = static_cast<IRQuality>(pending);
        }
    }

    void UpdateToneFilter()
    {
        const float t = mTone.load(std::memory_order_relaxed);
        if (t >= 1.0f)
        {
            mToneCoef.store(1.0f, std::memory_order_relaxed);
            return;
        }
        const float minHz = 1500.0f;
        const float maxHz = 20000.0f;
        const float cutoff = minHz + (maxHz - minHz) * t;
        const float x = static_cast<float>(2.0 * 3.14159265358979323846 * cutoff / std::max(1.0, mSampleRate));
        mToneCoef.store(1.0f - std::exp(-x), std::memory_order_relaxed);
    }

    RealtimeConvolver mConvolverLL;
    RealtimeConvolver mConvolverRR;
    RealtimeConvolver mConvolverLR;
    RealtimeConvolver mConvolverRL;

    std::vector<float> mImpulseLL;
    std::vector<float> mImpulseLR;
    std::vector<float> mImpulseRL;
    std::vector<float> mImpulseRR;

    // Impulses resampled to the playback rate (see EnsurePlaybackImpulses). Populated only
    // when the IR rate differs from the host rate; otherwise the raw impulses are used and
    // these stay empty.
    std::vector<float> mPlaybackLL;
    std::vector<float> mPlaybackLR;
    std::vector<float> mPlaybackRL;
    std::vector<float> mPlaybackRR;
    double mPlaybackCacheRate = 0.0;   // host rate the cache was built for
    double mPlaybackCacheIRRate = 0.0; // IR rate it was built from
    bool mPlaybackCacheValid = false;
    bool mPlaybackCacheResampled = false;

    std::filesystem::path mIRPath;
    double mIRSampleRate = 48000.0;
    std::uint16_t mIRChannels = 0;
    bool mHasTrueStereo = false;

    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mOutputBufferLL;
    std::vector<float> mOutputBufferRR;
    std::vector<float> mOutputBufferLR;
    std::vector<float> mOutputBufferRL;

    std::atomic<double> mMix{0.3};
    std::atomic<double> mOutputGain{1.0};
    IRQuality mQuality = IRQuality::Standard;
    std::atomic<int> mPendingQuality{-1};
    bool mLowLatency = true; // non-uniform (low-latency) convolution mode
    std::atomic<float> mTone{1.0f};
    std::atomic<float> mToneCoef{1.0f};
    // Set true during convolver rebuild to let the audio thread bypass safely.
    std::atomic<bool> mRebuilding{false};
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
};

inline void RegisterIRReverbEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbIr;
    info.aliases = {"reverb_ir"};
    info.displayName = "Convolution Reverb";
    info.category = "reverb";
    info.description = "Impulse response reverb";
    info.requiresResource = true;
    info.resourceType = "ir";
    info.parameters = {
        {"mix", "Mix", 0.3, 0.0, 1.0, "amount"},
        {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
        {"tone", "Tone", 1.0, 0.0, 1.0, "amount"},
        {"lowLatency", "Low Latency", 1.0, 0.0, 1.0, "toggle"},
        {"quality", "Quality", 3.0, 0.0, 3.0, "enum", "", false, 1.0, {"Economy", "Standard", "High", "Full"}}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<IRReverbEffect>(); });
}

} // namespace guitarfx
