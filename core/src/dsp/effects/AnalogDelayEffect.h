#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/Compander.h"
#include "dsp/effects/DelayEffectSpec.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace guitarfx
{
namespace analog_delay
{
/**
 * Bucket-brigade delay.
 *
 * A BBD is an analog shift register of N stages clocked at f_clock, and two clock phases
 * are needed per stage transfer, so:
 *
 *     delay_seconds = N / (2 * f_clock)
 *
 * The Time knob on a real pedal is the clock. That means the device's own Nyquist
 * frequency, f_clock/2, moves with the Time knob, and the reconstruction filter has to
 * come down with it — which is why a BBD delay gets darker as Time goes up. That coupling
 * is the effect. Everything else here is seasoning.
 *
 * At 4096 stages (MN3005, the chip in a DM-2): 40 ms gives a 51.2 kHz clock and full
 * bandwidth, 300 ms gives 6.8 kHz and a 3.4 kHz ceiling, 600 ms gives 3.4 kHz and 1.7 kHz.
 * At 1024 stages (MN3007) the same 300 ms gives an 853 Hz ceiling, which is why those
 * chips only ever appeared in short delays and choruses. Measured repeat bandwidth at
 * 4096 stages: 7.4 kHz at 50 ms, 2.3 kHz at 300 ms, 1.2 kHz at 600 ms.
 *
 * What is modelled: the clock rate, the reconstruction filter that tracks it, charge
 * transfer loss, the NE571-style compander, the BBD noise floor and clipping.
 *
 * What is not: the line runs at host rate with Hermite interpolation rather than being
 * resampled to the clock, so there is no genuine clock aliasing and no clock whine. Those
 * are arguably faults rather than features. If the absence ever proves audible the fix is
 * an oversampled variable-rate inner line, not a steeper filter.
 */

enum Param : std::size_t
{
    kTime,
    kSyncMode,
    kSyncDivision,
    kGlide,
    kFeedback,
    kStages,
    kTone,
    kCompander,
    kSaturation,
    kNoise,
    kModRate,
    kModDepth,
    kMix,
    kLevel,
    kSpread,
    kDucking,
    kParamCount
};

/// The one place parameter ranges live: registration and SetParam's clamping both read it.
/// Entries are in `Param` order.
inline constexpr std::array<delay_spec::ParamSpec, kParamCount> kParams = {{
    {"time", "Time", 320.0, 20.0, 800.0, "ms", "Time", false, 0.0},
    {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "Time", false, 1.0},
    {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "Time", false, 1.0},
    {"glide", "Glide", 40.0, 0.0, 500.0, "ms", "Time", true, 0.0},
    {"feedback", "Feedback", 0.35, 0.0, 1.15, "amount", "Time", false, 0.0},
    {"stages", "BBD Stages", 3.0, 0.0, 3.0, "enum", "BBD", false, 1.0},
    {"tone", "Tone", 0.5, 0.0, 1.0, "amount", "BBD", false, 0.0},
    {"compander", "Compander", 0.7, 0.0, 1.0, "amount", "BBD", false, 0.0},
    {"saturation", "Saturation", 0.35, 0.0, 1.0, "amount", "BBD", false, 0.0},
    {"noise", "Noise Floor", 0.2, 0.0, 1.0, "amount", "BBD", true, 0.0},
    {"modRate", "Mod Rate", 0.4, 0.0, 8.0, "Hz", "Modulation", false, 0.0},
    {"modDepth", "Mod Depth", 0.0, 0.0, 12.0, "ms", "Modulation", false, 0.0},
    {"mix", "Mix", 0.3, 0.0, 1.0, "amount", "Output", false, 0.0},
    {"level", "Repeat Level", 0.0, -12.0, 12.0, "dB", "Output", false, 0.0},
    {"spread", "Spread", 0.0, 0.0, 50.0, "ms", "Output", true, 0.0},
    {"ducking", "Ducking", 0.0, 0.0, 1.0, "amount", "Output", true, 0.0},
}};

/// Real devices, in the order the `stages` enum presents them.
inline constexpr std::array<double, 4> kStageCounts = {1024.0, 2048.0, 3328.0, 4096.0};

[[nodiscard]] inline std::vector<std::string> LabelsFor(const std::string& id)
{
    if (id == "stages")
    {
        return {"1024 (MN3007)", "2048 (MN3008)", "3328 (MN3011)", "4096 (MN3005)"};
    }

    return delay_spec::TempoLabelsFor(id);
}

/// The pedal's own fixed filtering, which is what limits bandwidth at short delays where
/// the clock is fast enough not to.
inline constexpr double kFilterCeilingHz = 9000.0;
inline constexpr double kFilterFloorHz = 200.0;
/// Reconstruction corner as a fraction of the clock. Below the clock's Nyquist with room
/// for the filter's own skirt.
inline constexpr double kClockFilterFraction = 0.40;
/// Charge transfer loss, as a gentler one-pole above the reconstruction corner. Fewer
/// stages means fewer transfers and less loss.
inline constexpr double kStageLossFraction = 0.45;
inline constexpr double kStageLossReference = 4096.0;

inline constexpr double kDcBlockHz = 30.0;
inline constexpr double kToneMinHz = 800.0;
inline constexpr double kToneDecades = 1.0;

/// BBD noise floor at `noise` = 1, roughly -70 dBFS before the expander acts on it. Gated
/// on the input by `IdleGate`: ungated, a delay nobody was playing into hissed at about
/// -95 dBFS for as long as it sat in the chain.
inline constexpr float kNoiseScale = 3.0e-4f;

/// The BBD's headroom, applied to everything written to the line whatever Saturation says
/// (see `RailClip`). Linear to -6 dBFS in the compressed domain, approaching full scale
/// above it. The compander means the input-equivalent rail sits higher still, which is
/// the reason real pedals carry one.
///
/// The rail is fixed rather than tied to Feedback. Raising saturator drive with Feedback was
/// tried first and measured backwards: a tanh saturator's ceiling is 1/gain, so more
/// feedback gave *quieter* self-oscillation (0.49 at 0.95 down to 0.08 at 1.15).
inline constexpr float kRailKnee = 0.5f;
inline constexpr float kRailCeiling = 1.0f;
/// Backstop on the value written to the line and on the wet output. The rail and the
/// matched compander clamps should make both unreachable.
inline constexpr float kWriteCeiling = 4.0f;

static_assert((delay_line::Compander::kControlInterval & (delay_line::Compander::kControlInterval - 1)) == 0,
              "the control interval is tested with a mask");
} // namespace analog_delay

class AnalogDelayEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    AnalogDelayEffect()
    {
        for (std::size_t index = 0; index < analog_delay::kParamCount; ++index)
        {
            mValues[index] = analog_delay::kParams[index].defaultValue;
        }
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        mMaxBlockSize = std::max(1, maxBlockSize);

        // Longest delay plus spread plus modulation, with room for the interpolator.
        const double maxDelayMs = analog_delay::kParams[analog_delay::kTime].maxValue +
                                  analog_delay::kParams[analog_delay::kSpread].maxValue +
                                  analog_delay::kParams[analog_delay::kModDepth].maxValue + 8.0;
        const auto maxSamples = static_cast<std::size_t>(mSampleRate * maxDelayMs * 0.001) + 8;

        for (auto& channel : mChannels)
        {
            channel.line.Resize(maxSamples);
            channel.compander.Prepare(mSampleRate);
            channel.noiseGate.Prepare(mSampleRate);
            channel.duckDetector.Prepare(mSampleRate);
            channel.duckDetector.SetTimes(1.0, 120.0);
        }

        mDelayScratch.assign(static_cast<std::size_t>(mMaxBlockSize), 0.0f);
        mGlide.Prepare(mSampleRate);
        mModLfo.Prepare(mSampleRate);

        UpdateGlide();
        UpdateModulation();
        UpdateCompander();
        UpdateTone();
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            channel.line.Clear();
            channel.reconstruct.Reset();
            channel.stageLoss.Reset();
            channel.tone.Reset();
            channel.dcBlock.Reset();
            channel.compander.Reset();
            channel.noiseGate.Reset();
            channel.duckDetector.Reset();
        }

        mChannels[0].noise.Seed(0x1234abcdu);
        mChannels[1].noise.Seed(0x89ab1357u);
        mModLfo.Reset();
        mGlide.Snap(TargetDelaySamples());
        UpdateClockFilters(true);
        mPrimed = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (numSamples <= 0 || mChannels[0].line.Empty())
        {
            return;
        }

        const int count = FillDelaySchedule(numSamples);

        if (inputs[0] && outputs[0])
        {
            ProcessChannel(mChannels[0], inputs[0], outputs[0], count, 0.0);
        }

        if (inputs[1] && outputs[1])
        {
            const double spreadSamples = mValues[analog_delay::kSpread] * 0.001 * mSampleRate;
            ProcessChannel(mChannels[1], inputs[1], outputs[1], count, spreadSamples);
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "bpm")
        {
            const double bpm = tempo_sync::ClampBpm(value);

            if (bpm != mBpm)
            {
                mBpm = bpm;
                RetargetDelay();
            }

            return;
        }

        const std::size_t index = delay_spec::FindParam(analog_delay::kParams, key);

        if (index >= analog_delay::kParamCount)
        {
            return;
        }

        const double clamped = delay_spec::ClampToSpec(analog_delay::kParams[index], value);

        if (clamped == mValues[index])
        {
            return;
        }

        mValues[index] = clamped;
        OnParamChanged(index);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bpm")
        {
            return mBpm;
        }

        if (key == "effectiveTimeMs")
        {
            return EffectiveDelayMs();
        }

        if (key == "clockHz")
        {
            return ClockHz(EffectiveDelayMs());
        }

        const std::size_t index = delay_spec::FindParam(analog_delay::kParams, key);
        return (index < analog_delay::kParamCount) ? mValues[index] : 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "delay_analog";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "delay";
    }

    /// The BBD clock rate for a given delay, in Hz. Public so tests can hold the
    /// reconstruction filter to it rather than to a magic number.
    [[nodiscard]] double ClockHz(double delayMs) const
    {
        const double stages = analog_delay::kStageCounts[StageIndex()];
        return stages / (2.0 * std::max(1.0e-4, delayMs * 0.001));
    }

    [[nodiscard]] double ReconstructionCornerHz(double delayMs) const
    {
        return std::clamp(analog_delay::kClockFilterFraction * ClockHz(delayMs), analog_delay::kFilterFloorHz,
                          analog_delay::kFilterCeilingHz);
    }

  private:
    struct ChannelState
    {
        delay_line::FractionalDelayLine line;
        delay_line::Lowpass4 reconstruct;
        delay_line::OnePoleLp stageLoss;
        delay_line::OnePoleLp tone;
        delay_line::OnePoleHp dcBlock;
        delay_line::Compander compander;
        delay_line::IdleGate noiseGate;
        delay_line::EnvelopeFollower duckDetector;
        delay_line::Rng noise;
    };

    [[nodiscard]] std::size_t StageIndex() const
    {
        const auto index = static_cast<std::size_t>(std::lround(mValues[analog_delay::kStages]));
        return std::min(index, analog_delay::kStageCounts.size() - 1);
    }

    [[nodiscard]] double EffectiveDelayMs() const
    {
        return delay_spec::ResolveDelayMs(analog_delay::kParams[analog_delay::kTime], mValues[analog_delay::kTime],
                                          mValues[analog_delay::kSyncMode], mValues[analog_delay::kSyncDivision], mBpm);
    }

    [[nodiscard]] double TargetDelaySamples() const
    {
        return EffectiveDelayMs() * 0.001 * mSampleRate;
    }

    void OnParamChanged(std::size_t index)
    {
        switch (index)
        {
        case analog_delay::kTime:
        case analog_delay::kSyncMode:
        case analog_delay::kSyncDivision:
            RetargetDelay();
            break;
        case analog_delay::kGlide:
            UpdateGlide();
            break;
        case analog_delay::kStages:
            UpdateClockFilters(true);
            break;
        case analog_delay::kTone:
            UpdateTone();
            break;
        case analog_delay::kCompander:
            UpdateCompander();
            break;
        case analog_delay::kModRate:
        case analog_delay::kModDepth:
            UpdateModulation();
            break;
        default:
            break;
        }
    }

    /// Glides only once audio is running. The executor applies a node's stored parameters
    /// after Prepare, so gliding from the start would give every freshly loaded preset a
    /// pitch sweep from the default Time to its own.
    void RetargetDelay()
    {
        if (mPrimed)
        {
            mGlide.SetTarget(TargetDelaySamples());
            return;
        }

        mGlide.Snap(TargetDelaySamples());
        UpdateClockFilters(true);
    }

    /// Glide 0 is instant, so it is exempt from the speed cap too.
    void UpdateGlide()
    {
        const double glideMs = mValues[analog_delay::kGlide];
        mGlide.SetTimeConstantMs(glideMs / delay_spec::kGlideConstantsToSettle);
        mGlide.SetMaxStep(glideMs > 0.0 ? delay_line::kMaxGlideStep : 0.0);
    }

    void UpdateModulation()
    {
        mModLfo.SetRate(mValues[analog_delay::kModRate]);
        mModSamples = static_cast<float>(mValues[analog_delay::kModDepth] * 0.001 * mSampleRate);
    }

    void UpdateTone()
    {
        const double hz =
            analog_delay::kToneMinHz * std::pow(10.0, mValues[analog_delay::kTone] * analog_delay::kToneDecades);

        for (auto& channel : mChannels)
        {
            channel.tone.SetCutoff(hz, mSampleRate);
            channel.dcBlock.SetCutoff(analog_delay::kDcBlockHz, mSampleRate);
        }
    }

    void UpdateCompander()
    {
        for (auto& channel : mChannels)
        {
            channel.compander.SetAmount(mValues[analog_delay::kCompander]);
        }
    }

    /// The reconstruction filter tracks the clock, which tracks the delay time. Updated per
    /// block from the glide's current value rather than per sample: a filter corner lagging
    /// a glide by one block is 1.3 ms at a 64-sample buffer, and recomputing four biquads
    /// per sample to remove that would cost more than the rest of the effect.
    void UpdateClockFilters(bool force)
    {
        const double delaySamples = std::max(1.0, mGlide.Value());
        const double delayMs = delaySamples * 1000.0 / mSampleRate;

        if (!force && std::fabs(delayMs - mFilterDelayMs) < mFilterDelayMs * 0.002)
        {
            return;
        }

        mFilterDelayMs = delayMs;

        const double clock = ClockHz(delayMs);
        const double corner = ReconstructionCornerHz(delayMs);
        const double stageScale =
            std::sqrt(analog_delay::kStageLossReference / analog_delay::kStageCounts[StageIndex()]);
        const double stageCorner = std::clamp(analog_delay::kStageLossFraction * clock * stageScale,
                                              analog_delay::kFilterFloorHz, mSampleRate * 0.45);

        for (auto& channel : mChannels)
        {
            channel.reconstruct.SetCutoff(corner, mSampleRate);
            channel.stageLoss.SetCutoff(stageCorner, mSampleRate);
        }
    }

    /// Fills the per-sample delay schedule once, so both channels read the same glide and
    /// modulation instead of each advancing them.
    [[nodiscard]] int FillDelaySchedule(int numSamples)
    {
        const int count = std::min(numSamples, static_cast<int>(mDelayScratch.size()));
        UpdateClockFilters(false);
        mPrimed = true;

        const bool modulating = mModSamples > 0.0f && mValues[analog_delay::kModRate] > 0.0;

        for (int i = 0; i < count; ++i)
        {
            float delay = static_cast<float>(mGlide.Next());

            if (modulating)
            {
                delay += mModSamples * mModLfo.Next();
            }

            mDelayScratch[static_cast<std::size_t>(i)] = delay;
        }

        return count;
    }

    void ProcessChannel(ChannelState& channel, const float* input, float* output, int numSamples, double extraDelay)
    {
        constexpr int kControlMask = delay_line::Compander::kControlInterval - 1;
        const auto feedback = static_cast<float>(mValues[analog_delay::kFeedback]);
        const auto wet = static_cast<float>(mValues[analog_delay::kMix]);
        const float dry = 1.0f - wet;
        const auto ducking = static_cast<float>(mValues[analog_delay::kDucking]);
        const auto repeatGain = static_cast<float>(DbToLinearGain(mValues[analog_delay::kLevel]));
        const float noiseAmount = static_cast<float>(mValues[analog_delay::kNoise]) * analog_delay::kNoiseScale;
        const float driveGain = delay_spec::SaturationDrive(mValues[analog_delay::kSaturation]);
        const bool companding = channel.compander.Active();

        for (int i = 0; i < numSamples; ++i)
        {
            if (companding && (i & kControlMask) == 0)
            {
                channel.compander.UpdateGains();
            }

            // A non-finite input would poison the compander's and gate's detectors for good:
            // measured, one NaN left the repeats silent from then on, the IsFinite guard on
            // the line catching the NaN gain by writing zero every sample. Scrubbed here, so
            // nothing downstream ever sees one; finite input passes through untouched.
            const float in = IsFinite(input[i]) ? input[i] : 0.0f;

            // Read before write (FractionalDelayLine's contract), with this sample's own
            // repeat fed straight back, so a repeat lands at exactly Time. Writing first
            // put every repeat one sample early.
            const double delaySamples = static_cast<double>(mDelayScratch[static_cast<std::size_t>(i)]) + extraDelay;
            float band = channel.line.ReadHermite(delaySamples);

            // The noise floor is generated inside the device, so it is filtered with the
            // signal and expanded with it. That is what makes it pump rather than hiss.
            if (noiseAmount > 0.0f)
            {
                const float gate = channel.noiseGate.Process(in);

                if (gate > 0.0f)
                {
                    band += channel.noise.NextBipolar() * noiseAmount * gate;
                }
            }

            // Tone sits inside the loop, so each repeat is darker than the last.
            band = channel.reconstruct.Process(band);
            band = channel.stageLoss.Process(band);
            band = channel.dcBlock.Process(band);
            band = channel.tone.Process(band);
            band = delay_line::FlushDenormal(band);

            if (!IsFinite(band))
            {
                band = 0.0f;
            }

            // The whole BBD loop lives in the compressed domain: the input is compressed on
            // its way in, the repeats are fed back *before* the expander and so are never
            // compressed twice, and the expander only ever sees the line on its way out.
            // That keeps the compander out of the loop's gain entirely, so the loop cannot
            // gain more than Feedback times the filters, whatever the detectors are doing.
            // With the tap after the expander instead, Feedback 0.80 was measured running
            // away to an amplitude of 14.
            float pre = companding ? channel.compander.Compress(in) : in;
            pre = delay_line::SoftSaturate(pre + band * feedback, driveGain);
            pre = delay_line::RailClip(pre, analog_delay::kRailKnee, analog_delay::kRailCeiling);
            pre = std::clamp(pre, -analog_delay::kWriteCeiling, analog_delay::kWriteCeiling);

            if (!IsFinite(pre))
            {
                pre = 0.0f;
            }

            channel.line.Write(pre);

            if (companding)
            {
                band = channel.compander.Expand(band);
            }

            band = std::clamp(band, -analog_delay::kWriteCeiling, analog_delay::kWriteCeiling);

            float duckGain = 1.0f;

            if (ducking > 0.0f)
            {
                const float envelope = channel.duckDetector.Process(in);
                duckGain = 1.0f - ducking * std::min(envelope * 4.0f, 1.0f);
            }

            output[i] = in * dry + band * repeatGain * wet * duckGain;
        }
    }

    std::array<double, analog_delay::kParamCount> mValues{};
    ChannelState mChannels[2];
    std::vector<float> mDelayScratch;
    delay_line::GlideRamp mGlide;
    delay_line::LfoPhasor mModLfo;

    double mSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    double mBpm = tempo_sync::kDefaultBpm;
    double mFilterDelayMs = 0.0;
    float mModSamples = 0.0f;
    bool mPrimed = false;
};

namespace analog_delay
{
/// Order is fixed once shipped: saved presets reference these by index.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    using delay_spec::MakePreset;

    return {
        MakePreset("dm2", "DM-2", true,
                   {{"time", 300.0},
                    {"feedback", 0.35},
                    {"stages", 3.0},
                    {"tone", 0.45},
                    {"compander", 0.8},
                    {"saturation", 0.35},
                    {"noise", 0.2},
                    {"modDepth", 0.0},
                    {"mix", 0.3}}),
        MakePreset("memory-man", "Memory Man", false,
                   {{"time", 500.0},
                    {"feedback", 0.42},
                    {"stages", 3.0},
                    {"tone", 0.5},
                    {"compander", 0.75},
                    {"saturation", 0.4},
                    {"noise", 0.25},
                    {"modRate", 0.6},
                    {"modDepth", 6.0},
                    {"mix", 0.32}}),
        MakePreset("short-analog", "Short Analog", false,
                   {{"time", 80.0},
                    {"feedback", 0.2},
                    {"stages", 0.0},
                    {"tone", 0.6},
                    {"compander", 0.7},
                    {"saturation", 0.3},
                    {"noise", 0.15},
                    {"modDepth", 0.0},
                    {"mix", 0.28}}),
        MakePreset("clean-analog", "Clean Analog", false,
                   {{"time", 250.0},
                    {"feedback", 0.3},
                    {"stages", 3.0},
                    {"tone", 0.75},
                    {"compander", 0.1},
                    {"saturation", 0.15},
                    {"noise", 0.05},
                    {"modDepth", 0.0},
                    {"mix", 0.3}}),
        MakePreset("runaway", "Runaway", false,
                   {{"time", 400.0},
                    {"feedback", 1.12},
                    {"stages", 3.0},
                    {"tone", 0.4},
                    {"compander", 0.85},
                    {"saturation", 0.6},
                    {"noise", 0.3},
                    {"modRate", 0.3},
                    {"modDepth", 3.0},
                    {"mix", 0.4}}),
    };
}
} // namespace analog_delay

inline void RegisterAnalogDelayEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDelayAnalog;
    info.aliases = {"delay_analog"};
    info.displayName = "Analog Delay";
    info.category = "delay";
    info.description = "Bucket-brigade delay. Time sets the BBD clock, so the repeats lose bandwidth as "
                       "the delay lengthens, with companding, saturation and self-oscillation.";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.presets = analog_delay::FactoryPresets();
    info.parameters = delay_spec::BuildParameterDefs(analog_delay::kParams, analog_delay::LabelsFor);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<AnalogDelayEffect>(); });
}
} // namespace guitarfx
