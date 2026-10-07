#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
namespace vibe
{
enum Param : std::size_t
{
    kMode,
    kRate,
    kSyncMode,
    kSyncDivision,
    kIntensity,
    kThrob,
    kLevel,
    kParamCount
};

/// Index order is stored in presets: append, never reorder.
enum class Mode
{
    Chorus,
    Vibrato
};

inline constexpr const char* kModeLabels[] = {"Chorus", "Vibrato"};

inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"mode", "Mode", 0.0, 0.0, 1.0, "enum", "", false, 1.0, kModeLabels},
    LogTaper({"rate", "Speed", 2.0, 0.3, 12.0, "Hz", "", false, 0.0}),
    {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "timing", false, 1.0, tempo_sync::kSyncModeLabelNames},
    {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "timing", false, 1.0, tempo_sync::kDivisionLabelNames},
    {"intensity", "Intensity", 0.75, 0.0, 1.0, "amount", "", false, 0.0},
    {"throb", "Throb", 0.5, 0.0, 1.0, "amount", "", true, 0.0},
    {"level", "Level", 0.0, -12.0, 12.0, "dB", "", false, 0.0},
}};

inline constexpr std::size_t kStages = 4;

/// The four phase-shift capacitors, in farads. They are wildly unequal, so each stage sweeps
/// its own part of the spectrum: one in the bass, two through the mids and one high up.
/// That spread, not the number of stages, is what separates the sound from a phaser's.
inline constexpr std::array<double, kStages> kStageCapacitance = {15.0e-9, 220.0e-9, 470.0e-12, 4.7e-9};

/// The photocells' resistance in full light, in ohms; darker, it rises as light^-kCellLaw.
inline constexpr double kBrightOhms = 4000.0;
inline constexpr double kCellLaw = 0.7;
/// The least light the cells see: the lamp is never fully out, so the stages never stop.
inline constexpr double kMinLight = 0.02;

/// The lamp's own light at the bottom of the LFO swing, before Intensity adds to it.
inline constexpr double kLampBias = 0.12;

/// An incandescent filament heats faster than it cools.
inline constexpr double kLampHeatMs = 6.0;
inline constexpr double kLampCoolMs = 18.0;
/// The cells respond quickly to more light and slowly to less, and Throb scales the slow
/// side: that lag is the lopsided, pulsing sweep.
inline constexpr double kCellBrightenMs = 3.0;
inline constexpr double kCellDarkenMinMs = 10.0;
inline constexpr double kCellDarkenMaxMs = 90.0;

/// Coefficients are designed every this many samples and glide in between.
inline constexpr int kControlInterval = 8;

/// Chorus mode sums the dry and the shifted signal, and where they cancel the level drops, the
/// more the further the lamp swings: about 1 dB at Intensity 0 and 6.6 dB at full on the demo
/// riffs. This puts it back, at each quarter of Intensity's travel, so Chorus sits where
/// Vibrato and bypass do. Speed and Throb still move it a couple of dB either way.
inline constexpr std::array<double, 5> kChorusMakeupDb = {0.8, 2.3, 4.5, 6.0, 6.6};

[[nodiscard]] inline double ChorusMakeupDb(double intensity) noexcept
{
    const double position = std::clamp(intensity, 0.0, 1.0) * static_cast<double>(kChorusMakeupDb.size() - 1);
    const auto below = std::min(static_cast<std::size_t>(position), kChorusMakeupDb.size() - 2);
    const double fraction = position - static_cast<double>(below);
    return kChorusMakeupDb[below] + (kChorusMakeupDb[below + 1] - kChorusMakeupDb[below]) * fraction;
}

inline constexpr double kSmoothingMs = 20.0;
} // namespace vibe

/**
 * A photocell vibe: four phase-shift stages swept by a lamp.
 *
 * An LFO lights a lamp, and four light-dependent resistors facing it each set the corner of
 * one first-order all-pass stage. What makes it a vibe rather than a phaser is the lamp and
 * the cells: the filament lags the LFO, the cells brighten fast and darken slowly, so the
 * sweep is lopsided and throbs instead of gliding; and the stages' capacitors differ by up to
 * 470 times, so their notches spread across the spectrum unevenly as they move.
 *
 * Chorus mixes the stages' output with the dry signal: moving notches. Vibrato is the stages
 * alone: the phase moving is a pitch wobble. Intensity is how far the lamp swings and Throb
 * how slowly the cells let go. It changes nothing between the channels, so it keeps a mono
 * input mono and runs on one channel when the chain is mono.
 */
class VibeEffect : public EffectProcessor
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

    VibeEffect()
    {
        mValues = DefaultParamValues(vibe::kParams);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mSmoothing = static_cast<float>(1.0 - std::exp(-1000.0 / (vibe::kSmoothingMs * sampleRate)));
        mPrepared = true;
        Reset();
    }

    void Reset() override
    {
        mPhase = 0.0;
        mStates = {};
        mLamp = static_cast<float>(vibe::kLampBias);
        mCell = mLamp;
        mCountdown = 0;
        TakeUpTargets();
        mWet = mWetTarget;
        mDry = mDryTarget;
        DesignCoefficients(mCoefficients);
        mCoefficientSteps = {};
        mStarted = false;
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
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

        const float* in[2] = {inputs[0] ? inputs[0] : inputs[1], inputs[1] ? inputs[1] : inputs[0]};
        Render(in, outputs, 2, numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        if (!mPrepared || !input)
        {
            EffectProcessor::ProcessMono(input, output, numSamples);
            return;
        }

        const float* in[2] = {input, input};
        float* out[2] = {output, nullptr};
        Render(in, out, 1, numSamples);
    }

    // Can run on the audio thread (MIDI and DAW automation): it stores the value and nothing else.
    void SetParam(const std::string& key, double value) override
    {
        if (key == "bpm")
        {
            mBpm = tempo_sync::ClampBpm(IsFinite(value) ? value : tempo_sync::kDefaultBpm);
            return;
        }

        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(vibe::kParams, key);

        if (index != vibe::kParamCount)
        {
            mValues[index] = NormaliseParamValue(vibe::kParams[index], value);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(vibe::kParams, key); index != vibe::kParamCount)
        {
            return mValues[index];
        }

        if (key == "bpm")
        {
            return mBpm;
        }

        if (key == "effectiveRate")
        {
            return EffectiveRateHz();
        }

        // Read-only state, for tests: the light the cells see, 0 to 1.
        if (key == "light")
        {
            return mCell;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "vibe";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    using StageCoefficients = std::array<float, vibe::kStages>;

    [[nodiscard]] double EffectiveRateHz() const noexcept
    {
        const double free = mValues[vibe::kRate];

        if (static_cast<int>(mValues[vibe::kSyncMode]) != tempo_sync::kSyncModeTempo)
        {
            return free;
        }

        const double synced = tempo_sync::DivisionRateHz(mBpm, static_cast<int>(mValues[vibe::kSyncDivision]));
        return std::clamp(synced, vibe::kParams[vibe::kRate].minValue, vibe::kParams[vibe::kRate].maxValue);
    }

    void TakeUpTargets() noexcept
    {
        const bool chorus = static_cast<int>(mValues[vibe::kMode]) == static_cast<int>(vibe::Mode::Chorus);
        const double makeupDb = chorus ? vibe::ChorusMakeupDb(mValues[vibe::kIntensity]) : 0.0;
        const double level = std::pow(10.0, (mValues[vibe::kLevel] + makeupDb) / 20.0);
        mWetTarget = static_cast<float>(chorus ? 0.5 * level : level);
        mDryTarget = static_cast<float>(chorus ? 0.5 * level : 0.0);
    }

    /// Advances the LFO, the lamp and the cells by one control interval of `samples`, and
    /// designs the four stages for where the cells have got to.
    void AdvanceControl(int samples, StageCoefficients& coefficients) noexcept
    {
        const double rate = EffectiveRateHz();
        const double intensity = mValues[vibe::kIntensity];
        const double throb = mValues[vibe::kThrob];
        const double seconds = static_cast<double>(samples) / mSampleRate;

        mPhase += rate * seconds;
        mPhase -= std::floor(mPhase);

        // The LFO drives the lamp's current; its light goes as the power, so as the square.
        const double swing = 0.5 * (1.0 + delay_line::FastSin01(static_cast<float>(mPhase)));
        const double drive = vibe::kLampBias + (1.0 - vibe::kLampBias) * intensity * swing;
        const auto light = static_cast<float>(drive * drive);

        mLamp += static_cast<float>(Approach(light > mLamp ? vibe::kLampHeatMs : vibe::kLampCoolMs, seconds)) *
                 (light - mLamp);
        const double darkenMs = vibe::kCellDarkenMinMs + (vibe::kCellDarkenMaxMs - vibe::kCellDarkenMinMs) * throb;
        mCell +=
            static_cast<float>(Approach(mLamp > mCell ? vibe::kCellBrightenMs : darkenMs, seconds)) * (mLamp - mCell);

        DesignCoefficients(coefficients);
    }

    [[nodiscard]] static double Approach(double timeConstantMs, double seconds) noexcept
    {
        return 1.0 - std::exp(-seconds * 1000.0 / timeConstantMs);
    }

    void DesignCoefficients(StageCoefficients& coefficients) const noexcept
    {
        const double ohms =
            vibe::kBrightOhms * std::pow(std::max(vibe::kMinLight, static_cast<double>(mCell)), -vibe::kCellLaw);

        for (std::size_t stage = 0; stage < vibe::kStages; ++stage)
        {
            const double cornerHz = 1.0 / (delay_line::kTwoPi * ohms * vibe::kStageCapacitance[stage]);
            const double clamped = std::clamp(cornerHz, 5.0, 0.45 * mSampleRate);
            const double g = std::tan(delay_line::kPi * clamped / mSampleRate);
            coefficients[stage] = static_cast<float>((1.0 - g) / (1.0 + g));
        }
    }

    void Render(const float* const* inputs, float** outputs, int channels, int numSamples) noexcept
    {
        TakeUpTargets();

        // The first block after a reset starts at the mode and level the knobs ask for, so a node
        // set up after Prepare (a preset loading) does not glide in from the defaults.
        if (!mStarted)
        {
            mWet = mWetTarget;
            mDry = mDryTarget;
            mStarted = true;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            if (mCountdown <= 0)
            {
                // Glide from where the coefficients are to the next design over one interval.
                StageCoefficients next{};
                AdvanceControl(vibe::kControlInterval, next);

                for (std::size_t stage = 0; stage < vibe::kStages; ++stage)
                {
                    mCoefficientSteps[stage] =
                        (next[stage] - mCoefficients[stage]) / static_cast<float>(vibe::kControlInterval);
                }

                mCountdown = vibe::kControlInterval;
            }

            --mCountdown;

            for (std::size_t stage = 0; stage < vibe::kStages; ++stage)
            {
                mCoefficients[stage] += mCoefficientSteps[stage];
            }

            mWet += mSmoothing * (mWetTarget - mWet);
            mDry += mSmoothing * (mDryTarget - mDry);

            for (int channel = 0; channel < channels; ++channel)
            {
                const float raw = inputs[channel] ? inputs[channel][i] : 0.0f;
                const float dry = IsFinite(raw) ? raw : 0.0f;
                float x = dry;
                auto& state = mStates[static_cast<std::size_t>(channel)];

                for (std::size_t stage = 0; stage < vibe::kStages; ++stage)
                {
                    const float a = mCoefficients[stage];
                    const float y = -a * x + state[stage];
                    state[stage] = delay_line::FlushDenormal(x + a * y);
                    x = y;
                }

                if (outputs[channel])
                {
                    outputs[channel][i] = dry * mDry + x * mWet;
                }
            }
        }

        // A value too large to be audio could overflow a stage; start clean rather than hold it.
        for (const auto& state : mStates)
        {
            for (const float s : state)
            {
                if (!IsFinite(s))
                {
                    mStates = {};
                    return;
                }
            }
        }
    }

    std::array<double, vibe::kParamCount> mValues{};
    double mBpm = tempo_sync::kDefaultBpm;
    bool mPrepared = false;
    bool mStarted = false;

    double mPhase = 0.0;
    float mLamp = 0.0f;
    float mCell = 0.0f;
    int mCountdown = 0;
    StageCoefficients mCoefficients{};
    StageCoefficients mCoefficientSteps{};
    std::array<std::array<float, vibe::kStages>, 2> mStates{};

    float mSmoothing = 1.0f;
    float mWet = 0.5f;
    float mWetTarget = 0.5f;
    float mDry = 0.5f;
    float mDryTarget = 0.5f;
};

namespace vibe
{
/// Division is left out of a preset with Sync off, so the player's own survives.
/// Keep ids stable once shipped; the UIs list them.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"syncDivision"});

    return {
        b.Defaults("classic-vibe", "Classic Vibe"),
        b.Make("slow-throb", "Slow Throb", {{"rate", 0.9}, {"intensity", 0.85}, {"throb", 0.75}}),
        b.Make("fast-chorus", "Fast Chorus", {{"rate", 6.5}, {"intensity", 0.6}, {"throb", 0.4}, {"level", 3.0}}),
        b.Make("vibrato", "Vibrato", {{"mode", 1.0}, {"rate", 5.0}, {"intensity", 0.6}}),
        b.Make("seasick", "Seasick", {{"mode", 1.0}, {"rate", 1.2}, {"intensity", 1.0}, {"throb", 0.8}}),
        b.Make("subtle-shimmer", "Subtle Shimmer", {{"rate", 3.0}, {"intensity", 0.35}, {"throb", 0.3}}),
        // One sweep a beat.
        b.Make("tempo-vibe", "Tempo Vibe", {{"rate", 2.0}, {"syncMode", 1.0}, {"syncDivision", 4.0}}),
    };
}
} // namespace vibe

inline void RegisterVibeEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kVibe;
    info.aliases = {"vibe"};
    info.displayName = "Vibe";
    info.category = "modulation";
    info.description = "Photocell vibe in the Uni-Vibe style: four unequal phase stages swept by a lamp, for the "
                       "throbbing chorus and vibrato of late-60s records";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.parameters = BuildParameterDefs(vibe::kParams);
    info.presets = vibe::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<VibeEffect>(); });
}
} // namespace guitarfx
