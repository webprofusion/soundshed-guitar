#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/SimdFFT.h"
#include "dsp/IRTypes.h"
#include "dsp/IRWavLoader.h"
#include "dsp/ImpulseResampler.h"
#include "dsp/IrLeadingSilence.h"
#include "dsp/IrSlotAlignment.h"
#include "dsp/effects/SpeakerDrive.h"
#include "util/PathEncoding.h"
#include "util/SessionLog.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>
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
    /// Keeps its channels apart once dual mono switches off the slot pan's fold to mono
    /// (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void SetDualMono(bool dualMono) override
    {
        mDualMono.store(dualMono, std::memory_order_relaxed);
    }

    /// A mono input holds the same samples on both sides (the executor makes it so), which lets a
    /// mono IR convolve once for both (ProcessSlotPair).
    void SetInputLayout(ChannelLayout layout) override
    {
        mInputMono.store(layout == ChannelLayout::Mono, std::memory_order_relaxed);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!mDualLaneLease.IsHeld())
        {
            mDualLaneLease = rtparallel::DualLaneExecutor::Acquire(); // here, so Process() never starts its thread
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mPrepared = true;
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
        mSpeakerDrive.Prepare(sampleRate);
        mAlignment.SetBothSlotsLoaded(!mImpulseL.empty() && !mImpulseBL.empty());
        mAlignment.Prepare(sampleRate, maxBlockSize);

        mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
        mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
        mDrivenBufferL.resize(static_cast<size_t>(maxBlockSize));
        mDrivenBufferR.resize(static_cast<size_t>(maxBlockSize));
        mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
        mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
        mOutputBufferBL.resize(static_cast<size_t>(maxBlockSize));
        mOutputBufferBR.resize(static_cast<size_t>(maxBlockSize));
        mPrevOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
        mPrevOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
        mPrevOutputBufferBL.resize(static_cast<size_t>(maxBlockSize));
        mPrevOutputBufferBR.resize(static_cast<size_t>(maxBlockSize));

        ApplyPendingRebuildSettings();

        // Reinitialize convolvers if we have IRs loaded
        if (!mImpulseL.empty())
        {
            InitializeConvolverA();
        }

        if (!mImpulseBL.empty())
        {
            InitializeConvolverB();
        }

        RefreshBlendCorrelation();
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
        mSpeakerDrive.Reset();
        mAlignment.Reset();
        mResourceTransitionSamplesRemaining = 0;
        mPrevHasSlotA = false;
        mPrevHasSlotB = false;
        // Both sides are cleared alike.
        mRightStaleA = false;
        mRightStaleB = false;
        mSameInputA = kSidesAlike;
        mSameInputB = kSidesAlike;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
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
            {
                std::fill_n(outputs[0], numSamples, 0.0f);
            }

            if (outputs[1])
            {
                std::fill_n(outputs[1], numSamples, 0.0f);
            }

            return;
        }

        // Either slot can play alone (BlendedSlotMix), so the cab is ready with either.
        const bool hasA = SlotAReady();
        const bool hasB = SlotBReady();

        // Before Prepare nothing is built and the buffers are empty: pass the input through.
        if (!mPrepared)
        {
            if (outputs[0])
            {
                if (inputs[0])
                {
                    std::copy_n(inputs[0], numSamples, outputs[0]);
                }
                else
                {
                    std::fill_n(outputs[0], numSamples, 0.0f);
                }
            }

            if (outputs[1])
            {
                if (inputs[1])
                {
                    std::copy_n(inputs[1], numSamples, outputs[1]);
                }
                else if (inputs[0])
                {
                    std::copy_n(inputs[0], numSamples, outputs[1]);
                }
                else
                {
                    std::fill_n(outputs[1], numSamples, 0.0f);
                }
            }

            return;
        }

        // Copy to float buffers
        for (int i = 0; i < numSamples; ++i)
        {
            mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0f;
            mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0f);
        }

        // The clean dry signal. The convolvers have no latency, so it needs no delay to line up
        // with the wet one below 100% Mix.
        const float* dryInL = mInputBufferL.data();
        const float* dryInR = mInputBufferR.data();

        // Full bypass (disabled or mix=0), or no IR loaded: the dry signal alone. The effect is
        // intentionally transparent when no cabinet IR has been selected.
        if (!mEnabled || mMix <= 0.0 || (!hasA && !hasB))
        {
            // The speaker cools while nothing reaches it. Held over the bypass, its compression
            // would come back still holding down a loud passage from before it.
            if (mSpeakerDrive.IsActive())
            {
                mSpeakerDrive.Reset();
            }

            if (outputs[0])
            {
                std::copy_n(dryInL, numSamples, outputs[0]);
            }

            if (outputs[1])
            {
                std::copy_n(dryInR, numSamples, outputs[1]);
            }

            return;
        }

        // Speaker drive bends what reaches the IR, as a pushed speaker would; the dry path
        // mixed in later stays clean, so the driven signal goes to its own buffers.
        const float* irInL = mInputBufferL.data();
        const float* irInR = mInputBufferR.data();

        if (mSpeakerDrive.IsActive())
        {
            for (int i = 0; i < numSamples; ++i)
            {
                mDrivenBufferL[i] = static_cast<float>(mSpeakerDrive.Process(mInputBufferL[i], 0));
                mDrivenBufferR[i] = static_cast<float>(mSpeakerDrive.Process(mInputBufferR[i], 1));
                mSpeakerDrive.AdvanceSample();
            }

            irInL = mDrivenBufferL.data();
            irInR = mDrivenBufferR.data();
        }

        // Process through convolvers
        const bool allowParallel = rtparallel::ShouldParallelizeStereoWork(numSamples);
        // L/R split: slot A takes left input, slot B takes right input (requires both slots loaded).
        const bool useLRSplit = mLRSplitEnabled && hasA && hasB;

        // Each slot reads its own copy of the input, the later one delayed by the alignment offset.
        mAlignment.SetBothSlotsLoaded(hasA && hasB);
        mAlignment.Process(irInL, irInR, numSamples);
        const float* inAL = mAlignment.SlotInput(0, 0);
        const float* inAR = mAlignment.SlotInput(0, 1);
        const float* inBL = mAlignment.SlotInput(1, 0);
        const float* inBR = mAlignment.SlotInput(1, 1);

        if (useLRSplit)
        {
            // The right convolvers sit idle here, so their history no longer follows the left's.
            mSameInputA = 0;
            mSameInputB = 0;

            // Run slot A (left) and slot B (right) in parallel
            bool ran = false;

            if (allowParallel)
            {
                ran = rtparallel::DualLaneExecutor::Instance().Run(
                    [&]() { mConvolverBL.Process(inBR, mOutputBufferBL.data(), numSamples); },
                    [&]() { mConvolverL.Process(inAL, mOutputBufferL.data(), numSamples); });
            }

            if (!ran)
            {
                mConvolverL.Process(inAL, mOutputBufferL.data(), numSamples);
                mConvolverBL.Process(inBR, mOutputBufferBL.data(), numSamples);
            }
        }
        else
        {
            // A mono input through a mono IR: both sides would convolve the same samples with the
            // same impulse, so the right takes the left's result, bit for bit, at half the cost.
            const bool monoInput = mInputMono.load(std::memory_order_relaxed);

            if (hasA)
            {
                ProcessSlotPair(mConvolverL, mConvolverR, mRightStaleA, mSameInputA,
                                monoInput && IsMonoImpulse(mIsStereo, mImpulseR), inAL, inAR, mOutputBufferL.data(),
                                mOutputBufferR.data(), numSamples, allowParallel);
            }

            if (hasB)
            {
                ProcessSlotPair(mConvolverBL, mConvolverBR, mRightStaleB, mSameInputB,
                                monoInput && IsMonoImpulse(mIsStereoB, mImpulseBR), inBL, inBR, mOutputBufferBL.data(),
                                mOutputBufferBR.data(), numSamples, allowParallel);
            }
        }

        // The outgoing side of a crossfade: the slots that were playing, either or both. Read once,
        // since the fade can end partway through the block.
        const bool prevHasA = mPrevHasSlotA;
        const bool prevHasB = mPrevHasSlotB;
        const bool transitionActive = mResourceTransitionSamplesRemaining > 0 && (prevHasA || prevHasB);

        if (transitionActive)
        {
            if (useLRSplit)
            {
                // Mirror the live split routing for the fade-out of the previous state.
                bool ran = false;

                if (allowParallel)
                {
                    ran = rtparallel::DualLaneExecutor::Instance().Run(
                        [&]() {
                            if (prevHasB)
                            {
                                mPrevConvolverBL.Process(inBR, mPrevOutputBufferBL.data(), numSamples);
                            }
                        },
                        [&]() {
                            if (prevHasA)
                            {
                                mPrevConvolverL.Process(inAL, mPrevOutputBufferL.data(), numSamples);
                            }
                        });
                }

                if (!ran)
                {
                    if (prevHasA)
                    {
                        mPrevConvolverL.Process(inAL, mPrevOutputBufferL.data(), numSamples);
                    }

                    if (prevHasB)
                    {
                        mPrevConvolverBL.Process(inBR, mPrevOutputBufferBL.data(), numSamples);
                    }
                }
            }
            else
            {
                if (prevHasA)
                {
                    bool ranPrevParallel = false;

                    if (allowParallel)
                    {
                        ranPrevParallel = rtparallel::DualLaneExecutor::Instance().Run(
                            [&]() { mPrevConvolverR.Process(inAR, mPrevOutputBufferR.data(), numSamples); },
                            [&]() { mPrevConvolverL.Process(inAL, mPrevOutputBufferL.data(), numSamples); });
                    }

                    if (!ranPrevParallel)
                    {
                        mPrevConvolverL.Process(inAL, mPrevOutputBufferL.data(), numSamples);
                        mPrevConvolverR.Process(inAR, mPrevOutputBufferR.data(), numSamples);
                    }
                }

                if (prevHasB)
                {
                    bool ranPrevBParallel = false;

                    if (allowParallel)
                    {
                        ranPrevBParallel = rtparallel::DualLaneExecutor::Instance().Run(
                            [&]() { mPrevConvolverBR.Process(inBR, mPrevOutputBufferBR.data(), numSamples); },
                            [&]() { mPrevConvolverBL.Process(inBL, mPrevOutputBufferBL.data(), numSamples); });
                    }

                    if (!ranPrevBParallel)
                    {
                        mPrevConvolverBL.Process(inBL, mPrevOutputBufferBL.data(), numSamples);
                        mPrevConvolverBR.Process(inBR, mPrevOutputBufferBR.data(), numSamples);
                    }
                }
            }
        }

        // Apply wet/dry mix and output gain
        const float wetGain = static_cast<float>(mMix * mOutputGain);
        const float dryGain = static_cast<float>(1.0 - mMix);
        const bool wetOnly = mMix >= 0.9999;

        // In L/R split mode each slot handles one channel at full gain; normal mode uses blend,
        // with a lone IR in full (BlendedSlotMix).
        double slotAGain, slotBGain, autoCompGain;
        // The outgoing side of a crossfade plays at the levels its own slots had: a slot added or
        // cleared fades from what was heard. Auto Gain Comp is applied after the crossfade, so
        // the outgoing levels carry the ratio of its comp to the live one. That holds for a
        // reload with the same slots too, since the comp depends on how alike the two IRs are.
        double prevSlotAGain, prevSlotBGain;

        if (useLRSplit)
        {
            slotAGain = mSlotAGain * (mSlotAPolarityInverted ? -1.0 : 1.0);
            slotBGain = mSlotBGain * (mSlotBPolarityInverted ? -1.0 : 1.0);
            autoCompGain = 1.0;
            prevSlotAGain = slotAGain;
            prevSlotBGain = slotBGain;
        }
        else
        {
            // Where IR B sits against IR A this block, mid-glide included: how alike the two
            // sound depends on it.
            const double offsetSamples = mAlignment.CurrentOffsetSamples();
            const SlotMix live = BlendedSlotMix(hasA, hasB, mBlendCorrelation, offsetSamples);
            slotAGain = live.slotAGain;
            slotBGain = live.slotBGain;
            autoCompGain = live.compGain;
            prevSlotAGain = slotAGain;
            prevSlotBGain = slotBGain;

            if (transitionActive)
            {
                const SlotMix outgoing = BlendedSlotMix(prevHasA, prevHasB, mPrevBlendCorrelation, offsetSamples);
                const double compRatio = outgoing.compGain / autoCompGain;
                prevSlotAGain = outgoing.slotAGain * compRatio;
                prevSlotBGain = outgoing.slotBGain * compRatio;
            }
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

        const bool hasPanB = (hasB || (transitionActive && prevHasB)) && std::abs(mSlotBPan) > 1e-6;
        const bool dualMono = mDualMono.load(std::memory_order_relaxed);
        double slotBPanL = 1.0, slotBPanR = 1.0;

        if (hasPanB)
        {
            const double angle = (mSlotBPan + 1.0) * kPi / 4.0;
            slotBPanL = kSqrt2 * std::cos(angle);
            slotBPanR = kSqrt2 * std::sin(angle);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // The outgoing side of a crossfade is blended in here, slot by slot, before mic
            // emulation and pan. Blended after them, an outgoing slot panned hard left spent the
            // 30 ms fade centred and uncoloured: a jump in position and tone on every IR change.
            double oldWeight = 0.0;

            if (transitionActive)
            {
                const int progressed = mResourceTransitionSamplesTotal - mResourceTransitionSamplesRemaining;
                const double newWeight =
                    static_cast<double>(progressed) / static_cast<double>(std::max(1, mResourceTransitionSamplesTotal));
                oldWeight = 1.0 - std::clamp(newWeight, 0.0, 1.0);
            }

            const double liveWeight = 1.0 - oldWeight;
            const bool fadeA = transitionActive && prevHasA;
            const bool fadeB = transitionActive && prevHasB;
            double wetL, wetR;

            if (useLRSplit)
            {
                // Slot A → left channel; slot B → right channel.
                wetL = mOutputBufferL[i] * slotAGain * liveWeight;
                wetR = mOutputBufferBL[i] * slotBGain * liveWeight;

                if (fadeA)
                {
                    wetL += mPrevOutputBufferL[i] * prevSlotAGain * oldWeight;
                }

                if (fadeB)
                {
                    wetR += mPrevOutputBufferBL[i] * prevSlotBGain * oldWeight;
                }

                if (mMicEmulationEnabled)
                {
                    wetL = ProcessMicPositionSlotA(wetL, 0);
                    wetR = ProcessMicPositionSlotB(wetR, 1);
                }
            }
            else
            {
                wetL = 0.0;
                wetR = 0.0;

                if (hasA || fadeA)
                {
                    double slotAL = hasA ? mOutputBufferL[i] * slotAGain * liveWeight : 0.0;
                    double slotAR = hasA ? mOutputBufferR[i] * slotAGain * liveWeight : 0.0;

                    if (fadeA)
                    {
                        slotAL += mPrevOutputBufferL[i] * prevSlotAGain * oldWeight;
                        slotAR += mPrevOutputBufferR[i] * prevSlotAGain * oldWeight;
                    }

                    if (mMicEmulationEnabled)
                    {
                        slotAL = ProcessMicPositionSlotA(slotAL, 0);
                        slotAR = ProcessMicPositionSlotA(slotAR, 1);
                    }

                    if (hasPanA && dualMono)
                    {
                        // Dual mono: a balance, each side kept to itself.
                        slotAL *= slotAPanL;
                        slotAR *= slotAPanR;
                    }
                    else if (hasPanA)
                    {
                        const double monoA = (slotAL + slotAR) * 0.5;
                        slotAL = monoA * slotAPanL;
                        slotAR = monoA * slotAPanR;
                    }

                    wetL = slotAL;
                    wetR = slotAR;
                }

                if (hasB || fadeB)
                {
                    double slotBL = hasB ? mOutputBufferBL[i] * slotBGain * liveWeight : 0.0;
                    double slotBR = hasB ? mOutputBufferBR[i] * slotBGain * liveWeight : 0.0;

                    if (fadeB)
                    {
                        slotBL += mPrevOutputBufferBL[i] * prevSlotBGain * oldWeight;
                        slotBR += mPrevOutputBufferBR[i] * prevSlotBGain * oldWeight;
                    }

                    if (mMicEmulationEnabled)
                    {
                        slotBL = ProcessMicPositionSlotB(slotBL, 0);
                        slotBR = ProcessMicPositionSlotB(slotBR, 1);
                    }

                    if (hasPanB && dualMono)
                    {
                        slotBL *= slotBPanL;
                        slotBR *= slotBPanR;
                    }
                    else if (hasPanB)
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
                // transitionActive holds for the whole block, so the fade can end partway through
                // it: count down to zero and clear the outgoing side once, on the sample it ends.
                // Counting on past zero cleared all four convolvers again on every sample left.
                if (mResourceTransitionSamplesRemaining > 0 && --mResourceTransitionSamplesRemaining == 0)
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
                {
                    outputs[0][i] = static_cast<float>(wetL) * wetGain;
                }
                else
                {
                    outputs[0][i] = static_cast<float>(wetL) * wetGain + dryInL[i] * dryGain;
                }
            }

            if (outputs[1])
            {
                if (wetOnly)
                {
                    outputs[1][i] = static_cast<float>(wetR) * wetGain;
                }
                else
                {
                    outputs[1][i] = static_cast<float>(wetR) * wetGain + dryInR[i] * dryGain;
                }
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "irBlend")
        {
            mIRBlend = std::clamp(value, 0.0, 1.0);
        }
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
        {
            mSlotAGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
        }
        else if (key == "slotBGain")
        {
            mSlotBGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
        }
        else if (key == "slotAPan")
        {
            mSlotAPan = std::clamp(value, -1.0, 1.0);
        }
        else if (key == "slotBPan")
        {
            mSlotBPan = std::clamp(value, -1.0, 1.0);
        }
        else if (key == "slotBOffset")
        {
            mAlignment.SetOffsetMs(value);
        }
        else if (key == "lrSplit")
        {
            mLRSplitEnabled = value > 0.5;
        }
        else if (key == "slotAPolarity")
        {
            mSlotAPolarityInverted = value > 0.5;
        }
        else if (key == "slotBPolarity")
        {
            mSlotBPolarityInverted = value > 0.5;
        }
        else if (key == "autoGainComp")
        {
            mAutoGainCompEnabled = value > 0.5;
        }
        else if (key == "normalizeIR")
        {
            // The normalisation gain is baked into the convolver coefficients.
            RequestRebuildSetting(mPendingNormalizeIR, mNormalizeIR, value > 0.5);
        }
        else if (key == "outputGain")
        {
            mOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
        }
        else if (key == "enabled")
        {
            mEnabled = value > 0.5;
        }
        else if (key == "quality")
        {
            // Where the IR is truncated, and so its normalisation gain, is built into the convolvers.
            RequestRebuildSetting(mPendingQuality, mQuality,
                                  static_cast<IRQuality>(static_cast<int>(std::clamp(value, 0.0, 3.0))));
        }
        else if (key == "trimLeadingSilence")
        {
            // The trim is taken from the impulses the convolvers are built from.
            RequestRebuildSetting(mPendingTrimLeadingSilence, mTrimLeadingSilence, value > 0.5);
        }
        else if (key == "air")
        {
            mAir = std::clamp(value, 0.0, 1.0);
            UpdateAirCoefficients();
        }
        else if (key == "speakerDrive")
        {
            mSpeakerDrive.SetDrive(value);
        }
        else if (key == "airMode")
        {
            const int mode = static_cast<int>(std::clamp(value, 0.0, 2.0));
            mAirMode = static_cast<AirMode>(mode);
            ResetAirState();
        }
        else if (key == "micEmulation")
        {
            mMicEmulationEnabled = value > 0.5;
        }
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

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    /// A Quality, Normalize or Trim Leading Silence change SetParam recorded (see
    /// RequestRebuildSetting), with copies of the impulses to build from: an IR load could replace
    /// them while the build runs.
    [[nodiscard]] std::unique_ptr<DeferredRebuild> TakeDeferredRebuild() override
    {
        if (mPendingQuality.load(std::memory_order_acquire) < 0 &&
            mPendingNormalizeIR.load(std::memory_order_acquire) < 0 &&
            mPendingTrimLeadingSilence.load(std::memory_order_acquire) < 0)
        {
            return nullptr;
        }

        auto work = std::make_unique<ConvolverRebuild>();
        work->generation = mBuildGeneration;
        work->settings = {mSampleRate, mMaxBlockSize, EffectiveQuality(),
                          EffectiveSetting(mPendingNormalizeIR, mNormalizeIR),
                          EffectiveSetting(mPendingTrimLeadingSilence, mTrimLeadingSilence)};
        work->slotA = {mImpulseL, mImpulseR, mIsStereo, mIRSampleRate};
        work->slotB = {mImpulseBL, mImpulseBR, mIsStereoB, mIRSampleRateB};
        return work;
    }

    void CommitDeferredRebuild(DeferredRebuild& taken) override
    {
        auto* work = dynamic_cast<ConvolverRebuild*>(&taken);

        // An IR load or a Prepare since the work was taken rebuilt the convolvers itself, with the
        // waiting settings folded in. A change requested while it was built supersedes it, and
        // that SetParam has already asked for another pass.
        if (!work || work->generation != mBuildGeneration || EffectiveQuality() != work->settings.quality ||
            EffectiveSetting(mPendingNormalizeIR, mNormalizeIR) != work->settings.normalize ||
            EffectiveSetting(mPendingTrimLeadingSilence, mTrimLeadingSilence) != work->settings.trimLeadingSilence)
        {
            return;
        }

        // What is playing becomes the outgoing side of the crossfade as it stands, history and
        // all, and the rebuilt convolvers go live. Swaps rather than assignments, so the outgoing
        // convolvers they displace end up in the work and are freed off the lock.
        SyncRightConvolvers();
        mPrevHasSlotA = false;
        mPrevHasSlotB = false;

        if (!work->slotA.left.empty())
        {
            mPrevHasSlotA = mConvolverL.IsInitialized() && mConvolverR.IsInitialized();
            SwapIn(mConvolverL, mPrevConvolverL, work->convolverL);
            SwapIn(mConvolverR, mPrevConvolverR, work->convolverR);
        }

        if (!work->slotB.left.empty())
        {
            mPrevHasSlotB = mConvolverBL.IsInitialized() && mConvolverBR.IsInitialized();
            SwapIn(mConvolverBL, mPrevConvolverBL, work->convolverBL);
            SwapIn(mConvolverBR, mPrevConvolverBR, work->convolverBR);
        }

        mNormalizeIR = work->settings.normalize;
        mTrimLeadingSilence = work->settings.trimLeadingSilence;
        mQuality = work->settings.quality;
        ClearPendingIf(mPendingNormalizeIR, mNormalizeIR ? 1 : 0);
        ClearPendingIf(mPendingTrimLeadingSilence, mTrimLeadingSilence ? 1 : 0);
        ClearPendingIf(mPendingQuality, static_cast<int>(mQuality));

        // The pair's measurement goes with its convolvers, swapped the same way so the one
        // displaced is freed with the work.
        std::swap(mPrevBlendCorrelation, mBlendCorrelation);
        std::swap(mBlendCorrelation, work->blendCorrelation);
        BeginResourceTransition();
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "mix")
        {
            return mMix;
        }

        if (key == "irBlend")
        {
            return mIRBlend;
        }

        if (key == "lowCutHz")
        {
            return mLowCutHz;
        }

        if (key == "highCutHz")
        {
            return mHighCutHz;
        }

        if (key == "slotAGain")
        {
            return 20.0 * std::log10(std::max(mSlotAGain, 1e-9));
        }

        if (key == "slotBGain")
        {
            return 20.0 * std::log10(std::max(mSlotBGain, 1e-9));
        }

        if (key == "slotAPan")
        {
            return mSlotAPan;
        }

        if (key == "slotBPan")
        {
            return mSlotBPan;
        }

        if (key == "slotBOffset")
        {
            return mAlignment.GetOffsetMs();
        }

        if (key == "lrSplit")
        {
            return mLRSplitEnabled ? 1.0 : 0.0;
        }

        if (key == "slotAPolarity")
        {
            return mSlotAPolarityInverted ? 1.0 : 0.0;
        }

        if (key == "slotBPolarity")
        {
            return mSlotBPolarityInverted ? 1.0 : 0.0;
        }

        if (key == "autoGainComp")
        {
            return mAutoGainCompEnabled ? 1.0 : 0.0;
        }

        if (key == "normalizeIR")
        {
            return EffectiveSetting(mPendingNormalizeIR, mNormalizeIR) ? 1.0 : 0.0;
        }

        if (key == "outputGain")
        {
            return 20.0 * std::log10(mOutputGain);
        }

        if (key == "enabled")
        {
            return mEnabled ? 1.0 : 0.0;
        }

        if (key == "quality")
        {
            return static_cast<double>(EffectiveQuality());
        }

        if (key == "trimLeadingSilence")
        {
            return EffectiveSetting(mPendingTrimLeadingSilence, mTrimLeadingSilence) ? 1.0 : 0.0;
        }

        if (key == "air")
        {
            return mAir;
        }

        if (key == "speakerDrive")
        {
            return mSpeakerDrive.GetDrive();
        }

        if (key == "airMode")
        {
            return static_cast<double>(mAirMode);
        }

        if (key == "micEmulation")
        {
            return mMicEmulationEnabled ? 1.0 : 0.0;
        }

        if (key == "micRadialA")
        {
            return mMicRadialA;
        }

        if (key == "micProximityA")
        {
            return mMicProximityA;
        }

        if (key == "micRadialB")
        {
            return mMicRadialB;
        }

        if (key == "micProximityB")
        {
            return mMicProximityB;
        }

        return 0.0;
    }

    bool LoadResource(const std::filesystem::path& resourcePath) override
    {
        if (!util::PathExists(resourcePath))
        {
            util::AppendSessionLog("[IRCabEffect] ERROR: IR file not found: " + util::PathToUtf8(resourcePath));
            return false;
        }

        // Single-resource load path (legacy): load slot A and clear slot B.
        if (!LoadWavFile(resourcePath))
        {
            util::AppendSessionLog("[IRCabEffect] ERROR: Failed to load/parse IR WAV file: " +
                                   util::PathToUtf8(resourcePath));
            return false;
        }

        // After the parse, so a file that will not load leaves the cab playing what it was.
        CapturePreviousConvolvers();
        mIRPath = resourcePath;
        mImpulseBL.clear();
        mImpulseBR.clear();
        mIRPathB.clear();
        UnloadConvolverPair(mConvolverBL, mConvolverBR);
        ApplyPendingRebuildSettings();
        const bool loaded = !mPrepared || InitializeConvolverA(); // else Prepare() builds it (see mPrepared)
        RefreshBlendCorrelation();

        if (!loaded)
        {
            util::AppendSessionLog("[IRCabEffect] ERROR: Failed to initialize convolver for: " +
                                   util::PathToUtf8(resourcePath));
            return false;
        }

        BeginResourceTransition();
        return true;
    }

    bool LoadResources(const std::vector<ResourceRef>& refs, const std::vector<std::filesystem::path>& paths) override
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

                if (slotIndex == 0)
                {
                    slotAIdx = static_cast<int>(i);
                }
                else if (slotIndex == 1)
                {
                    slotBIdx = static_cast<int>(i);
                }
            }
        }

        if (!hasSlotMetadata)
        {
            if (!paths.empty())
            {
                slotAIdx = 0;
            }

            if (paths.size() >= 2)
            {
                slotBIdx = 1;
            }
        }

        CapturePreviousConvolvers();
        ApplyPendingRebuildSettings();

        // --- Slot A ---
        bool loadedA = false;

        if (slotAIdx >= 0)
        {
            if (!util::PathExists(paths[slotAIdx]))
            {
                util::AppendSessionLog("[IRCabEffect] ERROR: IR A file not found: " +
                                       util::PathToUtf8(paths[slotAIdx]));
            }
            else if (LoadWavFile(paths[slotAIdx]))
            {
                mIRPath = paths[slotAIdx];
                loadedA = true; // built below, once IR B is in
            }
            else
            {
                util::AppendSessionLog("[IRCabEffect] ERROR: Failed to load IR A: " +
                                       util::PathToUtf8(paths[slotAIdx]));
            }
        }

        // --- Slot B ---
        mImpulseBL.clear();
        mImpulseBR.clear();
        mIRPathB.clear();
        UnloadConvolverPair(mConvolverBL, mConvolverBR);
        bool loadedB = false;

        if (slotBIdx >= 0)
        {
            if (!util::PathExists(paths[slotBIdx]))
            {
                util::AppendSessionLog("[IRCabEffect] WARNING: IR B file not found: " +
                                       util::PathToUtf8(paths[slotBIdx]));
            }
            else if (LoadWavFileInto(paths[slotBIdx], mImpulseBL, mImpulseBR, mIRSampleRateB, mIsStereoB))
            {
                mIRPathB = paths[slotBIdx];

                if (slotBIdx < static_cast<int>(refs.size()) && refs[slotBIdx].parameterValue.has_value())
                {
                    mIRBlend = std::clamp(*refs[slotBIdx].parameterValue, 0.0, 1.0);
                }

                loadedB = true;
            }
            else
            {
                util::AppendSessionLog("[IRCabEffect] WARNING: Failed to load IR B: " +
                                       util::PathToUtf8(paths[slotBIdx]));
            }
        }

        // Both files are read before either convolver is built: the leading-silence trim reads
        // both slots' impulses, so each build has to see the pair that will play.
        if (loadedA)
        {
            loadedA = !mPrepared || InitializeConvolverA(); // else Prepare() builds it (see mPrepared)

            if (!loadedA)
            {
                util::AppendSessionLog("[IRCabEffect] ERROR: Failed to initialize convolver A for: " +
                                       util::PathToUtf8(paths[slotAIdx]));
            }
        }

        if (!loadedA)
        {
            mImpulseL.clear();
            mImpulseR.clear();
            mIRPath.clear();
            UnloadConvolverPair(mConvolverL, mConvolverR);
        }

        if (loadedB)
        {
            loadedB = !mPrepared || InitializeConvolverB();

            if (!loadedB)
            {
                util::AppendSessionLog("[IRCabEffect] WARNING: Failed to initialize convolver B for: " +
                                       util::PathToUtf8(paths[slotBIdx]));
            }
        }

        RefreshBlendCorrelation();
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
        return SlotAReady() || SlotBReady();
    }

    [[nodiscard]] std::filesystem::path GetResourcePath() const override
    {
        return mIRPath;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "cab_ir";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "cab";
    }

    // ── The two slots' sum, described ─────────────────────────────────────────
    // The filter designs below are the ones the cab plays (the Update*Coefficients functions
    // unpack them), and DescribeSlotSum says how Process puts the two slots together. The IR
    // alignment panel's response (dsp/IrPairResponse.h) is worked out from them, so the curve it
    // draws cannot drift from what the cab plays.

    /// Mic Position on one slot: an off-axis HF shelf (on-axis 0 dB to off-axis -12 dB at 4 kHz)
    /// and a close-mic low-mid peak (far 0 dB to close +6 dB at 150 Hz).
    struct MicPositionFilters
    {
        BiquadCoefficients shelf;
        BiquadCoefficients proximity;
    };

    [[nodiscard]] static MicPositionFilters DesignMicPosition(double radial, double proximity, double sampleRate)
    {
        const double shelfGainDb = radial * -12.0;
        return {biquad::HighShelf(4000.0, biquad::ShelfQFromSlope(0.7, shelfGainDb), shelfGainDb, sampleRate),
                biquad::Peaking(150.0, 1.0, proximity * 6.0, sampleRate)};
    }

    /// Air: a high-shelf lift in the air band (up to +12 dB at 7 kHz) and a high-mid presence
    /// peak (up to +8 dB at 3.6 kHz); Air Mode picks one or both. Off below a trace of Air.
    struct AirFilters
    {
        bool active = false;
        BiquadCoefficients shelf;
        BiquadCoefficients peak;
    };

    [[nodiscard]] static AirFilters DesignAir(double air, double sampleRate)
    {
        const double clamped = std::clamp(air, 0.0, 1.0);

        if (sampleRate <= 0.0 || clamped <= 0.0001)
        {
            return {};
        }

        const double shelfGainDb = clamped * 12.0;
        return {true, biquad::HighShelf(7000.0, biquad::ShelfQFromSlope(0.7, shelfGainDb), shelfGainDb, sampleRate),
                biquad::Peaking(3600.0, 1.8, clamped * 8.0, sampleRate)};
    }

    /// Low Cut and High Cut: a Butterworth section each, off at its end of the range.
    struct CabFilters
    {
        bool lowCutActive = false;
        bool highCutActive = false;
        BiquadCoefficients lowCut;
        BiquadCoefficients highCut;
    };

    [[nodiscard]] static CabFilters DesignCabFilters(double lowCutHz, double highCutHz, double sampleRate)
    {
        CabFilters cuts;

        if (sampleRate <= 0.0)
        {
            return cuts;
        }

        const double nyquist = sampleRate * 0.5;
        cuts.lowCutActive = lowCutHz > 20.5;
        cuts.highCutActive = (highCutHz < 19999.5) && (highCutHz < (nyquist - 100.0));

        if (cuts.lowCutActive)
        {
            cuts.lowCut =
                biquad::HighPass(std::clamp(lowCutHz, 20.0, nyquist - 100.0), biquad::kButterworthQ, sampleRate);
        }

        if (cuts.highCutActive)
        {
            cuts.highCut =
                biquad::LowPass(std::clamp(highCutHz, 1000.0, nyquist - 100.0), biquad::kButterworthQ, sampleRate);
        }

        return cuts;
    }

    /// What Auto Gain Comp needs to know about the two IRs it blends: each one's energy, and how
    /// alike they are (their cross-correlation) at every IR B Offset. Measured when the
    /// convolvers are built, never on the audio thread, which only reads it.
    struct BlendCorrelation
    {
        double energyA = 0.0;
        double energyB = 0.0;
        int maxLag = 0;            // whole samples either side of no offset
        std::vector<double> cross; // the sum of a[n] * b[n - lag], at cross[lag + maxLag]

        [[nodiscard]] bool Valid() const noexcept
        {
            return !cross.empty() && energyA > 0.0 && energyB > 0.0;
        }

        /// The cross term with IR B `lagSamples` later than IR A, linear between whole lags.
        [[nodiscard]] double CrossAt(double lagSamples) const noexcept
        {
            const double reach = static_cast<double>(maxLag);
            const double position = std::clamp(lagSamples, -reach, reach) + reach;
            const auto below = static_cast<size_t>(position);
            const size_t above = std::min(below + 1, cross.size() - 1);
            const double fraction = position - static_cast<double>(below);
            return cross[below] + fraction * (cross[above] - cross[below]);
        }
    };

    /// Two mono impulses measured as Auto Gain Comp measures the slots' built ones, at `sampleRate`.
    [[nodiscard]] static BlendCorrelation MeasureBlendCorrelation(const std::vector<float>& monoA,
                                                                  const std::vector<float>& monoB, double sampleRate)
    {
        return ComputeBlendCorrelation(BuiltImpulse{monoA, {}}, BuiltImpulse{monoB, {}}, sampleRate);
    }

    /// How Process sums the slots outside L/R Split, with this instance's parameters: each slot's
    /// gain into the sum (IR Blend, Level, Invert; a lone IR in full) and the filters it alone goes
    /// through (Mic Position, when on), then Auto Gain Comp and the filters on the sum (Low Cut,
    /// High Cut, Air). `correlation` is the pair's, and `offsetSamples` where IR B sits against
    /// IR A at the rate the instance was prepared at. Speaker Drive, which depends on level, is
    /// left out, as the offline analyses leave it out.
    struct SlotSum
    {
        bool normalize = true; // whether each slot's impulse plays normalised (Normalize)
        double slotAGain = 0.0;
        double slotBGain = 0.0;
        double compGain = 1.0;
        std::vector<BiquadCoefficients> slotAFilters;
        std::vector<BiquadCoefficients> slotBFilters;
        std::vector<BiquadCoefficients> sumFilters;
    };

    [[nodiscard]] SlotSum DescribeSlotSum(bool hasA, bool hasB, const BlendCorrelation& correlation,
                                          double offsetSamples) const
    {
        SlotSum sum;
        const SlotMix mix = BlendedSlotMix(hasA, hasB, correlation, offsetSamples);
        sum.normalize = EffectiveSetting(mPendingNormalizeIR, mNormalizeIR);
        sum.slotAGain = mix.slotAGain;
        sum.slotBGain = mix.slotBGain;
        sum.compGain = mix.compGain;

        if (mMicEmulationEnabled)
        {
            const MicPositionFilters micA = DesignMicPosition(mMicRadialA, mMicProximityA, mSampleRate);
            const MicPositionFilters micB = DesignMicPosition(mMicRadialB, mMicProximityB, mSampleRate);
            sum.slotAFilters = {micA.shelf, micA.proximity};
            sum.slotBFilters = {micB.shelf, micB.proximity};
        }

        const CabFilters cuts = DesignCabFilters(mLowCutHz, mHighCutHz, mSampleRate);

        if (cuts.lowCutActive)
        {
            sum.sumFilters.push_back(cuts.lowCut);
        }

        if (cuts.highCutActive)
        {
            sum.sumFilters.push_back(cuts.highCut);
        }

        if (const AirFilters air = DesignAir(mAir, mSampleRate); air.active)
        {
            if (mAirMode != AirMode::OptionB_Peak)
            {
                sum.sumFilters.push_back(air.shelf);
            }

            if (mAirMode != AirMode::OptionA_Shelf)
            {
                sum.sumFilters.push_back(air.peak);
            }
        }

        return sum;
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    // Host rate the IR normalisation gain is anchored to (see ComputeL2NormGain).
    static constexpr double kNormalizationReferenceRate = 48000.0;

    // WAV parsing utilities (shared)

    // Find optimal truncation point based on energy threshold
    static size_t FindEnergyTruncationPoint(const std::vector<float>& samples, float threshold = 0.001f)
    {
        if (samples.empty())
        {
            return 0;
        }

        double totalEnergy = 0.0;

        for (const float s : samples)
        {
            totalEnergy += static_cast<double>(s) * static_cast<double>(s);
        }

        if (totalEnergy < 1e-10)
        {
            return samples.size();
        }

        const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
        double cumulativeEnergy = 0.0;

        for (size_t i = 0; i < samples.size(); ++i)
        {
            cumulativeEnergy += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);

            if (cumulativeEnergy >= targetEnergy)
            {
                return std::min(i + 256, samples.size()); // Add small buffer after threshold
            }
        }

        return samples.size();
    }

    static size_t FindEnergyTruncationPointStereo(const std::vector<float>& left, const std::vector<float>& right,
                                                  float threshold = 0.001f)
    {
        const size_t length = std::min(left.size(), right.size());

        if (length == 0)
        {
            return 0;
        }

        double totalEnergy = 0.0;

        for (size_t i = 0; i < length; ++i)
        {
            totalEnergy += static_cast<double>(left[i]) * static_cast<double>(left[i]);
            totalEnergy += static_cast<double>(right[i]) * static_cast<double>(right[i]);
        }

        if (totalEnergy < 1e-10)
        {
            return length;
        }

        const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
        double cumulativeEnergy = 0.0;

        for (size_t i = 0; i < length; ++i)
        {
            cumulativeEnergy += static_cast<double>(left[i]) * static_cast<double>(left[i]);
            cumulativeEnergy += static_cast<double>(right[i]) * static_cast<double>(right[i]);

            if (cumulativeEnergy >= targetEnergy)
            {
                return std::min(i + 256, length); // Add small buffer after threshold
            }
        }

        return length;
    }

    // Get processed (potentially truncated) IR based on quality setting. `leadingSamples` is how
    // much of the front the leading-silence trim will take off afterwards, at the IR's rate: the
    // quality's length is counted from there.
    static std::vector<float> GetProcessedImpulse(const std::vector<float>& samples, double impulseSampleRate,
                                                  IRQuality quality, double hostSampleRate, size_t leadingSamples = 0)
    {
        if (samples.empty())
        {
            return {};
        }

        // For Full quality, return the complete IR
        if (quality == IRQuality::Full)
        {
            return samples;
        }

        const double qualitySampleRate = impulseSampleRate > 0.0 ? impulseSampleRate : hostSampleRate;
        const size_t qualitySamples = GetMaxIRSamples(quality, qualitySampleRate);
        const size_t maxSamples = qualitySamples > 0 ? qualitySamples + leadingSamples : 0;

        if (maxSamples == 0 || samples.size() <= maxSamples)
        {
            return samples;
        }

        // Find smart truncation point based on energy
        const size_t energyTruncPoint = FindEnergyTruncationPoint(samples, 0.001f);
        const size_t truncLength = std::min({samples.size(), maxSamples, energyTruncPoint});
        std::vector<float> truncated(samples.begin(), samples.begin() + truncLength);
        ApplyTruncationFade(truncated);
        return truncated;
    }

    /// GetProcessedImpulse for a stereo IR: both channels are cut at one point, found from their
    /// joint energy, with one fade. Cut apart, each at its own point, the longer channel was then
    /// trimmed to the shorter's length with no fade: cut off mid-signal.
    static void GetProcessedImpulseStereo(const std::vector<float>& left, const std::vector<float>& right,
                                          double impulseSampleRate, IRQuality quality, double hostSampleRate,
                                          size_t leadingSamples, std::vector<float>& outLeft,
                                          std::vector<float>& outRight)
    {
        const size_t length = std::min(left.size(), right.size());
        outLeft.assign(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(length));
        outRight.assign(right.begin(), right.begin() + static_cast<std::ptrdiff_t>(length));

        if (length == 0 || quality == IRQuality::Full)
        {
            return;
        }

        const double qualitySampleRate = impulseSampleRate > 0.0 ? impulseSampleRate : hostSampleRate;
        const size_t qualitySamples = GetMaxIRSamples(quality, qualitySampleRate);
        const size_t maxSamples = qualitySamples > 0 ? qualitySamples + leadingSamples : 0;

        if (maxSamples == 0 || length <= maxSamples)
        {
            return;
        }

        const size_t truncLength = std::min({length, maxSamples, FindEnergyTruncationPointStereo(left, right, 0.001f)});
        outLeft.resize(truncLength);
        outRight.resize(truncLength);
        ApplyTruncationFade(outLeft);
        ApplyTruncationFade(outRight);
    }

    /// Fades the last 64 samples of a truncated impulse out, so the cut does not click.
    static void ApplyTruncationFade(std::vector<float>& truncated)
    {
        constexpr size_t kFadeLength = 64;
        const size_t length = truncated.size();

        if (length > kFadeLength)
        {
            for (size_t i = 0; i < kFadeLength; ++i)
            {
                const float fadeGain = static_cast<float>(kFadeLength - 1 - i) / static_cast<float>(kFadeLength - 1);
                truncated[length - kFadeLength + i] *= fadeGain;
            }
        }
    }

    bool LoadWavFileInto(const std::filesystem::path& path, std::vector<float>& outImpulseL,
                         std::vector<float>& outImpulseR, double& outSampleRate, bool& outIsStereo)
    {
        IRWavData data;

        if (!irwav::LoadAudioFile(path, data))
        {
            return false;
        }

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
        {
            return false;
        }

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

    bool LoadWavFile(const std::filesystem::path& path)
    {
        return LoadWavFileInto(path, mImpulseL, mImpulseR, mIRSampleRate, mIsStereo);
    }

    /// What building a convolver pair reads from the effect, so a deferred rebuild can build from a
    /// copy, off the DSP lock.
    struct ConvolverBuildSettings
    {
        double hostSampleRate = 48000.0;
        int maxBlockSize = 0; // 0 until Prepare, and nothing is built before then
        IRQuality quality = IRQuality::Standard;
        bool normalize = true;
        bool trimLeadingSilence = false;
        // With the trim on, the leading silence both slots lose, in seconds: the earlier slot's
        // onset (IrLeadingSilence.h). Worked out from both slots' impulses before each build.
        double leadingSilenceSeconds = 0.0;
    };

    /// One slot's impulses as its convolvers were built from them, trimmed, truncated, resampled
    /// and normalised: what it plays. Kept only until Auto Gain Comp has measured the pair
    /// (RefreshBlendCorrelation). `right` is empty for a mono IR.
    struct BuiltImpulse
    {
        std::vector<float> left;
        std::vector<float> right;
    };

    /// The most Auto Gain Comp lifts a blend, +6 dB: a pair inverted against each other can
    /// cancel almost entirely, and lifting that without limit would be lifting noise.
    static constexpr double kMaxBlendCompGain = 2.0;

    /// Measures two slots' built impulses for Auto Gain Comp, at `hostRate`. A mono slot plays its
    /// one impulse on both channels; the channels are averaged. Invalid unless both are given.
    static BlendCorrelation ComputeBlendCorrelation(const BuiltImpulse& a, const BuiltImpulse& b, double hostRate)
    {
        BlendCorrelation result;

        if (a.left.empty() || b.left.empty())
        {
            return result;
        }

        result.maxLag = static_cast<int>(std::ceil(IrSlotAlignment::kMaxOffsetMs * hostRate / 1000.0)) + 1;
        result.cross.assign(static_cast<size_t>(2 * result.maxLag + 1), 0.0);
        const int channels = a.right.empty() && b.right.empty() ? 1 : 2;
        const double weight = 1.0 / static_cast<double>(channels);

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto& impulseA = channel == 1 && !a.right.empty() ? a.right : a.left;
            const auto& impulseB = channel == 1 && !b.right.empty() ? b.right : b.left;
            result.energyA += weight * ImpulseEnergy(impulseA);
            result.energyB += weight * ImpulseEnergy(impulseB);
            AccumulateCrossCorrelation(impulseA, impulseB, result.maxLag, weight, result.cross);
        }

        return result;
    }

    static double ImpulseEnergy(const std::vector<float>& impulse)
    {
        double sum = 0.0;

        for (const float s : impulse)
        {
            sum += static_cast<double>(s) * s;
        }

        return sum;
    }

    /// Adds `weight` times the sum of a[n] * b[n - lag] to cross[lag + maxLag], for every lag
    /// within maxLag, by FFT: a full-length IR has too many taps to correlate directly at every lag.
    static void AccumulateCrossCorrelation(const std::vector<float>& a, const std::vector<float>& b, int maxLag,
                                           double weight, std::vector<double>& cross)
    {
        // Long enough that no lag wraps round onto another.
        const size_t needed = a.size() + b.size() + 2 * static_cast<size_t>(maxLag) + 1;
        size_t size = 1;

        while (size < needed)
        {
            size <<= 1;
        }

        SimdFFT fft(size);
        std::vector<std::complex<float>> timeA(size), timeB(size), freqA(size), freqB(size);
        std::copy(a.begin(), a.end(), timeA.begin());
        std::copy(b.begin(), b.end(), timeB.begin());
        fft.Forward(freqA.data(), timeA.data());
        fft.Forward(freqB.data(), timeB.data());

        for (size_t k = 0; k < size; ++k)
        {
            freqA[k] *= std::conj(freqB[k]);
        }

        fft.Inverse(timeA.data(), freqA.data()); // unscaled: divide by size
        const double scale = weight / static_cast<double>(size);

        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            const size_t index = lag >= 0 ? static_cast<size_t>(lag) : size - static_cast<size_t>(-lag);
            cross[static_cast<size_t>(lag + maxLag)] += scale * static_cast<double>(timeA[index].real());
        }
    }

    bool InitializeConvolverFromImpulse(const std::vector<float>& impulseL, const std::vector<float>& impulseR,
                                        bool isStereo, double impulseSampleRate, RealtimeConvolver& convolverL,
                                        RealtimeConvolver& convolverR, BuiltImpulse* built = nullptr) const
    {
        ConvolverBuildSettings settings{mSampleRate, mMaxBlockSize, mQuality, mNormalizeIR, mTrimLeadingSilence};
        settings.leadingSilenceSeconds =
            SharedLeadingSilence(settings, mImpulseL, mImpulseR, mIRSampleRate, mImpulseBL, mImpulseBR, mIRSampleRateB);
        return BuildConvolverPair(settings, impulseL, impulseR, isStereo, impulseSampleRate, convolverL, convolverR,
                                  built);
    }

    /// With Trim Leading Silence on, the leading silence both slots lose, in seconds: the earlier of
    /// their onsets, so IR B keeps its place against IR A. 0 with it off.
    static double SharedLeadingSilence(const ConvolverBuildSettings& settings, const std::vector<float>& leftA,
                                       const std::vector<float>& rightA, double rateA, const std::vector<float>& leftB,
                                       const std::vector<float>& rightB, double rateB)
    {
        if (!settings.trimLeadingSilence)
        {
            return 0.0;
        }

        return ir_leading_silence::SharedOnsetSeconds(ir_leading_silence::FindOnsetSeconds(leftA, rightA, rateA),
                                                      ir_leading_silence::FindOnsetSeconds(leftB, rightB, rateB));
    }

    static bool BuildConvolverPair(const ConvolverBuildSettings& settings, const std::vector<float>& impulseL,
                                   const std::vector<float>& impulseR, bool isStereo, double impulseSampleRate,
                                   RealtimeConvolver& convolverL, RealtimeConvolver& convolverR,
                                   BuiltImpulse* built = nullptr)
    {
        const double hostRate = settings.hostSampleRate;

        if (impulseL.empty() || settings.maxBlockSize == 0)
        {
            return false;
        }

        const double sourceRate = impulseSampleRate > 0.0 ? impulseSampleRate : hostRate;
        // The leading silence comes off at the host's rate, after any resampling, so two slots at
        // different rates lose exactly the same time. Quality's length is counted after it.
        const size_t trim = ir_leading_silence::TrimSamples(settings.leadingSilenceSeconds, hostRate);
        const auto trimAtSourceRate = static_cast<size_t>(static_cast<double>(trim) * sourceRate / hostRate);

        if (isStereo && !impulseR.empty())
        {
            std::vector<float> processedL;
            std::vector<float> processedR;
            GetProcessedImpulseStereo(impulseL, impulseR, sourceRate, settings.quality, hostRate, trimAtSourceRate,
                                      processedL, processedR);

            if (processedL.empty() || processedR.empty())
            {
                return false;
            }

            if (std::abs(sourceRate - hostRate) > 1.0)
            {
                // Cabinet IR samples are FIR coefficients, so resampling needs to
                // preserve coefficient area rather than audio-waveform peak level.
                ResampleImpulseForConvolution(processedL, sourceRate, hostRate);
                ResampleImpulseForConvolution(processedR, sourceRate, hostRate);
            }

            ir_leading_silence::TrimFront(processedL, trim);
            ir_leading_silence::TrimFront(processedR, trim);

            if (settings.normalize)
            {
                // Scale IR samples so convolution preserves signal energy (unity-gain normalization).
                // Uses combined L+R L2 norm so stereo and mono IRs normalize consistently.
                const float normGain = ComputeL2NormGain(hostRate, processedL, processedR);

                for (auto& s : processedL)
                {
                    s *= normGain;
                }

                for (auto& s : processedR)
                {
                    s *= normGain;
                }
            }

            if (!convolverL.SetImpulse(processedL) || !convolverR.SetImpulse(processedR))
            {
                UnloadConvolverPair(convolverL, convolverR);
                return false;
            }

            if (built)
            {
                built->left = std::move(processedL);
                built->right = std::move(processedR);
            }

            return true;
        }

        std::vector<float> processedIR =
            GetProcessedImpulse(impulseL, sourceRate, settings.quality, hostRate, trimAtSourceRate);

        if (processedIR.empty())
        {
            return false;
        }

        if (std::abs(sourceRate - hostRate) > 1.0)
        {
            // Cabinet IR samples are FIR coefficients, so resampling needs to
            // preserve coefficient area rather than audio-waveform peak level.
            ResampleImpulseForConvolution(processedIR, sourceRate, hostRate);
        }

        ir_leading_silence::TrimFront(processedIR, trim);

        if (settings.normalize)
        {
            // Scale IR samples so convolution preserves signal energy (unity-gain normalization).
            const float normGain = ComputeL2NormGain(hostRate, processedIR);

            for (auto& s : processedIR)
            {
                s *= normGain;
            }
        }

        if (!convolverL.SetImpulse(processedIR) || !convolverR.SetImpulse(processedIR))
        {
            UnloadConvolverPair(convolverL, convolverR);
            return false;
        }

        if (built)
        {
            built->left = std::move(processedIR);
            built->right.clear();
        }

        return true;
    }

    /// What is playing becomes the outgoing side of a load's crossfade as it stands, history and
    /// all, as CommitDeferredRebuild() does. The live slots are left empty, for the load to build
    /// or unload. Until 2026-10-18 the outgoing side was rebuilt from the impulses with no history,
    /// so both sides of the fade started from silence: a ~1 ms near-dropout and a few dB low for
    /// ~10 ms on every IR change while playing.
    void CapturePreviousConvolvers()
    {
        SyncRightConvolvers();
        mPrevHasSlotA = SlotAReady();
        mPrevHasSlotB = SlotBReady();
        MoveToOutgoing(mConvolverL, mPrevConvolverL);
        MoveToOutgoing(mConvolverR, mPrevConvolverR);
        MoveToOutgoing(mConvolverBL, mPrevConvolverBL);
        MoveToOutgoing(mConvolverBR, mPrevConvolverBR);
        mPrevBlendCorrelation = std::move(mBlendCorrelation);
        mBlendCorrelation = BlendCorrelation();
    }

    /// After the live convolvers are built: measures the pair that will play for Auto Gain Comp,
    /// then lets the built copies go. A load or Prepare builds both slots together, so the two
    /// copies are always one pair; with either missing the comp falls back to unalike IRs.
    void RefreshBlendCorrelation()
    {
        mBlendCorrelation =
            SlotAReady() && SlotBReady() ? ComputeBlendCorrelation(mBuiltA, mBuiltB, mSampleRate) : BlendCorrelation();
        mBuiltA = BuiltImpulse();
        mBuiltB = BuiltImpulse();
    }

    /// Whether a slot's two convolvers were built from the same impulse: BuildConvolverPair builds
    /// both sides from the left one unless it has a stereo file with a right channel.
    static bool IsMonoImpulse(bool isStereo, const std::vector<float>& impulseRight) noexcept
    {
        return !isStereo || impulseRight.empty();
    }

    /// One slot's pair of convolvers for a block. With `mayShare` (a mono input and a mono impulse)
    /// the right sits out and copies the left's output, while the two inputs are the same bit for
    /// bit (Speaker Drive and the slot alignment keep a side's own history, which a stereo input
    /// before can leave apart for a while) and have been for as long as the convolvers' history
    /// reaches back, so no tail of an earlier stereo input is still ringing on the right.
    /// `rightStale` says the right has missed blocks; the first block that needs it again brings it
    /// level with the left first. Either way the output is what running both would give.
    static void ProcessSlotPair(RealtimeConvolver& left, RealtimeConvolver& right, bool& rightStale,
                                size_t& sameInputSamples, bool mayShare, const float* inL, const float* inR,
                                float* outL, float* outR, int numSamples, bool allowParallel)
    {
        const bool sameInput = mayShare && std::memcmp(inL, inR, static_cast<size_t>(numSamples) * sizeof(float)) == 0;
        // Counted before this block: while the histories converge, both still run it.
        const bool shareLeft = sameInput && sameInputSamples >= left.HistorySpan();

        if (!sameInput)
        {
            sameInputSamples = 0;
        }
        else if (sameInputSamples < kSidesAlike)
        {
            sameInputSamples += static_cast<size_t>(numSamples);
        }

        if (shareLeft)
        {
            left.Process(inL, outL, numSamples);
            std::copy_n(outL, numSamples, outR);
            rightStale = true;
            return;
        }

        if (rightStale)
        {
            right.CopyStateFrom(left);
            rightStale = false;
        }

        bool ranParallel = false;

        if (allowParallel)
        {
            ranParallel = rtparallel::DualLaneExecutor::Instance().Run([&]() { right.Process(inR, outR, numSamples); },
                                                                       [&]() { left.Process(inL, outL, numSamples); });
        }

        if (!ranParallel)
        {
            left.Process(inL, outL, numSamples);
            right.Process(inR, outR, numSamples);
        }
    }

    /// Brings a right convolver that sat out level with its left, before the pair is moved to the
    /// outgoing side of a crossfade, which always runs both. Allocation-free.
    void SyncRightConvolvers() noexcept
    {
        if (mRightStaleA)
        {
            mConvolverR.CopyStateFrom(mConvolverL);
            mRightStaleA = false;
        }

        if (mRightStaleB)
        {
            mConvolverBR.CopyStateFrom(mConvolverBL);
            mRightStaleB = false;
        }
    }

    /// A moved-from convolver keeps its flags but not its buffers, so `live` is replaced with an
    /// empty one rather than left as moved.
    static void MoveToOutgoing(RealtimeConvolver& live, RealtimeConvolver& outgoing)
    {
        outgoing = std::move(live);
        live = RealtimeConvolver();
    }

    bool InitializeConvolverA()
    {
        mRightStaleA = false; // both sides start from the same nothing
        mSameInputA = kSidesAlike;
        return InitializeConvolverFromImpulse(mImpulseL, mImpulseR, mIsStereo, mIRSampleRate, mConvolverL, mConvolverR,
                                              &mBuiltA);
    }

    bool InitializeConvolverB()
    {
        mRightStaleB = false;
        mSameInputB = kSidesAlike;
        return InitializeConvolverFromImpulse(mImpulseBL, mImpulseBR, mIsStereoB, mIRSampleRateB, mConvolverBL,
                                              mConvolverBR, &mBuiltB);
    }

    // Returns the gain to apply to IR samples so that convolution is unity-gain for a broadband
    // signal. Computed as 1/||h||_2 where ||h||_2 = sqrt(sum(h[n]^2)).
    // For stereo, the average L+R L2 norm is used so stereo and mono IRs normalise equivalently.
    //
    // playbackRate is the rate the (already resampled) impulse runs at. Resampling preserves
    // coefficient area, not energy, so ||h||_2 falls as 1/sqrt(rate) and a bare 1/||h||_2 gain
    // would make the same IR file progressively louder at higher host rates (+6 dB at 192 kHz
    // versus 48 kHz). Anchoring to a reference rate removes that dependency; 48 kHz is the
    // anchor so the normalisation there is unchanged.
    static float ComputeL2NormGain(double playbackRate, const std::vector<float>& samplesL,
                                   const std::vector<float>& samplesR = {})
    {
        double sumSq = 0.0;

        for (const float s : samplesL)
        {
            sumSq += static_cast<double>(s) * s;
        }

        if (!samplesR.empty())
        {
            for (const float s : samplesR)
            {
                sumSq += static_cast<double>(s) * s;
            }

            sumSq *= 0.5; // average channels so stereo == mono for the same impulse response
        }

        if (sumSq <= 1e-12)
        {
            return 1.0f;
        }

        const double rateCompensation =
            playbackRate > 0.0 ? std::sqrt(kNormalizationReferenceRate / playbackRate) : 1.0;
        return static_cast<float>(rateCompensation / std::sqrt(sumSq));
    }

    /// A slot plays once its convolvers are built: both channels' for a stereo IR.
    [[nodiscard]] bool SlotAReady() const
    {
        return mConvolverL.IsInitialized() && (!mIsStereo || mConvolverR.IsInitialized());
    }

    [[nodiscard]] bool SlotBReady() const
    {
        return mConvolverBL.IsInitialized() && (!mIsStereoB || mConvolverBR.IsInitialized());
    }

    /// Each slot's level outside L/R Split, with its Level and Invert, and the Auto Gain Comp
    /// applied after them.
    struct SlotMix
    {
        double slotAGain = 0.0;
        double slotBGain = 0.0;
        double compGain = 1.0;
    };

    /// IR Blend crossfades two IRs. A lone IR, in either slot, is the blend all the way to it: it
    /// plays in full whatever IR Blend says, as one IR in a cab should. `correlation` is the pair's
    /// (BlendCorrelation), and `offsetSamples` where IR B sits against IR A.
    [[nodiscard]] SlotMix BlendedSlotMix(bool hasA, bool hasB, const BlendCorrelation& correlation,
                                         double offsetSamples) const
    {
        const double blend = hasA && hasB ? std::clamp(mIRBlend, 0.0, 1.0) : (hasB ? 1.0 : 0.0);
        return {(1.0 - blend) * mSlotAGain * (mSlotAPolarityInverted ? -1.0 : 1.0),
                blend * mSlotBGain * (mSlotBPolarityInverted ? -1.0 : 1.0),
                ComputeBlendCompGain(hasA && hasB, blend, correlation, offsetSamples)};
    }

    /// Auto Gain Comp: the gain that keeps the blend as loud as the two IRs are on their own,
    /// interpolated by IR Blend in power. The slots' Level is left out, so it still sets their
    /// balance. Two IRs much alike (two mics on one speaker) sum close to amplitude, and the comp
    /// barely lifts them; unalike ones sum in power, and at IR Blend 0.5 it lifts them 3 dB. Until
    /// 2026-10-18 it took every pair as unalike, and played a two-mic blend up to ~2.6 dB loud.
    [[nodiscard]] double ComputeBlendCompGain(bool both, double blend, const BlendCorrelation& correlation,
                                              double offsetSamples) const
    {
        if (!mAutoGainCompEnabled || !both)
        {
            return 1.0; // a lone IR plays in full
        }

        const double blendA = 1.0 - blend;
        const double blendB = blend;

        if (!correlation.Valid())
        {
            // A pair not measured (not built yet): unit energy each, nothing alike.
            const double energy = blendA * blendA + blendB * blendB;
            return 1.0 / std::sqrt(std::max(1e-8, energy));
        }

        // The energies keep this right with Normalize off as well. Invert on one slot turns the
        // likeness negative: the pair cancels, and the comp lifts it, as far as kMaxBlendCompGain.
        const double polarity = mSlotAPolarityInverted != mSlotBPolarityInverted ? -1.0 : 1.0;
        const double target = blendA * correlation.energyA + blendB * correlation.energyB;
        const double actual = blendA * blendA * correlation.energyA + blendB * blendB * correlation.energyB +
                              2.0 * blendA * blendB * polarity * correlation.CrossAt(offsetSamples);
        return std::min(kMaxBlendCompGain, std::sqrt(target / std::max(actual, 1e-12 * target)));
    }

    /// Quality, Normalize and Trim Leading Silence are baked into the convolvers when they are built. With
    /// nothing built yet (a preset sets its params before its IRs load) the first build picks the value up.
    /// Otherwise the rebuild is left to the message thread (DeferredRebuild): SetParam can run on the audio
    /// thread, and the rebuild allocates.
    template <typename Setting>
    void RequestRebuildSetting(std::atomic<int>& pending, Setting& setting, Setting requested)
    {
        if (!HasResource())
        {
            setting = requested;
            pending.store(-1, std::memory_order_release);
            return;
        }

        // Asking for what is built cancels a change still waiting.
        const int target = requested == setting ? -1 : static_cast<int>(requested);

        if (pending.exchange(target, std::memory_order_acq_rel) != target)
        {
            DeferredRebuild::NoteRequested();
        }
    }

    [[nodiscard]] static bool EffectiveSetting(const std::atomic<int>& pending, bool setting)
    {
        const int value = pending.load(std::memory_order_acquire);
        return value >= 0 ? value != 0 : setting;
    }

    [[nodiscard]] IRQuality EffectiveQuality() const
    {
        const int pending = mPendingQuality.load(std::memory_order_acquire);
        return pending >= 0 ? static_cast<IRQuality>(pending) : mQuality;
    }

    /// Every build of the live convolvers starts here: a Quality, Normalize or Trim Leading Silence
    /// change still waiting for the message thread is folded into the build, and any rebuild
    /// already taken for it is dropped when it comes back.
    void ApplyPendingRebuildSettings()
    {
        ++mBuildGeneration;
        mQuality = EffectiveQuality();
        mNormalizeIR = EffectiveSetting(mPendingNormalizeIR, mNormalizeIR);
        mTrimLeadingSilence = EffectiveSetting(mPendingTrimLeadingSilence, mTrimLeadingSilence);
        mPendingQuality.store(-1, std::memory_order_release);
        mPendingNormalizeIR.store(-1, std::memory_order_release);
        mPendingTrimLeadingSilence.store(-1, std::memory_order_release);
    }

    /// A pending value the commit just built is no longer pending; a different one still is.
    static void ClearPendingIf(std::atomic<int>& pending, int built)
    {
        pending.compare_exchange_strong(built, -1, std::memory_order_acq_rel);
    }

    /// `previous` takes what is playing, `live` takes what was built, and `built` is left with the
    /// convolvers `previous` held, to be freed with the work. Moves between these steal buffers,
    /// so nothing is allocated or freed here.
    static void SwapIn(RealtimeConvolver& live, RealtimeConvolver& previous, RealtimeConvolver& built)
    {
        std::swap(previous, live);
        std::swap(live, built);
    }

    /// Empties a slot: IsInitialized() turns false, so the slot stops sounding. RealtimeConvolver's
    /// Reset() only clears history, which left a "cleared" slot playing its old IR. This frees the
    /// old buffers, as the builds around it in a load do; never call it from Process().
    static void UnloadConvolverPair(RealtimeConvolver& left, RealtimeConvolver& right)
    {
        left = RealtimeConvolver();
        right = RealtimeConvolver();
    }

    /// One IR slot's impulse, as a deferred rebuild copies it.
    struct SlotImpulse
    {
        std::vector<float> left;
        std::vector<float> right;
        bool isStereo = false;
        double sampleRate = 48000.0;
    };

    /// The convolver rebuild a Quality, Normalize or Trim Leading Silence change needs, built by
    /// the message thread off the DSP lock from copies of the impulses and settings.
    class ConvolverRebuild final : public DeferredRebuild
    {
      public:
        void Build() override
        {
            settings.leadingSilenceSeconds = SharedLeadingSilence(settings, slotA.left, slotA.right, slotA.sampleRate,
                                                                  slotB.left, slotB.right, slotB.sampleRate);

            // Same slots as the rebuild SetParam used to do: A always, B only when it has an IR.
            BuiltImpulse builtA;
            BuiltImpulse builtB;

            if (!slotA.left.empty())
            {
                BuildConvolverPair(settings, slotA.left, slotA.right, slotA.isStereo, slotA.sampleRate, convolverL,
                                   convolverR, &builtA);
            }

            if (!slotB.left.empty())
            {
                BuildConvolverPair(settings, slotB.left, slotB.right, slotB.isStereo, slotB.sampleRate, convolverBL,
                                   convolverBR, &builtB);
            }

            // Quality, Normalize and the trim all change what the pair plays, so it is measured again.
            blendCorrelation = ComputeBlendCorrelation(builtA, builtB, settings.hostSampleRate);
        }

        std::uint64_t generation = 0;
        ConvolverBuildSettings settings;
        SlotImpulse slotA;
        SlotImpulse slotB;
        RealtimeConvolver convolverL;
        RealtimeConvolver convolverR;
        RealtimeConvolver convolverBL;
        RealtimeConvolver convolverBR;
        BlendCorrelation blendCorrelation;
    };

    enum class AirMode
    {
        OptionA_Shelf,
        OptionB_Peak,
        OptionC_ShelfPlusPeak,
    };

    static double ProcessBiquad(double input, double b0, double b1, double b2, double a1, double a2, double& s1,
                                double& s2)
    {
        const double output = b0 * input + s1;
        s1 = b1 * input - a1 * output + s2;
        s2 = b2 * input - a2 * output;
        return output;
    }

    // The filter designs come from BiquadDesign.h, which also keeps them stable when a host
    // runs below twice their frequency. These unpack a design into the member quintuples
    // the processing code reads.

    static void Unpack(const BiquadCoefficients& c, double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        b0 = c.b0;
        b1 = c.b1;
        b2 = c.b2;
        a1 = c.a1;
        a2 = c.a2;
    }

    void UpdateAirCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            mAirActive = false;
            return;
        }

        const AirFilters air = DesignAir(mAir, mSampleRate);
        mAirActive = air.active;

        if (air.active)
        {
            Unpack(air.shelf, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2);
            Unpack(air.peak, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2);
        }
    }

    void ResetAirState()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            mAirShelfS1[ch] = mAirShelfS2[ch] = 0.0;
            mAirPeakS1[ch] = mAirPeakS2[ch] = 0.0;
        }
    }

    void ComputeHighPass(double freq, double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        Unpack(biquad::HighPass(freq, biquad::kButterworthQ, mSampleRate), b0, b1, b2, a1, a2);
    }

    void ComputeLowPass(double freq, double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        Unpack(biquad::LowPass(freq, biquad::kButterworthQ, mSampleRate), b0, b1, b2, a1, a2);
    }

    void UpdateCabFilterCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            mLowCutActive = false;
            mHighCutActive = false;
            return;
        }

        const CabFilters cuts = DesignCabFilters(mLowCutHz, mHighCutHz, mSampleRate);
        mLowCutActive = cuts.lowCutActive;
        mHighCutActive = cuts.highCutActive;
        Unpack(cuts.lowCut, mLowCutB0, mLowCutB1, mLowCutB2, mLowCutA1, mLowCutA2);
        Unpack(cuts.highCut, mHighCutB0, mHighCutB1, mHighCutB2, mHighCutA1, mHighCutA2);
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
            output = ProcessBiquad(output, mLowCutB0, mLowCutB1, mLowCutB2, mLowCutA1, mLowCutA2, mLowCutS1[channel],
                                   mLowCutS2[channel]);
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
            return ProcessBiquad(input, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2, mAirPeakS1[channel],
                                 mAirPeakS2[channel]);
        case AirMode::OptionC_ShelfPlusPeak:
        default: {
            const double shelf = ProcessBiquad(input, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2,
                                               mAirShelfS1[channel], mAirShelfS2[channel]);
            return ProcessBiquad(shelf, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2, mAirPeakS1[channel],
                                 mAirPeakS2[channel]);
        }
        }
    }

    void UpdateMicCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const MicPositionFilters micA = DesignMicPosition(mMicRadialA, mMicProximityA, mSampleRate);
        const MicPositionFilters micB = DesignMicPosition(mMicRadialB, mMicProximityB, mSampleRate);
        Unpack(micA.shelf, mMicAShelfB0, mMicAShelfB1, mMicAShelfB2, mMicAShelfA1, mMicAShelfA2);
        Unpack(micA.proximity, mMicAProxB0, mMicAProxB1, mMicAProxB2, mMicAProxA1, mMicAProxA2);
        Unpack(micB.shelf, mMicBShelfB0, mMicBShelfB1, mMicBShelfB2, mMicBShelfA1, mMicBShelfA2);
        Unpack(micB.proximity, mMicBProxB0, mMicBProxB1, mMicBProxB2, mMicBProxA1, mMicBProxA2);
    }

    void ResetMicPositionState()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            mMicAShelfS1[ch] = mMicAShelfS2[ch] = 0.0;
            mMicAProxS1[ch] = mMicAProxS2[ch] = 0.0;
            mMicBShelfS1[ch] = mMicBShelfS2[ch] = 0.0;
            mMicBProxS1[ch] = mMicBProxS2[ch] = 0.0;
        }
    }

    double ProcessMicPositionSlotA(double input, int channel)
    {
        const double shelf = ProcessBiquad(input, mMicAShelfB0, mMicAShelfB1, mMicAShelfB2, mMicAShelfA1, mMicAShelfA2,
                                           mMicAShelfS1[channel], mMicAShelfS2[channel]);
        return ProcessBiquad(shelf, mMicAProxB0, mMicAProxB1, mMicAProxB2, mMicAProxA1, mMicAProxA2,
                             mMicAProxS1[channel], mMicAProxS2[channel]);
    }

    double ProcessMicPositionSlotB(double input, int channel)
    {
        const double shelf = ProcessBiquad(input, mMicBShelfB0, mMicBShelfB1, mMicBShelfB2, mMicBShelfA1, mMicBShelfA2,
                                           mMicBShelfS1[channel], mMicBShelfS2[channel]);
        return ProcessBiquad(shelf, mMicBProxB0, mMicBProxB1, mMicBProxB2, mMicBProxA1, mMicBProxA2,
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
    BuiltImpulse mBuiltA; // what slot A's last build played, until RefreshBlendCorrelation reads it
    BuiltImpulse mBuiltB;
    BlendCorrelation mBlendCorrelation;     // the live pair, for Auto Gain Comp
    BlendCorrelation mPrevBlendCorrelation; // the outgoing pair of a crossfade

    std::vector<float> mInputBufferL; // the clean input, which is also the dry signal
    std::vector<float> mInputBufferR;
    std::vector<float> mDrivenBufferL; // the input through Speaker Drive, when it is on
    std::vector<float> mDrivenBufferR;
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
    std::atomic<bool> mDualMono{false};
    std::atomic<bool> mInputMono{false}; // SetInputLayout
    // A slot's right convolver sat out the last block (ProcessSlotPair) and has to catch up on the
    // left's history before it runs again. Audio thread, or under the DSP lock.
    bool mRightStaleA = false;
    bool mRightStaleB = false;
    // How long both sides of a slot have heard the same input, in samples, up to kSidesAlike, which
    // also stands for a pair that has been alike since it was built or reset.
    static constexpr size_t kSidesAlike = static_cast<size_t>(1) << 40;
    size_t mSameInputA = kSidesAlike;
    size_t mSameInputB = kSidesAlike;
    double mSlotAPan = 0.0; // -1.0 = full left, 0.0 = centre, 1.0 = full right
    double mSlotBPan = 0.0;
    bool mLRSplitEnabled = false; // when true, stereo input is split: L→IR A, R→IR B
    bool mSlotAPolarityInverted = false;
    bool mSlotBPolarityInverted = false;
    double mOutputGain = 1.0;
    bool mEnabled = true;
    IRQuality mQuality = IRQuality::Standard;
    bool mNormalizeIR = true;
    bool mAutoGainCompEnabled = true;
    // Off by default, so a preset from before it existed keeps its timing. See IrLeadingSilence.h.
    bool mTrimLeadingSilence = false;
    // Quality, Normalize and Trim Leading Silence as SetParam last asked for them, while the
    // convolvers are still built the other way (-1 when they match). See RequestRebuildSetting.
    std::atomic<int> mPendingQuality{-1};
    std::atomic<int> mPendingNormalizeIR{-1};
    std::atomic<int> mPendingTrimLeadingSilence{-1};
    // Counts builds of the live convolvers, so a deferred rebuild taken before one is dropped.
    std::uint64_t mBuildGeneration = 0;
    // Until the first Prepare() a build would be at the default rate and block size, and
    // Prepare() would throw it away and build again at the host's. An executor loads a new node's
    // IRs before it prepares it, so before then a load only keeps the impulses.
    bool mPrepared = false;
    bool mHasLoadedResource = false;
    bool mPrevHasSlotA = false;
    bool mPrevHasSlotB = false;
    int mResourceTransitionSamplesTotal = 1440;
    int mResourceTransitionSamplesRemaining = 0;

    double mAir = 0.0;
    AirMode mAirMode = AirMode::OptionA_Shelf;
    bool mAirActive = false;
    SpeakerDrive mSpeakerDrive;
    IrSlotAlignment mAlignment; // IR B's time against IR A (slotBOffset)

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
    double mMicRadialA = 0.0;    // 0=on-axis/center, 1=off-axis/edge
    double mMicProximityA = 0.0; // 0=far/neutral,    1=close-mic
    double mMicRadialB = 0.0;
    double mMicProximityB = 0.0;

    double mMicAShelfB0 = 1, mMicAShelfB1 = 0, mMicAShelfB2 = 0, mMicAShelfA1 = 0, mMicAShelfA2 = 0;
    double mMicAProxB0 = 1, mMicAProxB1 = 0, mMicAProxB2 = 0, mMicAProxA1 = 0, mMicAProxA2 = 0;
    double mMicBShelfB0 = 1, mMicBShelfB1 = 0, mMicBShelfB2 = 0, mMicBShelfA1 = 0, mMicBShelfA2 = 0;
    double mMicBProxB0 = 1, mMicBProxB1 = 0, mMicBProxB2 = 0, mMicBProxA1 = 0, mMicBProxA2 = 0;

    std::array<double, 2> mMicAShelfS1 = {}, mMicAShelfS2 = {};
    std::array<double, 2> mMicAProxS1 = {}, mMicAProxS2 = {};
    std::array<double, 2> mMicBShelfS1 = {}, mMicBShelfS2 = {};
    std::array<double, 2> mMicBProxS1 = {}, mMicBProxS2 = {};

    // Keeps the shared second lane running while this effect exists; taken in Prepare().
    rtparallel::DualLaneExecutor::Lease mDualLaneLease;
};

inline void RegisterIRCabEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kCabIr;
    info.aliases = {"cab_ir", "ir_cab"};
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
        slotA.resourceId = "irA";
        slotA.displayName = "IR A";
        slotA.nodeId = "";
        slotA.resourceType = "ir";
        slotA.resourceIndex = 0;
        slotA.allowBrowseFile = true;

        ExposedResource slotB;
        slotB.resourceId = "irB";
        slotB.displayName = "IR B";
        slotB.nodeId = "";
        slotB.resourceType = "ir";
        slotB.resourceIndex = 1;
        slotB.allowBrowseFile = true;

        info.exposedResources = {slotA, slotB};
    }

    info.parameters = {{"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Level"},
                       {"irBlend", "IR Blend", 0.0, 0.0, 1.0, "blend", "Blend"},
                       {"outputGain", "Output", 0.0, -24.0, 24.0, "dB", "Level"},
                       WithLogTaper({"lowCutHz", "Low Cut", 20.0, 20.0, 1000.0, "Hz", "Tone"}),
                       WithLogTaper({"highCutHz", "High Cut", 20000.0, 1000.0, 20000.0, "Hz", "Tone"}),
                       {"air", "Air", 0.0, 0.0, 1.0, "amount", "Tone"},
                       {"speakerDrive", "Speaker Drive", 0.0, 0.0, 1.0, "amount", "Tone"},
                       {"airMode", "Air Mode", 0.0, 0.0, 2.0, "enum", "Tone", true, 1.0, {"Shelf", "Presence", "Both"}},
                       {"micEmulation", "Mic Emulation", 0.0, 0.0, 1.0, "toggle", "Tone", true},
                       {"lrSplit", "L/R Split", 0.0, 0.0, 1.0, "toggle", "Blend", true},
                       {"slotAGain", "IR A Level", 0.0, -24.0, 24.0, "dB", "IR A", true},
                       {"slotAPan", "IR A Pan", 0.0, -1.0, 1.0, "pan", "IR A", true},
                       {"slotAPolarity", "IR A Invert", 0.0, 0.0, 1.0, "toggle", "IR A", true},
                       {"micRadialA", "Mic A Radial", 0.0, 0.0, 1.0, "amount", "IR A", true},
                       {"micProximityA", "Mic A Proximity", 0.0, 0.0, 1.0, "amount", "IR A", true},
                       {"slotBPolarity", "IR B Invert", 0.0, 0.0, 1.0, "toggle", "IR B", true},
                       {"slotBGain", "IR B Level", 0.0, -24.0, 24.0, "dB", "IR B", true},
                       {"slotBPan", "IR B Pan", 0.0, -1.0, 1.0, "pan", "IR B", true},
                       {"micRadialB", "Mic B Radial", 0.0, 0.0, 1.0, "amount", "IR B", true},
                       {"micProximityB", "Mic B Proximity", 0.0, 0.0, 1.0, "amount", "IR B", true},
                       {"slotBOffset", "IR B Offset", 0.0, -IrSlotAlignment::kMaxOffsetMs,
                        IrSlotAlignment::kMaxOffsetMs, "ms", "Alignment", true},
                       {"trimLeadingSilence", "Trim Leading Silence", 0.0, 0.0, 1.0, "toggle", "Tone", true},
                       {"quality",
                        "Quality",
                        1.0,
                        0.0,
                        3.0,
                        "enum",
                        "Tone",
                        true,
                        1.0,
                        {"Economy", "Standard", "High", "Full"}}}; // 0=Economy, 1=Standard, 2=High, 3=Full

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<IRCabEffect>(); });
}
} // namespace guitarfx
