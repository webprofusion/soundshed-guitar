#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/IRTypes.h"
#include "dsp/IRWavLoader.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
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

      // CRITICAL: Clear filter state BEFORE updating coefficients to prevent transients
      // when stale filter state is processed with new sample-rate-dependent coefficients.
      // This is essential when sample rate changes via ASIO preferences.
      ResetAirState();
      ResetCabFilterState();
      ResetMicPositionState();

      UpdateAirCoefficients();
      UpdateCabFilterCoefficients();
      UpdateMicCoefficients();

      mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferBL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferBR.resize(static_cast<size_t>(maxBlockSize));
      mPrevOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mPrevOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mPrevOutputBufferBL.resize(static_cast<size_t>(maxBlockSize));
      mPrevOutputBufferBR.resize(static_cast<size_t>(maxBlockSize));

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
      mPrevConvolverL.Reset();
      mPrevConvolverR.Reset();
      mPrevConvolverBL.Reset();
      mPrevConvolverBR.Reset();
      ResetAirState();
      ResetCabFilterState();
      ResetMicPositionState();
      mResourceTransitionSamplesRemaining = 0;
      mPrevHasSlotA = false;
      mPrevHasSlotB = false;
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
      const bool convolverReady = mConvolverL.IsInitialized() && rightReady;

      // Full bypass (disabled or mix=0): pass dry signal through unchanged.
      if (!mEnabled || mMix <= 0.0)
      {
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

      // No IR loaded: pass the dry signal through unchanged (full bypass).
      // The effect is intentionally transparent when no cabinet IR has been selected.
      if (!convolverReady)
      {
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

      // Copy to float buffers
      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0f;
        mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0f);
      }

      // Process through convolvers
      const bool allowParallel = rtparallel::ShouldParallelizeStereoWork(numSamples);
      const bool hasB = mConvolverBL.IsInitialized() && (!mIsStereoB || mConvolverBR.IsInitialized());
      // L/R split: slot A takes left input, slot B takes right input (requires both slots loaded).
      const bool useLRSplit = mLRSplitEnabled && hasB;

      if (useLRSplit)
      {
        // Run slot A (left) and slot B (right) in parallel
        bool ran = false;
        if (allowParallel)
          ran = rtparallel::DualLaneExecutor::Instance().Run(
            [&]() { mConvolverBL.Process(mInputBufferR.data(), mOutputBufferBL.data(), numSamples); },
            [&]() { mConvolverL.Process(mInputBufferL.data(), mOutputBufferL.data(), numSamples); });
        if (!ran)
        {
          mConvolverL.Process(mInputBufferL.data(), mOutputBufferL.data(), numSamples);
          mConvolverBL.Process(mInputBufferR.data(), mOutputBufferBL.data(), numSamples);
        }
      }
      else
      {
        bool ranParallel = false;
        if (allowParallel)
        {
          ranParallel = rtparallel::DualLaneExecutor::Instance().Run(
            [&]() { mConvolverR.Process(mInputBufferR.data(), mOutputBufferR.data(), numSamples); },
            [&]() { mConvolverL.Process(mInputBufferL.data(), mOutputBufferL.data(), numSamples); });
        }
        if (!ranParallel)
        {
          mConvolverL.Process(mInputBufferL.data(), mOutputBufferL.data(), numSamples);
          mConvolverR.Process(mInputBufferR.data(), mOutputBufferR.data(), numSamples);
        }

        if (hasB)
        {
          bool ranBParallel = false;
          if (allowParallel)
          {
            ranBParallel = rtparallel::DualLaneExecutor::Instance().Run(
              [&]() { mConvolverBR.Process(mInputBufferR.data(), mOutputBufferBR.data(), numSamples); },
              [&]() { mConvolverBL.Process(mInputBufferL.data(), mOutputBufferBL.data(), numSamples); });
          }
          if (!ranBParallel)
          {
            mConvolverBL.Process(mInputBufferL.data(), mOutputBufferBL.data(), numSamples);
            mConvolverBR.Process(mInputBufferR.data(), mOutputBufferBR.data(), numSamples);
          }
        }
      }

      const bool transitionActive = mResourceTransitionSamplesRemaining > 0 && mPrevHasSlotA;
      if (transitionActive)
      {
        if (useLRSplit)
        {
          // Mirror the live split routing for the fade-out of the previous state.
          bool ran = false;
          if (allowParallel)
            ran = rtparallel::DualLaneExecutor::Instance().Run(
              [&]() { if (mPrevHasSlotB) mPrevConvolverBL.Process(mInputBufferR.data(), mPrevOutputBufferBL.data(), numSamples); },
              [&]() { mPrevConvolverL.Process(mInputBufferL.data(), mPrevOutputBufferL.data(), numSamples); });
          if (!ran)
          {
            mPrevConvolverL.Process(mInputBufferL.data(), mPrevOutputBufferL.data(), numSamples);
            if (mPrevHasSlotB)
              mPrevConvolverBL.Process(mInputBufferR.data(), mPrevOutputBufferBL.data(), numSamples);
          }
        }
        else
        {
          bool ranPrevParallel = false;
          if (allowParallel)
          {
            ranPrevParallel = rtparallel::DualLaneExecutor::Instance().Run(
              [&]() { mPrevConvolverR.Process(mInputBufferR.data(), mPrevOutputBufferR.data(), numSamples); },
              [&]() { mPrevConvolverL.Process(mInputBufferL.data(), mPrevOutputBufferL.data(), numSamples); });
          }
          if (!ranPrevParallel)
          {
            mPrevConvolverL.Process(mInputBufferL.data(), mPrevOutputBufferL.data(), numSamples);
            mPrevConvolverR.Process(mInputBufferR.data(), mPrevOutputBufferR.data(), numSamples);
          }

          if (mPrevHasSlotB)
          {
            bool ranPrevBParallel = false;
            if (allowParallel)
            {
              ranPrevBParallel = rtparallel::DualLaneExecutor::Instance().Run(
                [&]() { mPrevConvolverBR.Process(mInputBufferR.data(), mPrevOutputBufferBR.data(), numSamples); },
                [&]() { mPrevConvolverBL.Process(mInputBufferL.data(), mPrevOutputBufferBL.data(), numSamples); });
            }
            if (!ranPrevBParallel)
            {
              mPrevConvolverBL.Process(mInputBufferL.data(), mPrevOutputBufferBL.data(), numSamples);
              mPrevConvolverBR.Process(mInputBufferR.data(), mPrevOutputBufferBR.data(), numSamples);
            }
          }
        }
      }

      // Apply wet/dry mix and output gain
      const float wetGain = static_cast<float>(mMix * mOutputGain);
      const float dryGain = static_cast<float>(1.0 - mMix);
      const bool wetOnly = mMix >= 0.9999;

      // In L/R split mode each slot handles one channel at full gain; normal mode uses blend.
      double blend, slotAGain, slotBGain, autoCompGain;
      if (useLRSplit)
      {
        blend        = 0.0;
        slotAGain    = mSlotAGain * (mSlotAPolarityInverted ? -1.0 : 1.0);
        slotBGain    = mSlotBGain * (mSlotBPolarityInverted ? -1.0 : 1.0);
        autoCompGain = 1.0;
      }
      else
      {
        blend        = hasB ? std::clamp(mIRBlend, 0.0, 1.0) : 0.0;
        slotAGain    = (1.0 - blend) * mSlotAGain * (mSlotAPolarityInverted ? -1.0 : 1.0);
        slotBGain    = blend * mSlotBGain * (mSlotBPolarityInverted ? -1.0 : 1.0);
        autoCompGain = ComputeBlendCompGain(hasB, blend);
      }

      // Equal-power pan gains (sqrt(2)-normalised so centre pan = unity gain).
      // hasPan* avoids mono-sum overhead when pan is at default centre.
      static constexpr double kSqrt2 = 1.41421356237309504880;
      const bool hasPanA = std::abs(mSlotAPan) > 1e-6;
      double slotAPanL = 1.0, slotAPanR = 1.0;
      if (hasPanA)
      {
        const double angle = (mSlotAPan + 1.0) * kPi / 4.0;
        slotAPanL = kSqrt2 * std::cos(angle);
        slotAPanR = kSqrt2 * std::sin(angle);
      }
      const bool hasPanB = hasB && std::abs(mSlotBPan) > 1e-6;
      double slotBPanL = 1.0, slotBPanR = 1.0;
      if (hasPanB)
      {
        const double angle = (mSlotBPan + 1.0) * kPi / 4.0;
        slotBPanL = kSqrt2 * std::cos(angle);
        slotBPanR = kSqrt2 * std::sin(angle);
      }

      for (int i = 0; i < numSamples; ++i)
      {
        double wetL, wetR;
        if (useLRSplit)
        {
          // Slot A → left channel; slot B → right channel.
          wetL = mOutputBufferL[i] * slotAGain;
          wetR = mOutputBufferBL[i] * slotBGain;
          if (mMicEmulationEnabled)
          {
            wetL = ProcessMicPositionSlotA(wetL, 0);
            wetR = ProcessMicPositionSlotB(wetR, 1);
          }
        }
        else
        {
          double slotAL = mOutputBufferL[i] * slotAGain;
          double slotAR = mOutputBufferR[i] * slotAGain;
          if (mMicEmulationEnabled)
          {
            slotAL = ProcessMicPositionSlotA(slotAL, 0);
            slotAR = ProcessMicPositionSlotA(slotAR, 1);
          }
          if (hasPanA)
          {
            const double monoA = (slotAL + slotAR) * 0.5;
            slotAL = monoA * slotAPanL;
            slotAR = monoA * slotAPanR;
          }
          wetL = slotAL;
          wetR = slotAR;

          if (hasB)
          {
            double slotBL = mOutputBufferBL[i] * slotBGain;
            double slotBR = mOutputBufferBR[i] * slotBGain;
            if (mMicEmulationEnabled)
            {
              slotBL = ProcessMicPositionSlotB(slotBL, 0);
              slotBR = ProcessMicPositionSlotB(slotBR, 1);
            }
            if (hasPanB)
            {
              const double monoB = (slotBL + slotBR) * 0.5;
              slotBL = monoB * slotBPanL;
              slotBR = monoB * slotBPanR;
            }
            wetL += slotBL;
            wetR += slotBR;
          }
        }

        if (transitionActive)
        {
          const int progressed = mResourceTransitionSamplesTotal - mResourceTransitionSamplesRemaining;
          const double newWeight = static_cast<double>(progressed) / static_cast<double>(std::max(1, mResourceTransitionSamplesTotal));
          const double oldWeight = 1.0 - std::clamp(newWeight, 0.0, 1.0);

          double prevWetL, prevWetR;
          if (useLRSplit)
          {
            prevWetL = mPrevOutputBufferL[i] * slotAGain;
            prevWetR = mPrevHasSlotB ? mPrevOutputBufferBL[i] * slotBGain : 0.0;
          }
          else
          {
            prevWetL = mPrevOutputBufferL[i] * slotAGain;
            prevWetR = mPrevOutputBufferR[i] * slotAGain;
            if (mPrevHasSlotB)
            {
              prevWetL += mPrevOutputBufferBL[i] * slotBGain;
              prevWetR += mPrevOutputBufferBR[i] * slotBGain;
            }
          }

          wetL = prevWetL * oldWeight + wetL * (1.0 - oldWeight);
          wetR = prevWetR * oldWeight + wetR * (1.0 - oldWeight);

          mResourceTransitionSamplesRemaining -= 1;
          if (mResourceTransitionSamplesRemaining <= 0)
          {
            mPrevHasSlotA = false;
            mPrevHasSlotB = false;
            mPrevConvolverL.Reset();
            mPrevConvolverR.Reset();
            mPrevConvolverBL.Reset();
            mPrevConvolverBR.Reset();
          }
        }

        wetL *= autoCompGain;
        wetR *= autoCompGain;

        wetL = ProcessCabFilters(wetL, 0);
        wetR = ProcessCabFilters(wetR, 1);

        if (mAirActive)
        {
          wetL = ProcessAirSample(wetL, 0);
          wetR = ProcessAirSample(wetR, 1);
        }

        if (outputs[0])
        {
          if (wetOnly)
            outputs[0][i] = static_cast<float>(wetL) * wetGain;
          else
          {
            const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
            outputs[0][i] = static_cast<float>(wetL) * wetGain + dryL * dryGain;
          }
        }
        if (outputs[1])
        {
          if (wetOnly)
            outputs[1][i] = static_cast<float>(wetR) * wetGain;
          else
          {
            const float dryR = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0f);
            outputs[1][i] = static_cast<float>(wetR) * wetGain + dryR * dryGain;
          }
        }
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
      else if (key == "slotAPan")
        mSlotAPan = std::clamp(value, -1.0, 1.0);
      else if (key == "slotBPan")
        mSlotBPan = std::clamp(value, -1.0, 1.0);
      else if (key == "lrSplit")
        mLRSplitEnabled = value > 0.5;
      else if (key == "slotAPolarity")
        mSlotAPolarityInverted = value > 0.5;
      else if (key == "slotBPolarity")
        mSlotBPolarityInverted = value > 0.5;
      else if (key == "autoGainComp")
        mAutoGainCompEnabled = value > 0.5;
      else if (key == "normalizeIR")
        mNormalizeIR = value > 0.5;
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
      else if (key == "micEmulation")
        mMicEmulationEnabled = value > 0.5;
      else if (key == "micRadialA")
      {
        mMicRadialA = std::clamp(value, 0.0, 1.0);
        UpdateMicCoefficients();
      }
      else if (key == "micProximityA")
      {
        mMicProximityA = std::clamp(value, 0.0, 1.0);
        UpdateMicCoefficients();
      }
      else if (key == "micRadialB")
      {
        mMicRadialB = std::clamp(value, 0.0, 1.0);
        UpdateMicCoefficients();
      }
      else if (key == "micProximityB")
      {
        mMicProximityB = std::clamp(value, 0.0, 1.0);
        UpdateMicCoefficients();
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
      if (key == "slotAPan")
        return mSlotAPan;
      if (key == "slotBPan")
        return mSlotBPan;
      if (key == "lrSplit")
        return mLRSplitEnabled ? 1.0 : 0.0;
      if (key == "slotAPolarity")
        return mSlotAPolarityInverted ? 1.0 : 0.0;
      if (key == "slotBPolarity")
        return mSlotBPolarityInverted ? 1.0 : 0.0;
      if (key == "autoGainComp")
        return mAutoGainCompEnabled ? 1.0 : 0.0;
      if (key == "normalizeIR")
        return mNormalizeIR ? 1.0 : 0.0;
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
      if (key == "micEmulation")  return mMicEmulationEnabled ? 1.0 : 0.0;
      if (key == "micRadialA")    return mMicRadialA;
      if (key == "micProximityA") return mMicProximityA;
      if (key == "micRadialB")    return mMicRadialB;
      if (key == "micProximityB") return mMicProximityB;
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      if (!std::filesystem::exists(resourcePath))
      {
        std::cerr << "[IRCabEffect] ERROR: IR file not found: " << resourcePath << "\n";
        return false;
      }

      CapturePreviousConvolvers();

      // Single-resource load path (legacy): load slot A and clear slot B.
      if (!LoadWavFile(resourcePath))
      {
        std::cerr << "[IRCabEffect] ERROR: Failed to load/parse IR WAV file: " << resourcePath << "\n";
        return false;
      }

      mIRPath = resourcePath;
      mImpulseBL.clear();
      mImpulseBR.clear();
      mIRPathB.clear();
      mConvolverBL.Reset();
      mConvolverBR.Reset();
      ApplyPendingQuality();
      const bool loaded = InitializeConvolverA();
      if (!loaded)
      {
        std::cerr << "[IRCabEffect] ERROR: Failed to initialize convolver for: " << resourcePath << "\n";
        return false;
      }

      BeginResourceTransition();
      return true;
    }

    bool LoadResources(const std::vector<ResourceRef> &refs,
                       const std::vector<std::filesystem::path> &paths) override
    {
      // Determine which path goes to slot A (index 0) and slot B (index 1).
      // The executor stamps each ref with a "resourceSlotIndex" metadata key so we
      // can route correctly even when slot A is empty but slot B is not.
      // Fall back to positional order for legacy presets that lack the metadata.
      int slotAIdx = -1;
      int slotBIdx = -1;
      bool hasSlotMetadata = false;
      for (std::size_t i = 0; i < refs.size() && i < paths.size(); ++i)
      {
        const auto it = refs[i].metadata.find("resourceSlotIndex");
        if (it != refs[i].metadata.end())
        {
          hasSlotMetadata = true;
          const int slotIndex = std::stoi(it->second);
          if (slotIndex == 0) slotAIdx = static_cast<int>(i);
          else if (slotIndex == 1) slotBIdx = static_cast<int>(i);
        }
      }
      if (!hasSlotMetadata)
      {
        if (!paths.empty()) slotAIdx = 0;
        if (paths.size() >= 2) slotBIdx = 1;
      }

      CapturePreviousConvolvers();

      // --- Slot A ---
      bool loadedA = false;
      if (slotAIdx >= 0)
      {
        if (!std::filesystem::exists(paths[slotAIdx]))
          std::cerr << "[IRCabEffect] ERROR: IR A file not found: " << paths[slotAIdx] << "\n";
        else if (LoadWavFile(paths[slotAIdx]))
        {
          mIRPath = paths[slotAIdx];
          ApplyPendingQuality();
          loadedA = InitializeConvolverA();
          if (!loadedA)
            std::cerr << "[IRCabEffect] ERROR: Failed to initialize convolver A for: " << paths[slotAIdx] << "\n";
        }
        else
          std::cerr << "[IRCabEffect] ERROR: Failed to load IR A: " << paths[slotAIdx] << "\n";
      }
      if (!loadedA)
      {
        mImpulseL.clear();
        mImpulseR.clear();
        mIRPath.clear();
        mConvolverL.Reset();
        mConvolverR.Reset();
      }

      // --- Slot B ---
      mImpulseBL.clear();
      mImpulseBR.clear();
      mIRPathB.clear();
      mConvolverBL.Reset();
      mConvolverBR.Reset();
      bool loadedB = false;
      if (slotBIdx >= 0)
      {
        if (!std::filesystem::exists(paths[slotBIdx]))
          std::cerr << "[IRCabEffect] WARNING: IR B file not found: " << paths[slotBIdx] << "\n";
        else if (LoadWavFileInto(paths[slotBIdx], mImpulseBL, mImpulseBR, mIRSampleRateB, mIsStereoB))
        {
          mIRPathB = paths[slotBIdx];
          if (slotBIdx < static_cast<int>(refs.size()) && refs[slotBIdx].parameterValue.has_value())
            mIRBlend = std::clamp(*refs[slotBIdx].parameterValue, 0.0, 1.0);
          loadedB = InitializeConvolverB();
          if (!loadedB)
            std::cerr << "[IRCabEffect] WARNING: Failed to initialize convolver B for: " << paths[slotBIdx] << "\n";
        }
        else
          std::cerr << "[IRCabEffect] WARNING: Failed to load IR B: " << paths[slotBIdx] << "\n";
      }

      BeginResourceTransition();
      return loadedA || loadedB;
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
      const bool slotAReady = mConvolverL.IsInitialized() && (!mIsStereo || mConvolverR.IsInitialized());
      const bool slotBReady = mConvolverBL.IsInitialized();
      return slotAReady || slotBReady;
    }
    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mIRPath; }

    [[nodiscard]] int GetLatencySamples() const override
    {
      // Report the convolver's algorithmic latency (partition size for FFT convolution, 0 for direct)
      return mConvolverL.IsInitialized() ? mConvolverL.GetLatency() : 0;
    }

    [[nodiscard]] std::string GetType() const override { return "cab_ir"; }
    [[nodiscard]] std::string GetCategory() const override { return "cab"; }

    [[nodiscard]] bool ProducesStereoOutput() const override
    {
      // Pan on either slot or L/R split mode produces distinct L and R output.
      return std::abs(mSlotAPan) > 1e-6 || std::abs(mSlotBPan) > 1e-6
          || (mLRSplitEnabled && mConvolverBL.IsInitialized());
    }

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
    std::vector<float> GetProcessedImpulse(const std::vector<float> &samples, double impulseSampleRate) const
    {
      if (samples.empty())
        return {};

      // For Full quality, return the complete IR
      if (mQuality == IRQuality::Full)
        return samples;

      const double qualitySampleRate = impulseSampleRate > 0.0 ? impulseSampleRate : mSampleRate;
      const size_t maxSamples = GetMaxIRSamples(mQuality, qualitySampleRate);
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

      const size_t maxSamples = GetMaxIRSamples(mQuality, mIRSampleRate);
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

    [[nodiscard]] static double Sinc(double value)
    {
      if (std::abs(value) < 1.0e-10)
        return 1.0;

      const double angle = kPi * value;
      return std::sin(angle) / angle;
    }

    [[nodiscard]] static double BlackmanWindow(double distance)
    {
      constexpr int kHalfTaps = 64;
      const double normalizedDistance = std::abs(distance) / static_cast<double>(kHalfTaps);
      if (normalizedDistance >= 1.0)
        return 0.0;

      return 0.42 + 0.5 * std::cos(kPi * normalizedDistance)
        + 0.08 * std::cos(2.0 * kPi * normalizedDistance);
    }

    static void ResampleImpulseForConvolution(std::vector<float> &samples, double sourceRate, double targetRate)
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
            continue;

          const double distance = sourcePosition - static_cast<double>(sourceIndex);
          const double tap = cutoff * Sinc(distance * cutoff) * BlackmanWindow(distance);
          sum += static_cast<double>(samples[static_cast<std::size_t>(sourceIndex)]) * tap;
        }

        resampled[outputIndex] = static_cast<float>(sum * coefficientScale);
      }

      samples = std::move(resampled);
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

    bool InitializeConvolverFromImpulse(const std::vector<float> &impulseL,
                                        const std::vector<float> &impulseR,
                                        bool isStereo,
                                        double impulseSampleRate,
                                        RealtimeConvolver &convolverL,
                                        RealtimeConvolver &convolverR)
    {
      if (impulseL.empty() || mMaxBlockSize == 0)
        return false;

      if (isStereo && !impulseR.empty())
      {
        const double sourceRate = impulseSampleRate > 0.0 ? impulseSampleRate : mSampleRate;
        std::vector<float> processedL = GetProcessedImpulse(impulseL, sourceRate);
        std::vector<float> processedR = GetProcessedImpulse(impulseR, sourceRate);

        if (processedL.empty() || processedR.empty())
          return false;

        const size_t length = std::min(processedL.size(), processedR.size());
        processedL.resize(length);
        processedR.resize(length);

        if (std::abs(sourceRate - mSampleRate) > 1.0)
        {
          // Cabinet IR samples are FIR coefficients, so resampling needs to
          // preserve coefficient area rather than audio-waveform peak level.
          ResampleImpulseForConvolution(processedL, sourceRate, mSampleRate);
          ResampleImpulseForConvolution(processedR, sourceRate, mSampleRate);
        }

        if (mNormalizeIR)
        {
          // Scale IR samples so convolution preserves signal energy (unity-gain normalization).
          // Uses combined L+R L2 norm so stereo and mono IRs normalize consistently.
          const float normGain = ComputeL2NormGain(processedL, processedR);
          for (auto &s : processedL) s *= normGain;
          for (auto &s : processedR) s *= normGain;
        }

        if (!convolverL.SetImpulse(processedL, mMaxBlockSize) ||
            !convolverR.SetImpulse(processedR, mMaxBlockSize))
        {
          convolverL.Reset();
          convolverR.Reset();
          return false;
        }

        return true;
      }

      const double sourceRate = impulseSampleRate > 0.0 ? impulseSampleRate : mSampleRate;
      std::vector<float> processedIR = GetProcessedImpulse(impulseL, sourceRate);
      if (processedIR.empty())
        return false;

      if (std::abs(sourceRate - mSampleRate) > 1.0)
      {
        // Cabinet IR samples are FIR coefficients, so resampling needs to
        // preserve coefficient area rather than audio-waveform peak level.
        ResampleImpulseForConvolution(processedIR, sourceRate, mSampleRate);
      }

      if (mNormalizeIR)
      {
        // Scale IR samples so convolution preserves signal energy (unity-gain normalization).
        const float normGain = ComputeL2NormGain(processedIR);
        for (auto &s : processedIR) s *= normGain;
      }

      if (!convolverL.SetImpulse(processedIR, mMaxBlockSize) ||
          !convolverR.SetImpulse(processedIR, mMaxBlockSize))
      {
        convolverL.Reset();
        convolverR.Reset();
        return false;
      }

      return true;
    }

    void CapturePreviousConvolvers()
    {
      mPrevHasSlotA = false;
      mPrevHasSlotB = false;
      mPrevConvolverL.Reset();
      mPrevConvolverR.Reset();
      mPrevConvolverBL.Reset();
      mPrevConvolverBR.Reset();

      if (HasResource())
      {
        mPrevHasSlotA = InitializeConvolverFromImpulse(
            mImpulseL,
            mImpulseR,
            mIsStereo,
            mIRSampleRate,
            mPrevConvolverL,
            mPrevConvolverR);
      }

      if (!mImpulseBL.empty() && mConvolverBL.IsInitialized())
      {
        mPrevHasSlotB = InitializeConvolverFromImpulse(
            mImpulseBL,
            mImpulseBR,
            mIsStereoB,
            mIRSampleRateB,
            mPrevConvolverBL,
            mPrevConvolverBR);
      }
    }

    bool InitializeConvolverA()
    {
      return InitializeConvolverFromImpulse(
          mImpulseL,
          mImpulseR,
          mIsStereo,
          mIRSampleRate,
          mConvolverL,
          mConvolverR);
    }

    bool InitializeConvolverB()
    {
      return InitializeConvolverFromImpulse(
          mImpulseBL,
          mImpulseBR,
          mIsStereoB,
          mIRSampleRateB,
          mConvolverBL,
          mConvolverBR);
    }

    // Returns the gain to apply to IR samples so that convolution is unity-gain for a broadband
    // signal. Computed as 1/||h||_2 where ||h||_2 = sqrt(sum(h[n]^2)).
    // For stereo, the average L+R L2 norm is used so stereo and mono IRs normalise equivalently.
    static float ComputeL2NormGain(const std::vector<float> &samplesL,
                                   const std::vector<float> &samplesR = {})
    {
      double sumSq = 0.0;
      for (const float s : samplesL)
        sumSq += static_cast<double>(s) * s;
      if (!samplesR.empty())
      {
        for (const float s : samplesR)
          sumSq += static_cast<double>(s) * s;
        sumSq *= 0.5; // average channels so stereo == mono for the same impulse response
      }
      return sumSq > 1e-12 ? static_cast<float>(1.0 / std::sqrt(sumSq)) : 1.0f;
    }

    static double ComputeSignalEnergy(const std::vector<float> &samples)
    {
      if (samples.empty())
        return 1.0;

      double sum = 0.0;
      for (const float sample : samples)
      {
        const double value = static_cast<double>(sample);
        sum += value * value;
      }

      const double mean = sum / static_cast<double>(samples.size());
      return std::max(1e-8, mean);
    }

    static double ComputeSignalEnergyStereo(const std::vector<float> &left,
                                            const std::vector<float> &right)
    {
      const size_t count = std::min(left.size(), right.size());
      if (count == 0)
      {
        return ComputeSignalEnergy(left.empty() ? right : left);
      }

      double sum = 0.0;
      for (size_t i = 0; i < count; ++i)
      {
        const double l = static_cast<double>(left[i]);
        const double r = static_cast<double>(right[i]);
        sum += (l * l + r * r) * 0.5;
      }

      const double mean = sum / static_cast<double>(count);
      return std::max(1e-8, mean);
    }

    double ComputeBlendCompGain(bool hasB, double blend) const
    {
      if (!mAutoGainCompEnabled)
        return 1.0;

      // With L2-normalised IRs every slot has unit energy in the convolver.
      // The blend position (irBlend) linearly interpolates between A and B, so
      // the combined energy is blendA² + blendB².  Normalising by its sqrt keeps
      // output level constant as the user sweeps the blend knob.
      const double blendA = 1.0 - blend;
      const double blendB = blend;
      const double energy = blendA * blendA + (hasB ? blendB * blendB : 0.0);
      return 1.0 / std::sqrt(std::max(1e-8, energy));
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
      const double shelfGainDb = clamped * 12.0; // 0..+12 dB
      const double shelfFreq = 7000.0;
      const double shelfSlope = 0.7;
      ComputeHighShelf(shelfFreq, shelfSlope, shelfGainDb, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2);

      // Option B (optional): high-mid presence peak.
      const double peakGainDb = clamped * 8.0; // 0..+8 dB
      const double peakFreq = 3600.0;
      const double peakQ = 1.8;
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
      mHighCutActive = (mHighCutHz < 19999.5) && (mHighCutHz < (nyquist - 100.0));

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

    void UpdateMicCoefficients()
    {
      if (mSampleRate <= 0.0) return;

      // Slot A — radial: HF shelf (on-axis=0dB → off-axis=-12dB at 4kHz)
      //          proximity: low-mid peaking boost (far=0dB → close=+6dB at 150Hz)
      ComputeHighShelf(4000.0, 0.7, mMicRadialA * -12.0,
                       mMicAShelfB0, mMicAShelfB1, mMicAShelfB2, mMicAShelfA1, mMicAShelfA2);
      ComputePeakingEQ(150.0, 1.0, mMicProximityA * 6.0,
                       mMicAProxB0, mMicAProxB1, mMicAProxB2, mMicAProxA1, mMicAProxA2);

      // Slot B
      ComputeHighShelf(4000.0, 0.7, mMicRadialB * -12.0,
                       mMicBShelfB0, mMicBShelfB1, mMicBShelfB2, mMicBShelfA1, mMicBShelfA2);
      ComputePeakingEQ(150.0, 1.0, mMicProximityB * 6.0,
                       mMicBProxB0, mMicBProxB1, mMicBProxB2, mMicBProxA1, mMicBProxA2);
    }

    void ResetMicPositionState()
    {
      for (int ch = 0; ch < 2; ++ch)
      {
        mMicAShelfS1[ch] = mMicAShelfS2[ch] = 0.0;
        mMicAProxS1[ch]  = mMicAProxS2[ch]  = 0.0;
        mMicBShelfS1[ch] = mMicBShelfS2[ch] = 0.0;
        mMicBProxS1[ch]  = mMicBProxS2[ch]  = 0.0;
      }
    }

    double ProcessMicPositionSlotA(double input, int channel)
    {
      const double shelf = ProcessBiquad(input,
          mMicAShelfB0, mMicAShelfB1, mMicAShelfB2, mMicAShelfA1, mMicAShelfA2,
          mMicAShelfS1[channel], mMicAShelfS2[channel]);
      return ProcessBiquad(shelf,
          mMicAProxB0, mMicAProxB1, mMicAProxB2, mMicAProxA1, mMicAProxA2,
          mMicAProxS1[channel], mMicAProxS2[channel]);
    }

    double ProcessMicPositionSlotB(double input, int channel)
    {
      const double shelf = ProcessBiquad(input,
          mMicBShelfB0, mMicBShelfB1, mMicBShelfB2, mMicBShelfA1, mMicBShelfA2,
          mMicBShelfS1[channel], mMicBShelfS2[channel]);
      return ProcessBiquad(shelf,
          mMicBProxB0, mMicBProxB1, mMicBProxB2, mMicBProxA1, mMicBProxA2,
          mMicBProxS1[channel], mMicBProxS2[channel]);
    }

    RealtimeConvolver mConvolverL;
    RealtimeConvolver mConvolverR;
    RealtimeConvolver mConvolverBL;
    RealtimeConvolver mConvolverBR;
    RealtimeConvolver mPrevConvolverL;
    RealtimeConvolver mPrevConvolverR;
    RealtimeConvolver mPrevConvolverBL;
    RealtimeConvolver mPrevConvolverBR;

    std::vector<float> mImpulseL;  // Original IR samples (left)
    std::vector<float> mImpulseR;  // Original IR samples (right, optional)
    std::vector<float> mImpulseBL; // Optional second IR samples (left)
    std::vector<float> mImpulseBR; // Optional second IR samples (right)
    std::filesystem::path mIRPath;
    std::filesystem::path mIRPathB;
    double mIRSampleRate = 48000.0;
    double mIRSampleRateB = 48000.0;
    bool mIsStereo = false;
    bool mIsStereoB = false;

    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mOutputBufferL;
    std::vector<float> mOutputBufferR;
    std::vector<float> mOutputBufferBL;
    std::vector<float> mOutputBufferBR;
    std::vector<float> mPrevOutputBufferL;
    std::vector<float> mPrevOutputBufferR;
    std::vector<float> mPrevOutputBufferBL;
    std::vector<float> mPrevOutputBufferBR;

    double mMix = 1.0;
    double mIRBlend = 0.0;
    double mLowCutHz = 20.0;
    double mHighCutHz = 20000.0;
    double mSlotAGain = 1.0;
    double mSlotBGain = 1.0;
    double mSlotAPan = 0.0; // -1.0 = full left, 0.0 = centre, 1.0 = full right
    double mSlotBPan = 0.0;
    bool mLRSplitEnabled = false; // when true, stereo input is split: L→IR A, R→IR B
    bool mSlotAPolarityInverted = false;
    bool mSlotBPolarityInverted = false;
    double mOutputGain = 1.0;
    bool mEnabled = true;
    IRQuality mQuality = IRQuality::Standard;
    std::atomic<int> mPendingQuality{-1};
    bool mNormalizeIR = true;
    bool mAutoGainCompEnabled = true;
    bool mHasLoadedResource = false;
    bool mPrevHasSlotA = false;
    bool mPrevHasSlotB = false;
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

    // Mic position emulation
    bool mMicEmulationEnabled = false;
    double mMicRadialA    = 0.0; // 0=on-axis/center, 1=off-axis/edge
    double mMicProximityA = 0.0; // 0=far/neutral,    1=close-mic
    double mMicRadialB    = 0.0;
    double mMicProximityB = 0.0;

    double mMicAShelfB0 = 1, mMicAShelfB1 = 0, mMicAShelfB2 = 0, mMicAShelfA1 = 0, mMicAShelfA2 = 0;
    double mMicAProxB0  = 1, mMicAProxB1  = 0, mMicAProxB2  = 0, mMicAProxA1  = 0, mMicAProxA2  = 0;
    double mMicBShelfB0 = 1, mMicBShelfB1 = 0, mMicBShelfB2 = 0, mMicBShelfA1 = 0, mMicBShelfA2 = 0;
    double mMicBProxB0  = 1, mMicBProxB1  = 0, mMicBProxB2  = 0, mMicBProxA1  = 0, mMicBProxA2  = 0;

    std::array<double, 2> mMicAShelfS1 = {}, mMicAShelfS2 = {};
    std::array<double, 2> mMicAProxS1  = {}, mMicAProxS2  = {};
    std::array<double, 2> mMicBShelfS1 = {}, mMicBShelfS2 = {};
    std::array<double, 2> mMicBProxS1  = {}, mMicBProxS2  = {};
  };

  inline void RegisterIRCabEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kCabIr;
    info.aliases = {"cab_ir"};
    info.displayName = "IR Cabinet";
    info.category = "cab";
    info.description = "Impulse response cabinet simulation";
    info.requiresResource = true;
    info.resourceType = "ir"; // .wav IR files — retained for legacy resource resolution and file-browse routing

    // Declare two explicit resource slots so the UI renders separate, labelled
    // pickers for IR A and IR B rather than a single anonymous IR picker.
    // nodeId is empty because this is not a composite effect (the slots live on
    // this node itself).  resourceIndex 0/1 maps to node.resources[0/1] and to
    // the "resourceSlotIndex" metadata the executor stamps on each resolved ref.
    {
      ExposedResource slotA;
      slotA.resourceId    = "irA";
      slotA.displayName   = "IR A";
      slotA.nodeId        = "";
      slotA.resourceType  = "ir";
      slotA.resourceIndex = 0;
      slotA.allowBrowseFile = true;

      ExposedResource slotB;
      slotB.resourceId    = "irB";
      slotB.displayName   = "IR B";
      slotB.nodeId        = "";
      slotB.resourceType  = "ir";
      slotB.resourceIndex = 1;
      slotB.allowBrowseFile = true;

      info.exposedResources = {slotA, slotB};
    }

    info.parameters = {
        {"mix",           "Mix",            1.0,  0.0,   1.0,     "amount", "Level"},
        {"irBlend",       "IR Blend",       0.0,  0.0,   1.0,     "blend", "Tone"},
        {"outputGain",    "Output",         0.0,  -24.0, 24.0,    "dB",     "Level"},
        {"lowCutHz",      "Low Cut",        20.0, 20.0,  1000.0,  "Hz",     "Tone"},
        {"highCutHz",     "High Cut",    20000.0, 1000.0,20000.0, "Hz",     "Tone"},
        {"air",           "Air",            0.0,  0.0,   1.0,     "amount", "Tone"},
        {"airMode",       "Air Mode",       0.0,  0.0,   2.0,     "enum",   "Tone",  true, 1.0, {"Shelf", "Presence", "Both"}},
        {"micEmulation",  "Mic Emulation",  0.0,  0.0,   1.0,     "toggle", "Tone",      true},
        {"lrSplit",       "L/R Split",      0.0,  0.0,   1.0,     "toggle", "IR A",  true},
        {"slotAGain",     "IR A Level",     0.0,  -24.0, 24.0,    "dB",     "IR A",  true},
        {"slotAPan",      "IR A Pan",       0.0,  -1.0,  1.0,     "amount", "IR A",  true},
        {"slotAPolarity", "IR A Invert",    0.0,  0.0,   1.0,     "toggle", "IR A",  true},
        {"micRadialA",    "Mic A Radial",   0.0,  0.0,   1.0,     "amount", "IR A",  true},
        {"micProximityA", "Mic A Proximity",0.0,  0.0,   1.0,     "amount", "IR A",  true},
        {"slotBPolarity", "IR B Invert",    0.0,  0.0,   1.0,     "toggle", "IR B",  true},
        {"slotBGain",     "IR B Level",     0.0,  -24.0, 24.0,    "dB",     "IR B",  true},
        {"slotBPan",      "IR B Pan",       0.0,  -1.0,  1.0,     "amount", "IR B",  true},
        {"micRadialB",    "Mic B Radial",   0.0,  0.0,   1.0,     "amount", "IR B",  true},
        {"micProximityB", "Mic B Proximity",0.0,  0.0,   1.0,     "amount", "IR B",  true},
        {"quality",       "Quality",        1.0,  0.0,   3.0,     "enum",    "Tone",      true, 1.0, {"Economy","Standard","High","Full"}}}; // 0=Economy, 1=Standard, 2=High, 3=Full

    EffectRegistry::Instance().Register(info.type, info, []()
                                        { return std::make_unique<IRCabEffect>(); });
  }

} // namespace guitarfx
