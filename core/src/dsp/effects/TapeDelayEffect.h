#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/DelayEffectSpec.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/TapeTransport.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace guitarfx
{
namespace tape_delay
{
/**
 * Tape echo: an Echoplex EP-3's single moving head, or a three-head machine's fixed ones.
 *
 * What makes it tape rather than a digital delay with a filter is that the delay time is a
 * moving mechanical thing:
 *
 *  - Wow and flutter modulate it continuously. `TapeTransport` models them as a *speed*
 *    error, so flutter is audible as pitch without the delay time swinging much at all.
 *  - A time change glides the read position rather than jumping, so it bends the pitch of
 *    everything already on the tape. `glide` sets how long the capstan takes to get there,
 *    and the speed cap (`kMaxGlideStep`) keeps it between half and one and a half speed.
 *  - The repeats darken and thicken with each pass, because the playback EQ (head bump,
 *    high and low cut, and the extra HF loss `age` brings) sits inside the feedback loop.
 *    Measured centroid of successive repeats at the defaults: 2.2, 1.6, 1.3 kHz.
 *
 * Record path: input plus feedback, through an asymmetric saturator (tape's curve is not
 * symmetric, and the second harmonic that gives is the warmth) and the device rail.
 * Playback: one to three heads, all riding the same wobbling tape, summed.
 */

enum Param : std::size_t
{
    kTime,
    kSyncMode,
    kSyncDivision,
    kGlide,
    kFeedback,
    kHeadMode,
    kWow,
    kFlutter,
    kAge,
    kSaturation,
    kHighCut,
    kLowCut,
    kHeadBump,
    kMix,
    kLevel,
    kSpread,
    kDucking,
    kParamCount
};

/// The one place parameter ranges live: registration and SetParam's clamping both read it.
/// Entries are in `Param` order.
inline constexpr std::array<delay_spec::ParamSpec, kParamCount> kParams = {{
    {"time", "Time", 400.0, 20.0, 1500.0, "ms", "Time", false, 0.0},
    {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "Time", false, 1.0},
    {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "Time", false, 1.0},
    {"glide", "Glide", 120.0, 0.0, 2000.0, "ms", "Time", false, 0.0},
    {"feedback", "Feedback", 0.35, 0.0, 1.10, "amount", "Time", false, 0.0},
    {"headMode", "Heads", 0.0, 0.0, 6.0, "enum", "Heads", false, 1.0},
    {"wow", "Wow", 0.25, 0.0, 1.0, "amount", "Tape", false, 0.0},
    {"flutter", "Flutter", 0.25, 0.0, 1.0, "amount", "Tape", false, 0.0},
    {"age", "Tape Age", 0.35, 0.0, 1.0, "amount", "Tape", false, 0.0},
    {"saturation", "Saturation", 0.30, 0.0, 1.0, "amount", "Tape", false, 0.0},
    {"highCut", "High Cut", 5000.0, 500.0, 16000.0, "Hz", "Tape", true, 0.0},
    {"lowCut", "Low Cut", 90.0, 20.0, 800.0, "Hz", "Tape", true, 0.0},
    {"headBump", "Head Bump", 0.35, 0.0, 1.0, "amount", "Tape", true, 0.0},
    {"mix", "Mix", 0.30, 0.0, 1.0, "amount", "Output", false, 0.0},
    {"level", "Repeat Level", 0.0, -12.0, 12.0, "dB", "Output", false, 0.0},
    {"spread", "Spread", 0.0, 0.0, 50.0, "ms", "Output", true, 0.0},
    {"ducking", "Ducking", 0.0, 0.0, 1.0, "amount", "Output", true, 0.0},
}};

/// Playback head positions as a fraction of Time, which is the longest head. These
/// approximate a Space Echo's head spacing; they are not taken from a service manual, so
/// user-facing text says "three-head tape echo" rather than naming the machine.
inline constexpr std::array<double, 3> kHeadRatios = {0.317, 0.633, 1.0};
inline constexpr std::size_t kHeadCount = kHeadRatios.size();

/// Which heads each `headMode` enables, as bits 0-2 for heads 1-3. Index 0 is head 3 alone,
/// so the default is a single echo at exactly Time.
inline constexpr std::array<std::uint8_t, 7> kHeadMasks = {0b100, 0b010, 0b001, 0b011, 0b110, 0b101, 0b111};

[[nodiscard]] inline std::vector<std::string> LabelsFor(const std::string& id)
{
    if (id == "headMode")
    {
        return {"Head 3", "Head 2", "Head 1", "Heads 1+2", "Heads 2+3", "Heads 1+3", "Heads 1+2+3"};
    }

    return delay_spec::TempoLabelsFor(id);
}

/// Age takes High Cut down to this fraction of itself at full wear.
inline constexpr double kAgedHighCutFraction = 0.35;

/// Head bump: a resonant lift from the repro head's geometry.
inline constexpr double kHeadBumpHz = 100.0;
inline constexpr double kHeadBumpQ = 1.0;
inline constexpr double kHeadBumpMaxDb = 4.0;

/// Tape hiss at `age` = 1, before the playback EQ, roughly -66 dBFS. Gated on the input by
/// `IdleGate`, so a worn tape nobody is playing into is silent.
inline constexpr float kHissScale = 5.0e-4f;

/// The loop-gain search grid: log-spaced, 20 Hz to just under Nyquist.
inline constexpr int kLoopGainGridPoints = 96;

/// Record-path asymmetry at full Saturation.
inline constexpr float kMaxSaturationBias = 0.2f;

/// Device rail on the record path; see `RailClip`.
inline constexpr float kRailKnee = 0.5f;
inline constexpr float kRailCeiling = 1.0f;
inline constexpr float kWriteCeiling = 4.0f;

/// The transport's worst-case offset is under 4 ms at 48 kHz (measured); this is headroom
/// for it in the buffer.
inline constexpr double kTransportHeadroomMs = 8.0;
} // namespace tape_delay

class TapeDelayEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    TapeDelayEffect()
    {
        for (std::size_t index = 0; index < tape_delay::kParamCount; ++index)
        {
            mValues[index] = tape_delay::kParams[index].defaultValue;
        }
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        mMaxBlockSize = std::max(1, maxBlockSize);

        const double maxDelayMs = tape_delay::kParams[tape_delay::kTime].maxValue +
                                  tape_delay::kParams[tape_delay::kSpread].maxValue + tape_delay::kTransportHeadroomMs;
        const auto maxSamples = static_cast<std::size_t>(mSampleRate * maxDelayMs * 0.001) + 8;

        for (auto& channel : mChannels)
        {
            channel.line.Resize(maxSamples);
            channel.duckDetector.Prepare(mSampleRate);
            channel.duckDetector.SetTimes(1.0, 120.0);
            channel.hissGate.Prepare(mSampleRate);
        }

        const auto scratchSize = static_cast<std::size_t>(mMaxBlockSize);
        mBaseScratch.assign(scratchSize, 0.0f);
        mWobbleScratch.assign(scratchSize, 0.0f);
        mDropoutScratch.assign(scratchSize, 1.0f);

        mGlide.Prepare(mSampleRate);
        mTransport.Prepare(mSampleRate);
        mDropout.Prepare(mSampleRate);

        UpdateGlide();
        UpdateTransport();
        UpdateHeads();
        UpdateFilters();
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            channel.line.Clear();
            channel.headBump.Reset();
            channel.highCut.Reset();
            channel.lowCut.Reset();
            channel.duckDetector.Reset();
            channel.hissGate.Reset();
        }

        mChannels[0].hiss.Seed(0x7a9e0001u);
        mChannels[1].hiss.Seed(0x7a9e0002u);
        mDropout.Reset();
        mTransport.Reset();
        mGlide.Snap(TargetDelaySamples());
        mPrimed = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (numSamples <= 0 || mChannels[0].line.Empty())
        {
            return;
        }

        const int count = FillSchedule(numSamples);

        if (inputs[0] && outputs[0])
        {
            ProcessChannel(mChannels[0], inputs[0], outputs[0], count, 0.0);
        }

        if (inputs[1] && outputs[1])
        {
            const double spreadSamples = mValues[tape_delay::kSpread] * 0.001 * mSampleRate;
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

        const std::size_t index = delay_spec::FindParam(tape_delay::kParams, key);

        if (index >= tape_delay::kParamCount)
        {
            return;
        }

        const double clamped = delay_spec::ClampToSpec(tape_delay::kParams[index], value);

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

        const std::size_t index = delay_spec::FindParam(tape_delay::kParams, key);
        return (index < tape_delay::kParamCount) ? mValues[index] : 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "delay_tape";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "delay";
    }

  private:
    struct ChannelState
    {
        delay_line::FractionalDelayLine line;
        delay_line::Biquad headBump;
        delay_line::OnePoleLp highCut;
        delay_line::OnePoleHp lowCut;
        delay_line::EnvelopeFollower duckDetector;
        delay_line::IdleGate hissGate;
        delay_line::Rng hiss;
    };

    /// Time is the longest head, so this is head 3's delay.
    [[nodiscard]] double EffectiveDelayMs() const
    {
        return delay_spec::ResolveDelayMs(tape_delay::kParams[tape_delay::kTime], mValues[tape_delay::kTime],
                                          mValues[tape_delay::kSyncMode], mValues[tape_delay::kSyncDivision], mBpm);
    }

    [[nodiscard]] double TargetDelaySamples() const
    {
        return EffectiveDelayMs() * 0.001 * mSampleRate;
    }

    void OnParamChanged(std::size_t index)
    {
        switch (index)
        {
        case tape_delay::kTime:
        case tape_delay::kSyncMode:
        case tape_delay::kSyncDivision:
            RetargetDelay();
            break;
        case tape_delay::kGlide:
            UpdateGlide();
            break;
        case tape_delay::kWow:
        case tape_delay::kFlutter:
            UpdateTransport();
            break;
        case tape_delay::kHeadMode:
            UpdateHeads();
            break;
        case tape_delay::kAge:
        case tape_delay::kHighCut:
        case tape_delay::kLowCut:
        case tape_delay::kHeadBump:
            UpdateFilters();
            break;
        default:
            break;
        }
    }

    /// Glides only once audio is running. The executor applies a node's stored parameters
    /// after Prepare, so gliding from the start would give every freshly loaded preset a
    /// pitch dive from the default Time to its own.
    void RetargetDelay()
    {
        if (mPrimed)
        {
            mGlide.SetTarget(TargetDelaySamples());
        }
        else
        {
            mGlide.Snap(TargetDelaySamples());
        }
    }

    /// Glide 0 is instant, as on the digital delay, so it is exempt from the speed cap too;
    /// with the cap applied even at 0, a 300-to-600 ms change still dived an octave.
    void UpdateGlide()
    {
        const double glideMs = mValues[tape_delay::kGlide];
        mGlide.SetTimeConstantMs(glideMs / delay_spec::kGlideConstantsToSettle);
        mGlide.SetMaxStep(glideMs > 0.0 ? delay_line::kMaxGlideStep : 0.0);
    }

    void UpdateTransport()
    {
        mTransport.SetWow(static_cast<float>(mValues[tape_delay::kWow]));
        mTransport.SetFlutter(static_cast<float>(mValues[tape_delay::kFlutter]));
    }

    /// Output sums the heads at 1/sqrt(n), so adding heads does not jump in level; the
    /// feedback sums them at 1/n, so however many heads feed back the loop gain stays at
    /// or under Feedback. Feeding back the sum rather than one head is what gives the
    /// multi-head modes their build-up.
    void UpdateHeads()
    {
        const auto mode = static_cast<std::size_t>(std::lround(mValues[tape_delay::kHeadMode]));
        mHeadMask = tape_delay::kHeadMasks[std::min(mode, tape_delay::kHeadMasks.size() - 1)];

        int active = 0;

        for (std::size_t head = 0; head < tape_delay::kHeadCount; ++head)
        {
            if ((mHeadMask >> head) & 1u)
            {
                ++active;
            }
        }

        active = std::max(1, active);
        mOutputHeadGain = 1.0f / std::sqrt(static_cast<float>(active));
        mFeedbackHeadGain = 1.0f / static_cast<float>(active);
    }

    void UpdateFilters()
    {
        const double age = mValues[tape_delay::kAge];
        const double highCutHz = mValues[tape_delay::kHighCut] * (1.0 - (1.0 - tape_delay::kAgedHighCutFraction) * age);
        const double bumpDb = mValues[tape_delay::kHeadBump] * tape_delay::kHeadBumpMaxDb;

        for (auto& channel : mChannels)
        {
            channel.highCut.SetCutoff(highCutHz, mSampleRate);
            channel.lowCut.SetCutoff(mValues[tape_delay::kLowCut], mSampleRate);

            if (bumpDb > 0.0)
            {
                channel.headBump.SetPeaking(tape_delay::kHeadBumpHz, mSampleRate, bumpDb, tape_delay::kHeadBumpQ);
            }
            else
            {
                channel.headBump.SetBypass();
            }
        }

        mLoopNormalize = static_cast<float>(1.0 / std::max(1.0, PeakLoopEqGain()));
        mHissLevel = static_cast<float>(age) * tape_delay::kHissScale;
        mDropout.SetAmount(static_cast<float>(age));
    }

    /// The loudest the in-loop playback EQ gets at any frequency.
    ///
    /// The head bump is a boost inside the loop: left alone, full bump with Low Cut at its
    /// minimum takes the loop over unity at 100 Hz for any Feedback above about 0.64. The
    /// feedback tap is divided by this peak whenever it exceeds one, so loop gain never
    /// passes Feedback at any frequency while the EQ's *shape* still compounds each pass —
    /// the repeats still get fatter, they just cannot run away below Feedback 1.
    ///
    /// Measured on the whole EQ rather than taken from the bump's own peak. Low Cut sits
    /// right under the bump, so the bump's peak alone overstates the loop by enough that
    /// Feedback at its 1.10 maximum could not self-oscillate at the default settings.
    /// Runs on a parameter change, never per sample.
    [[nodiscard]] double PeakLoopEqGain() const
    {
        const auto& channel = mChannels[0];
        const double lowHz = 20.0;
        const double highHz = mSampleRate * 0.49;
        const double ratio = std::pow(highHz / lowHz, 1.0 / (tape_delay::kLoopGainGridPoints - 1));
        double peak = 0.0;
        double hz = lowHz;

        for (int point = 0; point < tape_delay::kLoopGainGridPoints; ++point)
        {
            const double w = delay_line::kTwoPi * hz / mSampleRate;
            const double gain =
                channel.headBump.Magnitude(w) * channel.highCut.Magnitude(w) * channel.lowCut.Magnitude(w);
            peak = std::max(peak, gain);
            hz *= ratio;
        }

        return peak;
    }

    /// One pass per block for everything the whole tape shares — the capstan glide, wow and
    /// flutter, dropouts — so both channels read the same tape instead of each advancing it.
    [[nodiscard]] int FillSchedule(int numSamples)
    {
        const int count = std::min(numSamples, static_cast<int>(mBaseScratch.size()));
        const bool wobbling = !mTransport.Idle();
        const bool dropping = !mDropout.Idle();
        mPrimed = true;

        for (int i = 0; i < count; ++i)
        {
            const auto index = static_cast<std::size_t>(i);
            mBaseScratch[index] = static_cast<float>(mGlide.Next());
            mWobbleScratch[index] = wobbling ? mTransport.Next() : 0.0f;
            mDropoutScratch[index] = dropping ? mDropout.Next() : 1.0f;
        }

        return count;
    }

    void ProcessChannel(ChannelState& channel, const float* input, float* output, int numSamples, double extraDelay)
    {
        const auto wet = static_cast<float>(mValues[tape_delay::kMix]);
        const float dry = 1.0f - wet;
        const auto ducking = static_cast<float>(mValues[tape_delay::kDucking]);
        const auto repeatGain = static_cast<float>(DbToLinearGain(mValues[tape_delay::kLevel]));
        const float feedbackScale =
            static_cast<float>(mValues[tape_delay::kFeedback]) * mFeedbackHeadGain * mLoopNormalize;
        const float drive = delay_spec::SaturationDrive(mValues[tape_delay::kSaturation]);
        const float bias = static_cast<float>(mValues[tape_delay::kSaturation]) * tape_delay::kMaxSaturationBias;

        const double head1 = tape_delay::kHeadRatios[0];
        const double head2 = tape_delay::kHeadRatios[1];
        const bool useHead1 = (mHeadMask & 0b001) != 0;
        const bool useHead2 = (mHeadMask & 0b010) != 0;
        const bool useHead3 = (mHeadMask & 0b100) != 0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto index = static_cast<std::size_t>(i);

            // A non-finite input would poison the hiss gate's and ducking's detectors for
            // good (on the analog delay the same thing left the repeats silent). Scrubbed
            // here, so nothing downstream ever sees one; finite input passes untouched.
            const float in = IsFinite(input[i]) ? input[i] : 0.0f;

            // Read before write (FractionalDelayLine's contract), with this sample's own
            // playback fed straight back, so a repeat lands at exactly Time. Writing first
            // put every repeat one sample early. Every head rides the same tape, so they
            // share one wobble.
            const double base = static_cast<double>(mBaseScratch[index]);
            const double wobble = static_cast<double>(mWobbleScratch[index]) + extraDelay;
            float playback = 0.0f;

            if (useHead1)
            {
                playback += channel.line.ReadHermite(base * head1 + wobble);
            }

            if (useHead2)
            {
                playback += channel.line.ReadHermite(base * head2 + wobble);
            }

            if (useHead3)
            {
                playback += channel.line.ReadHermite(base + wobble);
            }

            // Hiss comes off the tape at the head, so it goes through the playback EQ and
            // round the loop with everything else.
            if (mHissLevel > 0.0f)
            {
                const float gate = channel.hissGate.Process(in);

                if (gate > 0.0f)
                {
                    playback += channel.hiss.NextBipolar() * mHissLevel * gate;
                }
            }

            playback = channel.headBump.Process(playback);
            playback = channel.highCut.Process(playback);
            playback = channel.lowCut.Process(playback);
            playback *= mDropoutScratch[index];
            playback = delay_line::FlushDenormal(playback);

            if (!IsFinite(playback))
            {
                playback = 0.0f;
            }

            float record = delay_line::AsymSaturate(in + playback * feedbackScale, drive, bias);
            record = delay_line::RailClip(record, tape_delay::kRailKnee, tape_delay::kRailCeiling);
            record = std::clamp(record, -tape_delay::kWriteCeiling, tape_delay::kWriteCeiling);

            if (!IsFinite(record))
            {
                record = 0.0f;
            }

            channel.line.Write(record);

            float duckGain = 1.0f;

            if (ducking > 0.0f)
            {
                const float envelope = channel.duckDetector.Process(in);
                duckGain = 1.0f - ducking * std::min(envelope * 4.0f, 1.0f);
            }

            const float repeats = playback * mOutputHeadGain * repeatGain;
            output[i] = in * dry + repeats * wet * duckGain;
        }
    }

    std::array<double, tape_delay::kParamCount> mValues{};
    ChannelState mChannels[2];
    std::vector<float> mBaseScratch;
    std::vector<float> mWobbleScratch;
    std::vector<float> mDropoutScratch;
    delay_line::GlideRamp mGlide;
    delay_line::TapeTransport mTransport;
    delay_line::TapeDropout mDropout;

    double mSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    double mBpm = tempo_sync::kDefaultBpm;
    std::uint8_t mHeadMask = 0b100;
    bool mPrimed = false;
    float mOutputHeadGain = 1.0f;
    float mFeedbackHeadGain = 1.0f;
    float mLoopNormalize = 1.0f;
    float mHissLevel = 0.0f;
};

namespace tape_delay
{
/// Order is fixed once shipped: saved presets reference these by index.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    using delay_spec::MakePreset;

    return {
        MakePreset("echoplex", "Echoplex EP-3", true,
                   {{"time", 400.0},
                    {"feedback", 0.38},
                    {"headMode", 0.0},
                    {"wow", 0.2},
                    {"flutter", 0.25},
                    {"age", 0.35},
                    {"saturation", 0.35},
                    {"headBump", 0.35},
                    {"mix", 0.3}}),
        MakePreset("slapback", "Slapback", false,
                   {{"time", 110.0},
                    {"feedback", 0.12},
                    {"headMode", 0.0},
                    {"wow", 0.08},
                    {"flutter", 0.15},
                    {"age", 0.2},
                    {"saturation", 0.3},
                    {"highCut", 6000.0},
                    {"mix", 0.35}}),
        MakePreset("three-head", "Three Heads", false,
                   {{"time", 480.0},
                    {"feedback", 0.45},
                    {"headMode", 6.0},
                    {"wow", 0.3},
                    {"flutter", 0.3},
                    {"age", 0.4},
                    {"saturation", 0.35},
                    {"headBump", 0.5},
                    {"mix", 0.3}}),
        MakePreset("worn-tape", "Worn Tape", false,
                   {{"time", 600.0},
                    {"feedback", 0.45},
                    {"headMode", 0.0},
                    {"wow", 0.6},
                    {"flutter", 0.5},
                    {"age", 0.85},
                    {"saturation", 0.6},
                    {"mix", 0.35}}),
        MakePreset("dub", "Dub", false,
                   {{"time", 450.0},
                    {"feedback", 1.02},
                    {"headMode", 4.0},
                    {"wow", 0.25},
                    {"flutter", 0.25},
                    {"age", 0.45},
                    {"saturation", 0.7},
                    {"lowCut", 150.0},
                    {"mix", 0.4}}),
    };
}
} // namespace tape_delay

inline void RegisterTapeDelayEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDelayTape;
    info.aliases = {"delay_tape"};
    info.displayName = "Tape Echo";
    info.category = "delay";
    info.description = "Tape echo with wow and flutter, pitch-bending time changes, tape saturation and wear, "
                       "and one to three playback heads.";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.presets = tape_delay::FactoryPresets();
    info.parameters = delay_spec::BuildParameterDefs(tape_delay::kParams, tape_delay::LabelsFor);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<TapeDelayEffect>(); });
}
} // namespace guitarfx
