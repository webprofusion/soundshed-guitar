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
#include <memory>
#include <utility>
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

        ApplyPendingRebuildSettings();
        UpdateToneFilter();

        if (HasImpulses())
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
        if (!mEnabled || !mConvolverLL.IsInitialized() || !mConvolverRR.IsInitialized())
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
            // Where the IR is truncated, and so its normalisation gain, is built into the convolvers.
            RequestRebuildSetting(mPendingQuality, static_cast<int>(mQuality),
                                  static_cast<int>(std::clamp(value, 0.0, 3.0)));
        }
        else if (key == "lowLatency")
        {
            // The partition layout is fixed when a convolver is built.
            RequestRebuildSetting(mPendingLowLatency, mLowLatency ? 1 : 0, value > 0.5 ? 1 : 0);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    /// A Quality or Low Latency change SetParam recorded (see RequestRebuildSetting), sharing the
    /// playback-rate impulses to build from rather than copying them: a reverb IR runs to seconds,
    /// and this runs under the DSP lock.
    [[nodiscard]] std::unique_ptr<DeferredRebuild> TakeDeferredRebuild() override
    {
        if ((mPendingQuality.load(std::memory_order_acquire) < 0 &&
             mPendingLowLatency.load(std::memory_order_acquire) < 0) ||
            !HasResource() || !HasImpulses() || mMaxBlockSize == 0)
        {
            return nullptr;
        }

        // The live convolvers were built from this cache, so it is already current.
        EnsurePlaybackImpulses();

        auto work = std::make_unique<ConvolverRebuild>();
        work->generation = mBuildGeneration;
        work->settings = {mSampleRate, mMaxBlockSize, EffectiveQuality(), EffectiveLowLatency()};
        work->hasTrueStereo = mHasTrueStereo;
        work->playback = mPlaybackImpulses;
        return work;
    }

    void CommitDeferredRebuild(DeferredRebuild& taken) override
    {
        auto* work = dynamic_cast<ConvolverRebuild*>(&taken);

        // An IR load or a Prepare since the work was taken rebuilt the convolvers itself, with the
        // waiting settings folded in. A change requested while it was built supersedes it, and
        // that SetParam has already asked for another pass.
        if (!work || !work->convolvers || work->generation != mBuildGeneration ||
            EffectiveQuality() != work->settings.quality || EffectiveLowLatency() != work->settings.lowLatency)
        {
            return;
        }

        // The new convolvers start with no history, so the tail of what was playing stops here.
        // Swaps rather than assignments, so the convolvers they displace end up in the work and
        // are freed off the lock.
        std::swap(mConvolverLL, work->convolvers->ll);
        std::swap(mConvolverRR, work->convolvers->rr);
        std::swap(mConvolverLR, work->convolvers->lr);
        std::swap(mConvolverRL, work->convolvers->rl);
        mQuality = work->settings.quality;
        mLowLatency = work->settings.lowLatency;
        ClearPendingIf(mPendingQuality, static_cast<int>(mQuality));
        ClearPendingIf(mPendingLowLatency, mLowLatency ? 1 : 0);
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
            return static_cast<double>(EffectiveQuality());
        }

        if (key == "lowLatency")
        {
            return EffectiveLowLatency() ? 1.0 : 0.0;
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
        ApplyPendingRebuildSettings();

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

    /// One IR's impulses at one sample rate. Never changed once built, and shared rather than
    /// copied: a deferred rebuild reads them off the DSP lock while an IR load may replace the
    /// effect's own.
    struct ImpulseSet
    {
        std::vector<float> ll;
        std::vector<float> lr;
        std::vector<float> rl;
        std::vector<float> rr;
    };

    /// What building the convolvers reads from the effect besides the impulses, so a deferred
    /// rebuild can build from a copy, off the DSP lock.
    struct ConvolverBuildSettings
    {
        double sampleRate = 48000.0;
        int maxBlockSize = 0;
        IRQuality quality = IRQuality::Standard;
        bool lowLatency = true;
    };

    /// The convolvers a deferred rebuild builds, allocated by the build rather than when the work
    /// is taken under the DSP lock.
    struct ConvolverSet
    {
        RealtimeConvolver ll;
        RealtimeConvolver rr;
        RealtimeConvolver lr;
        RealtimeConvolver rl;
    };

    /// The convolver rebuild a Quality or Low Latency change needs, built by the message thread off
    /// the DSP lock.
    class ConvolverRebuild final : public DeferredRebuild
    {
      public:
        void Build() override
        {
            convolvers = std::make_unique<ConvolverSet>();

            if (!BuildConvolvers(settings, *playback, hasTrueStereo, convolvers->ll, convolvers->rr, convolvers->lr,
                                 convolvers->rl))
            {
                convolvers.reset();
            }
        }

        std::uint64_t generation = 0;
        ConvolverBuildSettings settings;
        bool hasTrueStereo = false;
        std::shared_ptr<const ImpulseSet> playback;
        std::unique_ptr<ConvolverSet> convolvers; // null until built, and if the build failed
    };

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
        if (mPlaybackImpulses && mPlaybackCacheRate == mSampleRate && mPlaybackCacheIRRate == mIRSampleRate)
        {
            return;
        }

        // When no resampling is needed the playback set is the loaded one, so a matched-rate IR
        // costs no extra memory.
        if (std::abs(mIRSampleRate - mSampleRate) > 1.0)
        {
            auto resampled = std::make_shared<ImpulseSet>();
            resampled->ll = mImpulses->ll;
            resampled->rr = mImpulses->rr;
            ResampleImpulseForConvolution(resampled->ll, mIRSampleRate, mSampleRate);
            ResampleImpulseForConvolution(resampled->rr, mIRSampleRate, mSampleRate);

            if (mHasTrueStereo)
            {
                resampled->lr = mImpulses->lr;
                resampled->rl = mImpulses->rl;
                ResampleImpulseForConvolution(resampled->lr, mIRSampleRate, mSampleRate);
                ResampleImpulseForConvolution(resampled->rl, mIRSampleRate, mSampleRate);
            }

            mPlaybackImpulses = std::move(resampled);
        }
        else
        {
            mPlaybackImpulses = mImpulses;
        }

        mPlaybackCacheRate = mSampleRate;
        mPlaybackCacheIRRate = mIRSampleRate;
    }

    [[nodiscard]] bool HasImpulses() const
    {
        return mImpulses && !mImpulses->ll.empty() && !mImpulses->rr.empty();
    }

    static std::size_t GetMinimumImpulseLength(const ImpulseSet& playback, bool trueStereo)
    {
        if (trueStereo)
        {
            return std::min({playback.ll.size(), playback.lr.size(), playback.rl.size(), playback.rr.size()});
        }

        return std::min(playback.ll.size(), playback.rr.size());
    }

    // Lengths are in playback-rate samples, the rate `playback` is at.
    static std::size_t GetTruncationLength(const ImpulseSet& playback, bool trueStereo, IRQuality quality,
                                           double playbackRate)
    {
        const std::size_t minLength = GetMinimumImpulseLength(playback, trueStereo);

        if (minLength == 0)
        {
            return 0;
        }

        if (quality == IRQuality::Full)
        {
            return minLength;
        }

        // GetMaxReverbIRSamples returns a limit expressed in playback-rate samples, which is
        // the domain the cached impulses are already in.
        const size_t maxSamples = GetMaxReverbIRSamples(quality, playbackRate);

        if (maxSamples == 0 || minLength <= maxSamples)
        {
            return minLength;
        }

        const std::size_t energyTrunc = FindEnergyTruncationPoint(
            playback.ll, playback.rr, trueStereo ? &playback.lr : nullptr, trueStereo ? &playback.rl : nullptr, 0.001f);

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
        mPlaybackImpulses.reset(); // new impulse data; the cached playback copy is stale

        auto impulses = std::make_shared<ImpulseSet>();

        if (data.channels >= 4)
        {
            irwav::SplitToQuad(data, impulses->ll, impulses->lr, impulses->rl, impulses->rr);
            mHasTrueStereo =
                !impulses->ll.empty() && !impulses->lr.empty() && !impulses->rl.empty() && !impulses->rr.empty();

            if (!mHasTrueStereo)
            {
                std::cerr << "[IRReverbEffect] WARNING: 4-channel IR file has empty channels: " << path << "\n";
            }
        }
        else
        {
            irwav::SplitToStereo(data, impulses->ll, impulses->rr);
            mHasTrueStereo = false;
        }

        mImpulses = std::move(impulses);

        if (!HasImpulses())
        {
            std::cerr << "[IRReverbEffect] ERROR: IR file missing required stereo channels: " << path << "\n";
            return false;
        }

        return true;
    }

    bool InitializeConvolvers()
    {
        if (!HasImpulses() || mMaxBlockSize == 0)
        {
            return false;
        }

        // Resample once per (IR, playback rate); a quality change only re-truncates.
        EnsurePlaybackImpulses();
        return BuildConvolvers({mSampleRate, mMaxBlockSize, mQuality, mLowLatency}, *mPlaybackImpulses, mHasTrueStereo,
                               mConvolverLL, mConvolverRR, mConvolverLR, mConvolverRL);
    }

    /// Truncates the playback-rate impulses for the quality, normalises them and builds the
    /// convolvers from them, reading nothing from the effect, so a deferred rebuild can run it off
    /// the DSP lock. LR and RL are only built for a true-stereo IR.
    static bool BuildConvolvers(const ConvolverBuildSettings& settings, const ImpulseSet& playback, bool trueStereo,
                                RealtimeConvolver& convolverLL, RealtimeConvolver& convolverRR,
                                RealtimeConvolver& convolverLR, RealtimeConvolver& convolverRL)
    {
        const std::size_t truncLength =
            GetTruncationLength(playback, trueStereo, settings.quality, settings.sampleRate);

        if (truncLength == 0)
        {
            return false;
        }

        std::vector<float> processedLL = TruncateAndFade(playback.ll, truncLength);
        std::vector<float> processedRR = TruncateAndFade(playback.rr, truncLength);
        std::vector<float> processedLR;
        std::vector<float> processedRL;

        if (trueStereo)
        {
            processedLR = TruncateAndFade(playback.lr, truncLength);
            processedRL = TruncateAndFade(playback.rl, truncLength);
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
                ComputeL2NormGain(settings.sampleRate, processedLL, processedRR, trueStereo ? &processedLR : nullptr,
                                  trueStereo ? &processedRL : nullptr);

            for (float& s : processedLL)
            {
                s *= normGain;
            }

            for (float& s : processedRR)
            {
                s *= normGain;
            }

            if (trueStereo)
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

        convolverLL.SetLowLatencyMode(settings.lowLatency);
        convolverRR.SetLowLatencyMode(settings.lowLatency);
        convolverLR.SetLowLatencyMode(settings.lowLatency);
        convolverRL.SetLowLatencyMode(settings.lowLatency);

        if (!convolverLL.SetImpulse(processedLL, settings.maxBlockSize))
        {
            return false;
        }

        if (!convolverRR.SetImpulse(processedRR, settings.maxBlockSize))
        {
            return false;
        }

        if (trueStereo)
        {
            if (!convolverLR.SetImpulse(processedLR, settings.maxBlockSize))
            {
                return false;
            }

            if (!convolverRL.SetImpulse(processedRL, settings.maxBlockSize))
            {
                return false;
            }
        }

        return true;
    }

    /// Quality and Low Latency are built into the convolvers, so SetParam only records a change
    /// and the next build picks it up. With nothing built yet (a preset sets its params before its
    /// IR loads) that is the first build. Otherwise the rebuild is left to the message thread
    /// (DeferredRebuild): SetParam can run on the audio thread, and the rebuild allocates.
    void RequestRebuildSetting(std::atomic<int>& pending, int built, int requested)
    {
        // Asking for what is built cancels a change still waiting.
        const int target = requested == built ? -1 : requested;

        if (pending.exchange(target, std::memory_order_acq_rel) != target && HasResource())
        {
            DeferredRebuild::NoteRequested();
        }
    }

    [[nodiscard]] IRQuality EffectiveQuality() const
    {
        const int pending = mPendingQuality.load(std::memory_order_acquire);
        return pending >= 0 ? static_cast<IRQuality>(pending) : mQuality;
    }

    [[nodiscard]] bool EffectiveLowLatency() const
    {
        const int pending = mPendingLowLatency.load(std::memory_order_acquire);
        return pending >= 0 ? pending != 0 : mLowLatency;
    }

    /// Every build of the live convolvers starts here: a Quality or Low Latency change still
    /// waiting is folded into the build, and any rebuild already taken for it is dropped when it
    /// comes back.
    void ApplyPendingRebuildSettings()
    {
        ++mBuildGeneration;
        mQuality = EffectiveQuality();
        mLowLatency = EffectiveLowLatency();
        mPendingQuality.store(-1, std::memory_order_release);
        mPendingLowLatency.store(-1, std::memory_order_release);
    }

    /// A pending value the commit just built is no longer pending; a different one still is.
    static void ClearPendingIf(std::atomic<int>& pending, int built)
    {
        pending.compare_exchange_strong(built, -1, std::memory_order_acq_rel);
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

    std::shared_ptr<const ImpulseSet> mImpulses; // as loaded, at mIRSampleRate

    // The impulses at the playback rate (see EnsurePlaybackImpulses): mImpulses itself when the IR
    // rate matches the host rate.
    std::shared_ptr<const ImpulseSet> mPlaybackImpulses;
    double mPlaybackCacheRate = 0.0;   // host rate the cache was built for
    double mPlaybackCacheIRRate = 0.0; // IR rate it was built from

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
    bool mLowLatency = true; // non-uniform (low-latency) convolution mode
    // Quality and Low Latency as SetParam last asked for them, while the convolvers are built
    // another way or not built yet (-1 when they match). See RequestRebuildSetting.
    std::atomic<int> mPendingQuality{-1};
    std::atomic<int> mPendingLowLatency{-1};
    // Counts builds of the live convolvers, so a deferred rebuild taken before one is dropped.
    std::uint64_t mBuildGeneration = 0;
    std::atomic<float> mTone{1.0f};
    std::atomic<float> mToneCoef{1.0f};
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
