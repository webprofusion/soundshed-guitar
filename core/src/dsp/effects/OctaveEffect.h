#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/SignalsmithSupport.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Octaveeffect with octave up/down blend.
 *
 * Transparent (mix 0, or neither octave dialled in) bypasses both stretchers and
 * reports 0 latency, so the bypass keeps feeding the dry history: re-engaging
 * seeks the stretchers onto real audio rather than replaying whatever they were
 * left holding. See SignalsmithSupport.h for the configuration policy and the
 * measurements behind both.
 */
class OctaveEffect : public EffectProcessor
{
  public:
    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        const size_t bufferSize = static_cast<size_t>(std::max(1, maxBlockSize));
        mWetUpL.assign(bufferSize, 0.0f);
        mWetUpR.assign(bufferSize, 0.0f);
        mWetDownL.assign(bufferSize, 0.0f);
        mWetDownR.assign(bufferSize, 0.0f);
        mZero.assign(bufferSize, 0.0f);

        ConfigureSignalsmithLive(mUpStretch, 2, sampleRate);
        ConfigureSignalsmithLive(mDownStretch, 2, sampleRate);
        mConfigured = true;
        ApplyStretchSettings();
        UpdateToneCoefficient();
        mDry.Prepare(SignalsmithTotalLatencySamples(mUpStretch, mDownStretch),
                     std::max(mUpStretch.seekLength(), mDownStretch.seekLength()), maxBlockSize);
        Reset();
    }

    void Reset() override
    {
        if (mConfigured)
        {
            mUpStretch.reset();
            mDownStretch.reset();
        }

        mToneStateL = 0.0f;
        mToneStateR = 0.0f;
        mDry.Reset();
        // Nothing has been fed yet, so the next wet block re-seeks both
        // stretchers onto whatever history has accumulated by then.
        mNeedsEngage = true;
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

        if (!mConfigured)
        {
            CopyDry(inputs, outputs, numSamples);
            return;
        }

        if (static_cast<size_t>(numSamples) > mWetUpL.size())
        {
            mWetUpL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetUpR.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetDownL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetDownR.resize(static_cast<size_t>(numSamples), 0.0f);
            mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        float* leftInput = inputs[0] ? inputs[0] : mZero.data();
        float* rightInput = inputs[1] ? inputs[1] : leftInput;

        if (IsTransparent())
        {
            // The stretchers are idle but the history is not: it is what
            // re-engaging seeks onto, so keep recording the input.
            for (int i = 0; i < numSamples; ++i)
            {
                mDry.Push(leftInput[i], rightInput[i]);
            }

            CopyDry(inputs, outputs, numSamples);
            mNeedsEngage = true;
            return;
        }

        if (mNeedsEngage)
        {
            EngageSignalsmith(mUpStretch, mDry);
            EngageSignalsmith(mDownStretch, mDry);
            mNeedsEngage = false;
        }

        float* inputPtrs[2] = {leftInput, rightInput};
        float* upPtrs[2] = {mWetUpL.data(), mWetUpR.data()};
        float* downPtrs[2] = {mWetDownL.data(), mWetDownR.data()};

        mUpStretch.process(inputPtrs, numSamples, upPtrs, numSamples);
        mDownStretch.process(inputPtrs, numSamples, downPtrs, numSamples);

        const float dryMix = 1.0f - mMix;
        const float wetMix = mMix;

        const int latency = SignalsmithTotalLatencySamples(mUpStretch, mDownStretch);

        for (int i = 0; i < numSamples; ++i)
        {
            // Unconditional: the history has to stay current for a later Mix
            // turn-down or re-engage, not just for this block's blend.
            float dryL = leftInput[i];
            float dryR = rightInput[i];
            mDry.PushAndRead(leftInput[i], rightInput[i], latency, dryL, dryR);

            float wetL = mWetUpL[static_cast<size_t>(i)] * mOctaveUp + mWetDownL[static_cast<size_t>(i)] * mOctaveDown;
            float wetR = mWetUpR[static_cast<size_t>(i)] * mOctaveUp + mWetDownR[static_cast<size_t>(i)] * mOctaveDown;
            wetL = ApplyTone(wetL, mToneStateL);
            wetR = ApplyTone(wetR, mToneStateR);

            if (outputs[0])
            {
                outputs[0][i] = dryL * dryMix + wetL * wetMix;
            }

            if (outputs[1])
            {
                outputs[1][i] = dryR * dryMix + wetR * wetMix;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "octaveUp")
        {
            mOctaveUp = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "octaveDown")
        {
            mOctaveDown = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "tone")
        {
            mTone = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdateToneCoefficient();
        }
        else if (key == "mix")
        {
            mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "octaveUp")
        {
            return mOctaveUp;
        }

        if (key == "octaveDown")
        {
            return mOctaveDown;
        }

        if (key == "tone")
        {
            return mTone;
        }

        if (key == "mix")
        {
            return mMix;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "octave";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        // Transparent dry path reports 0; wet path reports full Signalsmith latency
        // (input + output halves) for host delay compensation.
        if (!mConfigured || IsTransparent())
        {
            return 0;
        }

        return SignalsmithTotalLatencySamples(mUpStretch, mDownStretch);
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kTonalityLimitHz = 16000.0;

    [[nodiscard]] bool IsTransparent() const
    {
        return mMix <= 0.0f || (mOctaveUp <= 0.0f && mOctaveDown <= 0.0f);
    }

    void CopyDry(float** inputs, float** outputs, int numSamples)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            if (!outputs[ch])
            {
                continue;
            }

            if (inputs[ch])
            {
                std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
            }
            else if (ch == 1 && inputs[0])
            {
                std::copy(inputs[0], inputs[0] + numSamples, outputs[ch]);
            }
            else
            {
                std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
            }
        }
    }

    void ApplyStretchSettings()
    {
        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }

        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mUpStretch.setTransposeSemitones(12.0f, tonalityLimit);
        mDownStretch.setTransposeSemitones(-12.0f, tonalityLimit);
    }

    void UpdateToneCoefficient()
    {
        const float minHz = 400.0f;
        const float maxHz = 4000.0f;
        const float cutoff = minHz + (maxHz - minHz) * mTone;
        const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
        mToneCoef = 1.0f - std::exp(-x);
    }

    float ApplyTone(float input, float& state)
    {
        state += mToneCoef * (input - state);
        return state;
    }

    double mSampleRate = 48000.0;
    int mMaxBlockSize = 0;
    bool mConfigured = false;
    bool mNeedsEngage = true;

    float mOctaveUp = 0.6f;
    float mOctaveDown = 0.6f;
    float mTone = 0.5f;
    float mMix = 1.0f;

    float mToneCoef = 0.0f;
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;

    signalsmith::stretch::SignalsmithStretch<float> mUpStretch;
    signalsmith::stretch::SignalsmithStretch<float> mDownStretch;
    SignalsmithDryHistory mDry;
    std::vector<float> mWetUpL;
    std::vector<float> mWetUpR;
    std::vector<float> mWetDownL;
    std::vector<float> mWetDownR;
    std::vector<float> mZero;
};

inline void RegisterOctaveEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kOctave;
    info.aliases = {"octave"};
    info.displayName = "Octave";
    info.category = "pitch";
    info.description = "High-quality octave up/down blend";
    info.requiresResource = false;
    info.parameters = {{"octaveUp", "Oct Up", 0.6, 0.0, 1.0, "amount"},
                       {"octaveDown", "Oct Down", 0.6, 0.0, 1.0, "amount"},
                       {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<OctaveEffect>(); });
}
} // namespace guitarfx
