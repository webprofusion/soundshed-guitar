#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/LinearRamp.h"
#include "dsp/effects/SimpleCabVoicing.h"
#include "dsp/effects/SpeakerDrive.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace guitarfx
{
/**
 * Lightweight, filter-based guitar cabinet: no IR required.
 *
 * Five cabinet types, a mic with type, position and distance, an optional second mic to
 * blend against it, speaker drive, stereo spread and level. What those controls do to the
 * sound is SimpleCabVoicing.h's business; this class runs the designs it produces and keeps
 * parameter changes smooth. An IR remains the option for one particular speaker and mic's
 * exact notches.
 */
class SimpleCabEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    SimpleCabEffect()
    {
        Configure(mSampleRate);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mMaxBlockSize = maxBlockSize;
        Configure(sampleRate);
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            ResetChannel(channel);
        }

        ForEachRamp([](auto& ramp) { ramp.Finish(); });
        mRampSamplesRemaining = 0;
        mStereo = mStereoTarget;
        mDrive.Reset();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        if (!mEnabled)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            AdvanceRamp();

            for (int ch = 0; ch < 2; ++ch)
            {
                if (!inputs[ch] || !outputs[ch])
                {
                    continue;
                }

                outputs[ch][i] = static_cast<float>(ProcessSample(static_cast<double>(inputs[ch][i]), ch));
            }

            mDrive.AdvanceSample();
            mWritePosition = (mWritePosition + 1) & mHistoryMask;
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "enabled")
        {
            mEnabled = value > 0.5;
            return;
        }

        const std::size_t index = FindParamSpec(simple_cab::kParams, key);

        if (index == simple_cab::kParamCount)
        {
            return;
        }

        const double normalised = NormaliseParamValue(simple_cab::kParams[index], value);

        if (mValues[index] == normalised)
        {
            return;
        }

        mValues[index] = normalised;

        if (index == simple_cab::kSpeakerDrive)
        {
            // Level-dependent, so not part of the linear design: nothing to redesign.
            mDrive.SetDrive(normalised);
            return;
        }

        UpdateDesign();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "enabled")
        {
            return mEnabled ? 1.0 : 0.0;
        }

        const std::size_t index = FindParamSpec(simple_cab::kParams, key);
        return index == simple_cab::kParamCount ? 0.0 : mValues[index];
    }

    [[nodiscard]] bool GetFrequencyResponse(std::span<const double> frequenciesHz,
                                            std::span<double> magnitudesDb) const override
    {
        if (frequenciesHz.size() != magnitudesDb.size())
        {
            return false;
        }

        const simple_cab::Design design = mVoicer.Build(simple_cab::ToSettings(mValues));

        for (std::size_t i = 0; i < frequenciesHz.size(); ++i)
        {
            magnitudesDb[i] = mVoicer.MagnitudeDb(design, frequenciesHz[i]);
        }

        return true;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "cab_simple";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "cab";
    }

  private:
    /// Coefficient, gain and mix changes glide over this long.
    static constexpr double kRampSeconds = 0.015;
    /// Delay taps slide at most this many samples per sample: a mic move bends the pitch by
    /// up to 5% while it travels rather than clicking.
    static constexpr double kMaxDelaySlew = 0.05;
    /// Speaker drive's excursion band reaches a little above the cabinet's resonance.
    static constexpr double kExcursionCornerRatio = 1.5;

    /// A mic tap's delay, sliding towards its target at a bounded rate.
    struct SlewedDelay
    {
        double current = 0.0;
        double target = 0.0;

        void Advance() noexcept
        {
            current += std::clamp(target - current, -kMaxDelaySlew, kMaxDelaySlew);
        }
    };

    struct MicRuntime
    {
        std::array<biquad::Ramp, simple_cab::kMicSectionCount> sections;
        std::array<biquad::State, simple_cab::kMicSectionCount> state;
        LinearRamp<double> gain;
        LinearRamp<double> reflectionGain;
        SlewedDelay direct;
        SlewedDelay reflection;
        double reflectionPole = 0.0;
        double bounce = 0.0; ///< the reflection's one-pole low-pass state
    };

    struct ChannelRuntime
    {
        std::array<biquad::Ramp, simple_cab::kCabinetSectionCount> cabinet;
        std::array<biquad::State, simple_cab::kCabinetSectionCount> cabinetState;
        std::array<MicRuntime, simple_cab::kMicCount> mics;
        /// Recent cabinet output, which the mic taps read behind.
        std::vector<double> history;
    };

    /// Everything that depends on the sample rate. The constructor runs it too, so an
    /// instance is usable before Prepare().
    void Configure(double sampleRate)
    {
        mSampleRate = sampleRate;
        mVoicer.Prepare(sampleRate);
        mDrive.Prepare(sampleRate);
        mRampSamples = std::max(1, static_cast<int>(std::lround(sampleRate * kRampSeconds)));

        // Interpolation reads one sample past the longest delay.
        const auto needed = static_cast<unsigned>(std::ceil(simple_cab::kMaxMicDelaySeconds * sampleRate)) + 2u;
        const unsigned size = std::bit_ceil(needed);
        mHistoryMask = static_cast<int>(size - 1);
        mWritePosition = 0;

        for (auto& channel : mChannels)
        {
            channel.history.assign(size, 0.0);
        }

        UpdateDesign();
        Reset();

        for (auto& channel : mChannels)
        {
            for (auto& mic : channel.mics)
            {
                mic.direct.current = mic.direct.target;
                mic.reflection.current = mic.reflection.target;
            }
        }
    }

    /// Rebuilds the design from the parameter values and glides towards it.
    void UpdateDesign() noexcept
    {
        const simple_cab::Settings settings = simple_cab::ToSettings(mValues);
        const simple_cab::Design design = mVoicer.Build(settings);
        mDrive.SetExcursionCornerHz(simple_cab::ResonanceHz(settings) * kExcursionCornerRatio);

        for (int ch = 0; ch < 2; ++ch)
        {
            ChannelRuntime& channel = mChannels[ch];
            const simple_cab::ChannelDesign& target = design.channels[ch];

            for (int s = 0; s < simple_cab::kCabinetSectionCount; ++s)
            {
                channel.cabinet[s].target = target.cabinet[s];
            }

            for (int m = 0; m < simple_cab::kMicCount; ++m)
            {
                MicRuntime& mic = channel.mics[m];
                const simple_cab::MicDesign& micTarget = target.mics[m];

                // A mic fading in starts clean rather than from state left when it went
                // quiet, and its taps start where they belong instead of sliding there.
                if (!IsMicAudible(mic) && micTarget.gain != 0.0)
                {
                    ResetMic(mic);
                    mic.direct.current = micTarget.directDelaySamples;
                    mic.reflection.current = micTarget.reflectionDelaySamples;
                }

                for (int s = 0; s < simple_cab::kMicSectionCount; ++s)
                {
                    mic.sections[s].target = micTarget.sections[s];
                }

                mic.gain.target = micTarget.gain;
                mic.reflectionGain.target = micTarget.reflectionGain;
                mic.direct.target = micTarget.directDelaySamples;
                mic.reflection.target = micTarget.reflectionDelaySamples;
                mic.reflectionPole = micTarget.reflectionPole;
            }
        }

        mWetGain.target = design.wetGain;
        mOutputGain.target = design.outputGain;
        mMix.target = design.mix;
        mStereoTarget = design.stereo;
        mStereo = mStereo || design.stereo;

        mRampSamplesRemaining = mRampSamples;
        ForEachRamp([this](auto& ramp) { ramp.Begin(mRampSamples); });
    }

    /// Applies `fn` to every ramp: coefficients, gains and mix, both channels.
    template <typename Fn> void ForEachRamp(Fn&& fn)
    {
        for (auto& channel : mChannels)
        {
            for (auto& ramp : channel.cabinet)
            {
                fn(ramp);
            }

            for (auto& mic : channel.mics)
            {
                for (auto& ramp : mic.sections)
                {
                    fn(ramp);
                }

                fn(mic.gain);
                fn(mic.reflectionGain);
            }
        }

        fn(mWetGain);
        fn(mOutputGain);
        fn(mMix);
    }

    void AdvanceRamp() noexcept
    {
        for (auto& channel : mChannels)
        {
            for (auto& mic : channel.mics)
            {
                mic.direct.Advance();
                mic.reflection.Advance();
            }
        }

        if (mRampSamplesRemaining <= 0)
        {
            return;
        }

        if (--mRampSamplesRemaining == 0)
        {
            ForEachRamp([](auto& ramp) { ramp.Finish(); });
            mStereo = mStereoTarget;
            return;
        }

        ForEachRamp([](auto& ramp) { ramp.Advance(); });
    }

    [[nodiscard]] static bool IsMicAudible(const MicRuntime& mic) noexcept
    {
        return mic.gain.current != 0.0 || mic.gain.target != 0.0;
    }

    static void ResetMic(MicRuntime& mic) noexcept
    {
        for (auto& state : mic.state)
        {
            state.Reset();
        }

        mic.bounce = 0.0;
    }

    /// The cabinet output `delaySamples` ago, linearly interpolated.
    [[nodiscard]] double ReadHistory(const ChannelRuntime& channel, double delaySamples) const noexcept
    {
        const int whole = static_cast<int>(delaySamples);
        const double fraction = delaySamples - whole;
        const double newer = channel.history[(mWritePosition - whole) & mHistoryMask];
        const double older = channel.history[(mWritePosition - whole - 1) & mHistoryMask];
        return newer + fraction * (older - newer);
    }

    [[nodiscard]] double ProcessSample(double dry, int ch) noexcept
    {
        // A NaN or infinity would stay in every filter's memory and silence the cab until the
        // node was rebuilt, so it plays as silence. IsFinite, because the fast-math builds
        // fold std::isfinite away.
        if (!IsFinite(dry))
        {
            dry = 0.0;
        }

        ChannelRuntime& channel = mChannels[ch];
        double cabinet = mDrive.Process(dry, ch);

        for (int s = 0; s < simple_cab::kCabinetSectionCount; ++s)
        {
            cabinet = channel.cabinetState[s].Process(channel.cabinet[s].current, cabinet);
        }

        channel.history[mWritePosition] = cabinet;
        double wet = 0.0;

        for (auto& mic : channel.mics)
        {
            if (!IsMicAudible(mic))
            {
                continue;
            }

            // The nearer mic reads the cabinet straight; only a mic behind it reads history.
            double taps = mic.direct.current == 0.0 ? cabinet : ReadHistory(channel, mic.direct.current);

            if (mic.reflectionGain.current > 0.0)
            {
                const double bounce = ReadHistory(channel, mic.reflection.current);
                mic.bounce = bounce + mic.reflectionPole * (mic.bounce - bounce);
                taps += mic.reflectionGain.current * mic.bounce;
            }

            for (int s = 0; s < simple_cab::kMicSectionCount; ++s)
            {
                taps = mic.state[s].Process(mic.sections[s].current, taps);
            }

            wet += mic.gain.current * taps;
        }

        const double mix = mMix.current;
        const double output = mOutputGain.current * ((1.0 - mix) * dry + mix * mWetGain.current * wet);

        // Belt and braces for anything that still overflows: start the channel afresh.
        if (!IsFinite(output))
        {
            ResetChannel(channel);
            return 0.0;
        }

        return output;
    }

    void ResetChannel(ChannelRuntime& channel) noexcept
    {
        for (auto& state : channel.cabinetState)
        {
            state.Reset();
        }

        for (auto& mic : channel.mics)
        {
            ResetMic(mic);
        }

        std::fill(channel.history.begin(), channel.history.end(), 0.0);
    }

    simple_cab::ParamValues mValues = simple_cab::kDefaultValues;
    simple_cab::Voicer mVoicer;
    SpeakerDrive mDrive;
    std::array<ChannelRuntime, 2> mChannels;
    LinearRamp<double> mWetGain;
    LinearRamp<double> mOutputGain;
    LinearRamp<double> mMix;
    int mRampSamples = 1;
    int mRampSamplesRemaining = 0;
    int mWritePosition = 0;
    int mHistoryMask = 0;
    bool mStereo = false;
    bool mStereoTarget = false;
};

namespace simple_cab
{
/// A factory preset: the defaults with `overrides` applied. It sets every voicing control,
/// so choosing one never leaves the last preset's spread or second mic behind, but not
/// Output: the level the player has set stays theirs.
[[nodiscard]] inline EffectPresetDefinition MakeFactoryPreset(const char* id, const char* name, bool isDefault,
                                                              std::initializer_list<std::pair<Param, double>> overrides)
{
    ParamValues values = kDefaultValues;

    for (const auto& [param, value] : overrides)
    {
        values[param] = NormaliseParamValue(kParams[param], value);
    }

    EffectPresetDefinition preset;
    preset.id = id;
    preset.displayName = name;
    preset.isFactory = true;
    preset.isDefault = isDefault;

    for (int i = 0; i < kParamCount; ++i)
    {
        if (i == kOutputGain)
        {
            continue;
        }

        preset.parameters[kParams[i].id] = values[i];
        preset.parameterOrder.emplace_back(kParams[i].id);
    }

    return preset;
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    return {
        MakeFactoryPreset("closed-4x12", "Closed 4x12", true, {}),
        MakeFactoryPreset("open-1x12-combo", "Open 1x12 Combo", false, {{kCabinet, 1.0}, {kMicPosition, 0.35}}),
        MakeFactoryPreset("ribbon-2x12", "Ribbon 2x12", false,
                          {{kCabinet, 2.0}, {kMicType, 1.0}, {kMicPosition, 0.45}}),
        MakeFactoryPreset("dual-mic-4x12", "Dual Mic 4x12", false,
                          {{kMic2Blend, 0.35}, {kMic2Type, 1.0}, {kMic2Position, 0.6}, {kMic2Distance, 0.15}}),
        MakeFactoryPreset("tweed-4x10-room", "Tweed 4x10 Room", false,
                          {{kCabinet, 4.0}, {kMicType, 2.0}, {kMicDistance, 0.6}}),
        MakeFactoryPreset("wide-2x12", "Wide 2x12", false, {{kCabinet, 3.0}, {kSpread, 0.7}}),
        MakeFactoryPreset("cranked-4x12", "Cranked 4x12", false, {{kSpeakerDrive, 0.6}, {kAutoLevel, 1.0}}),
        // Popular cabinets, each fitted to the broad shape of reference captures of that cab
        // (SimpleCabMatch with the cabinet type held) and kept at least 1.3 dB apart in shape from
        // every other preset here (TestFactoryPresetsAreDistinct). Auto Level holds them all at
        // the default voicing's loudness, so stepping through them doesn't jump in level.
        // clang-format off
        MakeFactoryPreset("deluxe-1x12", "Deluxe 1x12", false,
                          {{kCabinet, 1.0}, {kSize, 1.0}, {kBass, 0.53}, {kMids, 0.0}, {kPresence, 0.47},
                           {kBrightness, 0.0}, {kMicType, 2.0}, {kMicPosition, 0.58}, {kMicDistance, 0.34},
                           {kAutoLevel, 1.0}}),
        MakeFactoryPreset("ac30-2x12-blue", "AC30 2x12 Blue", false,
                          {{kCabinet, 2.0}, {kSize, 0.59}, {kBass, 0.0}, {kMids, 0.48}, {kPresence, 0.28},
                           {kBrightness, 0.2}, {kMicType, 2.0}, {kMicPosition, 0.52}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("twin-2x12", "Twin 2x12", false,
                          {{kCabinet, 2.0}, {kSize, 0.38}, {kBass, 0.0}, {kMids, 0.66}, {kPresence, 0.73},
                           {kBrightness, 1.0}, {kMicPosition, 0.75}, {kMicDistance, 0.16}, {kAutoLevel, 1.0},
                           {kMic2Blend, 0.23}, {kMic2Type, 0.0}, {kMic2Position, 0.58}}),
        MakeFactoryPreset("jc-120-2x12", "JC-120 2x12", false,
                          {{kCabinet, 2.0}, {kSize, 0.7}, {kBass, 0.53}, {kMids, 0.7}, {kPresence, 0.0},
                           {kBrightness, 0.0}, {kMicPosition, 0.28}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("dc30-2x12", "DC30 2x12", false,
                          {{kCabinet, 2.0}, {kSize, 0.64}, {kBass, 0.0}, {kMids, 0.72}, {kPresence, 0.0},
                           {kBrightness, 0.19}, {kMicType, 2.0}, {kMicPosition, 0.58}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("vintage-15-inch", "Vintage 15-inch", false,
                          {{kCabinet, 2.0}, {kSize, 0.22}, {kBass, 0.0}, {kMids, 0.34}, {kPresence, 0.59},
                           {kBrightness, 0.66}, {kMicType, 2.0}, {kMicPosition, 0.95}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("closed-2x12-v30", "Closed 2x12 V30", false,
                          {{kCabinet, 3.0}, {kSize, 0.22}, {kBass, 0.8}, {kMids, 1.0}, {kPresence, 0.94},
                           {kBrightness, 0.39}, {kMicType, 1.0}, {kMicPosition, 0.53}, {kMicDistance, 0.11},
                           {kAutoLevel, 1.0}, {kMic2Blend, 0.49}, {kMic2Type, 0.0}, {kMic2Position, 0.0}}),
        MakeFactoryPreset("closed-2x12-evm12l", "Closed 2x12 EVM12L", false,
                          {{kCabinet, 3.0}, {kSize, 0.23}, {kBass, 0.09}, {kMids, 0.63}, {kPresence, 0.56},
                           {kBrightness, 0.53}, {kMicType, 2.0}, {kMicPosition, 0.94}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("super-reverb-4x10", "Super Reverb 4x10", false,
                          {{kCabinet, 4.0}, {kSize, 1.0}, {kBass, 0.0}, {kMids, 0.77}, {kPresence, 0.19},
                           {kBrightness, 0.0}, {kMicType, 2.0}, {kMicPosition, 0.75}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("recto-4x12-v30", "Recto 4x12 V30", false,
                          {{kSize, 0.33}, {kBass, 0.23}, {kMids, 0.53}, {kPresence, 0.41}, {kBrightness, 0.42},
                           {kMicType, 2.0}, {kMicPosition, 0.41}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("1960-4x12-v30", "1960 4x12 V30", false,
                          {{kSize, 0.53}, {kBass, 0.88}, {kMids, 0.78}, {kMicType, 2.0}, {kMicPosition, 0.0},
                           {kAutoLevel, 1.0}}),
        MakeFactoryPreset("1960-4x12-greenback", "1960 4x12 Greenback", false,
                          {{kSize, 0.11}, {kBass, 0.28}, {kMids, 0.34}, {kPresence, 0.16}, {kBrightness, 0.34},
                           {kMicPosition, 0.28}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("1960-4x12-g12t-75", "1960 4x12 G12T-75", false,
                          {{kSize, 0.36}, {kBass, 0.14}, {kMids, 0.3}, {kPresence, 0.78}, {kBrightness, 1.0},
                           {kMicType, 1.0}, {kMicPosition, 0.0}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("ppc412-v30", "PPC412 V30", false,
                          {{kSize, 0.36}, {kBass, 0.34}, {kMids, 0.61}, {kPresence, 0.0}, {kBrightness, 0.42},
                           {kMicType, 2.0}, {kMicPosition, 0.59}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("xxl-4x12-v30", "XXL 4x12 V30", false,
                          {{kSize, 0.06}, {kBass, 0.0}, {kMids, 0.52}, {kPresence, 0.22}, {kBrightness, 0.38},
                           {kMicPosition, 0.42}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("fane-4x12", "Fane 4x12", false,
                          {{kSize, 0.34}, {kBass, 0.0}, {kMids, 0.69}, {kPresence, 0.91}, {kBrightness, 0.47},
                           {kMicType, 1.0}, {kMicPosition, 0.28}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("legacy-4x12-v30", "Legacy 4x12 V30", false,
                          {{kSize, 0.55}, {kBass, 0.08}, {kMids, 0.3}, {kPresence, 0.39}, {kBrightness, 0.06},
                           {kMicType, 2.0}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("alnico-cream-4x12", "Alnico Cream 4x12", false,
                          {{kSize, 1.0}, {kBass, 0.0}, {kMids, 0.72}, {kPresence, 0.0}, {kBrightness, 0.59},
                           {kMicType, 2.0}, {kMicPosition, 0.63}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("boutique-4x12", "Boutique 4x12", false,
                          {{kSize, 0.41}, {kBass, 0.83}, {kMids, 0.83}, {kPresence, 0.61}, {kBrightness, 0.42},
                           {kMicPosition, 0.36}, {kAutoLevel, 1.0}}),
        MakeFactoryPreset("svt-8x10-bass", "SVT 8x10 Bass", false,
                          {{kSize, 1.0}, {kBass, 0.86}, {kMids, 0.0}, {kPresence, 0.69}, {kBrightness, 0.67},
                           {kMicPosition, 0.0}, {kAutoLevel, 1.0}}),
        // clang-format on
    };
}
} // namespace simple_cab

inline void RegisterSimpleCabEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kCabSimple;
    info.aliases = {"cab_simple"};
    info.displayName = "Cybercab - Cab Sim";
    info.category = "cab";
    info.description = "Lightweight cabinet with five cab types, mic type, position and distance, a second mic, "
                       "speaker drive and stereo spread (no IR required)";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(simple_cab::kParams);
    info.presets = simple_cab::FactoryPresets();

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<SimpleCabEffect>(); });
}
} // namespace guitarfx
