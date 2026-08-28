#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/LevelTargets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace guitarfx
{
/**
 * Utility pass-through node that also computes live analyzer telemetry.
 * The signal is forwarded unchanged while this processor captures level
 * conversions and a rolling spectrogram snapshot for UI rendering.
 */
class InputAnalyzerEffect : public EffectProcessor
{
  public:
    static constexpr int kSpectrogramBins = 64;
    static constexpr int kBarkBands = 24;
    static constexpr double kSpectrogramMinDbfs = -120.0;
    static constexpr double kSpectrogramMaxDbfs = 0.0;
    static constexpr double kSpectrogramMinFrequencyHz = 20.0;
    static constexpr double kSpectrogramMaxFrequencyHz = 20000.0;
    static constexpr double kBarkMinDbfs = -96.0;
    static constexpr double kBarkMaxDbfs = 0.0;
    static constexpr double kBarkMinFrequencyHz = 20.0;
    static constexpr double kBarkMaxFrequencyHz = 15500.0;

    struct TelemetrySnapshot
    {
        bool valid = false;
        double peakPercent = 0.0;
        double rmsPercent = 0.0;
        double rmsDbu = 0.0;
        double rmsDbv = 0.0;
        double rmsVolts = 0.0;
        bool loudnessValid = false;
        double momentaryLufs = -std::numeric_limits<double>::infinity();
        double shortTermLufs = -std::numeric_limits<double>::infinity();
        double integratedLufs = -std::numeric_limits<double>::infinity();
        bool stereo = false;
        int activeChannelCount = 0;
        std::array<float, kSpectrogramBins> spectrogramBinsDb{};
        std::array<float, kBarkBands> barkBandsDb{};
        std::uint64_t generatedAtMs = 0;
    };

    struct BiquadCoefficients
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    struct BiquadFilter
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double z1 = 0.0;
        double z2 = 0.0;

        void Set(const BiquadCoefficients& coeffs)
        {
            b0 = coeffs.b0;
            b1 = coeffs.b1;
            b2 = coeffs.b2;
            a1 = coeffs.a1;
            a2 = coeffs.a2;
        }

        void Reset()
        {
            z1 = 0.0;
            z2 = 0.0;
        }

        double Process(double input)
        {
            const double output = (b0 * input) + z1;
            z1 = (b1 * input) - (a1 * output) + z2;
            z2 = (b2 * input) - (a2 * output);
            return output;
        }
    };

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        ConfigureLoudnessAnalysis();
        Reset();
    }

    void Reset() override
    {
        mTelemetryValid.store(false, std::memory_order_relaxed);
        mPeakPercent.store(0.0, std::memory_order_relaxed);
        mRmsPercent.store(0.0, std::memory_order_relaxed);
        mRmsDbu.store(0.0, std::memory_order_relaxed);
        mRmsDbv.store(0.0, std::memory_order_relaxed);
        mRmsVolts.store(0.0, std::memory_order_relaxed);
        mLoudnessValid.store(false, std::memory_order_relaxed);
        mMomentaryLufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
        mShortTermLufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
        mIntegratedLufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
        mActiveChannelCount.store(0, std::memory_order_relaxed);
        mStereoSignal.store(false, std::memory_order_relaxed);
        mStereoLatched = false;
        mGeneratedAtMs.store(0, std::memory_order_relaxed);
        mSmoothedPeakLinear = 0.0;
        mSmoothedRmsLinear = 0.0;
        mLoudnessSampleCount = 0;
        mNextIntegratedBlockSample = mMomentaryWindowSamples;
        mMomentaryFilledSamples = 0;
        mShortTermFilledSamples = 0;
        mMomentaryWriteIndex = 0;
        mShortTermWriteIndex = 0;
        mMomentaryWindowEnergy = 0.0;
        mShortTermWindowEnergy = 0.0;
        mLoudnessBlockCount = 0;
        mLoudnessBlockWriteIndex = 0;
        mLeftLoudnessFilter1.Reset();
        mLeftLoudnessFilter2.Reset();
        mRightLoudnessFilter1.Reset();
        mRightLoudnessFilter2.Reset();
        std::fill(mMomentaryEnergyRing.begin(), mMomentaryEnergyRing.end(), 0.0);
        std::fill(mShortTermEnergyRing.begin(), mShortTermEnergyRing.end(), 0.0);
        for (auto& bin : mSpectrogramBinsDb)
        {
            bin.store(static_cast<float>(kSpectrogramMinDbfs), std::memory_order_relaxed);
        }
        for (auto& band : mBarkBandsDb)
        {
            band.store(static_cast<float>(kBarkMinDbfs), std::memory_order_relaxed);
        }
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs)
        {
            return;
        }

        const float* leftIn = (inputs && inputs[0]) ? inputs[0] : nullptr;
        const float* rightIn = (inputs && inputs[1]) ? inputs[1] : nullptr;
        const bool hasSignalInput = leftIn != nullptr || rightIn != nullptr;

        for (int ch = 0; ch < 2; ++ch)
        {
            float* out = outputs[ch];
            const float* in = inputs ? inputs[ch] : nullptr;
            if (!out)
            {
                continue;
            }

            if (in)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    out[i] = in[i];
                }
            }
            else
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    out[i] = 0.0f;
                }
            }
        }

        if (!hasSignalInput || numSamples <= 0)
        {
            mActiveChannelCount.store(0, std::memory_order_relaxed);
            mStereoSignal.store(false, std::memory_order_relaxed);
            mStereoLatched = false;
            mTelemetryValid.store(false, std::memory_order_relaxed);
            return;
        }

        double leftPeak = 0.0;
        double rightPeak = 0.0;
        double leftSumSquares = 0.0;
        double rightSumSquares = 0.0;

        if (leftIn)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const double value = static_cast<double>(leftIn[i]);
                leftPeak = std::max(leftPeak, std::abs(value));
                leftSumSquares += value * value;
            }
        }

        if (rightIn)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const double value = static_cast<double>(rightIn[i]);
                rightPeak = std::max(rightPeak, std::abs(value));
                rightSumSquares += value * value;
            }
        }

        constexpr double kActiveChannelThreshold = 1.0e-7;
        const bool leftActive = leftIn && leftPeak > kActiveChannelThreshold;
        const bool rightActive = rightIn && rightPeak > kActiveChannelThreshold;
        const bool hasActiveSignal = leftActive || rightActive;
        if (!hasActiveSignal)
        {
            mActiveChannelCount.store(0, std::memory_order_relaxed);
            mStereoSignal.store(false, std::memory_order_relaxed);
            mStereoLatched = false;
            mTelemetryValid.store(false, std::memory_order_relaxed);
            return;
        }

        // Determine whether the measured signal carries distinct left/right
        // content. A single active channel, or two channels with effectively
        // identical (dual-mono) content, is reported as mono. Hysteresis on the
        // side-to-reference energy ratio prevents flicker near the threshold.
        bool stereoSignal = false;
        if (leftActive && rightActive)
        {
            double diffSumSquares = 0.0;
            for (int i = 0; i < numSamples; ++i)
            {
                const double diff = static_cast<double>(leftIn[i]) - static_cast<double>(rightIn[i]);
                diffSumSquares += diff * diff;
            }
            const double inv = 1.0 / static_cast<double>(numSamples);
            const double leftRms = std::sqrt(leftSumSquares * inv);
            const double rightRms = std::sqrt(rightSumSquares * inv);
            const double diffRms = std::sqrt(diffSumSquares * inv);
            const double referenceRms = std::max(leftRms, rightRms);
            const double ratio = referenceRms > kMinLinear ? diffRms / referenceRms : 0.0;
            mStereoLatched = mStereoLatched ? ratio > kStereoExitRatio : ratio > kStereoEnterRatio;
            stereoSignal = mStereoLatched;
        }
        else
        {
            mStereoLatched = false;
        }

        const double peak = std::max(leftPeak, rightPeak);
        const double sumSquares = (leftActive ? leftSumSquares : 0.0) + (rightActive ? rightSumSquares : 0.0);
        const std::size_t activeChannelCount =
            static_cast<std::size_t>(leftActive ? 1 : 0) + static_cast<std::size_t>(rightActive ? 1 : 0);
        const std::size_t sampleCount = static_cast<std::size_t>(numSamples) * activeChannelCount;
        const double rms = sampleCount > 0 ? std::sqrt(sumSquares / static_cast<double>(sampleCount)) : 0.0;

        const double deltaSeconds = (mSampleRate > 0.0) ? static_cast<double>(numSamples) / mSampleRate : 0.0;
        mSmoothedPeakLinear = SmoothWithBallistics(std::max(0.0, peak), mSmoothedPeakLinear, deltaSeconds,
                                                   kPeakAttackSeconds, kPeakReleaseSeconds);
        mSmoothedRmsLinear = SmoothWithBallistics(std::max(0.0, rms), mSmoothedRmsLinear, deltaSeconds,
                                                  kRmsAttackSeconds, kRmsReleaseSeconds);

        const double nominalDbfs = GetNominalOperatingLevelDbfs();
        const double rmsDbfs = ToDbfs(mSmoothedRmsLinear);
        const double rmsDbu = rmsDbfs - nominalDbfs + 4.0;
        const double rmsDbv = rmsDbu - kDbuToDbvOffset;
        const double rmsVolts = 0.775 * std::pow(10.0, rmsDbu / 20.0);

        mPeakPercent.store(std::clamp(mSmoothedPeakLinear * 100.0, 0.0, 100.0), std::memory_order_relaxed);
        mRmsPercent.store(std::clamp(mSmoothedRmsLinear * 100.0, 0.0, 100.0), std::memory_order_relaxed);
        mRmsDbu.store(rmsDbu, std::memory_order_relaxed);
        mRmsDbv.store(rmsDbv, std::memory_order_relaxed);
        mRmsVolts.store(std::max(0.0, rmsVolts), std::memory_order_relaxed);
        mActiveChannelCount.store(static_cast<int>(activeChannelCount), std::memory_order_relaxed);
        mStereoSignal.store(stereoSignal, std::memory_order_relaxed);

        UpdateLoudnessAnalysis(leftIn, rightIn, numSamples);

        const int sampleWindow = std::max(1, numSamples);
        const double logRange = std::log(kSpectrogramMaxFrequencyHz / kSpectrogramMinFrequencyHz);
        constexpr float kSmoothing = 0.72f;
        constexpr float kOneMinusSmoothing = 1.0f - kSmoothing;
        std::array<double, kBarkBands> barkPowerByBand{};
        std::array<int, kBarkBands> barkSampleCounts{};

        for (int bin = 0; bin < kSpectrogramBins; ++bin)
        {
            const double t =
                (kSpectrogramBins <= 1) ? 0.0 : static_cast<double>(bin) / static_cast<double>(kSpectrogramBins - 1);
            const double freq = kSpectrogramMinFrequencyHz * std::exp(logRange * t);
            const double omega = (2.0 * kPi * freq) / std::max(1.0, mSampleRate);
            const double coeff = 2.0 * std::cos(omega);
            double s0 = 0.0;
            double s1 = 0.0;
            double s2 = 0.0;

            for (int i = 0; i < sampleWindow; ++i)
            {
                const double left = leftActive ? static_cast<double>(leftIn[i]) : 0.0;
                const double right = rightActive ? static_cast<double>(rightIn[i]) : left;
                double mono = left;
                if (leftActive && rightActive)
                {
                    mono = 0.5 * (left + right);
                }
                else if (!leftActive && rightActive)
                {
                    mono = right;
                }
                const double window =
                    0.5 - 0.5 * std::cos((2.0 * kPi * static_cast<double>(i)) / static_cast<double>(sampleWindow));
                s0 = (mono * window) + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            const double power = std::max(0.0, (s1 * s1) + (s2 * s2) - (coeff * s1 * s2));
            const double magnitude = std::sqrt(power) / static_cast<double>(sampleWindow);
            const int barkBand = FindBarkBandIndex(freq);
            if (barkBand >= 0)
            {
                barkPowerByBand[static_cast<std::size_t>(barkBand)] += magnitude * magnitude;
                barkSampleCounts[static_cast<std::size_t>(barkBand)] += 1;
            }
            const float magnitudeDb =
                static_cast<float>(std::clamp(ToDbfs(magnitude), kSpectrogramMinDbfs, kSpectrogramMaxDbfs));
            const float previous = mSpectrogramBinsDb[static_cast<std::size_t>(bin)].load(std::memory_order_relaxed);
            const float smoothed = previous * kSmoothing + magnitudeDb * kOneMinusSmoothing;
            mSpectrogramBinsDb[static_cast<std::size_t>(bin)].store(smoothed, std::memory_order_relaxed);
        }

        constexpr float kBarkSmoothing = 0.68f;
        constexpr float kBarkOneMinusSmoothing = 1.0f - kBarkSmoothing;
        for (int band = 0; band < kBarkBands; ++band)
        {
            const int count = barkSampleCounts[static_cast<std::size_t>(band)];
            const double barkMagnitude =
                count > 0 ? std::sqrt(barkPowerByBand[static_cast<std::size_t>(band)] / static_cast<double>(count))
                          : 0.0;
            const float barkDb = static_cast<float>(std::clamp(ToDbfs(barkMagnitude), kBarkMinDbfs, kBarkMaxDbfs));
            const float previous = mBarkBandsDb[static_cast<std::size_t>(band)].load(std::memory_order_relaxed);
            const float smoothed = previous * kBarkSmoothing + barkDb * kBarkOneMinusSmoothing;
            mBarkBandsDb[static_cast<std::size_t>(band)].store(smoothed, std::memory_order_relaxed);
        }

        mGeneratedAtMs.store(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::system_clock::now().time_since_epoch())
                                                            .count()),
                             std::memory_order_relaxed);
        mTelemetryValid.store(true, std::memory_order_relaxed);
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "input_analyzer";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

    [[nodiscard]] TelemetrySnapshot GetTelemetrySnapshot() const
    {
        TelemetrySnapshot snapshot;
        snapshot.valid = mTelemetryValid.load(std::memory_order_relaxed);
        snapshot.peakPercent = mPeakPercent.load(std::memory_order_relaxed);
        snapshot.rmsPercent = mRmsPercent.load(std::memory_order_relaxed);
        snapshot.rmsDbu = mRmsDbu.load(std::memory_order_relaxed);
        snapshot.rmsDbv = mRmsDbv.load(std::memory_order_relaxed);
        snapshot.rmsVolts = mRmsVolts.load(std::memory_order_relaxed);
        snapshot.loudnessValid = mLoudnessValid.load(std::memory_order_relaxed);
        snapshot.momentaryLufs = mMomentaryLufs.load(std::memory_order_relaxed);
        snapshot.shortTermLufs = mShortTermLufs.load(std::memory_order_relaxed);
        snapshot.integratedLufs = mIntegratedLufs.load(std::memory_order_relaxed);
        snapshot.activeChannelCount = mActiveChannelCount.load(std::memory_order_relaxed);
        snapshot.stereo = mStereoSignal.load(std::memory_order_relaxed);
        for (int i = 0; i < kSpectrogramBins; ++i)
        {
            snapshot.spectrogramBinsDb[static_cast<std::size_t>(i)] =
                mSpectrogramBinsDb[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < kBarkBands; ++i)
        {
            snapshot.barkBandsDb[static_cast<std::size_t>(i)] =
                mBarkBandsDb[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        }
        snapshot.generatedAtMs = mGeneratedAtMs.load(std::memory_order_relaxed);
        return snapshot;
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kMinLinear = 1.0e-9;
    static constexpr double kDbuToDbvOffset = 2.21;
    static constexpr double kPeakAttackSeconds = 0.02;
    static constexpr double kPeakReleaseSeconds = 0.30;
    static constexpr double kRmsAttackSeconds = 0.06;
    static constexpr double kRmsReleaseSeconds = 0.35;
    static constexpr double kStereoEnterRatio = 0.05;
    static constexpr double kStereoExitRatio = 0.02;
    static constexpr double kMomentaryWindowSeconds = 0.4;
    static constexpr double kShortTermWindowSeconds = 3.0;
    static constexpr double kLoudnessHopSeconds = 0.1;
    static constexpr double kLoudnessAbsoluteGateLufs = -70.0;
    static constexpr double kLoudnessRelativeGateDb = -10.0;
    static constexpr std::size_t kLoudnessHistoryBlocks = 4096;
    static constexpr std::array<double, static_cast<std::size_t>(kBarkBands + 1)> kBarkBandEdgesHz{
        20.0,   100.0,  200.0,  300.0,  400.0,  510.0,  630.0,  770.0,  920.0,  1080.0, 1270.0,  1480.0, 1720.0,
        2000.0, 2320.0, 2700.0, 3150.0, 3700.0, 4400.0, 5300.0, 6400.0, 7700.0, 9500.0, 12000.0, 15500.0};

    static double ToDbfs(double linear)
    {
        if (linear <= kMinLinear || !std::isfinite(linear))
        {
            return kSpectrogramMinDbfs;
        }
        return 20.0 * std::log10(linear);
    }

    static double LufsToMeanSquare(double lufs)
    {
        if (!std::isfinite(lufs))
        {
            return 0.0;
        }
        return std::pow(10.0, (lufs + 0.691) / 10.0);
    }

    static double MeanSquareToLufs(double meanSquare)
    {
        if (!std::isfinite(meanSquare) || meanSquare <= kMinLinear)
        {
            return -std::numeric_limits<double>::infinity();
        }
        return -0.691 + 10.0 * std::log10(meanSquare);
    }

    static BiquadCoefficients DesignKWeightingHighShelf(double sampleRate)
    {
        constexpr double kGainDb = 3.99984385397;
        constexpr double kQ = 0.7071752369554193;
        constexpr double kFc = 1681.9744509555319;
        const double K = std::tan(3.14159265358979323846 * kFc / sampleRate);
        const double Vh = std::pow(10.0, kGainDb / 20.0);
        const double Vb = std::pow(Vh, 0.499666774155);
        const double a0 = 1.0 + K / kQ + K * K;

        BiquadCoefficients coeffs;
        coeffs.b0 = (Vh + Vb * K / kQ + K * K) / a0;
        coeffs.b1 = 2.0 * (K * K - Vh) / a0;
        coeffs.b2 = (Vh - Vb * K / kQ + K * K) / a0;
        coeffs.a1 = 2.0 * (K * K - 1.0) / a0;
        coeffs.a2 = (1.0 - K / kQ + K * K) / a0;
        return coeffs;
    }

    static BiquadCoefficients DesignKWeightingHighPass(double sampleRate)
    {
        constexpr double kQ = 0.5003270373253953;
        constexpr double kFc = 38.13547087613982;
        const double K = std::tan(3.14159265358979323846 * kFc / sampleRate);
        const double a0 = 1.0 + K / kQ + K * K;

        BiquadCoefficients coeffs;
        coeffs.b0 = 1.0 / a0;
        coeffs.b1 = -2.0 / a0;
        coeffs.b2 = 1.0 / a0;
        coeffs.a1 = 2.0 * (K * K - 1.0) / a0;
        coeffs.a2 = (1.0 - K / kQ + K * K) / a0;
        return coeffs;
    }

    static int FindBarkBandIndex(double frequencyHz)
    {
        if (!std::isfinite(frequencyHz) || frequencyHz < kBarkBandEdgesHz.front() ||
            frequencyHz > kBarkBandEdgesHz.back())
        {
            return -1;
        }
        for (int band = 0; band < kBarkBands; ++band)
        {
            const double low = kBarkBandEdgesHz[static_cast<std::size_t>(band)];
            const double high = kBarkBandEdgesHz[static_cast<std::size_t>(band + 1)];
            if (frequencyHz >= low && frequencyHz < high)
            {
                return band;
            }
        }
        return kBarkBands - 1;
    }

    void ConfigureLoudnessAnalysis()
    {
        mMomentaryWindowSamples =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(mSampleRate * kMomentaryWindowSeconds)));
        mShortTermWindowSamples =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(mSampleRate * kShortTermWindowSeconds)));
        mLoudnessHopSamples =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(mSampleRate * kLoudnessHopSeconds)));

        mMomentaryEnergyRing.assign(mMomentaryWindowSamples, 0.0);
        mShortTermEnergyRing.assign(mShortTermWindowSamples, 0.0);

        const auto highShelf = DesignKWeightingHighShelf(mSampleRate);
        const auto highPass = DesignKWeightingHighPass(mSampleRate);
        mLeftLoudnessFilter1.Set(highShelf);
        mLeftLoudnessFilter2.Set(highPass);
        mRightLoudnessFilter1.Set(highShelf);
        mRightLoudnessFilter2.Set(highPass);
    }

    void UpdateLoudnessAnalysis(const float* leftIn, const float* rightIn, int numSamples)
    {
        if (numSamples <= 0)
        {
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const double leftSample = leftIn ? static_cast<double>(leftIn[i]) : 0.0;
            const double rightSample = rightIn ? static_cast<double>(rightIn[i]) : 0.0;

            const double leftFiltered = mLeftLoudnessFilter2.Process(mLeftLoudnessFilter1.Process(leftSample));
            const double rightFiltered = mRightLoudnessFilter2.Process(mRightLoudnessFilter1.Process(rightSample));
            const double sampleEnergy = (leftFiltered * leftFiltered) + (rightFiltered * rightFiltered);

            PushWindowEnergy(sampleEnergy, mMomentaryEnergyRing, mMomentaryWindowEnergy, mMomentaryWriteIndex,
                             mMomentaryFilledSamples, mMomentaryWindowSamples);
            PushWindowEnergy(sampleEnergy, mShortTermEnergyRing, mShortTermWindowEnergy, mShortTermWriteIndex,
                             mShortTermFilledSamples, mShortTermWindowSamples);

            ++mLoudnessSampleCount;

            if (mMomentaryFilledSamples >= mMomentaryWindowSamples)
            {
                mMomentaryLufs.store(
                    MeanSquareToLufs(mMomentaryWindowEnergy / static_cast<double>(mMomentaryWindowSamples)),
                    std::memory_order_relaxed);
                mShortTermLufs.store(
                    MeanSquareToLufs(mShortTermWindowEnergy / static_cast<double>(mShortTermWindowSamples)),
                    std::memory_order_relaxed);
                mLoudnessValid.store(true, std::memory_order_relaxed);
            }

            while (mMomentaryFilledSamples >= mMomentaryWindowSamples &&
                   mLoudnessSampleCount >= mNextIntegratedBlockSample)
            {
                AppendIntegratedBlock(mMomentaryLufs.load(std::memory_order_relaxed));
                mNextIntegratedBlockSample += mLoudnessHopSamples;
            }
        }
    }

    static void PushWindowEnergy(double sampleEnergy, std::vector<double>& ring, double& windowEnergy,
                                 std::size_t& writeIndex, std::size_t& filledSamples, std::size_t windowSamples)
    {
        if (ring.size() != windowSamples || windowSamples == 0)
        {
            return;
        }

        if (filledSamples < windowSamples)
        {
            ring[writeIndex] = sampleEnergy;
            windowEnergy += sampleEnergy;
            ++filledSamples;
        }
        else
        {
            windowEnergy += sampleEnergy - ring[writeIndex];
            ring[writeIndex] = sampleEnergy;
        }

        ++writeIndex;
        if (writeIndex >= windowSamples)
        {
            writeIndex = 0;
        }
    }

    void AppendIntegratedBlock(double blockLufs)
    {
        if (!std::isfinite(blockLufs))
        {
            return;
        }

        mLoudnessBlocks[mLoudnessBlockWriteIndex] = blockLufs;
        if (mLoudnessBlockCount < kLoudnessHistoryBlocks)
        {
            ++mLoudnessBlockCount;
        }
        mLoudnessBlockWriteIndex = (mLoudnessBlockWriteIndex + 1) % kLoudnessHistoryBlocks;
        RecomputeIntegratedLoudness();
    }

    void RecomputeIntegratedLoudness()
    {
        if (mLoudnessBlockCount == 0)
        {
            mIntegratedLufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            return;
        }

        std::array<double, kLoudnessHistoryBlocks> energies{};
        std::size_t validCount = 0;
        for (std::size_t i = 0; i < mLoudnessBlockCount; ++i)
        {
            const std::size_t index =
                (mLoudnessBlockWriteIndex + kLoudnessHistoryBlocks - mLoudnessBlockCount + i) % kLoudnessHistoryBlocks;
            const double lufs = mLoudnessBlocks[index];
            if (!std::isfinite(lufs) || lufs < kLoudnessAbsoluteGateLufs)
            {
                continue;
            }
            energies[validCount++] = LufsToMeanSquare(lufs);
        }

        if (validCount == 0)
        {
            mIntegratedLufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            return;
        }

        double absEnergySum = 0.0;
        for (std::size_t i = 0; i < validCount; ++i)
        {
            absEnergySum += energies[i];
        }
        const double absAverageEnergy = absEnergySum / static_cast<double>(validCount);
        const double relativeGateLufs = MeanSquareToLufs(absAverageEnergy) + kLoudnessRelativeGateDb;

        double gatedEnergySum = 0.0;
        std::size_t gatedCount = 0;
        for (std::size_t i = 0; i < validCount; ++i)
        {
            const double lufs = MeanSquareToLufs(energies[i]);
            if (lufs > relativeGateLufs)
            {
                gatedEnergySum += energies[i];
                ++gatedCount;
            }
        }

        if (gatedCount == 0)
        {
            mIntegratedLufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_relaxed);
            return;
        }

        const double integratedEnergy = gatedEnergySum / static_cast<double>(gatedCount);
        mIntegratedLufs.store(MeanSquareToLufs(integratedEnergy), std::memory_order_relaxed);
    }

    static double BallisticAlpha(double deltaSeconds, double timeConstantSeconds)
    {
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0)
        {
            return 0.0;
        }
        if (!std::isfinite(timeConstantSeconds) || timeConstantSeconds <= 0.0)
        {
            return 0.0;
        }
        return std::exp(-deltaSeconds / timeConstantSeconds);
    }

    static double SmoothWithBallistics(double currentLinear, double previousLinear, double deltaSeconds,
                                       double attackSeconds, double releaseSeconds)
    {
        if (!std::isfinite(currentLinear))
        {
            currentLinear = 0.0;
        }
        if (!std::isfinite(previousLinear))
        {
            previousLinear = 0.0;
        }

        const double target = std::max(0.0, currentLinear);
        const double previous = std::max(0.0, previousLinear);
        const double alpha = BallisticAlpha(deltaSeconds, target > previous ? attackSeconds : releaseSeconds);
        return alpha * previous + (1.0 - alpha) * target;
    }

    std::atomic<bool> mTelemetryValid{false};
    std::atomic<double> mPeakPercent{0.0};
    std::atomic<double> mRmsPercent{0.0};
    std::atomic<double> mRmsDbu{0.0};
    std::atomic<double> mRmsDbv{0.0};
    std::atomic<double> mRmsVolts{0.0};
    std::atomic<bool> mLoudnessValid{false};
    std::atomic<double> mMomentaryLufs{-std::numeric_limits<double>::infinity()};
    std::atomic<double> mShortTermLufs{-std::numeric_limits<double>::infinity()};
    std::atomic<double> mIntegratedLufs{-std::numeric_limits<double>::infinity()};
    std::atomic<int> mActiveChannelCount{0};
    std::atomic<bool> mStereoSignal{false};
    std::array<std::atomic<float>, kSpectrogramBins> mSpectrogramBinsDb{};
    std::array<std::atomic<float>, kBarkBands> mBarkBandsDb{};
    std::atomic<std::uint64_t> mGeneratedAtMs{0};
    double mSmoothedPeakLinear = 0.0;
    double mSmoothedRmsLinear = 0.0;
    bool mStereoLatched = false;
    std::size_t mMomentaryWindowSamples = 1;
    std::size_t mShortTermWindowSamples = 1;
    std::size_t mLoudnessHopSamples = 1;
    std::size_t mLoudnessSampleCount = 0;
    std::size_t mNextIntegratedBlockSample = 1;
    std::size_t mMomentaryFilledSamples = 0;
    std::size_t mShortTermFilledSamples = 0;
    std::size_t mMomentaryWriteIndex = 0;
    std::size_t mShortTermWriteIndex = 0;
    std::size_t mLoudnessBlockCount = 0;
    std::size_t mLoudnessBlockWriteIndex = 0;
    double mMomentaryWindowEnergy = 0.0;
    double mShortTermWindowEnergy = 0.0;
    std::vector<double> mMomentaryEnergyRing;
    std::vector<double> mShortTermEnergyRing;
    std::array<double, kLoudnessHistoryBlocks> mLoudnessBlocks{};
    BiquadFilter mLeftLoudnessFilter1;
    BiquadFilter mLeftLoudnessFilter2;
    BiquadFilter mRightLoudnessFilter1;
    BiquadFilter mRightLoudnessFilter2;
};

inline void RegisterInputAnalyzerEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kInputAnalyzer;
    info.aliases = {"input_analyzer", "analyzer_input", "audio_analyzer"};
    info.displayName = "Signal Analyzer";
    info.category = "utility";
    info.description = "Live level analyzer with spectrogram";
    info.requiresResource = false;
    info.parameters = {};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<InputAnalyzerEffect>(); });
}
} // namespace guitarfx
