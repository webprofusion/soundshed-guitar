#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * High-quality pitch shift effect using dual-pointer overlap-add with crossfading.
   * Shifts pitch by semitones (-24 to +24) without clicks or artifacts.
   * 
   * Uses two read pointers offset by half the grain size, crossfading between them
   * to avoid discontinuities when the read pointer wraps around the circular buffer.
   */
  class PitchShiftEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Buffer size - longer for lower pitches, ~1 second of audio
      mBufferSize = static_cast<size_t>(sampleRate * 1.0);
      mBufferL.resize(mBufferSize, 0.0f);
      mBufferR.resize(mBufferSize, 0.0f);

      // Grain size determines the overlap window - ~50ms works well for most material
      mGrainSize = static_cast<size_t>(sampleRate * 0.050);
      mHalfGrain = mGrainSize / 2;

      // Precompute crossfade window (raised cosine / Hann window for smooth transitions)
      mCrossfadeWindow.resize(mGrainSize);
      for (size_t i = 0; i < mGrainSize; ++i)
      {
        // Hann window: 0.5 * (1 - cos(2*pi*i/N))
        double t = static_cast<double>(i) / static_cast<double>(mGrainSize - 1);
        mCrossfadeWindow[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * t)));
      }

      UpdatePitchRatio();
      Reset();
    }

    void Reset() override
    {
      std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
      std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
      mWriteIndex = 0;

      // Initialize two read pointers offset by half the grain size
      mReadPhase1 = 0.0;
      mReadPhase2 = static_cast<double>(mHalfGrain);
      mCrossfadePos = 0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      // If no pitch shift, pass through
      if (mSemitones == 0)
      {
        for (int ch = 0; ch < 2; ++ch)
        {
          if (inputs[ch] && outputs[ch])
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
        }
        return;
      }

      if (mBufferSize == 0)
        return;

      for (int i = 0; i < numSamples; ++i)
      {
        // Write input to circular buffer
        mBufferL[mWriteIndex] = inputs[0] ? inputs[0][i] : 0.0f;
        mBufferR[mWriteIndex] = inputs[1] ? inputs[1][i] : 0.0f;
        mWriteIndex = (mWriteIndex + 1) % mBufferSize;

        // Read from two positions with cubic interpolation
        float outL1 = ReadInterpolated(mBufferL, mReadPhase1);
        float outR1 = ReadInterpolated(mBufferR, mReadPhase1);
        float outL2 = ReadInterpolated(mBufferL, mReadPhase2);
        float outR2 = ReadInterpolated(mBufferR, mReadPhase2);

        // Crossfade between the two read pointers
        // Position in crossfade window determines the blend
        float fade1 = mCrossfadeWindow[mCrossfadePos];
        float fade2 = mCrossfadeWindow[(mCrossfadePos + mHalfGrain) % mGrainSize];

        // Normalize so fade1 + fade2 = 1.0 (approximately)
        float fadeSum = fade1 + fade2;
        if (fadeSum > 0.001f)
        {
          fade1 /= fadeSum;
          fade2 /= fadeSum;
        }

        float outL = outL1 * fade1 + outL2 * fade2;
        float outR = outR1 * fade1 + outR2 * fade2;

        // Apply wet/dry mix
        float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        float dryR = inputs[1] ? inputs[1][i] : dryL;

        if (outputs[0])
          outputs[0][i] = outL * static_cast<float>(mMix) + dryL * static_cast<float>(1.0 - mMix);
        if (outputs[1])
          outputs[1][i] = outR * static_cast<float>(mMix) + dryR * static_cast<float>(1.0 - mMix);

        // Advance read phases at pitch-shifted rate
        mReadPhase1 += mPitchRatio;
        mReadPhase2 += mPitchRatio;

        // Wrap phases to buffer size
        if (mReadPhase1 >= static_cast<double>(mBufferSize))
          mReadPhase1 -= static_cast<double>(mBufferSize);
        if (mReadPhase1 < 0)
          mReadPhase1 += static_cast<double>(mBufferSize);
        if (mReadPhase2 >= static_cast<double>(mBufferSize))
          mReadPhase2 -= static_cast<double>(mBufferSize);
        if (mReadPhase2 < 0)
          mReadPhase2 += static_cast<double>(mBufferSize);

        // Advance crossfade position
        mCrossfadePos = (mCrossfadePos + 1) % mGrainSize;

        // Keep read pointers at safe distance from write pointer
        // Reset if they get too close (within half grain)
        double writePos = static_cast<double>(mWriteIndex);
        double safeDistance = static_cast<double>(mHalfGrain);

        // Calculate distance from write pointer (accounting for wrap)
        auto calcDist = [this, writePos](double readPos) -> double {
          double dist = writePos - readPos;
          if (dist < 0) dist += static_cast<double>(mBufferSize);
          return dist;
        };

        double dist1 = calcDist(mReadPhase1);
        double dist2 = calcDist(mReadPhase2);

        // If a read pointer gets too close to write, reset it to a safe distance
        if (dist1 < safeDistance || dist1 > static_cast<double>(mBufferSize) - safeDistance)
        {
          mReadPhase1 = std::fmod(writePos - static_cast<double>(mGrainSize * 2), static_cast<double>(mBufferSize));
          if (mReadPhase1 < 0) mReadPhase1 += static_cast<double>(mBufferSize);
        }
        if (dist2 < safeDistance || dist2 > static_cast<double>(mBufferSize) - safeDistance)
        {
          mReadPhase2 = std::fmod(writePos - static_cast<double>(mGrainSize * 2 + mHalfGrain), static_cast<double>(mBufferSize));
          if (mReadPhase2 < 0) mReadPhase2 += static_cast<double>(mBufferSize);
        }
      }
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

    // Cubic interpolation for smoother reading
    float ReadInterpolated(const std::vector<float>& buffer, double phase) const
    {
      double idx = std::fmod(phase, static_cast<double>(mBufferSize));
      if (idx < 0) idx += static_cast<double>(mBufferSize);

      size_t i0 = static_cast<size_t>(idx);
      size_t i1 = (i0 + 1) % mBufferSize;
      size_t im1 = (i0 + mBufferSize - 1) % mBufferSize;
      size_t i2 = (i0 + 2) % mBufferSize;

      float frac = static_cast<float>(idx - static_cast<double>(i0));

      // Cubic Hermite interpolation
      float y0 = buffer[im1];
      float y1 = buffer[i0];
      float y2 = buffer[i1];
      float y3 = buffer[i2];

      float c0 = y1;
      float c1 = 0.5f * (y2 - y0);
      float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
      float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

      return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    static constexpr double kPi = 3.14159265358979323846;

    int mSemitones = 0;
    double mMix = 1.0;
    double mPitchRatio = 1.0;

    std::vector<float> mBufferL, mBufferR;
    std::vector<float> mCrossfadeWindow;
    size_t mBufferSize = 0;
    size_t mGrainSize = 0;
    size_t mHalfGrain = 0;
    size_t mWriteIndex = 0;

    // Two read pointers for overlap-add crossfading
    double mReadPhase1 = 0.0;
    double mReadPhase2 = 0.0;
    size_t mCrossfadePos = 0;
  };

  inline void RegisterPitchShiftEffect()
  {
    EffectTypeInfo info;
    info.type = "pitch_shift";
    info.displayName = "Pitch Shift";
    info.category = "modulation";
    info.description = "High-quality pitch shift effect for transpose up/down by semitones";
    info.requiresResource = false;
    info.parameters = {
        {"semitones", "Semitones", 0.0, -24.0, 24.0, "st"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "%"}};
    EffectRegistry::Instance().Register("pitch_shift", info, []()
                                        { return std::make_unique<PitchShiftEffect>(); });
  }

} // namespace guitarfx
