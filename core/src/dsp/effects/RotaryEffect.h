#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
namespace rotary
{
enum Param : std::size_t
{
    kSpeed,
    kSlowRate,
    kFastRate,
    kRamp,
    kDrive,
    kBalance,
    kSpread,
    kDepth,
    kLevel,
    kMix,
    kParamCount
};

/// Index order is stored in presets: append, never reorder.
enum class Speed
{
    Slow,
    Fast,
    Brake
};

inline constexpr const char* kSpeedLabels[] = {"Slow", "Fast", "Brake"};

inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"speed", "Speed", 0.0, 0.0, 2.0, "enum", "Rotor", false, 1.0, kSpeedLabels},
    {"slowRate", "Slow Speed", 0.8, 0.3, 2.0, "Hz", "Rotor", false, 0.0},
    {"fastRate", "Fast Speed", 6.7, 4.0, 9.0, "Hz", "Rotor", false, 0.0},
    {"ramp", "Ramp", 0.5, 0.0, 1.0, "amount", "Rotor", false, 0.0},
    {"drive", "Drive", 0.2, 0.0, 1.0, "amount", "Amp", false, 0.0},
    {"balance", "Horn/Drum", 0.0, -1.0, 1.0, "amount", "Amp", false, 0.0},
    {"spread", "Mic Spread", 0.8, 0.0, 1.0, "amount", "Mics", false, 0.0},
    {"depth", "Depth", 0.7, 0.0, 1.0, "amount", "Mics", false, 0.0},
    {"level", "Level", 0.0, -12.0, 12.0, "dB", "Output", false, 0.0},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0},
}};

/// The amp's crossover, where the horn takes over from the drum.
inline constexpr double kCrossoverHz = 800.0;

/// The drum turns a little slower than the horn at either setting, as a real cabinet's does.
inline constexpr double kDrumSlowRatio = 0.85;
inline constexpr double kDrumFastRatio = 0.88;

/// Seconds the rotors take to get most of the way to a new speed at Ramp's noon. The horn is
/// light and spins up in under a second; the drum is heavy and takes several.
inline constexpr double kHornUpSeconds = 0.8;
inline constexpr double kHornDownSeconds = 1.2;
inline constexpr double kDrumUpSeconds = 3.5;
inline constexpr double kDrumDownSeconds = 4.5;

/// How far each rotor's sound source swings, in metres. The horn's mouth travels round the
/// whole circle; the drum is a baffle over a fixed speaker, so its own movement is a fraction
/// of that. Divided by the speed of sound, this is the Doppler swing of the delay.
inline constexpr double kHornRadiusM = 0.15;
inline constexpr double kDrumRadiusM = 0.05;
inline constexpr double kSpeedOfSound = 343.0;

/// Every tap sits this far back, so the shortest Doppler reading never reaches the write head.
inline constexpr double kBaseDelayMs = 0.25;

/// The horn's sound off the back of the cabinet: a later, quieter copy of the horn heard when
/// the mouth points away. It is what keeps the horn's tremolo a swell rather than on and off.
inline constexpr double kReflectionDelayMs = 0.6;
inline constexpr float kReflectionGain = 0.3f;

/// How dark the horn gets pointing away from a mic, at full Depth.
inline constexpr double kHornShadowHz = 2200.0;

/// Drive's range: SoftSaturate's drive at full Drive.
inline constexpr float kMaxDrive = 6.0f;

/// Keeps the cabinet about as loud as bypass on a guitar at the nominal level, whatever Drive
/// and Depth are set to. Each rotor spends half its turn pointing away, which costs up to
/// 2.6 dB at full Depth, and the amp's saturation takes up to 4.9 dB off the peaks at full
/// Drive. Fitted on the demo riffs to within 0.2 dB.
[[nodiscard]] inline double MakeupDb(double drive, double depth) noexcept
{
    return 0.1 + drive * (5.4 - 0.5 * drive) + 2.6 * depth;
}

/// Knob changes are smoothed over this, so Level, Mix and the balance never step.
inline constexpr double kSmoothingMs = 20.0;
} // namespace rotary

/**
 * A rotating speaker cabinet: a horn above, a drum below, recorded by two mics.
 *
 * The input is summed to mono, through the cabinet's tube amp (Drive), and split at 800 Hz by
 * a Linkwitz-Riley crossover, so horn and drum add back to the flat input when nothing turns.
 * Each rotor's sound reaches a mic along a path that lengthens and shortens as it turns:
 * a delay swung by the rotor's radius over the speed of sound, which is the Doppler, and a
 * gain that follows where the rotor is pointing, which is the tremolo. The horn also darkens
 * as it points away, and a reflection off the back of the cabinet fills its troughs. Horn and
 * drum turn in opposite directions.
 *
 * Speed switches the rotors between Slow (chorale) and Fast (tremolo), or Brake to stop them.
 * Neither changes speed at once: the horn takes about a second to get there and the drum
 * several, which is the sound of a rotary switching. Ramp scales those times.
 *
 * Mic Spread moves the two mics apart round the cabinet, from one place (mono) to opposite
 * sides; Depth is how close they are, so how much Doppler and tremolo they hear.
 */
class RotaryEffect : public EffectProcessor
{
  public:
    RotaryEffect()
    {
        mValues = DefaultParamValues(rotary::kParams);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        const double capacityMs = rotary::kBaseDelayMs + rotary::kReflectionDelayMs +
                                  2000.0 * rotary::kHornRadiusM / rotary::kSpeedOfSound + 1.0;
        const auto capacity = static_cast<std::size_t>(std::ceil(capacityMs * 0.001 * sampleRate)) + 8;
        mHornLine.Resize(capacity);
        mDrumLine.Resize(capacity);

        mLowPass = biquad::LowPass(rotary::kCrossoverHz, biquad::kButterworthQ, sampleRate);
        mHighPass = biquad::HighPass(rotary::kCrossoverHz, biquad::kButterworthQ, sampleRate);

        for (auto& shadow : mShadow)
        {
            shadow.SetCutoff(rotary::kHornShadowHz, sampleRate);
        }

        mBaseDelay = rotary::kBaseDelayMs * 0.001 * sampleRate;
        mReflectionDelay = rotary::kReflectionDelayMs * 0.001 * sampleRate;
        mHornSwing = rotary::kHornRadiusM / rotary::kSpeedOfSound * sampleRate;
        mDrumSwing = rotary::kDrumRadiusM / rotary::kSpeedOfSound * sampleRate;
        mSmoothing = static_cast<float>(1.0 - std::exp(-1000.0 / (rotary::kSmoothingMs * sampleRate)));
        mPrepared = true;
        Reset();
    }

    void Reset() override
    {
        mHornLine.Clear();
        mDrumLine.Clear();

        for (auto& state : mLowStates)
        {
            state.Reset();
        }

        for (auto& state : mHighStates)
        {
            state.Reset();
        }

        for (auto& shadow : mShadow)
        {
            shadow.Reset();
        }

        mHornPhase = 0.0;
        mDrumPhase = 0.37;
        SnapToTargets();
        mStarted = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        if (!mPrepared)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        const float* inL = inputs[0] ? inputs[0] : inputs[1];
        const float* inR = inputs[1] ? inputs[1] : inL;
        TakeUpTargets();

        // The first block after a reset starts where the knobs are, so a node set up after
        // Prepare (a preset loading) neither glides from the defaults nor spins up from Slow.
        if (!mStarted)
        {
            SnapToTargets();
            mStarted = true;
        }

        const double hornTarget = HornTarget();
        const double drumTarget = DrumTarget();
        const double rampScale = RampScale();
        const double hornCoefficient =
            RampCoefficient((hornTarget > mHornHz ? rotary::kHornUpSeconds : rotary::kHornDownSeconds) * rampScale);
        const double drumCoefficient =
            RampCoefficient((drumTarget > mDrumHz ? rotary::kDrumUpSeconds : rotary::kDrumDownSeconds) * rampScale);
        const float drive = static_cast<float>(Value(rotary::kDrive)) * rotary::kMaxDrive;
        float wet[2] = {0.0f, 0.0f};

        for (int i = 0; i < numSamples; ++i)
        {
            const float rawL = inL ? inL[i] : 0.0f;
            const float rawR = inR ? inR[i] : 0.0f;
            const float l = IsFinite(rawL) ? rawL : 0.0f;
            const float r = IsFinite(rawR) ? rawR : 0.0f;

            mHornHz += hornCoefficient * (hornTarget - mHornHz);
            mDrumHz += drumCoefficient * (drumTarget - mDrumHz);
            mHornPhase = Wrap(mHornPhase + mHornHz / mSampleRate);
            // The drum turns the other way.
            mDrumPhase = Wrap(mDrumPhase - mDrumHz / mSampleRate);

            mHornGain += mSmoothing * (mHornGainTarget - mHornGain);
            mDrumGain += mSmoothing * (mDrumGainTarget - mDrumGain);
            mWet += mSmoothing * (mWetTarget - mWet);
            mDry += mSmoothing * (mDryTarget - mDry);
            mMicOffset += mSmoothing * (mMicOffsetTarget - mMicOffset);
            mDepth += mSmoothing * (mDepthTarget - mDepth);

            const float driven = delay_line::SoftSaturate(0.5f * (l + r), drive);
            const auto low = static_cast<float>(
                mLowStates[1].Process(mLowPass, mLowStates[0].Process(mLowPass, static_cast<double>(driven))));
            const auto high = static_cast<float>(
                mHighStates[1].Process(mHighPass, mHighStates[0].Process(mHighPass, static_cast<double>(driven))));

            for (std::size_t mic = 0; mic < 2; ++mic)
            {
                // The left mic sits Spread x 90 degrees one way round the cabinet, the right the
                // other, so at full Spread they face each other across it.
                const float micPhase = (mic == 0) ? -mMicOffset : mMicOffset;
                wet[mic] = HornAt(static_cast<float>(mHornPhase) - micPhase, mic) * mHornGain +
                           DrumAt(static_cast<float>(mDrumPhase) - micPhase) * mDrumGain;
            }

            mHornLine.Write(high);
            mDrumLine.Write(low);

            if (outputs[0])
            {
                outputs[0][i] = l * mDry + wet[0] * mWet;
            }

            if (outputs[1])
            {
                outputs[1][i] = r * mDry + wet[1] * mWet;
            }
        }

        GuardState(wet[0], wet[1]);
    }

    /// The mics hear the rotors from different places, so a mono input comes out stereo.
    [[nodiscard]] bool ProducesStereoOutput() const override
    {
        return Value(rotary::kSpread) > 0.0 && Value(rotary::kDepth) > 0.0 && Value(rotary::kMix) > 0.0;
    }

    // Can run on the audio thread (MIDI and DAW automation): it stores the value and nothing else.
    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(rotary::kParams, key);

        if (index != rotary::kParamCount)
        {
            mValues[index] = NormaliseParamValue(rotary::kParams[index], value);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(rotary::kParams, key); index != rotary::kParamCount)
        {
            return mValues[index];
        }

        // Read-only state, for tests and any future display.
        if (key == "hornHz")
        {
            return mHornHz;
        }

        if (key == "drumHz")
        {
            return mDrumHz;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "rotary";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    [[nodiscard]] double Value(std::size_t index) const noexcept
    {
        return mValues[index];
    }

    [[nodiscard]] rotary::Speed SpeedSetting() const noexcept
    {
        return static_cast<rotary::Speed>(std::clamp(static_cast<int>(Value(rotary::kSpeed)), 0, 2));
    }

    [[nodiscard]] double HornTarget() const noexcept
    {
        switch (SpeedSetting())
        {
        case rotary::Speed::Slow:
            return Value(rotary::kSlowRate);
        case rotary::Speed::Fast:
            return Value(rotary::kFastRate);
        case rotary::Speed::Brake:
            break;
        }

        return 0.0;
    }

    [[nodiscard]] double DrumTarget() const noexcept
    {
        switch (SpeedSetting())
        {
        case rotary::Speed::Slow:
            return Value(rotary::kSlowRate) * rotary::kDrumSlowRatio;
        case rotary::Speed::Fast:
            return Value(rotary::kFastRate) * rotary::kDrumFastRatio;
        case rotary::Speed::Brake:
            break;
        }

        return 0.0;
    }

    /// Ramp 0 is a fifth of the stock times, noon the stock times, full 1.8 times them.
    [[nodiscard]] double RampScale() const noexcept
    {
        return 0.2 + 1.6 * Value(rotary::kRamp);
    }

    /// Per-sample one-pole coefficient for a ramp that gets 95% of the way in `seconds`.
    [[nodiscard]] double RampCoefficient(double seconds) const noexcept
    {
        return 1.0 - std::exp(-3.0 / (std::max(1.0e-3, seconds) * mSampleRate));
    }

    [[nodiscard]] static double Wrap(double phase) noexcept
    {
        return phase - std::floor(phase);
    }

    /// The horn as one mic hears it, `phase` turns from pointing straight at it.
    [[nodiscard]] float HornAt(float phase, std::size_t mic) noexcept
    {
        // cos of the angle between the horn and the mic: 1 facing it, -1 facing away.
        const float facing = delay_line::FastSin01(phase + 0.25f);
        const float toward = 0.5f * (1.0f + facing);
        const auto depth = static_cast<float>(mDepth);

        // Nearest the mic when facing it, so the shortest delay; the rate the distance changes
        // at is the Doppler.
        const double swing = mHornSwing * depth;
        const float direct = mHornLine.ReadHermite(mBaseDelay + swing * (1.0 - facing));
        const float back = mHornLine.ReadHermite(mBaseDelay + mReflectionDelay + swing * (1.0 + facing));

        // The horn throws its treble forward: facing away the mic hears it darker as well as
        // quieter.
        const float dark = mShadow[mic].Process(direct);
        const float brightness = 1.0f - depth * (1.0f - toward);
        const float toned = dark + (direct - dark) * brightness;

        const float amount = 0.8f * depth;
        const float gain = (1.0f - amount) + amount * toward;
        const float backGain = (1.0f - amount) + amount * (1.0f - toward);
        return toned * gain + back * backGain * rotary::kReflectionGain;
    }

    [[nodiscard]] float DrumAt(float phase) const noexcept
    {
        const float facing = delay_line::FastSin01(phase + 0.25f);
        const auto depth = static_cast<float>(mDepth);
        const float sound = mDrumLine.ReadHermite(mBaseDelay + mDrumSwing * depth * (1.0 - facing));
        const float amount = 0.5f * depth;
        return sound * ((1.0f - amount) + amount * 0.5f * (1.0f + facing));
    }

    /// Everything the knobs ask for, once per block; Process glides toward it.
    void TakeUpTargets() noexcept
    {
        const double balance = Value(rotary::kBalance);
        mHornGainTarget = static_cast<float>(std::min(1.0, 1.0 + balance));
        mDrumGainTarget = static_cast<float>(std::min(1.0, 1.0 - balance));
        const double mix = Value(rotary::kMix);
        const double makeupDb = rotary::MakeupDb(Value(rotary::kDrive), Value(rotary::kDepth));
        mWetTarget = static_cast<float>(mix * std::pow(10.0, (Value(rotary::kLevel) + makeupDb) / 20.0));
        mDryTarget = static_cast<float>(1.0 - mix);
        mMicOffsetTarget = static_cast<float>(0.25 * Value(rotary::kSpread));
        mDepthTarget = static_cast<float>(Value(rotary::kDepth));
    }

    /// The rotors at their speed and every smoothed control at its target: a fresh cabinet is
    /// already turning at the speed it was left on, not spinning up from a standstill.
    void SnapToTargets() noexcept
    {
        mHornHz = HornTarget();
        mDrumHz = DrumTarget();
        TakeUpTargets();
        mHornGain = mHornGainTarget;
        mDrumGain = mDrumGainTarget;
        mWet = mWetTarget;
        mDry = mDryTarget;
        mMicOffset = mMicOffsetTarget;
        mDepth = mDepthTarget;
    }

    /// A NaN or an overflow that got into a filter or a line would otherwise stay there for
    /// good. Input is checked sample by sample, so this only catches values too large to be
    /// audio; `lastWet` is the block's final output from the rotors.
    void GuardState(float lastWetL, float lastWetR) noexcept
    {
        bool finite = IsFinite(lastWetL) && IsFinite(lastWetR);

        for (const auto& state : mLowStates)
        {
            finite = finite && IsFinite(state.s1) && IsFinite(state.s2);
        }

        for (const auto& state : mHighStates)
        {
            finite = finite && IsFinite(state.s1) && IsFinite(state.s2);
        }

        if (!finite)
        {
            Reset();
        }
    }

    std::array<double, rotary::kParamCount> mValues{};
    bool mPrepared = false;
    bool mStarted = false;

    delay_line::FractionalDelayLine mHornLine;
    delay_line::FractionalDelayLine mDrumLine;
    BiquadCoefficients mLowPass;
    BiquadCoefficients mHighPass;
    std::array<biquad::State, 2> mLowStates{};
    std::array<biquad::State, 2> mHighStates{};
    std::array<delay_line::OnePoleLp, 2> mShadow{};

    double mBaseDelay = 0.0;
    double mReflectionDelay = 0.0;
    double mHornSwing = 0.0;
    double mDrumSwing = 0.0;
    double mHornHz = 0.0;
    double mDrumHz = 0.0;
    double mHornPhase = 0.0;
    double mDrumPhase = 0.0;

    float mSmoothing = 1.0f;
    float mHornGain = 1.0f;
    float mHornGainTarget = 1.0f;
    float mDrumGain = 1.0f;
    float mDrumGainTarget = 1.0f;
    float mWet = 1.0f;
    float mWetTarget = 1.0f;
    float mDry = 0.0f;
    float mDryTarget = 0.0f;
    float mMicOffset = 0.0f;
    float mMicOffsetTarget = 0.0f;
    float mDepth = 0.0f;
    float mDepthTarget = 0.0f;
};

namespace rotary
{
/// Speed is in every preset: it is the sound, and the switch a footswitch then flips.
/// Keep ids stable once shipped; the UIs list them.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        b.Defaults("cabinet-122", "Cabinet 122"),
        b.Make("chorale", "Chorale", {{"speed", 0.0}, {"slowRate", 0.7}, {"depth", 0.8}, {"drive", 0.1}}),
        b.Make("tremolo-rotor", "Tremolo Rotor", {{"speed", 1.0}, {"depth", 0.8}, {"drive", 0.25}}),
        // The drum turned down: more of the brighter, wobblier horn. Level puts back what the
        // drum carried, most of a guitar's energy.
        b.Make("bright-horn", "Bright Horn", {{"speed", 1.0}, {"balance", 0.4}, {"depth", 0.75}, {"level", 3.0}}),
        b.Make("dirty-cabinet", "Dirty Cabinet", {{"speed", 1.0}, {"drive", 0.75}, {"depth", 0.75}}),
        // Slow ramps, for the long spin up and down between the two speeds.
        b.Make("slow-ramp", "Slow Ramp", {{"speed", 0.0}, {"ramp", 1.0}, {"depth", 0.75}}),
        // Lighter and a little slower, with less between the mics: a rotary in front of an amp.
        b.Make("guitar-rotor", "Guitar Rotor",
               {{"speed", 1.0}, {"fastRate", 6.0}, {"balance", 0.25}, {"spread", 0.5}, {"depth", 0.6}}),
    };
}
} // namespace rotary

inline void RegisterRotaryEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kRotary;
    info.aliases = {"rotary"};
    info.displayName = "Rotary";
    info.category = "modulation";
    info.description = "Rotating speaker cabinet: horn and drum with real spin-up and slow-down, switch between "
                       "Slow and Fast, two mics for stereo";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(rotary::kParams);
    info.presets = rotary::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<RotaryEffect>(); });
}
} // namespace guitarfx
