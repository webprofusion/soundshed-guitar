#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Doubler: mixes a short delayed copy with opposite polarity on the right
 * channel to create stereo width and thickness.
 */
class DoublerEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        // Allocate for max ~100ms delay
        const size_t maxSamples = static_cast<size_t>(sampleRate * 0.1);
        mBufferL.resize(maxSamples, 0.0f);
        mBufferR.resize(maxSamples, 0.0f);

        UpdateDelaySamples();
        Reset();
    }

    void Reset() override
    {
        std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
        std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
        mWriteIndex = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (mDelaySamples <= 0)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        const std::size_t bufSize = mBufferL.size();

        if (bufSize == 0)
        {
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : inL;

            // Circular delay keeps channel separation; polarity inversion on R widens
            // the image without summing extra low-frequency energy to mono.
            mBufferL[mWriteIndex] = inL;
            mBufferR[mWriteIndex] = inR;

            const int readIdx = static_cast<int>(mWriteIndex) - mDelaySamples;
            const std::size_t safeReadIdx =
                static_cast<std::size_t>((readIdx + static_cast<int>(bufSize)) % static_cast<int>(bufSize));
            const float delayedL = mBufferL[safeReadIdx];
            const float delayedR = mBufferR[safeReadIdx];

            const float mixAmount = static_cast<float>(mMix);

            if (outputs[0])
            {
                outputs[0][i] = inL + delayedL * mixAmount;
            }

            if (outputs[1])
            {
                outputs[1][i] = inR - delayedR * mixAmount;
            }

            mWriteIndex = (mWriteIndex + 1) % bufSize;
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "time" || key == "timeMs")
        {
            mDelayMs = std::clamp(value, 0.0, 100.0);
            UpdateDelaySamples();
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "time" || key == "timeMs")
        {
            return mDelayMs;
        }

        if (key == "mix")
        {
            return mMix;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "delay_doubler";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "delay";
    }

  private:
    void UpdateDelaySamples()
    {
        mDelaySamples = static_cast<int>(mSampleRate * mDelayMs * 0.001);
    }

    double mDelayMs = 6.0; // Default 6ms delay
    double mMix = 0.3;     // Default mix level
    int mDelaySamples = 0;

    std::vector<float> mBufferL;
    std::vector<float> mBufferR;
    std::size_t mWriteIndex = 0;
};

inline void RegisterDoublerEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDelayDoubler;
    info.displayName = "Doubler";
    info.category = "delay";
    info.description = "Creates stereo width by mixing a delayed copy of the signal";
    info.requiresResource = false;
    info.parameters = {{"time", "Delay Time", 6.0, 0.0, 100.0, "ms"}, {"mix", "Mix", 0.3, 0.0, 1.0, "amount"}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<DoublerEffect>(); });
}
} // namespace guitarfx
