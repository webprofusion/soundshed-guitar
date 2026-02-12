#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/IRTypes.h"
#include "dsp/IRWavLoader.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <vector>

namespace guitarfx
{
  /**
   * Impulse Response cabinet simulation effect.
   * Uses convolution to apply a cabinet IR to the signal.
   * Supports multi-channel WAV files, WAVE_FORMAT_EXTENSIBLE, and quality-based truncation.
   */
  class IRCabEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      mResourceTransitionSamplesTotal = std::max(1, static_cast<int>(std::round(mSampleRate * 0.03))); // 30 ms

      UpdateAirCoefficients();
      UpdateCabFilterCoefficients();
      ResetAirState();
      ResetCabFilterState();

      mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferBL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferBR.resize(static_cast<size_t>(maxBlockSize));

      ApplyPendingQuality();

      // Reinitialize convolvers if we have IRs loaded
      if (!mImpulseL.empty())
      {
        InitializeConvolverA();
      }
      if (!mImpulseBL.empty())
      {
        InitializeConvolverB();
      }
    }

    void Reset() override
    {
      mConvolverL.Reset();
      mConvolverR.Reset();
      mConvolverBL.Reset();
      mConvolverBR.Reset();
      ResetAirState();
      ResetCabFilterState();
      mResourceTransitionSamplesRemaining = 0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!outputs || numSamples <= 0)
      {
        return;
      }

      // Clamp to allocated buffer size to prevent out-of-bounds writes
      numSamples = std::min(numSamples, mMaxBlockSize);

      if (!inputs)
      {
        if (outputs[0])
          std::fill_n(outputs[0], numSamples, 0.0f);
        if (outputs[1])
          std::fill_n(outputs[1], numSamples, 0.0f);
        return;
      }

      const bool rightReady = !mIsStereo || mConvolverR.IsInitialized();
      if (!mEnabled || mMix <= 0.0 || !mConvolverL.IsInitialized() || !rightReady)
      {
        // Bypass: copy input to output, falling back L→R if R is null
        if (outputs[0])
        {
          if (inputs[0])
            std::copy_n(inputs[0], numSamples, outputs[0]);
          else
            std::fill_n(outputs[0], numSamples, 0.0f);
        }
        if (outputs[1])
        {
          if (inputs[1])
            std::copy_n(inputs[1], numSamples, outputs[1]);
          else if (inputs[0])
            std::copy_n(inputs[0], numSamples, outputs[1]);
          else
            std::fill_n(outputs[1], numSamples, 0.0f);
        }
        return;
      }

      // Copy to double buffers
      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0;
        mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0);
      }

      // Process through convolvers
      mConvolverL.Process(mInputBufferL.data(), mOutputBufferL.data(), numSamples);
      mConvolverR.Process(mInputBufferR.data(), mOutputBufferR.data(), numSamples);

      const bool hasB = mConvolverBL.IsInitialized() && (!mIsStereoB || mConvolverBR.IsInitialized());
      if (hasB)
      {
        mConvolverBL.Process(mInputBufferL.data(), mOutputBufferBL.data(), numSamples);
        mConvolverBR.Process(mInputBufferR.data(), mOutputBufferBR.data(), numSamples);
      }

      // Apply wet/dry mix and output gain
      float wetGain = static_cast<float>(mMix * mOutputGain);
      float dryGain = static_cast<float>((1.0 - mMix));

      const double blend = hasB ? std::clamp(mIRBlend, 0.0, 1.0) : 0.0;
      const double slotAGain = (1.0 - blend) * mSlotAGain * (mSlotAPolarityInverted ? -1.0 : 1.0);
      const double slotBGain = blend * mSlotBGain * (mSlotBPolarityInverted ? -1.0 : 1.0);

      for (int i = 0; i < numSamples; ++i)
      {
        double wetL = mOutputBufferL[i] * slotAGain;
        double wetR = mOutputBufferR[i] * slotAGain;

        if (hasB)
        {
          wetL += mOutputBufferBL[i] * slotBGain;
          wetR += mOutputBufferBR[i] * slotBGain;
        }

        wetL = ProcessCabFilters(wetL, 0);
        wetR = ProcessCabFilters(wetR, 1);

        if (mAirActive)
        {
          wetL = ProcessAirSample(wetL, 0);
          wetR = ProcessAirSample(wetR, 1);
        }

        double transitionWetGain = 1.0;
        if (mResourceTransitionSamplesRemaining > 0)
        {
          const int progressed = mResourceTransitionSamplesTotal - mResourceTransitionSamplesRemaining;
          transitionWetGain = static_cast<double>(progressed) / static_cast<double>(std::max(1, mResourceTransitionSamplesTotal));
          mResourceTransitionSamplesRemaining -= 1;
        }

        wetL *= transitionWetGain;
        wetR *= transitionWetGain;

        float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        float dryR = inputs[1] ? inputs[1][i] : dryL;

        if (outputs[0])
          outputs[0][i] = static_cast<float>(wetL) * wetGain + dryL * dryGain;
        if (outputs[1])
          outputs[1][i] = static_cast<float>(wetR) * wetGain + dryR * dryGain;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "mix")
        mMix = std::clamp(value, 0.0, 1.0);
      else if (key == "irBlend")
        mIRBlend = std::clamp(value, 0.0, 1.0);
      else if (key == "lowCutHz")
      {
        mLowCutHz = std::clamp(value, 20.0, 1000.0);
        UpdateCabFilterCoefficients();
      }
      else if (key == "highCutHz")
      {
        mHighCutHz = std::clamp(value, 1000.0, 20000.0);
        UpdateCabFilterCoefficients();
      }
      else if (key == "slotAGain")
        mSlotAGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "slotBGain")
        mSlotBGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "slotAPolarity")
        mSlotAPolarityInverted = value > 0.5;
      else if (key == "slotBPolarity")
        mSlotBPolarityInverted = value > 0.5;
      else if (key == "outputGain")
        mOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "enabled")
        mEnabled = value > 0.5;
      else if (key == "quality")
      {
        const int q = static_cast<int>(std::clamp(value, 0.0, 3.0));
        mPendingQuality.store(q, std::memory_order_release);
      }
      else if (key == "air")
      {
        mAir = std::clamp(value, 0.0, 1.0);
        UpdateAirCoefficients();
      }
      else if (key == "airMode")
      {
        const int mode = static_cast<int>(std::clamp(value, 0.0, 2.0));
        mAirMode = static_cast<AirMode>(mode);
        ResetAirState();
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "mix")
        return mMix;
      if (key == "irBlend")
        return mIRBlend;
      if (key == "lowCutHz")
        return mLowCutHz;
      if (key == "highCutHz")
        return mHighCutHz;
      if (key == "slotAGain")
        return 20.0 * std::log10(std::max(mSlotAGain, 1e-9));
      if (key == "slotBGain")
        return 20.0 * std::log10(std::max(mSlotBGain, 1e-9));
      if (key == "slotAPolarity")
        return mSlotAPolarityInverted ? 1.0 : 0.0;
      if (key == "slotBPolarity")
        return mSlotBPolarityInverted ? 1.0 : 0.0;
      if (key == "outputGain")
        return 20.0 * std::log10(mOutputGain);
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      if (key == "quality")
      {
        const int pending = mPendingQuality.load(std::memory_order_acquire);
        return pending >= 0 ? static_cast<double>(pending) : static_cast<double>(mQuality);
      }
      if (key == "air")
        return mAir;
      if (key == "airMode")
        return static_cast<double>(mAirMode);
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      // Single-resource load path (legacy): load slot A and clear slot B.
      if (!LoadWavFile(resourcePath))
        return false;

      mIRPath = resourcePath;
      mImpulseBL.clear();
      mImpulseBR.clear();
      mIRPathB.clear();
      mConvolverBL.Reset();
      mConvolverBR.Reset();
      ApplyPendingQuality();
      const bool loaded = InitializeConvolverA();
      if (loaded)
      {
        BeginResourceTransition();
      }
      return loaded;
    }

    bool LoadResources(const std::vector<ResourceRef> &refs,
                      const std::vector<std::filesystem::path> &paths) override
    {
      if (paths.empty())
        return false;

      if (!LoadResource(paths.front()))
        return false;

      if (paths.size() >= 2)
      {
        if (LoadWavFileInto(paths[1], mImpulseBL, mImpulseBR, mIRSampleRateB, mIsStereoB))
        {
          mIRPathB = paths[1];
          if (refs.size() > 1)
          {
            const auto &ref = refs[1];
            if (ref.parameterValue.has_value())
            {
              mIRBlend = std::clamp(*ref.parameterValue, 0.0, 1.0);
            }
          }
          InitializeConvolverB();
        }
      }

      BeginResourceTransition();

      return true;
    }

    void BeginResourceTransition()
    {
      if (!mHasLoadedResource)
      {
        mHasLoadedResource = true;
        mResourceTransitionSamplesRemaining = 0;
        return;
      }

      if (mResourceTransitionSamplesTotal <= 0)
      {
        mResourceTransitionSamplesTotal = std::max(1, static_cast<int>(std::round(mSampleRate * 0.03)));
      }

      mResourceTransitionSamplesRemaining = mResourceTransitionSamplesTotal;
    }

    [[nodiscard]] bool HasResource() const override
    {
      return mConvolverL.IsInitialized() && (!mIsStereo || mConvolverR.IsInitialized());
    }
    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mIRPath; }

    [[nodiscard]] std::string GetType() const override { return "cab_ir"; }
    [[nodiscard]] std::string GetCategory() const override { return "cab"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    // WAV parsing utilities (shared)

    // Find optimal truncation point based on energy threshold
    static size_t FindEnergyTruncationPoint(const std::vector<float> &samples, float threshold = 0.001f)
    {
      if (samples.empty())
        return 0;

      double totalEnergy = 0.0;
      for (const float s : samples)
        totalEnergy += static_cast<double>(s) * static_cast<double>(s);

      if (totalEnergy < 1e-10)
        return samples.size();

      const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
      double cumulativeEnergy = 0.0;

      for (size_t i = 0; i < samples.size(); ++i)
      {
        cumulativeEnergy += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
        if (cumulativeEnergy >= targetEnergy)
          return std::min(i + 256, samples.size()); // Add small buffer after threshold
      }
      return samples.size();
    }

    static size_t FindEnergyTruncationPointStereo(const std::vector<float> &left,
                                                  const std::vector<float> &right,
                                                  float threshold = 0.001f)
    {
      const size_t length = std::min(left.size(), right.size());
      if (length == 0)
        return 0;

      double totalEnergy = 0.0;
      for (size_t i = 0; i < length; ++i)
      {
        totalEnergy += static_cast<double>(left[i]) * static_cast<double>(left[i]);
        totalEnergy += static_cast<double>(right[i]) * static_cast<double>(right[i]);
      }

      if (totalEnergy < 1e-10)
        return length;

      const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
      double cumulativeEnergy = 0.0;

      for (size_t i = 0; i < length; ++i)
      {
        cumulativeEnergy += static_cast<double>(left[i]) * static_cast<double>(left[i]);
        cumulativeEnergy += static_cast<double>(right[i]) * static_cast<double>(right[i]);
        if (cumulativeEnergy >= targetEnergy)
          return std::min(i + 256, length); // Add small buffer after threshold
      }
      return length;
    }

    // Get processed (potentially truncated) IR based on quality setting
    std::vector<float> GetProcessedImpulse(const std::vector<float> &samples) const
    {
      if (samples.empty())
        return {};

      // For Full quality, return the complete IR
      if (mQuality == IRQuality::Full)
        return samples;

      const size_t maxSamples = GetMaxIRSamples(mQuality, mSampleRate);
      if (maxSamples == 0 || samples.size() <= maxSamples)
        return samples;

      // Find smart truncation point based on energy
      const size_t energyTruncPoint = FindEnergyTruncationPoint(samples, 0.001f);
      const size_t truncLength = std::min({samples.size(), maxSamples, energyTruncPoint});

      // Apply fade-out to avoid clicks (last 64 samples)
      std::vector<float> truncated(samples.begin(), samples.begin() + truncLength);

      constexpr size_t kFadeLength = 64;
      if (truncLength > kFadeLength)
      {
        for (size_t i = 0; i < kFadeLength; ++i)
        {
          const float fadeGain = static_cast<float>(kFadeLength - 1 - i) / static_cast<float>(kFadeLength - 1);
          truncated[truncLength - kFadeLength + i] *= fadeGain;
        }
      }
      return truncated;
    }

    void GetProcessedImpulseStereo(std::vector<float> &left, std::vector<float> &right) const
    {
      left.clear();
      right.clear();

      if (mImpulseL.empty() || mImpulseR.empty())
        return;

      if (mQuality == IRQuality::Full)
      {
        left = mImpulseL;
        right = mImpulseR;
        return;
      }

      const size_t maxSamples = GetMaxIRSamples(mQuality, mSampleRate);
      const size_t length = std::min(mImpulseL.size(), mImpulseR.size());
      if (maxSamples == 0 || length <= maxSamples)
      {
        left.assign(mImpulseL.begin(), mImpulseL.begin() + length);
        right.assign(mImpulseR.begin(), mImpulseR.begin() + length);
        return;
      }

      const size_t energyTruncPoint = FindEnergyTruncationPointStereo(mImpulseL, mImpulseR, 0.001f);
      const size_t truncLength = std::min({length, maxSamples, energyTruncPoint});

      left.assign(mImpulseL.begin(), mImpulseL.begin() + truncLength);
      right.assign(mImpulseR.begin(), mImpulseR.begin() + truncLength);

      constexpr size_t kFadeLength = 64;
      if (truncLength > kFadeLength)
      {
        for (size_t i = 0; i < kFadeLength; ++i)
        {
          const float fadeGain = static_cast<float>(kFadeLength - 1 - i) / static_cast<float>(kFadeLength - 1);
          left[truncLength - kFadeLength + i] *= fadeGain;
          right[truncLength - kFadeLength + i] *= fadeGain;
        }
      }
    }

    bool LoadWavFileInto(const std::filesystem::path &path,
                        std::vector<float> &outImpulseL,
                        std::vector<float> &outImpulseR,
                        double &outSampleRate,
                        bool &outIsStereo)
    {
      IRWavData data;
      if (!irwav::LoadWavFile(path, data))
        return false;

      std::vector<float> left;
      std::vector<float> right;
      if (data.channels >= 2)
      {
        irwav::SplitToStereo(data, left, right);
      }
      else
      {
        left = data.samples;
        right.clear();
      }

      if (left.empty())
        return false;

      outSampleRate = data.sampleRate;
      if (data.channels >= 2 && !right.empty())
      {
        const size_t length = std::min(left.size(), right.size());
        left.resize(length);
        right.resize(length);
        outImpulseL = std::move(left);
        outImpulseR = std::move(right);
        outIsStereo = true;
      }
      else
      {
        outImpulseL = std::move(left);
        outImpulseR.clear();
        outIsStereo = false;
      }
      return true;
    }

    bool LoadWavFile(const std::filesystem::path &path)
    {
      return LoadWavFileInto(path, mImpulseL, mImpulseR, mIRSampleRate, mIsStereo);
    }

    bool InitializeConvolverA()
    {
      if (mImpulseL.empty() || mMaxBlockSize == 0)
        return false;

      if (mIsStereo && !mImpulseR.empty())
      {
        std::vector<float> processedL;
        std::vector<float> processedR;
        GetProcessedImpulseStereo(processedL, processedR);

        if (processedL.empty() || processedR.empty())
          return false;

        if (std::abs(mIRSampleRate - mSampleRate) > 1.0)
        {
          irwav::ResampleLinear(processedL, mIRSampleRate, mSampleRate);
          irwav::ResampleLinear(processedR, mIRSampleRate, mSampleRate);
        }

        if (!mConvolverL.SetImpulse(processedL, mMaxBlockSize) ||
            !mConvolverR.SetImpulse(processedR, mMaxBlockSize))
        {
          mConvolverL.Reset();
          mConvolverR.Reset();
          return false;
        }

        return true;
      }

      std::vector<float> processedIR = GetProcessedImpulse(mImpulseL);
      if (processedIR.empty())
        return false;

      if (std::abs(mIRSampleRate - mSampleRate) > 1.0)
        irwav::ResampleLinear(processedIR, mIRSampleRate, mSampleRate);

      if (!mConvolverL.SetImpulse(processedIR, mMaxBlockSize) ||
          !mConvolverR.SetImpulse(processedIR, mMaxBlockSize))
      {
        mConvolverL.Reset();
        mConvolverR.Reset();
        return false;
      }

      return true;
    }

    bool InitializeConvolverB()
    {
      if (mImpulseBL.empty() || mMaxBlockSize == 0)
        return false;

      if (mIsStereoB && !mImpulseBR.empty())
      {
        std::vector<float> processedL = GetProcessedImpulse(mImpulseBL);
        std::vector<float> processedR = GetProcessedImpulse(mImpulseBR);

        if (processedL.empty() || processedR.empty())
          return false;

        const size_t length = std::min(processedL.size(), processedR.size());
        processedL.resize(length);
        processedR.resize(length);

        if (std::abs(mIRSampleRateB - mSampleRate) > 1.0)
        {
          irwav::ResampleLinear(processedL, mIRSampleRateB, mSampleRate);
          irwav::ResampleLinear(processedR, mIRSampleRateB, mSampleRate);
        }

        if (!mConvolverBL.SetImpulse(processedL, mMaxBlockSize) ||
            !mConvolverBR.SetImpulse(processedR, mMaxBlockSize))
        {
          mConvolverBL.Reset();
          mConvolverBR.Reset();
          return false;
        }

        return true;
      }

      std::vector<float> processedIR = GetProcessedImpulse(mImpulseBL);
      if (processedIR.empty())
        return false;

      if (std::abs(mIRSampleRateB - mSampleRate) > 1.0)
        irwav::ResampleLinear(processedIR, mIRSampleRateB, mSampleRate);

      if (!mConvolverBL.SetImpulse(processedIR, mMaxBlockSize) ||
          !mConvolverBR.SetImpulse(processedIR, mMaxBlockSize))
      {
        mConvolverBL.Reset();
        mConvolverBR.Reset();
        return false;
      }

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

    enum class AirMode
    {
      OptionA_Shelf,
      OptionB_Peak,
      OptionC_ShelfPlusPeak,
    };

    static double ProcessBiquad(double input, double b0, double b1, double b2,
                                double a1, double a2, double &s1, double &s2)
    {
      const double output = b0 * input + s1;
      s1 = b1 * input - a1 * output + s2;
      s2 = b2 * input - a2 * output;
      return output;
    }

    void ComputeHighShelf(double freq, double slope, double gainDb,
                          double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double sqrtA = std::sqrt(A);
      const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

      const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
      b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
      a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    }

    void ComputePeakingEQ(double freq, double Q, double gainDb,
                          double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * Q);

      const double a0 = 1.0 + alpha / A;
      b0 = (1.0 + alpha * A) / a0;
      b1 = (-2.0 * cosw0) / a0;
      b2 = (1.0 - alpha * A) / a0;
      a1 = (-2.0 * cosw0) / a0;
      a2 = (1.0 - alpha / A) / a0;
    }

    void UpdateAirCoefficients()
    {
      if (mSampleRate <= 0.0)
      {
        mAirActive = false;
        return;
      }

      const double clamped = std::clamp(mAir, 0.0, 1.0);
      if (clamped <= 0.0001)
      {
        mAirActive = false;
        return;
      }

      // Option A (default): subtle high-shelf lift in the air band.
      const double shelfGainDb = clamped * 6.0; // 0..+6 dB
      const double shelfFreq = 9000.0;
      const double shelfSlope = 0.7;
      ComputeHighShelf(shelfFreq, shelfSlope, shelfGainDb, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2);

      // Option B (optional): high-mid presence peak.
      const double peakGainDb = clamped * 4.0; // 0..+4 dB
      const double peakFreq = 4500.0;
      const double peakQ = 1.2;
      ComputePeakingEQ(peakFreq, peakQ, peakGainDb, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2);

      // Option C (optional): shelf + peak combined (handled in ProcessAirSample).
      mAirActive = true;
    }

    void ResetAirState()
    {
      for (int ch = 0; ch < 2; ++ch)
      {
        mAirShelfS1[ch] = mAirShelfS2[ch] = 0.0;
        mAirPeakS1[ch] = mAirPeakS2[ch] = 0.0;
      }
    }

    void ComputeHighPass(double freq,
                         double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double q = 0.70710678;
      const double alpha = sinw0 / (2.0 * q);

      const double a0 = 1.0 + alpha;
      b0 = ((1.0 + cosw0) / 2.0) / a0;
      b1 = (-(1.0 + cosw0)) / a0;
      b2 = ((1.0 + cosw0) / 2.0) / a0;
      a1 = (-2.0 * cosw0) / a0;
      a2 = (1.0 - alpha) / a0;
    }

    void ComputeLowPass(double freq,
                        double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double q = 0.70710678;
      const double alpha = sinw0 / (2.0 * q);

      const double a0 = 1.0 + alpha;
      b0 = ((1.0 - cosw0) / 2.0) / a0;
      b1 = (1.0 - cosw0) / a0;
      b2 = ((1.0 - cosw0) / 2.0) / a0;
      a1 = (-2.0 * cosw0) / a0;
      a2 = (1.0 - alpha) / a0;
    }

    void UpdateCabFilterCoefficients()
    {
      if (mSampleRate <= 0.0)
      {
        mLowCutActive = false;
        mHighCutActive = false;
        return;
      }

      const double nyquist = mSampleRate * 0.5;
      mLowCutActive = mLowCutHz > 20.5;
      mHighCutActive = mHighCutHz < (nyquist - 100.0);

      if (mLowCutActive)
      {
        const double clamped = std::clamp(mLowCutHz, 20.0, nyquist - 100.0);
        ComputeHighPass(clamped, mLowCutB0, mLowCutB1, mLowCutB2, mLowCutA1, mLowCutA2);
      }
      if (mHighCutActive)
      {
        const double clamped = std::clamp(mHighCutHz, 1000.0, nyquist - 100.0);
        ComputeLowPass(clamped, mHighCutB0, mHighCutB1, mHighCutB2, mHighCutA1, mHighCutA2);
      }
    }

    void ResetCabFilterState()
    {
      for (int ch = 0; ch < 2; ++ch)
      {
        mLowCutS1[ch] = mLowCutS2[ch] = 0.0;
        mHighCutS1[ch] = mHighCutS2[ch] = 0.0;
      }
    }

    double ProcessCabFilters(double input, int channel)
    {
      double output = input;
      if (mLowCutActive)
      {
        output = ProcessBiquad(output, mLowCutB0, mLowCutB1, mLowCutB2, mLowCutA1, mLowCutA2,
                               mLowCutS1[channel], mLowCutS2[channel]);
      }
      if (mHighCutActive)
      {
        output = ProcessBiquad(output, mHighCutB0, mHighCutB1, mHighCutB2, mHighCutA1, mHighCutA2,
                               mHighCutS1[channel], mHighCutS2[channel]);
      }
      return output;
    }

    double ProcessAirSample(double input, int channel)
    {
      switch (mAirMode)
      {
      case AirMode::OptionA_Shelf:
        return ProcessBiquad(input, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2,
                             mAirShelfS1[channel], mAirShelfS2[channel]);
      case AirMode::OptionB_Peak:
        return ProcessBiquad(input, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2,
                             mAirPeakS1[channel], mAirPeakS2[channel]);
      case AirMode::OptionC_ShelfPlusPeak:
      default:
      {
        const double shelf = ProcessBiquad(input, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2,
                                           mAirShelfS1[channel], mAirShelfS2[channel]);
        return ProcessBiquad(shelf, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2,
                             mAirPeakS1[channel], mAirPeakS2[channel]);
      }
      }
    }

    RealtimeConvolver mConvolverL;
    RealtimeConvolver mConvolverR;
    RealtimeConvolver mConvolverBL;
    RealtimeConvolver mConvolverBR;

    std::vector<float> mImpulseL;     // Original IR samples (left)
    std::vector<float> mImpulseR;     // Original IR samples (right, optional)
    std::vector<float> mImpulseBL;    // Optional second IR samples (left)
    std::vector<float> mImpulseBR;    // Optional second IR samples (right)
    std::filesystem::path mIRPath;
    std::filesystem::path mIRPathB;
    double mIRSampleRate = 48000.0;
    double mIRSampleRateB = 48000.0;
    bool mIsStereo = false;
    bool mIsStereoB = false;

    std::vector<double> mInputBufferL;
    std::vector<double> mInputBufferR;
    std::vector<double> mOutputBufferL;
    std::vector<double> mOutputBufferR;
    std::vector<double> mOutputBufferBL;
    std::vector<double> mOutputBufferBR;

    double mMix = 1.0;
    double mIRBlend = 0.0;
    double mLowCutHz = 20.0;
    double mHighCutHz = 20000.0;
    double mSlotAGain = 1.0;
    double mSlotBGain = 1.0;
    bool mSlotAPolarityInverted = false;
    bool mSlotBPolarityInverted = false;
    double mOutputGain = 1.0;
    bool mEnabled = true;
    IRQuality mQuality = IRQuality::Standard;
    std::atomic<int> mPendingQuality{-1};
    bool mHasLoadedResource = false;
    int mResourceTransitionSamplesTotal = 1440;
    int mResourceTransitionSamplesRemaining = 0;

    double mAir = 0.0;
    AirMode mAirMode = AirMode::OptionA_Shelf;
    bool mAirActive = false;

    // Air filter coefficients
    double mAirShelfB0 = 0, mAirShelfB1 = 0, mAirShelfB2 = 0, mAirShelfA1 = 0, mAirShelfA2 = 0;
    double mAirPeakB0 = 0, mAirPeakB1 = 0, mAirPeakB2 = 0, mAirPeakA1 = 0, mAirPeakA2 = 0;

    // Air filter state (per channel)
    std::array<double, 2> mAirShelfS1 = {}, mAirShelfS2 = {};
    std::array<double, 2> mAirPeakS1 = {}, mAirPeakS2 = {};

    bool mLowCutActive = false;
    bool mHighCutActive = false;
    double mLowCutB0 = 0, mLowCutB1 = 0, mLowCutB2 = 0, mLowCutA1 = 0, mLowCutA2 = 0;
    double mHighCutB0 = 0, mHighCutB1 = 0, mHighCutB2 = 0, mHighCutA1 = 0, mHighCutA2 = 0;
    std::array<double, 2> mLowCutS1 = {}, mLowCutS2 = {};
    std::array<double, 2> mHighCutS1 = {}, mHighCutS2 = {};
  };

  inline void RegisterIRCabEffect()
  {
    EffectTypeInfo info;
    info.type = "cab_ir";
    info.displayName = "IR Cabinet";
    info.category = "cab";
    info.description = "Impulse response cabinet simulation";
    info.requiresResource = true;
    info.resourceType = "ir"; // .wav IR files
    info.parameters = {
        {"mix", "Mix", 1.0, 0.0, 1.0, ""},
        {"irBlend", "IR Blend", 0.0, 0.0, 1.0, ""},
        {"lowCutHz", "Low Cut", 20.0, 20.0, 1000.0, "Hz"},
        {"highCutHz", "High Cut", 20000.0, 1000.0, 20000.0, "Hz"},
        {"slotAGain", "IR A Level", 0.0, -24.0, 24.0, "dB"},
        {"slotBGain", "IR B Level", 0.0, -24.0, 24.0, "dB"},
        {"slotAPolarity", "IR A Invert", 0.0, 0.0, 1.0, "toggle"},
        {"slotBPolarity", "IR B Invert", 0.0, 0.0, 1.0, "toggle"},
        {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
      {"air", "Air", 0.0, 0.0, 1.0, "amount"},
      {"airMode", "Air Mode", 0.0, 0.0, 2.0, "enum"},
        {"quality", "Quality", 1.0, 0.0, 3.0, ""}}; // 0=Economy, 1=Standard, 2=High, 3=Full

    EffectRegistry::Instance().Register("cab_ir", info, []()
                                        { return std::make_unique<IRCabEffect>(); });
  }

} // namespace guitarfx
