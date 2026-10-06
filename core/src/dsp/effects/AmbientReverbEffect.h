#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/GrainPitchShifter.h"
#include "dsp/effects/ReverbPresets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace guitarfx
{
/**
 * Long, diffuse reverb: six modulated combs into four allpass diffusers, with early
 * reflections off a pre-delay line.
 *
 * Shimmer feeds the late reverb back into the combs through a pitch shifter, an octave up by
 * default, so each pass round the tank comes back higher and the tail climbs into a halo.
 * Shimmer is how much goes round. The loop is band-limited and soft-limited, so even at full
 * Shimmer and Decay it settles rather than running away.
 *
 * Freeze holds the tail: the combs stop losing energy (feedback to one, damping off) and stop
 * taking new input, so what was ringing rings on, unchanged, under whatever is played next.
 * Turning it off lets the tail decay from there at the Decay setting.
 */
class AmbientReverbEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        constexpr double maxSizeScale = 2.8;
        constexpr double extraModulationMs = 8.0;

        const size_t maxPreDelaySamples = DelayMsToSamples(kMaxPreDelayMs + kMaxEarlyTapMs + 4.0);
        mPreDelayL.assign(maxPreDelaySamples, 0.0f);
        mPreDelayR.assign(maxPreDelaySamples, 0.0f);
        mPreDelayWrite = 0;

        for (size_t index = 0; index < kCombCount; ++index)
        {
            const size_t lenL = DelayMsToSamples(kCombMsL[index] * maxSizeScale + extraModulationMs);
            const size_t lenR = DelayMsToSamples(kCombMsR[index] * maxSizeScale + extraModulationMs);
            mCombBufferL[index].assign(lenL, 0.0f);
            mCombBufferR[index].assign(lenR, 0.0f);
            mCombWriteL[index] = 0;
            mCombWriteR[index] = 0;
            mCombFilterStateL[index] = 0.0f;
            mCombFilterStateR[index] = 0.0f;
        }

        for (size_t index = 0; index < kAllpassCount; ++index)
        {
            const size_t lenL = DelayMsToSamples(kAllpassMsL[index] * maxSizeScale + 4.0);
            const size_t lenR = DelayMsToSamples(kAllpassMsR[index] * maxSizeScale + 4.0);
            mAllpassBufferL[index].assign(lenL, 0.0f);
            mAllpassBufferR[index].assign(lenR, 0.0f);
            mAllpassWriteL[index] = 0;
            mAllpassWriteR[index] = 0;
        }

        for (size_t tap = 0; tap < kEarlyTapCount; ++tap)
        {
            mEarlyTapSamples[tap] = DelayMsToSamples(kEarlyTapMs[tap]);
            mEarlyTapMirrorSamples[tap] = DelayMsToSamples(kEarlyTapMs[kEarlyTapCount - 1 - tap] + 3.0);
        }

        mSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.015)));
        mSizeSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.18)));
        mLateGain = static_cast<float>(std::pow(10.0, kLateMakeupDb / 20.0) / static_cast<double>(kCombCount));
        mFreezeCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * kFreezeFadeS)));
        mShimmerReturn.Prepare(mSampleRate);

        for (size_t index = 0; index < kCombCount; ++index)
        {
            mCombPrecompSinOffsets[index] = static_cast<float>(std::sin(kCombModPhaseOffsets[index]));
            mCombPrecompCosOffsets[index] = static_cast<float>(std::cos(kCombModPhaseOffsets[index]));
        }

        UpdateParameters();
        mFeedback = mFeedbackTarget;
        mDamp = mDampTarget;
        mDiffusion = mDiffusionTarget;
        mToneCoeff = mToneCoeffTarget;
        mSizeScale = mSizeScaleTarget;
        mMixSmoothed = static_cast<float>(mMix);
        mWidthSmoothed = static_cast<float>(mWidth);
        mOutputGainSmoothed = mOutputGainTarget;
        mFreezeSmoothed = mFreeze ? 1.0f : 0.0f;
        mShimmerSmoothed = mShimmerTarget;

        Reset();
    }

    void Reset() override
    {
        std::fill(mPreDelayL.begin(), mPreDelayL.end(), 0.0f);
        std::fill(mPreDelayR.begin(), mPreDelayR.end(), 0.0f);
        mPreDelayWrite = 0;

        for (size_t index = 0; index < kCombCount; ++index)
        {
            std::fill(mCombBufferL[index].begin(), mCombBufferL[index].end(), 0.0f);
            std::fill(mCombBufferR[index].begin(), mCombBufferR[index].end(), 0.0f);
            mCombWriteL[index] = 0;
            mCombWriteR[index] = 0;
            mCombFilterStateL[index] = 0.0f;
            mCombFilterStateR[index] = 0.0f;
        }

        for (size_t index = 0; index < kAllpassCount; ++index)
        {
            std::fill(mAllpassBufferL[index].begin(), mAllpassBufferL[index].end(), 0.0f);
            std::fill(mAllpassBufferR[index].begin(), mAllpassBufferR[index].end(), 0.0f);
            mAllpassWriteL[index] = 0;
            mAllpassWriteR[index] = 0;
        }

        mWetToneStateL = 0.0f;
        mWetToneStateR = 0.0f;
        mInputHpPrevL = 0.0f;
        mInputHpPrevR = 0.0f;
        mInputHpStateL = 0.0f;
        mInputHpStateR = 0.0f;
        mLfoSin = 0.0f;
        mLfoCos = 1.0f;
        mShimmerReturn.Reset();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (mPreDelayL.empty() || mCombBufferL[0].empty() || mAllpassBufferL[0].empty())
        {
            CopyInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (!mEnabled)
        {
            CopyInputToOutput(inputs, outputs, numSamples);
            return;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const float inL = inputs && inputs[0] ? inputs[0][sampleIndex] : 0.0f;
            const float inR = inputs && inputs[1] ? inputs[1][sampleIndex] : inL;

            mFeedback += (mFeedbackTarget - mFeedback) * mSmoothCoeff;
            mDamp += (mDampTarget - mDamp) * mSmoothCoeff;
            mDiffusion += (mDiffusionTarget - mDiffusion) * mSmoothCoeff;
            mToneCoeff += (mToneCoeffTarget - mToneCoeff) * mSmoothCoeff;
            mSizeScale += (mSizeScaleTarget - mSizeScale) * mSizeSmoothCoeff;
            mMixSmoothed += (static_cast<float>(mMix) - mMixSmoothed) * mSmoothCoeff;
            mWidthSmoothed += (static_cast<float>(mWidth) - mWidthSmoothed) * mSmoothCoeff;
            mOutputGainSmoothed += (mOutputGainTarget - mOutputGainSmoothed) * mSmoothCoeff;
            mFreezeSmoothed += ((mFreeze ? 1.0f : 0.0f) - mFreezeSmoothed) * mFreezeCoeff;
            mShimmerSmoothed += (mShimmerTarget - mShimmerSmoothed) * mSmoothCoeff;
            // Frozen, the tank takes nothing new: not the input, and not the shimmer, which
            // would otherwise keep adding to a loop that no longer loses anything.
            const float intake = 1.0f - mFreezeSmoothed;

            const float wetMix = mMixSmoothed;
            const float dryMix = 1.0f - wetMix;

            // Highpass the reverb feed. The comb loop runs to 0.95 feedback around a lowpass with
            // unity DC gain, so without this any DC or sub-bass entering the tank accumulates —
            // it measured louder in the 20-120 Hz band than at mid.
            const float hpL = ProcessInputHighpass(inL, mInputHpPrevL, mInputHpStateL);
            const float hpR = ProcessInputHighpass(inR, mInputHpPrevR, mInputHpStateR);

            const float monoIn = 0.5f * (hpL + hpR);
            const float sideIn = 0.5f * (hpL - hpR);
            const float wetInL = monoIn + sideIn * 0.3f;
            const float wetInR = monoIn - sideIn * 0.3f;

            mPreDelayL[mPreDelayWrite] = wetInL;
            mPreDelayR[mPreDelayWrite] = wetInR;

            const float preL = ReadFromDelay(mPreDelayL, mPreDelayWrite, mPreDelaySamples);
            const float preR = ReadFromDelay(mPreDelayR, mPreDelayWrite, mPreDelaySamples);

            float earlyL = preL * 0.38f;
            float earlyR = preR * 0.38f;
            const float shimmerFeed = mShimmerSmoothed * intake;

            for (size_t tap = 0; tap < kEarlyTapCount; ++tap)
            {
                earlyL += ReadFromDelay(mPreDelayL, mPreDelayWrite, mPreDelaySamples + mEarlyTapSamples[tap]) *
                          kEarlyTapGains[tap];
                // The mirror pattern reverses the tap TIMES, so it has to reverse the gains with
                // them. Pairing reversed times with forward gains made the right channel's latest
                // reflection its loudest — early reflections that swelled instead of decaying.
                earlyR += ReadFromDelay(mPreDelayR, mPreDelayWrite, mPreDelaySamples + mEarlyTapMirrorSamples[tap]) *
                          kEarlyTapGains[kEarlyTapCount - 1 - tap];
            }

            if (++mPreDelayWrite >= mPreDelayL.size())
            {
                mPreDelayWrite = 0;
            }

            // The LFO is a rotating unit phasor, so sin and cos of the phase are already in hand;
            // per-comb sin(phase+offset) then comes from the angle-addition identity rather than
            // N separate sin() calls.
            const float sinPhi = mLfoSin;
            const float cosPhi = mLfoCos;
            AdvanceLfo();

            const float feedL = (preL + earlyL * 0.24f + preR * 0.08f) * intake + mShimmerReturn.Left() * shimmerFeed;
            const float feedR = (preR + earlyR * 0.24f + preL * 0.08f) * intake + mShimmerReturn.Right() * shimmerFeed;

            float combSumL = 0.0f;
            float combSumR = 0.0f;

            for (size_t combIndex = 0; combIndex < kCombCount; ++combIndex)
            {
                // sin(φ+offset) via the angle-addition identity — a phase-shifted LFO per comb.
                // (`sinPhi * sin(φ+offset)` ring-modulated at 2× the LFO rate; summing sinPhi onto
                // it instead collapses to 2·cos(δ/2)·sin(φ+δ/2), which left the six combs with
                // depths spanning 14:1 — the last of them barely moved.)
                const float phase =
                    sinPhi * mCombPrecompCosOffsets[combIndex] + cosPhi * mCombPrecompSinOffsets[combIndex];
                const float modSamples = mModDepthSamples * phase * (0.6f + 0.07f * static_cast<float>(combIndex));

                const float delayL = std::clamp(DelayMsToSamplesFloat(kCombMsL[combIndex] * mSizeScale) + modSamples,
                                                1.0f, static_cast<float>(mCombBufferL[combIndex].size() - 2));
                const float delayR = std::clamp(DelayMsToSamplesFloat(kCombMsR[combIndex] * mSizeScale) - modSamples,
                                                1.0f, static_cast<float>(mCombBufferR[combIndex].size() - 2));

                const float delayedL = ReadFromDelayFractional(mCombBufferL[combIndex], mCombWriteL[combIndex], delayL);
                const float delayedR = ReadFromDelayFractional(mCombBufferR[combIndex], mCombWriteR[combIndex], delayR);

                mCombFilterStateL[combIndex] = FlushNearZero(
                    mCombFilterStateL[combIndex] + (delayedL - mCombFilterStateL[combIndex]) * (1.0f - mDamp));
                mCombFilterStateR[combIndex] = FlushNearZero(
                    mCombFilterStateR[combIndex] + (delayedR - mCombFilterStateR[combIndex]) * (1.0f - mDamp));

                mCombBufferL[combIndex][mCombWriteL[combIndex]] =
                    FlushNearZero(feedL + mCombFilterStateL[combIndex] * mFeedback);
                mCombBufferR[combIndex][mCombWriteR[combIndex]] =
                    FlushNearZero(feedR + mCombFilterStateR[combIndex] * mFeedback);

                if (++mCombWriteL[combIndex] >= mCombBufferL[combIndex].size())
                {
                    mCombWriteL[combIndex] = 0;
                }

                if (++mCombWriteR[combIndex] >= mCombBufferR[combIndex].size())
                {
                    mCombWriteR[combIndex] = 0;
                }

                combSumL += delayedL;
                combSumR += delayedR;
            }

            float lateL = combSumL * mLateGain;
            float lateR = combSumR * mLateGain;

            const float crossFeed = 0.05f + static_cast<float>(mSpace) * 0.12f;
            const float lateCrossL = lateL + lateR * crossFeed;
            const float lateCrossR = lateR + lateL * crossFeed;
            lateL = lateCrossL;
            lateR = lateCrossR;

            for (size_t allpassIndex = 0; allpassIndex < kAllpassCount; ++allpassIndex)
            {
                const float delayL = std::clamp(DelayMsToSamplesFloat(kAllpassMsL[allpassIndex] * mSizeScale), 1.0f,
                                                static_cast<float>(mAllpassBufferL[allpassIndex].size() - 2));
                const float delayR = std::clamp(DelayMsToSamplesFloat(kAllpassMsR[allpassIndex] * mSizeScale), 1.0f,
                                                static_cast<float>(mAllpassBufferR[allpassIndex].size() - 2));
                lateL = ProcessAllpass(mAllpassBufferL[allpassIndex], mAllpassWriteL[allpassIndex], delayL, lateL,
                                       mDiffusion);
                lateR = ProcessAllpass(mAllpassBufferR[allpassIndex], mAllpassWriteR[allpassIndex], delayR, lateR,
                                       mDiffusion);
            }

            if (shimmerFeed > 0.0f)
            {
                mShimmerReturn.Process(lateL, lateR);
            }
            else if (!mShimmerReturn.Silent())
            {
                mShimmerReturn.Reset();
            }

            float wetL = earlyL * 0.22f * intake + lateL * 0.78f;
            float wetR = earlyR * 0.22f * intake + lateR * 0.78f;

            mWetToneStateL = FlushNearZero(mWetToneStateL + (wetL - mWetToneStateL) * mToneCoeff);
            mWetToneStateR = FlushNearZero(mWetToneStateR + (wetR - mWetToneStateR) * mToneCoeff);
            wetL = mWetToneStateL;
            wetR = mWetToneStateR;

            const float wetMid = 0.5f * (wetL + wetR);
            const float wetSide = 0.5f * (wetL - wetR);
            const float width = mWidthSmoothed;
            wetL = (wetMid + wetSide * width) * mOutputGainSmoothed;
            wetR = (wetMid - wetSide * width) * mOutputGainSmoothed;

            if (outputs[0])
            {
                outputs[0][sampleIndex] = inL * dryMix + wetL * wetMix;
            }

            if (outputs[1])
            {
                outputs[1][sampleIndex] = inR * dryMix + wetR * wetMix;
            }
        }

        // The shimmer loop is the one place a value too large to be audio could circulate.
        if (!IsFinite(mShimmerReturn.Left()) || !IsFinite(mShimmerReturn.Right()))
        {
            Reset();
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "decay")
        {
            mDecay = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "space")
        {
            mSpace = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "diffusion")
        {
            mDiffusionAmount = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "preDelay")
        {
            mPreDelayMs = std::clamp(value, 0.0, kMaxPreDelayMs);
        }
        else if (key == "tone")
        {
            mTone = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "width")
        {
            mWidth = std::clamp(value, 0.0, 1.25);
        }
        else if (key == "modRate")
        {
            mModRateHz = std::clamp(value, 0.02, 2.0);
        }
        else if (key == "modDepth")
        {
            mModDepth = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "outputGain")
        {
            mOutputGainDb = std::clamp(value, -18.0, 12.0);
        }
        else if (key == "shimmer")
        {
            mShimmer = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "shimmerPitch")
        {
            mShimmerPitch = static_cast<int>(
                std::lround(std::clamp(value, 0.0, static_cast<double>(kShimmerSemitones.size() - 1))));
        }
        else if (key == "freeze")
        {
            mFreeze = value >= 0.5;
        }
        else
        {
            return;
        }

        UpdateParameters();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "decay")
        {
            return mDecay;
        }

        if (key == "space")
        {
            return mSpace;
        }

        if (key == "diffusion")
        {
            return mDiffusionAmount;
        }

        if (key == "preDelay")
        {
            return mPreDelayMs;
        }

        if (key == "tone")
        {
            return mTone;
        }

        if (key == "width")
        {
            return mWidth;
        }

        if (key == "modRate")
        {
            return mModRateHz;
        }

        if (key == "modDepth")
        {
            return mModDepth;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "outputGain")
        {
            return mOutputGainDb;
        }

        if (key == "shimmer")
        {
            return mShimmer;
        }

        if (key == "shimmerPitch")
        {
            return mShimmerPitch;
        }

        if (key == "freeze")
        {
            return mFreeze ? 1.0 : 0.0;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "reverb_ambient";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "reverb";
    }

  private:
    static constexpr size_t kCombCount = 6;
    static constexpr size_t kAllpassCount = 4;
    static constexpr size_t kEarlyTapCount = 5;
    static constexpr double kTwoPi = 6.2831853071795864769;
    static constexpr double kMaxPreDelayMs = 200.0;
    static constexpr double kMaxEarlyTapMs = 52.0;

    static constexpr std::array<double, kCombCount> kCombMsL = {41.2, 48.9, 56.7, 64.1, 72.4, 81.8};
    static constexpr std::array<double, kCombCount> kCombMsR = {42.8, 50.4, 58.1, 65.8, 74.0, 83.1};
    static constexpr std::array<double, kAllpassCount> kAllpassMsL = {8.1, 12.7, 17.8, 24.9};
    static constexpr std::array<double, kAllpassCount> kAllpassMsR = {8.9, 13.6, 18.7, 26.1};
    static constexpr std::array<double, kEarlyTapCount> kEarlyTapMs = {7.0, 13.0, 21.0, 34.0, 49.0};
    static constexpr std::array<float, kEarlyTapCount> kEarlyTapGains = {0.24f, 0.18f, 0.14f, 0.10f, 0.07f};
    static constexpr std::array<double, kCombCount> kCombModPhaseOffsets = {0.0, 0.9, 1.7, 2.6, 3.8, 4.9};
    static constexpr double kFeedHighpassHz = 70.0;
    static constexpr double kRt60MinS = 0.8;
    static constexpr double kRt60MaxS = 20.0;
    // Fixed gain on the late reverb. Until the diffusers were fixed they were resonant combs, and
    // their gain -- 12.4 dB at the default Diffusion -- set the late level that the Mix default and
    // saved presets were balanced against. This puts it back, on the late path alone because that
    // is where the diffusers sit, so the early reflections keep their old balance against it. It
    // follows no control, so Diffusion stays out of the level.
    static constexpr double kLateMakeupDb = 12.4;

    /// The shimmer's intervals, in the order Shimmer Pitch lists them.
    static constexpr std::array<double, 4> kShimmerSemitones = {12.0, 7.0, 19.0, -12.0};
    /// Each interval's loop gain at full Shimmer, the most that still lets the tail die away
    /// with every other control at its maximum. A fifth climbs out of the damped band more
    /// slowly than an octave, so it needs the least. The comb sum carries kLateMakeupDb, so the
    /// return is scaled back by that as well.
    static constexpr std::array<double, 4> kShimmerLoopGain = {1.5, 1.0, 1.5, 1.5};
    /// How long Freeze takes to close the tank to new input, and to open it again.
    static constexpr double kFreezeFadeS = 0.06;

    size_t DelayMsToSamples(double ms) const
    {
        return std::max<size_t>(1, static_cast<size_t>(ms * mSampleRate * 0.001));
    }

    float DelayMsToSamplesFloat(double ms) const
    {
        return static_cast<float>(std::max(1.0, ms * mSampleRate * 0.001));
    }

    static float ReadFromDelay(const std::vector<float>& buffer, size_t writePos, size_t delaySamples)
    {
        if (buffer.empty())
        {
            return 0.0f;
        }

        const size_t back = std::min(delaySamples, buffer.size() - 1);
        const size_t readPos = (writePos + buffer.size() - back) % buffer.size();
        return buffer[readPos];
    }

    float ReadFromDelayFractional(const std::vector<float>& buffer, size_t writePos, float delaySamples) const
    {
        if (buffer.empty())
        {
            return 0.0f;
        }

        const size_t delayFloor = static_cast<size_t>(delaySamples);
        const float frac = delaySamples - static_cast<float>(delayFloor);
        const float sample0 = ReadFromDelay(buffer, writePos, delayFloor);
        const float sample1 = ReadFromDelay(buffer, writePos, delayFloor + 1);
        return sample0 + frac * (sample1 - sample0);
    }

    float ProcessAllpass(std::vector<float>& buffer, size_t& writePos, float delaySamples, float input, float gain)
    {
        const float delayed = ReadFromDelayFractional(buffer, writePos, delaySamples);
        const float output = delayed - input * gain;
        // The delay line is fed the allpass OUTPUT. Feeding back the delay read instead gives
        // ((1+g²)z^-M - g)/(1 - g·z^-M) — a resonant comb, not a flat diffuser, so the chain
        // adds tens of dB of gain and ripple and Diffusion turns into a volume control.
        buffer[writePos] = input + output * gain;

        if (++writePos >= buffer.size())
        {
            writePos = 0;
        }

        return output;
    }

    static void CopyInputToOutput(float** inputs, float** outputs, int numSamples)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            if (!outputs[channel])
            {
                continue;
            }

            if (inputs && inputs[channel])
            {
                std::copy_n(inputs[channel], numSamples, outputs[channel]);
            }
            else
            {
                std::fill_n(outputs[channel], numSamples, 0.0f);
            }
        }
    }

    // Advances the LFO by one sample as a rotation of the unit phasor (sin, cos). There is no
    // phase accumulator and so no wrap: a polynomial sin() evaluated over a wrapped [-π, π]
    // phase steps discontinuously at ±π, which moves every comb delay at once and ticks audibly
    // once per LFO cycle.
    void AdvanceLfo() noexcept
    {
        const float s = mLfoSin * mLfoRotCos + mLfoCos * mLfoRotSin;
        const float c = mLfoCos * mLfoRotCos - mLfoSin * mLfoRotSin;
        // One Newton step back onto the unit circle. Without it the magnitude creeps over the
        // hours a session runs, because at the lowest mod rates cos(increment) rounds to 1.0f.
        const float correction = 1.5f - 0.5f * (s * s + c * c);
        mLfoSin = s * correction;
        mLfoCos = c * correction;
    }

    float ProcessInputHighpass(float input, float& prevIn, float& prevOut) const
    {
        const float output = mInputHpAlpha * (prevOut + input - prevIn);
        prevIn = input;
        // Flushed: the pole sits above 0.99, so once the input goes quiet the state decays into
        // denormal range and stays there, and denormal arithmetic on the audio thread is slow.
        prevOut = FlushNearZero(output);
        return output;
    }

    static float FlushNearZero(float x) noexcept
    {
        return std::fabs(x) < 1.0e-9f ? 0.0f : x;
    }

    void UpdateParameters()
    {
        mPreDelaySamples = DelayMsToSamples(mPreDelayMs);
        mSizeScaleTarget = static_cast<float>(0.95 + mSpace * 1.55);
        // Feedback derived from the RT60 Decay is asking for. A gain that moves linearly with the
        // knob is very uneven in time (RT60 goes as 1/-log10(g)), and the old floor of 0.68 meant
        // decay=0 still ran past 3.5 s — the control could not reach a short tail at all. Space
        // now sets size and density only; it no longer stretches the tail behind the Decay knob.
        double meanCombMs = 0.0;

        for (size_t index = 0; index < kCombCount; ++index)
        {
            meanCombMs += kCombMsL[index] + kCombMsR[index];
        }

        meanCombMs *= mSizeScaleTarget / (2.0 * static_cast<double>(kCombCount));

        const double rt60S = kRt60MinS * std::pow(kRt60MaxS / kRt60MinS, std::clamp(mDecay, 0.0, 1.0));
        mFeedbackTarget =
            static_cast<float>(std::clamp(std::pow(10.0, -3.0 * (meanCombMs * 0.001) / rt60S), 0.3, 0.96));
        mDampTarget = static_cast<float>(std::clamp(0.86 - mTone * 0.64, 0.16, 0.88));
        mDiffusionTarget = static_cast<float>(std::clamp(0.48 + mDiffusionAmount * 0.40, 0.38, 0.92));
        mToneCoeffTarget = static_cast<float>(std::clamp(0.05 + mTone * 0.28, 0.05, 0.33));
        const double modInc = kTwoPi * std::clamp(mModRateHz, 0.02, 2.0) / std::max(1.0, mSampleRate);
        mLfoRotSin = static_cast<float>(std::sin(modInc));
        mLfoRotCos = static_cast<float>(std::cos(modInc));

        const double dt = 1.0 / std::max(1.0, mSampleRate);
        const double rc = 1.0 / (2.0 * 3.14159265358979323846 * kFeedHighpassHz);
        mInputHpAlpha = static_cast<float>(rc / (rc + dt));
        mModDepthSamples = DelayMsToSamplesFloat(0.08 + mModDepth * (1.2 + mSpace * 1.8));
        mOutputGainTarget = static_cast<float>(std::pow(10.0, mOutputGainDb / 20.0));
        mShimmerTarget = static_cast<float>(mShimmer * kShimmerLoopGain[static_cast<size_t>(mShimmerPitch)] *
                                            std::pow(10.0, -kLateMakeupDb / 20.0));
        mShimmerReturn.SetSemitones(kShimmerSemitones[static_cast<size_t>(mShimmerPitch)]);

        if (mFreeze)
        {
            mFeedbackTarget = 1.0f;
            mDampTarget = 0.0f;
        }
    }

    std::vector<float> mPreDelayL;
    std::vector<float> mPreDelayR;
    size_t mPreDelayWrite = 0;
    size_t mPreDelaySamples = 1;

    std::array<std::vector<float>, kCombCount> mCombBufferL;
    std::array<std::vector<float>, kCombCount> mCombBufferR;
    std::array<size_t, kCombCount> mCombWriteL{};
    std::array<size_t, kCombCount> mCombWriteR{};
    std::array<float, kCombCount> mCombFilterStateL{};
    std::array<float, kCombCount> mCombFilterStateR{};

    std::array<std::vector<float>, kAllpassCount> mAllpassBufferL;
    std::array<std::vector<float>, kAllpassCount> mAllpassBufferR;
    std::array<size_t, kAllpassCount> mAllpassWriteL{};
    std::array<size_t, kAllpassCount> mAllpassWriteR{};

    std::array<size_t, kEarlyTapCount> mEarlyTapSamples{};
    std::array<size_t, kEarlyTapCount> mEarlyTapMirrorSamples{};

    double mDecay = 0.50;
    double mSpace = 0.72;
    double mDiffusionAmount = 0.4;
    double mPreDelayMs = 26.0;
    double mTone = 0.42;
    double mWidth = 1.08;
    double mModRateHz = 0.18;
    double mModDepth = 0.38;
    double mMix = 0.28;
    double mOutputGainDb = 0.0;
    double mShimmer = 0.0;
    int mShimmerPitch = 0;
    bool mFreeze = false;

    float mFeedback = 0.82f;
    float mFeedbackTarget = 0.82f;
    float mDamp = 0.46f;
    float mDampTarget = 0.46f;
    float mDiffusion = 0.72f;
    float mDiffusionTarget = 0.72f;
    float mToneCoeff = 0.16f;
    float mToneCoeffTarget = 0.16f;
    float mSizeScale = 1.8f;
    float mSizeScaleTarget = 1.8f;
    float mMixSmoothed = 0.28f;
    float mWidthSmoothed = 1.08f;
    float mOutputGainSmoothed = 1.0f;
    float mOutputGainTarget = 1.0f;
    float mModDepthSamples = 6.0f;
    // Averages the combs and applies kLateMakeupDb; set in Prepare.
    float mLateGain = 1.0f / static_cast<float>(kCombCount);
    float mSmoothCoeff = 0.0f;
    float mSizeSmoothCoeff = 0.0f;
    float mWetToneStateL = 0.0f;
    float mWetToneStateR = 0.0f;
    // LFO held as a rotating unit phasor rather than a wrapped phase accumulator.
    float mLfoSin = 0.0f;
    float mLfoCos = 1.0f;
    float mLfoRotSin = 0.0f;
    float mLfoRotCos = 1.0f;

    // Highpass on the reverb feed — keeps DC and sub-bass out of the comb loop.
    float mInputHpAlpha = 0.995f;
    float mInputHpPrevL = 0.0f;
    float mInputHpPrevR = 0.0f;
    float mInputHpStateL = 0.0f;
    float mInputHpStateR = 0.0f;

    ShimmerReturn mShimmerReturn;
    float mShimmerTarget = 0.0f;
    float mShimmerSmoothed = 0.0f;
    float mFreezeSmoothed = 0.0f;
    float mFreezeCoeff = 1.0f;

    // Precomputed sin/cos of per-comb LFO phase offsets — avoids kCombCount sin() calls per sample.
    std::array<float, kCombCount> mCombPrecompSinOffsets{};
    std::array<float, kCombCount> mCombPrecompCosOffsets{};
};

inline void RegisterAmbientReverbEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbAmbient;
    info.aliases = {"reverb_ambient"};
    info.displayName = "Ambient Reverb";
    info.category = "reverb";
    info.description = "Long, diffuse reverb with slow modulation and wide stereo bloom";
    info.requiresResource = false;
    info.parameters = {{"decay", "Decay", 0.50, 0.0, 1.0, "", "space"},
                       {"space", "Space", 0.72, 0.0, 1.0, "", "space"},
                       {"diffusion", "Diffusion", 0.40, 0.0, 1.0, "", "space"},
                       {"preDelay", "Pre-Delay", 26.0, 0.0, 200.0, "ms", "space"},
                       {"tone", "Tone", 0.42, 0.0, 1.0, "", "tone"},
                       {"width", "Width", 1.08, 0.0, 1.25, "", "tone"},
                       {"modRate", "Mod Rate", 0.18, 0.02, 2.0, "Hz", "modulation"},
                       {"modDepth", "Mod Depth", 0.38, 0.0, 1.0, "", "modulation"},
                       {"mix", "Mix", 0.28, 0.0, 1.0, "", "tone"},
                       {"outputGain", "Output", 0.0, -18.0, 12.0, "dB", "tone", true},
                       {"shimmer", "Shimmer", 0.0, 0.0, 1.0, "", "shimmer"},
                       {"shimmerPitch",
                        "Shimmer Pitch",
                        0.0,
                        0.0,
                        3.0,
                        "enum",
                        "shimmer",
                        false,
                        1.0,
                        {"Octave Up", "Fifth Up", "Octave + Fifth", "Octave Down"}},
                       {"freeze", "Freeze", 0.0, 0.0, 1.0, "toggle", "space", false, 1.0}};
    info.presets = reverb_presets::Ambient(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<AmbientReverbEffect>(); });
}
} // namespace guitarfx