#pragma once

/**
 * Acoustic: an electric guitar voiced toward a steel-string acoustic.
 *
 * A magnetic pickup hears only the strings: plenty of low mids, little above 5 kHz, and none
 * of the box an acoustic's sound comes out of. This re-voices that signal in the three places
 * the two instruments differ most:
 *
 *   - the pickup's low-mid weight is taken out (a broad cut around 700 Hz)
 *   - a body is put in: two resonances, the air in the box and the top plate, whose pitch is
 *     what tells a small guitar from a large one
 *   - the top end the pickup rolled off is put back, with the pick's click around 3 kHz
 *
 * It is a fixed equaliser, not a model of a guitar, and it cannot add the sustain an electric
 * lacks or remove the sustain it has. It does best on a clean amp setting or straight into the
 * cabinet-less path, with the neck pickup, and it is level-matched so switching it in changes
 * the voice and not the volume.
 */

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace guitarfx
{
namespace acoustic_sim
{
/// One body size: where its two resonances sit. Index order is stored in presets: append,
/// never reorder.
struct Body
{
    double airHz; ///< the air in the box (its Helmholtz resonance)
    double topHz; ///< the top plate's first mode
    double lowCutHz;
};

inline constexpr std::array<Body, 3> kBodies = {{
    {118.0, 225.0, 90.0}, // Parlour: small, so both sit high and there is little under them
    {100.0, 195.0, 75.0}, // Dreadnought
    {88.0, 172.0, 65.0},  // Jumbo
}};

inline const std::vector<std::string>& BodyLabels()
{
    static const std::vector<std::string> labels = {"Parlour", "Dreadnought", "Jumbo"};
    return labels;
}

inline constexpr int kSectionCount = 7;

// What each control reaches at full.
inline constexpr double kBodyResonanceDb = 9.0;
inline constexpr double kBodyQ = 2.4;
inline constexpr double kPickupCutHz = 700.0;
inline constexpr double kPickupCutDb = -8.0;
inline constexpr double kPickupCutQ = 0.7;
inline constexpr double kTopShelfHz = 5000.0;
inline constexpr double kTopShelfDb = 14.0;
inline constexpr double kPickHz = 3200.0;
inline constexpr double kPickDb = 5.0;
inline constexpr double kPickQ = 1.4;
/// Above the strings' own top, where a shelf this tall would only lift hiss.
inline constexpr double kAirLowPassHz = 14000.0;

/// The sections for a setting, in the order they run.
[[nodiscard]] inline std::array<BiquadCoefficients, kSectionCount> Design(int size, double body, double top,
                                                                          double sampleRate) noexcept
{
    const Body& box = kBodies[static_cast<std::size_t>(std::clamp(size, 0, static_cast<int>(kBodies.size()) - 1))];
    const double b = std::clamp(body, 0.0, 1.0);
    const double t = std::clamp(top, 0.0, 1.0);
    // The cut follows whichever of the two controls is further up: either one is "more acoustic".
    const double voiced = std::max(b, t);

    return {{
        biquad::HighPass(box.lowCutHz, biquad::kButterworthQ, sampleRate),
        biquad::Peaking(box.airHz, kBodyQ, b * kBodyResonanceDb, sampleRate),
        biquad::Peaking(box.topHz, kBodyQ, b * kBodyResonanceDb * 0.75, sampleRate),
        biquad::Peaking(kPickupCutHz, kPickupCutQ, voiced * kPickupCutDb, sampleRate),
        biquad::Peaking(kPickHz, kPickQ, t * kPickDb, sampleRate),
        biquad::HighShelf(kTopShelfHz, biquad::kButterworthQ, t * kTopShelfDb, sampleRate),
        biquad::LowPass(kAirLowPassHz, biquad::kButterworthQ, sampleRate),
    }};
}

[[nodiscard]] inline double MagnitudeDb(const std::array<BiquadCoefficients, kSectionCount>& sections, double hz,
                                        double sampleRate) noexcept
{
    std::complex<double> response = 1.0;

    for (const auto& section : sections)
    {
        response *= biquad::Response(section, hz, sampleRate);
    }

    return 20.0 * std::log10(std::max(std::abs(response), 1.0e-12));
}

/// The trim that keeps a guitar as loud through a setting as without it: the mean level change
/// over the band a guitar's energy sits in (80 Hz to 5 kHz, log-spaced), taken back out. Each
/// point counts in proportion to 1/f, about how a guitar's energy falls with frequency, so the
/// body resonances weigh as much as they are heard to.
[[nodiscard]] inline double LevelTrimDb(const std::array<BiquadCoefficients, kSectionCount>& sections,
                                        double sampleRate) noexcept
{
    constexpr int kProbes = 24;
    double power = 0.0;
    double weight = 0.0;

    for (int i = 0; i < kProbes; ++i)
    {
        const double hz = 80.0 * std::pow(5000.0 / 80.0, static_cast<double>(i) / (kProbes - 1));
        power += std::pow(10.0, MagnitudeDb(sections, hz, sampleRate) / 10.0) / hz;
        weight += 1.0 / hz;
    }

    return -10.0 * std::log10(power / weight);
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"level"});

    return {
        b.Defaults("dreadnought", "Dreadnought"),
        b.Make("parlour", "Parlour", {{"size", 0.0}, {"body", 0.4}, {"top", 0.6}}),
        b.Make("jumbo-strum", "Jumbo Strum", {{"size", 2.0}, {"body", 0.7}, {"top", 0.55}}),
        b.Make("bright-fingerstyle", "Bright Fingerstyle", {{"size", 1.0}, {"body", 0.45}, {"top", 0.8}}),
        b.Make("hint-of-wood", "Hint of Wood", {{"size", 1.0}, {"body", 0.5}, {"top", 0.5}, {"mix", 0.5}}),
    };
}
} // namespace acoustic_sim

class AcousticSimEffect : public EffectProcessor
{
  public:
    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mDesignedKey = -1;
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mStates)
        {
            for (auto& state : channel)
            {
                state.Reset();
            }
        }
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        UpdateDesign();

        for (int channel = 0; channel < 2; ++channel)
        {
            RunChannel(channel, inputs[channel], outputs[channel], numSamples);
        }
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        UpdateDesign();
        RunChannel(0, input, output, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "size")
        {
            mSize.store(static_cast<int>(std::lround(std::clamp(value, 0.0, 2.0))), std::memory_order_relaxed);
        }
        else if (key == "body")
        {
            mBody.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "top")
        {
            mTop.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "level")
        {
            mLevelDb.store(static_cast<float>(std::clamp(value, -12.0, 12.0)), std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "size")
        {
            return mSize.load(std::memory_order_relaxed);
        }

        if (key == "body")
        {
            return mBody.load(std::memory_order_relaxed);
        }

        if (key == "top")
        {
            return mTop.load(std::memory_order_relaxed);
        }

        if (key == "level")
        {
            return mLevelDb.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "acoustic_sim";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "eq";
    }

  private:
    /// Redesigns when a control has moved. Cheap enough for the audio thread: seven biquads and
    /// two dozen response points, with no allocation.
    void UpdateDesign() noexcept
    {
        const int size = mSize.load(std::memory_order_relaxed);
        const float body = mBody.load(std::memory_order_relaxed);
        const float top = mTop.load(std::memory_order_relaxed);
        const int key = size * 1000003 + static_cast<int>(std::lround(body * 1000.0f)) * 1009 +
                        static_cast<int>(std::lround(top * 1000.0f));

        if (key == mDesignedKey)
        {
            return;
        }

        mSections = acoustic_sim::Design(size, body, top, mSampleRate);
        mTrim = static_cast<float>(std::pow(10.0, acoustic_sim::LevelTrimDb(mSections, mSampleRate) / 20.0));
        mDesignedKey = key;
    }

    void RunChannel(int channel, const float* input, float* output, int numSamples) noexcept
    {
        if (!output)
        {
            return;
        }

        const float mix = mMix.load(std::memory_order_relaxed);
        const float gain = mTrim * std::pow(10.0f, mLevelDb.load(std::memory_order_relaxed) / 20.0f);
        auto& states = mStates[static_cast<std::size_t>(channel)];

        for (int i = 0; i < numSamples; ++i)
        {
            const float raw = input ? input[i] : 0.0f;
            const float dry = IsFinite(raw) ? raw : 0.0f;
            // Held to a sane range on its way into the filters: a value far beyond audio would
            // ring in the body resonances for seconds before it had decayed to nothing.
            double wet = static_cast<double>(std::clamp(dry, -kInputLimit, kInputLimit));

            for (int s = 0; s < acoustic_sim::kSectionCount; ++s)
            {
                wet = states[static_cast<std::size_t>(s)].Process(mSections[static_cast<std::size_t>(s)], wet);
            }

            const float result = dry + mix * (static_cast<float>(wet) * gain - dry);
            output[i] = IsFinite(result) ? result : 0.0f;
        }

        for (auto& state : states)
        {
            if (!IsFinite(state.s1) || !IsFinite(state.s2))
            {
                state.Reset();
            }
        }
    }

    static constexpr float kInputLimit = 8.0f;

    std::atomic<int> mSize{1};
    std::atomic<float> mBody{0.6f};
    std::atomic<float> mTop{0.6f};
    std::atomic<float> mLevelDb{0.0f};
    std::atomic<float> mMix{1.0f};

    std::array<BiquadCoefficients, acoustic_sim::kSectionCount> mSections{};
    std::array<std::array<biquad::State, acoustic_sim::kSectionCount>, 2> mStates{};
    float mTrim = 1.0f;
    int mDesignedKey = -1;
};

inline void RegisterAcousticSimEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kAcousticSim;
    info.aliases = {"acoustic_sim"};
    info.displayName = "Acoustic";
    info.category = "eq";
    info.description = "Voices an electric guitar toward a steel-string acoustic: a body, a brighter top "
                       "and less pickup";
    info.requiresResource = false;
    info.parameters = {
        {"size", "Size", 1.0, 0.0, 2.0, "enum", "", false, 1.0, acoustic_sim::BodyLabels()},
        {"body", "Body", 0.6, 0.0, 1.0, "amount"},
        {"top", "Top", 0.6, 0.0, 1.0, "amount"},
        {"level", "Level", 0.0, -12.0, 12.0, "dB"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
    };
    info.presets = acoustic_sim::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<AcousticSimEffect>(); });
}
} // namespace guitarfx
