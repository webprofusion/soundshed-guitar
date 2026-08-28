#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/SignalsmithLatency.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Transpose effect using Signalsmith Stretch for integer semitone steps.
 *
 * Latency contract (Signalsmith docs):
 *   - When shifting: report inputLatency() + outputLatency(); delay dry by that
 *     amount before wet/dry mix so partial mix does not comb.
 *   - When transparent (0 st): bypass Stretch and report 0 latency.
 *   - presetCheaper(..., splitComputation=false) keeps total latency lower.
 */
class TransposeEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

        // Latency/quality: cheaper preset with splitComputation off (no extra hop).
        mStretch.presetCheaper(2, static_cast<float>(sampleRate), false);
        //      mStretch.presetDefault(2, static_cast<float>(sampleRate), true);
        mConfigured = true;
        ApplyTranspose();
        EnsureDryDelayCapacity();
        Reset();
    }

    void Reset() override
    {
        if (mConfigured)
        {
            mStretch.reset();
        }
        std::fill(mDryDelayL.begin(), mDryDelayL.end(), 0.0f);
        std::fill(mDryDelayR.begin(), mDryDelayR.end(), 0.0f);
        mDryWritePos = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);
        if (numSamples <= 0)
        {
            return;
        }

        // Transparent at 0 st: no Stretch latency, report 0 via GetLatencySamples().
        if (IsTransparent())
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (!mConfigured)
        {
            return;
        }

        if (static_cast<size_t>(numSamples) > mWetL.size())
        {
            mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
            mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        float* inputPtrs[2] = {inputs[0] ? inputs[0] : mZero.data(), inputs[1] ? inputs[1] : mZero.data()};
        float* wetPtrs[2] = {mWetL.data(), mWetR.data()};

        // Equal in/out lengths = pitch-only (no time stretch).
        mStretch.process(inputPtrs, numSamples, wetPtrs, numSamples);

        const float dryMix = static_cast<float>(1.0 - mMix);
        const float wetMix = static_cast<float>(mMix);
        const bool needDry = dryMix > 0.0f;
        if (needDry)
        {
            EnsureDryDelayCapacity();
        }

        const int latency = SignalsmithTotalLatencySamples(mStretch);

        for (int i = 0; i < numSamples; ++i)
        {
            float dryL = 0.0f;
            float dryR = 0.0f;
            if (needDry)
            {
                PushAndReadDry(inputs[0] ? inputs[0][i] : 0.0f, inputs[1] ? inputs[1][i] : 0.0f, latency, dryL, dryR);
            }

            if (outputs[0])
            {
                outputs[0][i] = dryL * dryMix + mWetL[static_cast<size_t>(i)] * wetMix;
            }
            if (outputs[1])
            {
                outputs[1][i] = dryR * dryMix + mWetR[static_cast<size_t>(i)] * wetMix;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "semitones")
        {
            const int clamped = static_cast<int>(std::round(std::clamp(value, -36.0, 12.0)));
            if (clamped != mSemitones)
            {
                mSemitones = clamped;
                ApplyTranspose();
            }
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
        if (key == "semitones")
        {
            return static_cast<double>(mSemitones);
        }
        if (key == "mix")
        {
            return mMix;
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "transpose";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        if (!mConfigured || IsTransparent())
        {
            return 0;
        }
        return SignalsmithTotalLatencySamples(mStretch);
    }

  private:
    [[nodiscard]] bool IsTransparent() const
    {
        return mSemitones == 0;
    }

    void ApplyTranspose()
    {
        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }
        // Tonality limit is normalised to sample rate (Signalsmith API contract).
        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mStretch.setTransposeSemitones(static_cast<float>(mSemitones), tonalityLimit);
    }

    void EnsureDryDelayCapacity()
    {
        if (!mConfigured)
        {
            return;
        }
        const int latency = SignalsmithTotalLatencySamples(mStretch);
        const size_t needed = static_cast<size_t>(std::max(latency, 0) + std::max(mMaxBlockSize, 1) + 8);
        if (mDryDelayL.size() < needed)
        {
            mDryDelayL.assign(needed, 0.0f);
            mDryDelayR.assign(needed, 0.0f);
            mDryWritePos = 0;
        }
    }

    void PushAndReadDry(float inL, float inR, int latency, float& outL, float& outR)
    {
        if (mDryDelayL.empty() || latency <= 0)
        {
            outL = inL;
            outR = inR;
            return;
        }

        const size_t size = mDryDelayL.size();
        mDryDelayL[mDryWritePos] = inL;
        mDryDelayR[mDryWritePos] = inR;

        const size_t delay = static_cast<size_t>(std::min(latency, static_cast<int>(size) - 1));
        const size_t readPos = (mDryWritePos + size - delay) % size;
        outL = mDryDelayL[readPos];
        outR = mDryDelayR[readPos];

        mDryWritePos = (mDryWritePos + 1) % size;
    }

    static constexpr double kTonalityLimitHz = 16000.0; // 8000

    int mSemitones = 0;
    double mMix = 1.0;
    bool mConfigured = false;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
    std::vector<float> mDryDelayL;
    std::vector<float> mDryDelayR;
    size_t mDryWritePos = 0;
};

inline void RegisterTransposeEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTranspose;
    info.aliases = {"transpose"};
    info.displayName = "Transpose";
    info.category = "pitch";
    info.description = "High-quality transpose effect";
    info.requiresResource = false;
    info.parameters = {{"semitones", "Semitones", 0.0, -36.0, 12.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<TransposeEffect>(); });
}

} // namespace guitarfx
