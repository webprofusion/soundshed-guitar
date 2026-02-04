#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Mixer Effect - Combines multiple inputs with per-input level, pan, and delay.
   * 
   * This effect is designed to work with the signal graph's mixer node type.
   * Each input port (0-3) can have independent:
   *   - Level (gain in dB)
   *   - Pan (stereo position, -1 to +1)
   *   - Delay (in milliseconds, for time-alignment or effects)
   * 
   * The actual input accumulation is handled by SignalGraphExecutor, but this
   * effect processes per-input parameters when configured via the node's params.
   * 
   * NOTE: In the current architecture, the executor accumulates inputs before
   * calling Process(). For true per-input processing, the executor would need
   * to call a multi-input process method. This implementation provides the
   * parameter storage and master output control.
   */
  class MixerEffect : public EffectProcessor
  {
  public:
    static constexpr int kMaxInputs = 4;
    static constexpr double kMaxDelayMs = 500.0;

    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Allocate delay buffers for each input
      const size_t maxDelaySamples = static_cast<size_t>(kMaxDelayMs * mSampleRate / 1000.0) + 1;
      for (int i = 0; i < kMaxInputs; ++i)
      {
        mDelayBufferL[i].assign(maxDelaySamples, 0.0f);
        mDelayBufferR[i].assign(maxDelaySamples, 0.0f);
        mDelayWritePos[i] = 0;
      }

      UpdateCoefficients();
    }

    void Reset() override
    {
      for (int i = 0; i < kMaxInputs; ++i)
      {
        std::fill(mDelayBufferL[i].begin(), mDelayBufferL[i].end(), 0.0f);
        std::fill(mDelayBufferR[i].begin(), mDelayBufferR[i].end(), 0.0f);
        mDelayWritePos[i] = 0;
      }
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!inputs || !outputs)
        return;

      // In the current architecture, inputs are pre-accumulated by the executor.
      // We apply master gain here. Per-input processing would require executor changes.
      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : 0.0f;

        // Apply master gain
        inL *= mMasterGain;
        inR *= mMasterGain;

        if (outputs[0])
          outputs[0][i] = inL;
        if (outputs[1])
          outputs[1][i] = inR;
      }
    }

    /**
     * Process a specific input port with its own level, pan, and delay.
     * This is called by the executor for each input edge to a mixer node.
     */
    void ProcessInput(int inputIndex, const float *inputL, const float *inputR,
                      float *outputL, float *outputR, int numSamples)
    {
      if (inputIndex < 0 || inputIndex >= kMaxInputs)
        return;

      const float level = mInputLevel[inputIndex];
      const float panL = mInputPanL[inputIndex];
      const float panR = mInputPanR[inputIndex];
      const size_t delaySamples = mInputDelaySamples[inputIndex];

      auto &delayL = mDelayBufferL[inputIndex];
      auto &delayR = mDelayBufferR[inputIndex];
      size_t &writePos = mDelayWritePos[inputIndex];

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputL ? inputL[i] : 0.0f;
        const float inR = inputR ? inputR[i] : 0.0f;

        // Write to delay buffer
        delayL[writePos] = inL;
        delayR[writePos] = inR;

        // Read from delay buffer
        size_t readPos = (writePos + delayL.size() - delaySamples) % delayL.size();
        float delayedL = delayL[readPos];
        float delayedR = delayR[readPos];

        // Apply level and pan
        // Pan law: constant power (-3dB at center)
        float outL = delayedL * level * panL;
        float outR = delayedR * level * panR;

        // Accumulate to output
        if (outputL)
          outputL[i] += outL;
        if (outputR)
          outputR[i] += outR;

        writePos = (writePos + 1) % delayL.size();
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "masterLevel")
      {
        mMasterLevelDb = std::clamp(value, -60.0, 12.0);
        mMasterGain = static_cast<float>(std::pow(10.0, mMasterLevelDb / 20.0));
      }
      else if (key.rfind("level_", 0) == 0)
      {
        // Per-input level: level_0, level_1, etc.
        int idx = std::stoi(key.substr(6));
        if (idx >= 0 && idx < kMaxInputs)
        {
          mInputLevelDb[idx] = std::clamp(value, -60.0, 12.0);
          UpdateInputCoefficients(idx);
        }
      }
      else if (key.rfind("pan_", 0) == 0)
      {
        // Per-input pan: pan_0, pan_1, etc.
        int idx = std::stoi(key.substr(4));
        if (idx >= 0 && idx < kMaxInputs)
        {
          mInputPan[idx] = std::clamp(value, -1.0, 1.0);
          UpdateInputCoefficients(idx);
        }
      }
      else if (key.rfind("delay_", 0) == 0)
      {
        // Per-input delay: delay_0, delay_1, etc.
        int idx = std::stoi(key.substr(6));
        if (idx >= 0 && idx < kMaxInputs)
        {
          mInputDelayMs[idx] = std::clamp(value, 0.0, kMaxDelayMs);
          UpdateInputCoefficients(idx);
        }
      }
      else if (key.rfind("mute_", 0) == 0)
      {
        // Per-input mute: mute_0, mute_1, etc.
        int idx = std::stoi(key.substr(5));
        if (idx >= 0 && idx < kMaxInputs)
        {
          mInputMute[idx] = value > 0.5;
          UpdateInputCoefficients(idx);
        }
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "masterLevel")
        return mMasterLevelDb;
      if (key.rfind("level_", 0) == 0)
      {
        int idx = std::stoi(key.substr(6));
        if (idx >= 0 && idx < kMaxInputs)
          return mInputLevelDb[idx];
      }
      if (key.rfind("pan_", 0) == 0)
      {
        int idx = std::stoi(key.substr(4));
        if (idx >= 0 && idx < kMaxInputs)
          return mInputPan[idx];
      }
      if (key.rfind("delay_", 0) == 0)
      {
        int idx = std::stoi(key.substr(6));
        if (idx >= 0 && idx < kMaxInputs)
          return mInputDelayMs[idx];
      }
      if (key.rfind("mute_", 0) == 0)
      {
        int idx = std::stoi(key.substr(5));
        if (idx >= 0 && idx < kMaxInputs)
          return mInputMute[idx] ? 1.0 : 0.0;
      }
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "mixer"; }
    [[nodiscard]] std::string GetCategory() const override { return "utility"; }

    // Accessors for SignalGraphExecutor to use per-input processing
    [[nodiscard]] float GetInputLevel(int idx) const
    {
      return (idx >= 0 && idx < kMaxInputs) ? mInputLevel[idx] : 1.0f;
    }
    [[nodiscard]] float GetInputPanL(int idx) const
    {
      return (idx >= 0 && idx < kMaxInputs) ? mInputPanL[idx] : 1.0f;
    }
    [[nodiscard]] float GetInputPanR(int idx) const
    {
      return (idx >= 0 && idx < kMaxInputs) ? mInputPanR[idx] : 1.0f;
    }
    [[nodiscard]] size_t GetInputDelaySamples(int idx) const
    {
      return (idx >= 0 && idx < kMaxInputs) ? mInputDelaySamples[idx] : 0;
    }
    [[nodiscard]] bool IsInputMuted(int idx) const
    {
      return (idx >= 0 && idx < kMaxInputs) ? mInputMute[idx] : false;
    }

  private:
    void UpdateCoefficients()
    {
      mMasterGain = static_cast<float>(std::pow(10.0, mMasterLevelDb / 20.0));
      for (int i = 0; i < kMaxInputs; ++i)
      {
        UpdateInputCoefficients(i);
      }
    }

    void UpdateInputCoefficients(int idx)
    {
      if (idx < 0 || idx >= kMaxInputs)
        return;

      // Level
      double levelDb = mInputMute[idx] ? -100.0 : mInputLevelDb[idx];
      mInputLevel[idx] = static_cast<float>(std::pow(10.0, levelDb / 20.0));

      // Pan using constant power law
      // pan = -1: full left, pan = 0: center, pan = +1: full right
      const double pan = mInputPan[idx];
      const double angle = (pan + 1.0) * 0.25 * 3.14159265358979323846; // 0 to π/2
      mInputPanL[idx] = static_cast<float>(std::cos(angle));
      mInputPanR[idx] = static_cast<float>(std::sin(angle));

      // Delay
      mInputDelaySamples[idx] = static_cast<size_t>(mInputDelayMs[idx] * mSampleRate / 1000.0);
    }

    // Master output
    double mMasterLevelDb = 0.0;
    float mMasterGain = 1.0f;

    // Per-input parameters (stored values)
    std::array<double, kMaxInputs> mInputLevelDb = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kMaxInputs> mInputPan = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kMaxInputs> mInputDelayMs = {0.0, 0.0, 0.0, 0.0};
    std::array<bool, kMaxInputs> mInputMute = {false, false, false, false};

    // Per-input coefficients (computed)
    std::array<float, kMaxInputs> mInputLevel = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, kMaxInputs> mInputPanL = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, kMaxInputs> mInputPanR = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<size_t, kMaxInputs> mInputDelaySamples = {0, 0, 0, 0};

    // Delay buffers
    std::array<std::vector<float>, kMaxInputs> mDelayBufferL;
    std::array<std::vector<float>, kMaxInputs> mDelayBufferR;
    std::array<size_t, kMaxInputs> mDelayWritePos = {0, 0, 0, 0};
  };

  inline void RegisterMixerEffect()
  {
    EffectTypeInfo info;
    info.type = "mixer";
    info.displayName = "Mixer";
    info.category = "utility";
    info.description = "Mix parallel branches with per-input level, pan, and delay";
    info.requiresResource = false;
    info.parameters = {
      {"masterLevel", "Master", 0.0, -60.0, 12.0, "dB"},
      // Per-input parameters are dynamic based on connected inputs
      // UI will generate controls for level_N, pan_N, delay_N, mute_N
    };

    EffectRegistry::Instance().Register("mixer", info, []()
      { return std::make_unique<MixerEffect>(); });
  }

} // namespace guitarfx
