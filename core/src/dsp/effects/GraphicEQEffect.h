#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <string_view>

namespace guitarfx
{
class GraphicEQEffect : public EffectProcessor
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

    static constexpr int kMinBands = 5;
    static constexpr int kMaxBands = 10;
    static constexpr std::array<double, kMaxBands> kDefaultFrequencies = {31.25,  62.5,   125.0,  250.0,  500.0,
                                                                          1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        UpdateCoefficients();
        Reset();
    }

    void Reset() override
    {
        for (auto& band : mBands)
        {
            band.state = {};
        }
    }

    /// Each channel runs through the bands one band at a time over the block. A band at 0 dB
    /// would only pass the signal through, and a switched-off band or one past Bands passes it
    /// by definition, so neither costs a filter: their state follows the signal instead, as a
    /// flat band's would, so one that comes back from 0 dB sounds exactly as if it had run all
    /// along, and one switched back on or added starts from the signal rather than from where it
    /// stopped.
    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (numSamples <= 0)
        {
            return;
        }

        // Left first, unless its output is the right's input, which it would overwrite.
        const int first = outputs[0] && outputs[0] == inputs[1] ? 1 : 0;

        for (int step = 0; step < 2; ++step)
        {
            const int channel = step == 0 ? first : 1 - first;
            float* samples = outputs[channel];

            if (!samples)
            {
                continue;
            }

            if (!inputs[channel])
            {
                std::fill_n(samples, numSamples, 0.0f);
            }
            else if (inputs[channel] != samples)
            {
                std::copy_n(inputs[channel], numSamples, samples);
            }

            for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
            {
                auto& band = mBands[bandIndex];
                auto& state = band.state[channel];

                if (bandIndex < mBandCount && band.enabled && !band.flat)
                {
                    FilterBlock(samples, numSamples, band, state);
                }
                else
                {
                    FollowSignal(samples, numSamples, state);
                }
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "bandCount")
        {
            mBandCount = static_cast<int>(std::clamp(std::round(FiniteOr(value, 10.0)), static_cast<double>(kMinBands),
                                                     static_cast<double>(kMaxBands)));
        }
        else if (key == "preset")
        {
            mPreset = static_cast<int>(std::round(FiniteOr(value, 0.0)));
        }
        else
        {
            const int bandIndex = ParseBandIndex(key);

            if (bandIndex < 0)
            {
                return;
            }

            auto& band = mBands[bandIndex];
            const std::string_view suffix = BandSuffix(key);

            if (suffix == "Enabled")
            {
                band.enabled = value >= 0.5;
            }
            else if (suffix == "Gain")
            {
                band.gainDb = ClampFinite(value, -18.0, 18.0, 0.0);
            }
            else if (suffix == "Freq")
            {
                band.frequencyHz = ClampFrequencyForBand(bandIndex, value);
            }
            else if (suffix == "Q")
            {
                band.q = ClampFinite(value, 0.2, 8.0, 1.0);
            }
            else
            {
                return;
            }
        }

        UpdateCoefficients();
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bandCount")
        {
            return mBandCount;
        }

        if (key == "preset")
        {
            return mPreset;
        }

        const int bandIndex = ParseBandIndex(key);

        if (bandIndex < 0)
        {
            return 0.0;
        }

        const auto& band = mBands[bandIndex];
        const std::string_view suffix = BandSuffix(key);

        if (suffix == "Enabled")
        {
            return band.enabled ? 1.0 : 0.0;
        }

        if (suffix == "Gain")
        {
            return band.gainDb;
        }

        if (suffix == "Freq")
        {
            return band.frequencyHz;
        }

        if (suffix == "Q")
        {
            return band.q;
        }

        return 0.0;
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "eq_graphic";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "eq";
    }

    /// The curve the bands make, from the coefficients they play with: the plots draw what is
    /// heard, at the rate the effect was prepared at (the analysis prepares at 48 kHz).
    [[nodiscard]] bool GetFrequencyResponse(std::span<const double> frequenciesHz,
                                            std::span<double> magnitudesDb) const override
    {
        if (frequenciesHz.size() != magnitudesDb.size())
        {
            return false;
        }

        const auto sections = PlayingSections();

        for (std::size_t i = 0; i < frequenciesHz.size(); ++i)
        {
            magnitudesDb[i] = biquad::CascadeMagnitudeDb(sections, frequenciesHz[i], mSampleRate);
        }

        return true;
    }

    [[nodiscard]] std::vector<std::vector<double>> GetBandResponses(
        std::span<const double> frequenciesHz) const override
    {
        std::vector<std::vector<double>> curves;

        for (const auto& section : PlayingSections())
        {
            auto& curve = curves.emplace_back(frequenciesHz.size());

            for (std::size_t i = 0; i < frequenciesHz.size(); ++i)
            {
                curve[i] = biquad::CascadeMagnitudeDb(std::array{section}, frequenciesHz[i], mSampleRate);
            }
        }

        return curves;
    }

  private:
    /// The bands that play, as the sections Process applies.
    [[nodiscard]] std::vector<BiquadCoefficients> PlayingSections() const
    {
        std::vector<BiquadCoefficients> sections;

        for (int bandIndex = 0; bandIndex < mBandCount; ++bandIndex)
        {
            const auto& band = mBands[bandIndex];

            if (band.enabled)
            {
                sections.push_back({band.b0, band.b1, band.b2, band.a1, band.a2});
            }
        }

        return sections;
    }

    struct BiquadState
    {
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };

    struct Band
    {
        bool enabled = true;
        bool flat = true; // the coefficients pass the signal through unchanged
        double gainDb = 0.0;
        double frequencyHz = 1000.0;
        double q = 1.0;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        std::array<BiquadState, 2> state{}; // left, right
    };

    static void FilterBlock(float* samples, int numSamples, const Band& band, BiquadState& state)
    {
        BiquadState s = state;

        for (int i = 0; i < numSamples; ++i)
        {
            samples[i] = ProcessSample(samples[i], band.b0, band.b1, band.b2, band.a1, band.a2, s.x1, s.x2, s.y1, s.y2);
        }

        state = s;
    }

    /// The state a flat band would be left in after these samples: its input and output are both
    /// the signal, with a non-finite sample taken as silence, as ProcessSample takes it.
    static void FollowSignal(const float* samples, int numSamples, BiquadState& state)
    {
        const auto clean = [](float sample) { return IsFinite(sample) ? sample : 0.0f; };

        if (numSamples >= 2)
        {
            state.x2 = state.y2 = clean(samples[numSamples - 2]);
        }
        else
        {
            state.x2 = state.x1;
            state.y2 = state.y1;
        }

        state.x1 = state.y1 = clean(samples[numSamples - 1]);
    }

    static double FiniteOr(double value, double fallback)
    {
        return IsFinite(value) ? value : fallback;
    }

    static double ClampFinite(double value, double minimum, double maximum, double fallback)
    {
        return std::clamp(FiniteOr(value, fallback), minimum, maximum);
    }

    [[nodiscard]] double MaxFrequency() const
    {
        return std::max(20.0, std::min(20000.0, mSampleRate * 0.49));
    }

    /// The frequency as set, within the control's range. The cap below Nyquist is applied only to
    /// the filter design: stored, it outlived the sample rate, so a band prepared at 32 kHz stayed
    /// at 15.7 kHz when the host went back to 48 kHz.
    [[nodiscard]] static double ClampFrequencyForBand(int bandIndex, double value)
    {
        return ClampFinite(value, 20.0, 20000.0, kDefaultFrequencies[bandIndex]);
    }

    /// What follows "band<N>" in a key ParseBandIndex accepted.
    [[nodiscard]] static std::string_view BandSuffix(const std::string& key)
    {
        return std::string_view(key).substr(key.find_first_not_of("0123456789", 4));
    }

    /// "band<N><suffix>", N from 1 to kMaxBands, with a suffix after the digits: a bare "band10"
    /// is not a band key, and taking its suffix threw std::out_of_range.
    static int ParseBandIndex(const std::string& key)
    {
        if (key.rfind("band", 0) != 0 || key.size() < 6)
        {
            return -1;
        }

        std::size_t cursor = 4;
        int oneBasedIndex = 0;

        while (cursor < key.size() && key[cursor] >= '0' && key[cursor] <= '9')
        {
            oneBasedIndex = oneBasedIndex * 10 + (key[cursor] - '0');
            ++cursor;
        }

        return cursor == 4 || cursor == key.size() || oneBasedIndex < 1 || oneBasedIndex > kMaxBands
                   ? -1
                   : oneBasedIndex - 1;
    }

    static float ProcessSample(float input, float b0, float b1, float b2, float a1, float a2, float& x1, float& x2,
                               float& y1, float& y2)
    {
        const float sanitizedInput = IsFinite(input) ? input : 0.0f;
        float output = b0 * sanitizedInput + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

        if (!IsFinite(output))
        {
            x1 = x2 = y1 = y2 = 0.0f;
            return sanitizedInput;
        }

        x2 = x1;
        x1 = sanitizedInput;
        y2 = y1;
        y1 = output;
        return output;
    }

    void UpdateCoefficients()
    {
        const double maxFrequency = MaxFrequency();

        for (int index = 0; index < kMaxBands; ++index)
        {
            auto& band = mBands[index];
            band.gainDb = ClampFinite(band.gainDb, -18.0, 18.0, 0.0);
            band.frequencyHz = ClampFrequencyForBand(index, band.frequencyHz);
            const double designFrequencyHz = std::min(band.frequencyHz, maxFrequency);
            band.q = ClampFinite(band.q, 0.2, 8.0, 1.0);

            band.flat = true;

            if (std::abs(band.gainDb) < 0.001)
            {
                band.b0 = 1.0f;
                band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
                continue;
            }

            const double amplitude = std::pow(10.0, band.gainDb / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * designFrequencyHz / mSampleRate;
            const double alpha = std::sin(w0) / (2.0 * band.q);
            const double a0 = 1.0 + alpha / amplitude;

            if (!IsFinite(a0) || std::abs(a0) < 1.0e-9)
            {
                band.b0 = 1.0f;
                band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
                continue;
            }

            band.b0 = static_cast<float>((1.0 + alpha * amplitude) / a0);
            band.b1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
            band.b2 = static_cast<float>((1.0 - alpha * amplitude) / a0);
            band.a1 = band.b1;
            band.a2 = static_cast<float>((1.0 - alpha / amplitude) / a0);
            band.flat = false;
        }
    }

    double mSampleRate = 48000.0;
    int mBandCount = kMinBands;
    int mPreset = 0;
    std::array<Band, kMaxBands> mBands = [] {
        std::array<Band, kMaxBands> bands{};

        for (int index = 0; index < kMaxBands; ++index)
        {
            bands[index].frequencyHz = kDefaultFrequencies[index];
        }

        return bands;
    }();
};

inline EffectPresetDefinition MakeGraphicEQFactoryPreset(
    std::string id, std::string displayName, int presetIndex, int bandCount,
    const std::array<double, GraphicEQEffect::kMaxBands>& frequencies,
    const std::array<double, GraphicEQEffect::kMaxBands>& qValues,
    const std::array<double, GraphicEQEffect::kMaxBands>& gains)
{
    EffectPresetDefinition preset;
    preset.id = std::move(id);
    preset.displayName = std::move(displayName);
    preset.parameters = {{"preset", presetIndex}, {"bandCount", bandCount}};

    for (int index = 0; index < GraphicEQEffect::kMaxBands; ++index)
    {
        const std::string prefix = "band" + std::to_string(index + 1);
        preset.parameters[prefix + "Enabled"] = index < bandCount ? 1.0 : 0.0;
        preset.parameters[prefix + "Freq"] = frequencies[index];
        preset.parameters[prefix + "Q"] = qValues[index];
        preset.parameters[prefix + "Gain"] = gains[index];
        preset.parameterOrder.push_back(prefix + "Freq");
    }

    for (const auto& [key, value] : preset.parameters)
    {
        (void)value;

        if (std::find(preset.parameterOrder.begin(), preset.parameterOrder.end(), key) == preset.parameterOrder.end())
        {
            preset.parameterOrder.push_back(key);
        }
    }

    return preset;
}

inline void RegisterGraphicEQEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kEqGraphic;
    info.aliases = {"eq_graphic"};
    info.displayName = "Graphic Equalizer";
    info.category = "eq";
    info.description = "Graphic equalizer with flat 5 and 10 band layouts for Bass, Guitar, and General Purpose";
    info.requiresResource = false;
    // Every profile is flat. A profile chooses the *band layout* — how many bands
    // and at which centre frequencies and Q — and nothing else, so switching
    // profiles never imposes a voicing. Curves belong to the user's own saved
    // effect presets, which sit alongside this dropdown.
    info.presets = {
        MakeGraphicEQFactoryPreset("bass-5", "Bass · 5 Band", 0, 5,
                                   {55, 120, 300, 900, 3500, 5000, 7000, 9000, 12000, 16000},
                                   {0.8, 1.0, 1.1, 1.0, 0.9, 1.0, 1.0, 1.0, 1.0, 1.0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
        MakeGraphicEQFactoryPreset("bass-10", "Bass · 10 Band", 1, 10,
                                   {45, 65, 90, 120, 150, 250, 500, 900, 2000, 5000},
                                   {2.0, 2.0, 2.0, 2.0, 2.0, 1.2, 1.1, 1.0, 0.9, 0.8}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
        MakeGraphicEQFactoryPreset("guitar-5", "Guitar · 5 Band", 2, 5,
                                   {80, 250, 800, 5000, 10000, 12000, 14000, 16000, 18000, 19000},
                                   {1.0, 1.1, 1.0, 0.9, 0.8, 1.0, 1.0, 1.0, 1.0, 1.0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
        MakeGraphicEQFactoryPreset("guitar-10", "Guitar · 10 Band", 3, 10,
                                   {70, 85, 100, 250, 400, 800, 1000, 3000, 5000, 10000},
                                   {1.0, 2.0, 1.4, 1.1, 1.1, 1.0, 1.0, 0.9, 0.9, 0.8}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
        MakeGraphicEQFactoryPreset("general-5", "General Purpose · 5 Band", 4, 5,
                                   {60, 250, 1000, 4000, 10000, 12000, 14000, 16000, 18000, 19000},
                                   {0.9, 1.0, 1.0, 0.9, 0.8, 1.0, 1.0, 1.0, 1.0, 1.0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
        MakeGraphicEQFactoryPreset("general-10", "General Purpose · 10 Band", 5, 10,
                                   {31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000},
                                   {1.4142135623730951, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951,
                                    1.4142135623730951, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951,
                                    1.4142135623730951, 1.4142135623730951},
                                   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
    };

    // All profiles are flat, so a new Graphic EQ is audibly neutral whichever it
    // starts on; this only picks which band layout it starts with. Marking rather
    // than reordering avoids remapping the "preset" enum indices that already-saved
    // presets reference.
    for (auto& preset : info.presets)
    {
        preset.isDefault = (preset.id == "general-5");
    }

    std::vector<std::string> profileLabels;
    profileLabels.reserve(info.presets.size());

    for (const auto& preset : info.presets)
    {
        profileLabels.push_back(preset.displayName);
    }

    info.parameters = {{"preset", "Profile", 0.0, 0.0, 5.0, "enum", "Profile", false, 1.0, std::move(profileLabels)},
                       {"bandCount", "Bands", 5.0, 5.0, 10.0, "amount", "Profile", true, 1.0, {}}};

    for (int index = 1; index <= GraphicEQEffect::kMaxBands; ++index)
    {
        const std::string prefix = "band" + std::to_string(index);
        const std::string group = "Band " + std::to_string(index);
        info.parameters.push_back({prefix + "Enabled", "Enabled", 0.0, 0.0, 1.0, "toggle", group, true, 1.0, {}});
        info.parameters.push_back({prefix + "Gain", "Gain", 0.0, -18.0, 18.0, "dB", group, true, 0.1, {}});
        info.parameters.push_back({prefix + "Freq",
                                   "Frequency",
                                   GraphicEQEffect::kDefaultFrequencies[index - 1],
                                   20.0,
                                   20000.0,
                                   "Hz",
                                   group,
                                   true,
                                   1.0,
                                   {}});
        info.parameters.push_back({prefix + "Q", "Q", 1.0, 0.2, 8.0, "amount", group, true, 0.01, {}});
    }

    EffectRegistry::Instance().Register(info.type, info, [] { return std::make_unique<GraphicEQEffect>(); });
}
} // namespace guitarfx
