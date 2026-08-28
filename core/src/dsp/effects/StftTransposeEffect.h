#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"

#include <StftPitchShift/STFT.h>
#include <StftPitchShift/StftPitchShiftCore.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

namespace guitarfx
{
namespace detail
{
struct StftTransposeOptions
{
    int mode = 0;
    double quefrencySeconds = 0.0;
    double timbre = 1.0;
    bool normalization = true;
};

struct StftTransposeProfile
{
    size_t analysisWindow = 1024;
    size_t synthesisWindow = 256;
    size_t overlap = 4;

    [[nodiscard]] std::tuple<size_t, size_t> asTuple() const
    {
        return {analysisWindow, synthesisWindow};
    }
};

class StftTransposeChannel
{
  public:
    [[nodiscard]] static int GetExpectedLatencySamples(int semitones, int mode)
    {
        const int clamped = std::clamp(semitones, -15, 12);
        if (clamped == 0)
        {
            return 0;
        }

        const StftTransposeProfile profile = SelectProfile(clamped, mode, 0);
        return static_cast<int>(profile.analysisWindow - profile.synthesisWindow);
    }

    void Prepare(double sampleRate, int maxBlockSize, int semitones, const StftTransposeOptions& options)
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = std::max(1, maxBlockSize);
        mSemitones = semitones;
        mOptions = SanitizeOptions(options);
        mFactor = std::pow(2.0, static_cast<double>(mSemitones) / 12.0);
        RebuildPipeline();
    }

    void Reset()
    {
        std::fill(mInputStreamBuffer.begin(), mInputStreamBuffer.end(), 0.0);
        std::fill(mOutputStreamBuffer.begin(), mOutputStreamBuffer.end(), 0.0);
        std::fill(mInputFrame.begin(), mInputFrame.end(), 0.0f);
        mOutputQueue.clear();
        mOutputReadIndex = 0;
        mInputFill = 0;

        if (mCore)
        {
            ApplyCoreOptions();
        }
    }

    void Configure(int semitones, const StftTransposeOptions& options)
    {
        const int clamped = std::clamp(semitones, -15, 12);
        const StftTransposeOptions sanitizedOptions = SanitizeOptions(options);
        const double factor = std::pow(2.0, static_cast<double>(clamped) / 12.0);
        if (clamped == mSemitones && sanitizedOptions.mode == mOptions.mode &&
            std::abs(sanitizedOptions.quefrencySeconds - mOptions.quefrencySeconds) < 1.0e-9 &&
            std::abs(sanitizedOptions.timbre - mOptions.timbre) < 1.0e-9 &&
            sanitizedOptions.normalization == mOptions.normalization)
        {
            return;
        }

        mSemitones = clamped;
        mFactor = factor;
        mOptions = sanitizedOptions;

        const StftTransposeProfile nextProfile = SelectProfile(mSemitones, mOptions.mode, mMaxBlockSize);
        const bool profileChanged = nextProfile.analysisWindow != mProfile.analysisWindow ||
                                    nextProfile.synthesisWindow != mProfile.synthesisWindow ||
                                    nextProfile.overlap != mProfile.overlap;

        if (profileChanged)
        {
            mProfile = nextProfile;
            RebuildPipeline();
        }
        else if (mCore)
        {
            ApplyCoreOptions();
            Reset();
        }
    }

    [[nodiscard]] int GetLatencySamples() const
    {
        return mLatencySamples;
    }

    void Process(const float* input, float* output, int numSamples)
    {
        if (!input || !output || numSamples <= 0 || !mStft || !mCore)
        {
            return;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            if (mOutputReadIndex < mOutputQueue.size())
            {
                const float queued = static_cast<float>(mOutputQueue[mOutputReadIndex++]);
                output[sampleIndex] = std::isfinite(queued) ? std::clamp(queued, -8.0f, 8.0f) : 0.0f;
            }
            else
            {
                output[sampleIndex] = 0.0f;
            }

            mInputFrame[static_cast<size_t>(mInputFill++)] = input[sampleIndex];
            if (mInputFill == static_cast<int>(mProfile.synthesisWindow))
            {
                ProcessSynthesisBlock();
                mInputFill = 0;
            }
        }

        CompactOutputQueue();
    }

  private:
    [[nodiscard]] static StftTransposeOptions SanitizeOptions(const StftTransposeOptions& options)
    {
        StftTransposeOptions sanitized = options;
        sanitized.mode = std::clamp(options.mode, 0, 1);
        sanitized.quefrencySeconds = std::clamp(options.quefrencySeconds, 0.0, 0.005);
        sanitized.timbre = std::clamp(options.timbre, 0.5, 2.0);
        return sanitized;
    }

    static StftTransposeProfile SelectProfile(int semitones, int mode, int /*maxBlockSize*/)
    {
        const int amount = std::abs(semitones);
        if (mode > 0)
        {
            if (amount <= 2)
            {
                return {1024, 256, 8};
            }
            return {2048, 512, 8};
        }

        if (amount <= 2)
        {
            return {512, 128, 4};
        }
        return {1024, 256, 4};
    }

    void ApplyCoreOptions()
    {
        if (!mCore)
        {
            return;
        }

        mCore->factors({mFactor});
        mCore->quefrency(mOptions.quefrencySeconds);
        mCore->distortion(mOptions.timbre);
        mCore->normalization(mOptions.normalization);
    }

    void RebuildPipeline()
    {
        mProfile = SelectProfile(mSemitones, mOptions.mode, mMaxBlockSize);
        mHopSize = std::max<size_t>(1, mProfile.synthesisWindow / mProfile.overlap);
        mLatencySamples = static_cast<int>(mProfile.analysisWindow - mProfile.synthesisWindow);

        const size_t totalBufferSize = mProfile.analysisWindow + mProfile.synthesisWindow;
        mInputStreamBuffer.assign(totalBufferSize, 0.0);
        mOutputStreamBuffer.assign(totalBufferSize, 0.0);
        mInputFrame.assign(mProfile.synthesisWindow, 0.0f);
        mOutputQueue.clear();
        mOutputReadIndex = 0;
        mInputFill = 0;

        mStft = std::make_unique<stftpitchshift::STFT<double>>(mProfile.asTuple(), mHopSize, false);
        mCore = std::make_unique<stftpitchshift::StftPitchShiftCore<double>>(mProfile.asTuple(), mHopSize, mSampleRate);
        ApplyCoreOptions();

        Reset();
    }

    void ProcessSynthesisBlock()
    {
        std::copy(mInputStreamBuffer.begin() + static_cast<std::ptrdiff_t>(mProfile.synthesisWindow),
                  mInputStreamBuffer.end(), mInputStreamBuffer.begin());

        std::transform(mInputFrame.begin(), mInputFrame.end(),
                       mInputStreamBuffer.begin() + static_cast<std::ptrdiff_t>(mProfile.analysisWindow),
                       [](float value) { return static_cast<double>(value); });

        (*mStft)(std::span<const double>(mInputStreamBuffer.data(), mInputStreamBuffer.size()),
                 std::span<double>(mOutputStreamBuffer.data(), mOutputStreamBuffer.size()),
                 [&](std::span<std::complex<double>> dft) { mCore->shiftpitch(dft); });

        const auto blockBegin = mOutputStreamBuffer.begin() +
                                static_cast<std::ptrdiff_t>(mProfile.analysisWindow - mProfile.synthesisWindow);
        const auto blockEnd = blockBegin + static_cast<std::ptrdiff_t>(mProfile.synthesisWindow);
        mOutputQueue.insert(mOutputQueue.end(), blockBegin, blockEnd);

        std::copy(mOutputStreamBuffer.begin() + static_cast<std::ptrdiff_t>(mProfile.synthesisWindow),
                  mOutputStreamBuffer.end(), mOutputStreamBuffer.begin());
        std::fill(mOutputStreamBuffer.begin() + static_cast<std::ptrdiff_t>(mProfile.analysisWindow),
                  mOutputStreamBuffer.end(), 0.0);
    }

    void CompactOutputQueue()
    {
        if (mOutputReadIndex == 0)
        {
            return;
        }

        if (mOutputReadIndex >= mOutputQueue.size())
        {
            mOutputQueue.clear();
            mOutputReadIndex = 0;
            return;
        }

        if (mOutputReadIndex >= mProfile.synthesisWindow)
        {
            mOutputQueue.erase(mOutputQueue.begin(),
                               mOutputQueue.begin() + static_cast<std::ptrdiff_t>(mOutputReadIndex));
            mOutputReadIndex = 0;
        }
    }

    double mSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    int mSemitones = 0;
    int mInputFill = 0;
    int mLatencySamples = 768;
    double mFactor = 1.0;
    size_t mHopSize = 64;
    size_t mOutputReadIndex = 0;
    StftTransposeProfile mProfile;
    StftTransposeOptions mOptions;

    std::unique_ptr<stftpitchshift::STFT<double>> mStft;
    std::unique_ptr<stftpitchshift::StftPitchShiftCore<double>> mCore;
    std::vector<double> mInputStreamBuffer;
    std::vector<double> mOutputStreamBuffer;
    std::vector<double> mOutputQueue;
    std::vector<float> mInputFrame;
};
} // namespace detail

class StftTransposeEffect : public EffectProcessor
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
        mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

        mActiveSemitones = mRequestedSemitones.load(std::memory_order_relaxed);
        mActiveMode = std::clamp(mRequestedMode.load(std::memory_order_relaxed), 0, 1);
        mActiveMix = std::clamp(mRequestedMix.load(std::memory_order_relaxed), 0.0, 1.0);
        mActiveQuefrencyMs = std::clamp(mRequestedQuefrencyMs.load(std::memory_order_relaxed), 0.0, 5.0);
        mActiveTimbre = std::clamp(mRequestedTimbre.load(std::memory_order_relaxed), 0.5, 2.0);
        mActiveNormalization = mRequestedNormalization.load(std::memory_order_relaxed) > 0.5;
        mSemitoneChangePending.store(false, std::memory_order_relaxed);
        mOptionChangePending.store(false, std::memory_order_relaxed);

        const detail::StftTransposeOptions options = BuildActiveOptions();

        mLeft.Prepare(sampleRate, maxBlockSize, mActiveSemitones, options);
        mRight.Prepare(sampleRate, maxBlockSize, mActiveSemitones, options);
        mReportedLatencySamples.store(
            detail::StftTransposeChannel::GetExpectedLatencySamples(mActiveSemitones, mActiveMode),
            std::memory_order_relaxed);
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
            mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        const float* leftInput = inputs[0] ? inputs[0] : mZero.data();
        const float* rightInput = inputs[1] ? inputs[1] : mZero.data();
        mLeft.Process(leftInput, mWetL.data(), numSamples);
        mRight.Process(rightInput, mWetR.data(), numSamples);

        const float dryMix = static_cast<float>(1.0 - mActiveMix);
        const float wetMix = static_cast<float>(mActiveMix);
        for (int i = 0; i < numSamples; ++i)
        {
            if (outputs[0])
            {
                outputs[0][i] = leftInput[i] * dryMix + mWetL[static_cast<size_t>(i)] * wetMix;
            }
            if (outputs[1])
            {
                outputs[1][i] = rightInput[i] * dryMix + mWetR[static_cast<size_t>(i)] * wetMix;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "semitones")
        {
            const int semitones = static_cast<int>(std::round(std::clamp(value, -12.0, 12.0)));
            if (semitones != mRequestedSemitones.load(std::memory_order_relaxed))
            {
                mRequestedSemitones.store(semitones, std::memory_order_relaxed);
                mReportedLatencySamples.store(detail::StftTransposeChannel::GetExpectedLatencySamples(
                                                  semitones, mRequestedMode.load(std::memory_order_relaxed)),
                                              std::memory_order_relaxed);
                mSemitoneChangePending.store(true, std::memory_order_release);
            }
        }
        else if (key == "mix")
        {
            mRequestedMix.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "mode")
        {
            const int mode = value >= 0.5 ? 1 : 0;
            if (mode != mRequestedMode.load(std::memory_order_relaxed))
            {
                mRequestedMode.store(mode, std::memory_order_relaxed);
                mReportedLatencySamples.store(detail::StftTransposeChannel::GetExpectedLatencySamples(
                                                  mRequestedSemitones.load(std::memory_order_relaxed), mode),
                                              std::memory_order_relaxed);
                mOptionChangePending.store(true, std::memory_order_release);
            }
        }
        else if (key == "quefrencyMs")
        {
            mRequestedQuefrencyMs.store(std::clamp(value, 0.0, 5.0), std::memory_order_relaxed);
            mOptionChangePending.store(true, std::memory_order_release);
        }
        else if (key == "timbre")
        {
            mRequestedTimbre.store(std::clamp(value, 0.5, 2.0), std::memory_order_relaxed);
            mOptionChangePending.store(true, std::memory_order_release);
        }
        else if (key == "normalize")
        {
            mRequestedNormalization.store(value > 0.5 ? 1.0 : 0.0, std::memory_order_relaxed);
            mOptionChangePending.store(true, std::memory_order_release);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "semitones")
        {
            return static_cast<double>(mRequestedSemitones.load(std::memory_order_relaxed));
        }
        if (key == "mix")
        {
            return mRequestedMix.load(std::memory_order_relaxed);
        }
        if (key == "mode")
        {
            return static_cast<double>(mRequestedMode.load(std::memory_order_relaxed));
        }
        if (key == "quefrencyMs")
        {
            return mRequestedQuefrencyMs.load(std::memory_order_relaxed);
        }
        if (key == "timbre")
        {
            return mRequestedTimbre.load(std::memory_order_relaxed);
        }
        if (key == "normalize")
        {
            return mRequestedNormalization.load(std::memory_order_relaxed);
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "transpose_stft";
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
    [[nodiscard]] detail::StftTransposeOptions BuildActiveOptions() const
    {
        detail::StftTransposeOptions options;
        options.mode = mActiveMode;
        options.quefrencySeconds = mActiveQuefrencyMs * 1.0e-3;
        options.timbre = mActiveTimbre;
        options.normalization = mActiveNormalization;
        return options;
    }

    void ApplyPendingRealtimeParams()
    {
        mActiveMix = std::clamp(mRequestedMix.load(std::memory_order_relaxed), 0.0, 1.0);

        const bool semitoneChanged = mSemitoneChangePending.exchange(false, std::memory_order_acq_rel);
        const bool optionChanged = mOptionChangePending.exchange(false, std::memory_order_acq_rel);

        if (!semitoneChanged && !optionChanged)
        {
            return;
        }

        const int requestedSemitones = mRequestedSemitones.load(std::memory_order_acquire);
        const int requestedMode = std::clamp(mRequestedMode.load(std::memory_order_acquire), 0, 1);
        const double requestedQuefrencyMs = std::clamp(mRequestedQuefrencyMs.load(std::memory_order_acquire), 0.0, 5.0);
        const double requestedTimbre = std::clamp(mRequestedTimbre.load(std::memory_order_acquire), 0.5, 2.0);
        const bool requestedNormalization = mRequestedNormalization.load(std::memory_order_acquire) > 0.5;

        if (requestedSemitones == mActiveSemitones && requestedMode == mActiveMode &&
            std::abs(requestedQuefrencyMs - mActiveQuefrencyMs) < 1.0e-9 &&
            std::abs(requestedTimbre - mActiveTimbre) < 1.0e-9 && requestedNormalization == mActiveNormalization)
        {
            return;
        }

        mActiveMode = requestedMode;
        mActiveQuefrencyMs = requestedQuefrencyMs;
        mActiveTimbre = requestedTimbre;
        mActiveNormalization = requestedNormalization;

        const detail::StftTransposeOptions options = BuildActiveOptions();
        mLeft.Configure(requestedSemitones, options);
        mRight.Configure(requestedSemitones, options);
        mActiveSemitones = requestedSemitones;
        mReportedLatencySamples.store(
            detail::StftTransposeChannel::GetExpectedLatencySamples(mActiveSemitones, mActiveMode),
            std::memory_order_relaxed);
    }

    std::atomic<int> mRequestedSemitones{0};
    std::atomic<int> mRequestedMode{0};
    std::atomic<double> mRequestedMix{1.0};
    std::atomic<double> mRequestedQuefrencyMs{0.0};
    std::atomic<double> mRequestedTimbre{1.0};
    std::atomic<double> mRequestedNormalization{1.0};
    std::atomic<bool> mSemitoneChangePending{false};
    std::atomic<bool> mOptionChangePending{false};
    std::atomic<int> mReportedLatencySamples{0};
    bool mConfigured = false;
    int mActiveSemitones = 0;
    int mActiveMode = 0;
    double mActiveMix = 1.0;
    double mActiveQuefrencyMs = 0.0;
    double mActiveTimbre = 1.0;
    bool mActiveNormalization = true;

    detail::StftTransposeChannel mLeft;
    detail::StftTransposeChannel mRight;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
};

inline void RegisterStftTransposeEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTransposeStft;
    info.aliases = {"transpose_stft"};
    info.displayName = "Transpose (STFT)";
    info.category = "pitch";
    info.description = "STFT phase-vocoder transpose for A/B comparison with the default transpose";
    info.requiresResource = false;
    info.parameters = {{"semitones", "Semitones", 0.0, -12.0, 12.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                       {"mode", "Mode", 0.0, 0.0, 1.0, "enum", "", false, 1.0, {"Low Latency", "Polyphonic"}},
                       {"quefrencyMs", "Quefrency", 0.0, 0.0, 5.0, "ms", "", false, 0.1},
                       {"timbre", "Timbre", 1.0, 0.5, 2.0, "ratio", "", false, 0.01},
                       {"normalize", "Normalize", 1.0, 0.0, 1.0, "enum", "", false, 1.0, {"Off", "On"}}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<StftTransposeEffect>(); });
}

} // namespace guitarfx