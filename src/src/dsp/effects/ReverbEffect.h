#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>
#include <vector>
#include <array>

namespace namguitar
{
  /**
   * Simple algorithmic reverb effect using parallel comb filters and allpass filters.
   */
  class ReverbEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Scale delay times for sample rate
      const double scale = sampleRate / 44100.0;

      // Comb filter delay times (in samples)
      const std::array<size_t, 8> combDelays = {
        static_cast<size_t>(1116 * scale),
        static_cast<size_t>(1188 * scale),
        static_cast<size_t>(1277 * scale),
        static_cast<size_t>(1356 * scale),
        static_cast<size_t>(1422 * scale),
        static_cast<size_t>(1491 * scale),
        static_cast<size_t>(1557 * scale),
        static_cast<size_t>(1617 * scale)
      };

      // Allpass filter delay times
      const std::array<size_t, 4> allpassDelays = {
        static_cast<size_t>(556 * scale),
        static_cast<size_t>(441 * scale),
        static_cast<size_t>(341 * scale),
        static_cast<size_t>(225 * scale)
      };

      // Allocate comb buffers
      for (size_t i = 0; i < 8; ++i)
      {
        mCombBufferL[i].resize(combDelays[i], 0.0f);
        mCombBufferR[i].resize(combDelays[i], 0.0f);
        mCombPos[i] = 0;
        mCombFilterStore[i] = 0.0f;
      }

      // Allocate allpass buffers
      for (size_t i = 0; i < 4; ++i)
      {
        mAllpassBufferL[i].resize(allpassDelays[i], 0.0f);
        mAllpassBufferR[i].resize(allpassDelays[i], 0.0f);
        mAllpassPos[i] = 0;
      }

      UpdateParameters();
    }

    void Reset() override
    {
      for (size_t i = 0; i < 8; ++i)
      {
        std::fill(mCombBufferL[i].begin(), mCombBufferL[i].end(), 0.0f);
        std::fill(mCombBufferR[i].begin(), mCombBufferR[i].end(), 0.0f);
        mCombFilterStore[i] = 0.0f;
      }

      for (size_t i = 0; i < 4; ++i)
      {
        std::fill(mAllpassBufferL[i].begin(), mAllpassBufferL[i].end(), 0.0f);
        std::fill(mAllpassBufferR[i].begin(), mAllpassBufferR[i].end(), 0.0f);
      }
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
      const float wet = static_cast<float>(mMix);
      const float dry = 1.0f - wet;

      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : 0.0f;

        // Sum input
        float input = (inL + inR) * 0.5f;

        // Parallel comb filters
        float outL = 0.0f;
        float outR = 0.0f;

        for (size_t c = 0; c < 8; ++c)
        {
          float delayed = mCombBufferL[c][mCombPos[c]];

          // Lowpass filter for damping
          mCombFilterStore[c] = delayed * (1.0f - mDamp) + mCombFilterStore[c] * mDamp;

          // Write back with feedback
          mCombBufferL[c][mCombPos[c]] = input + mCombFilterStore[c] * mFeedback;

          // Alternate left/right
          if (c % 2 == 0)
            outL += delayed;
          else
            outR += delayed;

          mCombPos[c] = (mCombPos[c] + 1) % mCombBufferL[c].size();
        }

        // Scale comb output
        outL *= 0.25f;
        outR *= 0.25f;

        // Allpass filters in series
        for (size_t a = 0; a < 4; ++a)
        {
          float bufOutL = mAllpassBufferL[a][mAllpassPos[a]];
          float bufOutR = mAllpassBufferR[a][mAllpassPos[a]];

          mAllpassBufferL[a][mAllpassPos[a]] = outL + bufOutL * 0.5f;
          mAllpassBufferR[a][mAllpassPos[a]] = outR + bufOutR * 0.5f;

          outL = bufOutL - outL;
          outR = bufOutR - outR;

          mAllpassPos[a] = (mAllpassPos[a] + 1) % mAllpassBufferL[a].size();
        }

        // Mix
        if (outputs[0])
          outputs[0][i] = inL * dry + outL * wet;
        if (outputs[1])
          outputs[1][i] = inR * dry + outR * wet;
      }
    }

    void SetParam(const std::string& key, double value) override
    {
      if (key == "decay")
      {
        mDecay = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "damping")
      {
        mDamping = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "decay")
        return mDecay;
      if (key == "damping")
        return mDamping;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "reverb_room"; }
    [[nodiscard]] std::string GetCategory() const override { return "reverb"; }

  private:
    void UpdateParameters()
    {
      mFeedback = static_cast<float>(0.7 + mDecay * 0.28);
      mDamp = static_cast<float>(mDamping * 0.4);
    }

    // Comb filters
    std::array<std::vector<float>, 8> mCombBufferL;
    std::array<std::vector<float>, 8> mCombBufferR;
    std::array<size_t, 8> mCombPos = {};
    std::array<float, 8> mCombFilterStore = {};

    // Allpass filters
    std::array<std::vector<float>, 4> mAllpassBufferL;
    std::array<std::vector<float>, 4> mAllpassBufferR;
    std::array<size_t, 4> mAllpassPos = {};

    // Parameters
    double mDecay = 0.5;
    double mDamping = 0.5;
    double mMix = 0.3;

    // Computed
    float mFeedback = 0.84f;
    float mDamp = 0.2f;
  };

  inline void RegisterReverbEffect()
  {
    EffectTypeInfo info;
    info.type = "reverb_room";
    info.displayName = "Room Reverb";
    info.category = "reverb";
    info.description = "Algorithmic room reverb";
    info.requiresResource = false;
    info.parameters = {
      {"decay", "Decay", 0.5, 0.0, 1.0, ""},
      {"damping", "Damping", 0.5, 0.0, 1.0, ""},
      {"mix", "Mix", 0.3, 0.0, 1.0, ""}
    };

    EffectRegistry::Instance().Register("reverb_room", info, []() {
      return std::make_unique<ReverbEffect>();
    });
  }

} // namespace namguitar
