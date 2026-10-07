#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/FiniteCheck.h"
#include "dsp/HalfBandIir.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/DriveOutputLimiter.h"
#include "dsp/effects/DriveStages.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>

namespace guitarfx::drive
{
/// Knob changes are applied in chunks of this many host samples: often enough that a sweep
/// is smooth, rarely enough that redesigning a model's filters costs nothing measurable.
inline constexpr int kChunkSamples = 16;

/// Time constant of the knob smoothing.
inline constexpr double kKnobSmoothingMs = 25.0;

/// Length of each half of the fade that covers a Model or Clipping switch.
inline constexpr double kSwitchFadeMs = 8.0;

/**
 * What the three drive pedals share: their knobs, oversampling, levels and switching.
 *
 * `Traits` describes one pedal family: its parameter table and the circuits behind its Model
 * switch. Per channel and per sample the signal runs
 *
 *     volts -> Pre -> up x N -> Shape at N x -> down -> Post -> x Level -+- mix -> limiter
 *     volts ------------------ same up/down round trip -----------------+
 *
 * `Pre` and `Post` are the linear filters either side of the clipping, which run at the host
 * rate; only `Shape`, which holds every nonlinearity, runs oversampled (OversamplingFactor).
 * The dry signal takes the same filter round trip as the wet one, so Mix blends them in phase.
 *
 * Traits provides:
 *  - `kParams`, the EffectParamSpec table, and `kLevel` and `kMix`, the indices of the two
 *    parameters this class applies itself.
 *  - `kTypeName`, the legacy type string GetType() reports.
 *  - `Coefficients` and `State` (with `Reset()`); `Design(Coefficients&, values, rate, osRate)`
 *    designs a model at the current knob values; `Pre`, `Shape` and `Post` process one sample.
 *
 * Continuous knobs glide over kKnobSmoothingMs. A stepped parameter (a Model or Clipping
 * switch) changes the circuit outright, so the wet signal fades out, the switch lands in
 * silence with the circuit's memory cleared, and it fades back in.
 */
template <typename Traits> class DrivePedal : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    static constexpr std::size_t kParamCount = std::tuple_size_v<std::remove_cvref_t<decltype(Traits::kParams)>>;
    using Values = std::array<double, kParamCount>;
    using Coefficients = typename Traits::Coefficients;
    using State = typename Traits::State;

    DrivePedal()
    {
        for (std::size_t index = 0; index < kParamCount; ++index)
        {
            mTargets[index].store(Traits::kParams[index].defaultValue, std::memory_order_relaxed);
            mCurrent[index] = Traits::kParams[index].defaultValue;
        }
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        const int factor = OversamplingFactor(sampleRate);
        mOversampledRate = sampleRate * factor;

        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            mWet[channel].Prepare(factor);
            mDry[channel].Prepare(factor);
        }

        mSmoothing = 1.0 - std::exp(-kChunkSamples / (kKnobSmoothingMs * 0.001 * sampleRate));
        mFadeStep = 1.0 / std::max(1.0, kSwitchFadeMs * 0.001 * sampleRate);
        mNominalDbfs = GetNominalOperatingLevelDbfs();
        mVoltsPerUnit = VoltsPerUnit(mNominalDbfs);
        mDesigned = false;
        Reset();
    }

    void Reset() override
    {
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            mStates[channel].Reset();
            mWet[channel].Reset();
            mDry[channel].Reset();
        }

        mPrimed = false;
        mFade = 1.0;
        mDryRunning = false;
    }

    /// The nonlinear part runs near 384 kHz whatever the host rate: 8x at 44.1 and 48 kHz,
    /// 4x at 88.2 and 96, 2x above. With every clipper antialiased (see ClipCurve), that holds
    /// the audible aliasing of a 1.3 kHz note at full drive 60 dB or more below the signal on
    /// every model (the Metal Zone is the worst; most are past 78 dB). At 4x the high-gain
    /// models were at 43-50 dB.
    [[nodiscard]] static int OversamplingFactor(double sampleRate) noexcept
    {
        return sampleRate < 60000.0 ? 8 : (sampleRate < 120000.0 ? 4 : 2);
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        const float* inputs[1] = {input};
        float* outputs[1] = {output};
        ProcessChannels(inputs, outputs, 1, numSamples);
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        const float* channelInputs[2] = {inputs ? inputs[0] : nullptr, inputs ? inputs[1] : nullptr};
        float* channelOutputs[2] = {outputs[0], outputs[1]};
        ProcessChannels(channelInputs, channelOutputs, 2, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        const std::size_t index = FindParamSpec(Traits::kParams, key);

        if (index < kParamCount && IsFinite(value))
        {
            mTargets[index].store(NormaliseParamValue(Traits::kParams[index], value), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        const std::size_t index = FindParamSpec(Traits::kParams, key);
        return index < kParamCount ? mTargets[index].load(std::memory_order_relaxed) : 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return Traits::kTypeName;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "drive";
    }

  private:
    [[nodiscard]] static bool IsStepped(std::size_t index) noexcept
    {
        return Traits::kParams[index].step > 0.0;
    }

    /// Moves the knobs one chunk on and redesigns the circuit if any of them moved.
    void AdvanceKnobs()
    {
        bool redesign = false;
        bool switchPending = false;

        for (std::size_t index = 0; index < kParamCount; ++index)
        {
            const double target = mTargets[index].load(std::memory_order_relaxed);

            if (mCurrent[index] == target)
            {
                continue;
            }

            if (IsStepped(index))
            {
                switchPending = true;
                continue;
            }

            const auto& spec = Traits::kParams[index];
            double next = mPrimed ? mCurrent[index] + (target - mCurrent[index]) * mSmoothing : target;

            if (std::fabs(target - next) <= 1.0e-6 * (spec.maxValue - spec.minValue))
            {
                next = target;
            }

            mCurrent[index] = next;
            redesign = redesign || (index != Traits::kLevel && index != Traits::kMix);
        }

        // Before any audio has run there is nothing to fade: a preset's switches land at once.
        if (switchPending && (!mPrimed || mFade <= 0.0))
        {
            for (std::size_t index = 0; index < kParamCount; ++index)
            {
                if (IsStepped(index))
                {
                    mCurrent[index] = mTargets[index].load(std::memory_order_relaxed);
                }
            }

            for (auto& state : mStates)
            {
                state.Reset();
            }

            switchPending = false;
            redesign = true;
        }

        mSwitchPending = switchPending;

        if (redesign || !mDesigned)
        {
            Traits::Design(mCoefficients, mCurrent, mSampleRate, mOversampledRate);
            mDesigned = true;
        }
    }

    void ProcessChannels(const float* const* inputs, float* const* outputs, int channels, int numSamples)
    {
        const double nominal = GetNominalOperatingLevelDbfs();

        if (nominal != mNominalDbfs)
        {
            mNominalDbfs = nominal;
            mVoltsPerUnit = VoltsPerUnit(nominal);
        }

        const double voltsPerUnit = mVoltsPerUnit;
        const double unitsPerVolt = 1.0 / voltsPerUnit;
        const int factor = mWet[0].Factor();

        for (int start = 0; start < numSamples; start += kChunkSamples)
        {
            const int count = std::min(kChunkSamples, numSamples - start);
            const double levelFrom = DbToGain(mCurrent[Traits::kLevel]);
            const double mixFrom = mCurrent[Traits::kMix];
            AdvanceKnobs();
            mPrimed = true;
            const double levelStep = (DbToGain(mCurrent[Traits::kLevel]) - levelFrom) / count;
            const double mixStep = (mCurrent[Traits::kMix] - mixFrom) / count;

            // Fully wet is the usual setting, so the dry round trip only runs when some dry is
            // heard. It restarts from silence, under a mix that is only just leaving 1.
            const bool dryHeard = mixFrom < 1.0 || mCurrent[Traits::kMix] < 1.0;

            if (dryHeard && !mDryRunning)
            {
                mDry[0].Reset();
                mDry[1].Reset();
            }

            mDryRunning = dryHeard;
            std::array<double, kChunkSamples> wetGain = {};
            std::array<double, kChunkSamples> dryGain = {};

            for (int i = 0; i < count; ++i)
            {
                mFade = mSwitchPending ? std::max(0.0, mFade - mFadeStep) : std::min(1.0, mFade + mFadeStep);
                const double mix = mixFrom + mixStep * (i + 1);
                wetGain[static_cast<std::size_t>(i)] = (levelFrom + levelStep * (i + 1)) * mix * mFade * unitsPerVolt;
                dryGain[static_cast<std::size_t>(i)] = (1.0 - mix) * unitsPerVolt;
            }

            for (int channel = 0; channel < channels; ++channel)
            {
                float* output = outputs[channel];

                if (!output)
                {
                    continue;
                }

                const float* input = inputs[channel];
                State& state = mStates[static_cast<std::size_t>(channel)];
                halfband::Oversampler& wet = mWet[static_cast<std::size_t>(channel)];
                halfband::Oversampler& dry = mDry[static_cast<std::size_t>(channel)];
                bool corrupted = false;

                for (int i = 0; i < count; ++i)
                {
                    const float raw = input ? input[start + i] : 0.0f;
                    const double volts = (IsFinite(raw) ? static_cast<double>(raw) : 0.0) * voltsPerUnit;

                    double lanes[halfband::Oversampler::kMaxFactor];
                    wet.Up(Traits::Pre(mCoefficients, state, volts), lanes);

                    for (int lane = 0; lane < factor; ++lane)
                    {
                        lanes[lane] = Traits::Shape(mCoefficients, state, lanes[lane]);
                    }

                    const double shaped = Traits::Post(mCoefficients, state, wet.Down(lanes));
                    double mixed = shaped * wetGain[static_cast<std::size_t>(i)];

                    if (dryHeard)
                    {
                        dry.Up(volts, lanes);
                        mixed += dry.Down(lanes) * dryGain[static_cast<std::size_t>(i)];
                    }

                    if (!IsFinite(mixed))
                    {
                        mixed = 0.0;
                        corrupted = true;
                    }

                    output[start + i] = drive_output_limiter::SoftClipNearCeiling(static_cast<float>(mixed));
                }

                if (corrupted)
                {
                    state.Reset();
                    wet.Reset();
                    dry.Reset();
                }
            }
        }
    }

    std::array<std::atomic<double>, kParamCount> mTargets;
    Values mCurrent = {};
    Coefficients mCoefficients = {};
    std::array<State, 2> mStates = {};
    std::array<halfband::Oversampler, 2> mWet = {};
    std::array<halfband::Oversampler, 2> mDry = {};
    double mOversampledRate = 192000.0;
    double mSmoothing = 1.0;
    double mFadeStep = 1.0;
    double mFade = 1.0;
    double mNominalDbfs = kDefaultNominalOperatingLevelDbfs;
    double mVoltsPerUnit = VoltsPerUnit(kDefaultNominalOperatingLevelDbfs);
    bool mSwitchPending = false;
    bool mPrimed = false;
    bool mDesigned = false;
    bool mDryRunning = false;
};
} // namespace guitarfx::drive
