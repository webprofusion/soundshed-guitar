#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>
#include <vector>


namespace guitarfx
{
  /**
   * High-quality pitch shift effect using granular delay-line modulation.
   * Shifts pitch by semitones (-24 to +24) with very low latency.
   * 
   * Uses two rotating read pointers (taps) with crossfading (Hann window).
   * This preserves transient timing and playback speed exactly while shifting pitch.
   */
  class PitchShiftEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Grain size: trade-off between latency and low-frequency artifacts.
      // 15ms @ 48kHz = 720 samples. Good for guitar (>66Hz).
      // Lower values (e.g. 10ms) reduce latency but may cause roughness on low notes.
      mGrainSize = static_cast<size_t>(sampleRate * 0.015);
      if (mGrainSize < 64) mGrainSize = 64;

      // Small constant safety buffer for interpolation (need to look 1 sample ahead of "current" reading point max)
      mMinLatency = 4.0;

      // Buffer size: Needs to hold at least one grain + safety margin.
      // When pitching down, we read further into the past.
      // Ideally buffer is circular.
      mBufferSize = mGrainSize * 4; 
      mBufferL.assign(mBufferSize, 0.0f);
      mBufferR.assign(mBufferSize, 0.0f);

      // Precompute Hann window
      mWindow.resize(mGrainSize);
      for (size_t i = 0; i < mGrainSize; ++i)
      {
        // 0.5 * (1 - cos(2*pi*i/N))
        double phase = static_cast<double>(i) / static_cast<double>(mGrainSize);
        mWindow[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * phase)));
      }

      UpdatePitchRatio();
      Reset();
    }

    void Reset() override
    {
      std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
      std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
      mWriteIndex = 0;
      mDelayPos = 0.0; // Start with 0 delay (minus min latency)
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      // If no pitch shift, pass through (zero latency optimization)
      if (mSemitones == 0)
      {
        for (int ch = 0; ch < 2; ++ch)
        {
          if (inputs[ch] && outputs[ch])
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
        }
        return;
      }

      // Delta delay per sample
      // Pitch ratio R. delay' = 1 - R.
      // If R=2 (+1 octave), delay changes by -1 per sample (reads faster, delay reduces).
      // If R=0.5 (-1 octave), delay changes by +0.5 per sample (reads slower, delay increases).
      double delayDelta = 1.0 - mPitchRatio;
      double grainSize = static_cast<double>(mGrainSize);

      for (int i = 0; i < numSamples; ++i)
      {
        // Write to buffer
        mBufferL[mWriteIndex] = inputs[0] ? inputs[0][i] : 0.0f;
        mBufferR[mWriteIndex] = inputs[1] ? inputs[1][i] : 0.0f;

        // Calculate tap delays
        // Tap 1
        double d1 = mDelayPos;
        // Tap 2 offset by 50%
        double d2 = mDelayPos + grainSize * 0.5;
        if (d2 >= grainSize) d2 -= grainSize;

        // Apply windowing based on delay position within grain
        // Window index: d1 mapped to 0..GrainSize
        // We can interpolate window or just truncate since it's slow varying
        size_t wIdx1 = static_cast<size_t>(d1);
        if (wIdx1 >= mGrainSize) wIdx1 = mGrainSize - 1;
        
        size_t wIdx2 = static_cast<size_t>(d2);
        if (wIdx2 >= mGrainSize) wIdx2 = mGrainSize - 1;

        float gain1 = mWindow[wIdx1];
        float gain2 = mWindow[wIdx2];

        // Add minimum safety latency to avoid reading ahead of write pointer
        // (Since d1 moves between 0 and GrainSize)
        // Read pos = writePos - (d + minLatency)
        double r1 = static_cast<double>(mWriteIndex) - (d1 + mMinLatency);
        double r2 = static_cast<double>(mWriteIndex) - (d2 + mMinLatency);

        // Read samples
        float l1 = ReadInterpolated(mBufferL, r1);
        float r1_val = ReadInterpolated(mBufferR, r1);
        
        float l2 = ReadInterpolated(mBufferL, r2);
        float r2_val = ReadInterpolated(mBufferR, r2);

        // Sum
        float outL = l1 * gain1 + l2 * gain2;
        float outR = r1_val * gain1 + r2_val * gain2;

        // Mix
        float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        float dryR = inputs[1] ? inputs[1][i] : dryL; // Mono fallback

        if (outputs[0])
          outputs[0][i] = outL * static_cast<float>(mMix) + dryL * static_cast<float>(1.0 - mMix);
        if (outputs[1])
          outputs[1][i] = outR * static_cast<float>(mMix) + dryR * static_cast<float>(1.0 - mMix);

        // Increment write
        mWriteIndex++;
        if (mWriteIndex >= mBufferSize) mWriteIndex = 0;

        // Increment delay modulation
        mDelayPos += delayDelta;
        
        // Wrap delay
        while (mDelayPos >= grainSize) mDelayPos -= grainSize;
        while (mDelayPos < 0.0) mDelayPos += grainSize;
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
      // ratio = 2^(semitones/12)
      mPitchRatio = std::pow(2.0, static_cast<double>(mSemitones) / 12.0);
    }

    // Cubic interpolation
    // 'pos' is the read position index (can be negative or > buffer size, will wrap)
    float ReadInterpolated(const std::vector<float>& buffer, double pos) const
    {
      // Handle wrapping
      // We expect pos to be roughly around mWriteIndex, but could be far if delay is large (unlikely here)
      // or wraps around 0.
      
      double size = static_cast<double>(mBufferSize);
      double idx = std::fmod(pos, size);
      if (idx < 0.0) idx += size;

      size_t i0 = static_cast<size_t>(idx);
      float frac = static_cast<float>(idx - static_cast<double>(i0));

      // Indices with wrapping
      size_t i1 = (i0 + 1);
      if (i1 >= mBufferSize) i1 = 0;
      
      size_t i2 = (i0 + 2);
      if (i2 >= mBufferSize) i2 -= mBufferSize; // optimization for common case
      
      size_t im1 = i0;
      if (im1 == 0) im1 = mBufferSize - 1; else im1--;

      float y0 = buffer[im1];
      float y1 = buffer[i0];
      float y2 = buffer[i1];
      float y3 = buffer[i2];

      // Cubic
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
    std::vector<float> mWindow;
    
    size_t mBufferSize = 0;
    size_t mGrainSize = 0;
    size_t mWriteIndex = 0;
    
    double mDelayPos = 0.0;
    double mMinLatency = 0.0;
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
