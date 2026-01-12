#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Simple pitch shift effect using linear interpolation on a circular buffer.
   * Shifts pitch by semitones (-24 to +24).
   */
  class PitchShiftEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Allocate circular buffers (~500ms worth of samples)
      const size_t bufferSize = static_cast<size_t>(sampleRate * 0.5);
      mBufferL.resize(bufferSize, 0.0f);
      mBufferR.resize(bufferSize, 0.0f);
      mShiftedL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      mShiftedR.resize(static_cast<size_t>(maxBlockSize), 0.0f);

      UpdatePitchRatio();
      Reset();
    }

    void Reset() override
    {
      std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
      std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
      mWriteIndex = 0;
      mReadPhase = 0.0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      // If no pitch shift, pass through
      if (mSemitones == 0)
      {
        for (int ch = 0; ch < 2; ++ch)
        {
          if (inputs[ch] && outputs[ch])
          {
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
          }
        }
        return;
      }

      const std::size_t bufSize = mBufferL.size();
      if (bufSize == 0)
        return;

      // Write input to circular buffer
      for (int i = 0; i < numSamples; ++i)
      {
        mBufferL[mWriteIndex] = inputs[0] ? inputs[0][i] : 0.0f;
        mBufferR[mWriteIndex] = inputs[1] ? inputs[1][i] : 0.0f;
        mWriteIndex = (mWriteIndex + 1) % bufSize;
      }

      // Read at shifted rate with linear interpolation
      for (int i = 0; i < numSamples; ++i)
      {
        const double readIdx = std::fmod(mReadPhase, static_cast<double>(bufSize));
        const std::size_t idx0 = static_cast<std::size_t>(readIdx);
        const std::size_t idx1 = (idx0 + 1) % bufSize;
        const double frac = readIdx - static_cast<double>(idx0);

        // Linear interpolation
        mShiftedL[static_cast<std::size_t>(i)] = static_cast<float>(
            mBufferL[idx0] * (1.0 - frac) + mBufferL[idx1] * frac);
        mShiftedR[static_cast<std::size_t>(i)] = static_cast<float>(
            mBufferR[idx0] * (1.0 - frac) + mBufferR[idx1] * frac);

        // Advance read phase at pitch-shifted rate
        mReadPhase = std::fmod(mReadPhase + mPitchRatio, static_cast<double>(bufSize));
      }

      // Copy to output
      if (outputs[0])
        std::copy(mShiftedL.begin(), mShiftedL.begin() + numSamples, outputs[0]);
      if (outputs[1])
        std::copy(mShiftedR.begin(), mShiftedR.begin() + numSamples, outputs[1]);
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "semitones")
      {
        mSemitones = static_cast<int>(std::clamp(value, -24.0, 24.0));
        UpdatePitchRatio();
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "semitones")
        return static_cast<double>(mSemitones);
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "pitch_shift"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

  private:
    void UpdatePitchRatio()
    {
      // Convert semitones to pitch ratio: ratio = 2^(semitones/12)
      mPitchRatio = std::pow(2.0, static_cast<double>(mSemitones) / 12.0);
    }

    int mSemitones = 0;
    double mMix = 1.0;
    double mPitchRatio = 1.0;

    std::vector<float> mBufferL, mBufferR;
    std::vector<float> mShiftedL, mShiftedR;
    std::size_t mWriteIndex = 0;
    double mReadPhase = 0.0;
  };

  inline void RegisterPitchShiftEffect()
  {
    EffectTypeInfo info;
    info.type = "pitch_shift";
    info.displayName = "Pitch Shift";
    info.category = "modulation";
    info.description = "Simple pitch shift effect for transpose up/down by semitones";
    info.requiresResource = false;
    info.parameters = {
        {"semitones", "Semitones", 0.0, -24.0, 24.0, "st"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "%"}};
    EffectRegistry::Instance().Register("pitch_shift", info, []()
                                        { return std::make_unique<PitchShiftEffect>(); });
  }

} // namespace guitarfx
