#pragma once

#include "dsp/ChannelLayout.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/MultibandCompressorSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>

namespace guitarfx
{
/**
 * Multi-band Compressor: four frequency bands, each compressed on its own level with its own
 * threshold, ratio, knee and makeup. Each band can be turned off (it passes untouched) or
 * soloed (only the soloed bands are heard).
 *
 *   in ─ four-band LR4 split ─┬─ band 1 ─ × gain 1 ─┐
 *                             ├─ …                   ├─ Σ ─ out
 *                             └─ band 4 ─ × gain 4 ─┘
 *        each band's mean square ─ gain computer ─┘
 *
 * gain = Output · heard · (on · (Mix · compression · makeup + (1 − Mix)) + (1 − on))
 *
 * A boomy low string can be held down without squashing the pick attack above it, or fizz
 * tamed without thinning the body. The node panel draws the bands over the live spectrum,
 * before and after, with each band's gain reduction (ReadBandMeters).
 *
 * - The split is fourth-order Linkwitz-Riley crossovers with all-pass compensation
 *   (dsp/LinkwitzRileyCrossover.h), at three crossovers the player moves. Below every
 *   threshold the bands add up to a flat all-pass of the input.
 * - Gain reduction is a knee curve on each band's detector, smoothed in dB by Attack and
 *   Release (slower in the lower bands), worked out every kControlInterval samples; the
 *   applied gain ramps per sample between.
 * - Mix blends each band with itself uncompressed, so dry and wet share the crossover's phase.
 * - On and Solo glide like Makeup, so switching a band does not click.
 * - One detector per band drives both channels, except in dual mono.
 * - No latency.
 */
class MultibandCompressorEffect : public EffectProcessor
{
  public:
    static constexpr int kBands = multiband_compressor::kBands;
    static constexpr int kSplits = multiband_compressor::kSplits;

    MultibandCompressorEffect()
    {
        for (std::size_t index = 0; index < multiband_compressor::kParamCount; ++index)
        {
            mValues[index].store(static_cast<float>(multiband_compressor::kParams[index].defaultValue),
                                 std::memory_order_relaxed);
        }

        PrepareRate();
        Reset();
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void SetDualMono(bool dualMono) override
    {
        mDualMono.store(dualMono, std::memory_order_relaxed);
    }

    /// Identical sides in, identical sides out (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    /// A mono path turning stereo hands the right side the left's filters, levels and gains,
    /// so it carries on from where the left is instead of starting from rest.
    void SetInputLayout(ChannelLayout layout) override
    {
        if (layout == ChannelLayout::Stereo && mLayout == ChannelLayout::Mono)
        {
            mChannels[1] = mChannels[0];
        }

        mLayout = layout;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        PrepareRate();
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            channel.crossover.Reset();
            channel.level.fill(0.0);
            channel.reductionDb.fill(0.0);
            channel.gain.fill(1.0f);
            channel.step.fill(0.0f);
        }

        mControlCountdown = 0;
        // The first control step lands on Makeup, Mix and Output rather than gliding to them.
        mLevelsPrimed = false;

        for (std::size_t band = 0; band < kBands; ++band)
        {
            mMeterReductionDb[band].store(0.0f, std::memory_order_relaxed);
            // Silent until a block says otherwise, as the distortion's meters are, not 0 dBFS.
            mMeterLevelDb[band].store(-120.0f, std::memory_order_relaxed);
        }
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* inL = inputs[0] ? inputs[0] : inputs[1];
        const float* inR = inputs[1] ? inputs[1] : inL;

        if (!inL)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                if (outputs[channel])
                {
                    std::fill_n(outputs[channel], numSamples, 0.0f);
                }
            }

            return;
        }

        Render<2>({inL, inR}, {outputs[0], outputs[1]}, numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        if (!input)
        {
            std::fill_n(output, numSamples, 0.0f);
            return;
        }

        Render<1>({input, input}, {output, nullptr}, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(multiband_compressor::kParams, key);

        if (index == multiband_compressor::kParamCount)
        {
            return;
        }

        mValues[index].store(static_cast<float>(NormaliseParamValue(multiband_compressor::kParams[index], value)),
                             std::memory_order_relaxed);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(multiband_compressor::kParams, key);
            index != multiband_compressor::kParamCount)
        {
            return mValues[index].load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "dynamics_multiband_compressor";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

    [[nodiscard]] int BandMeterCount() const override
    {
        return kBands;
    }

    int ReadBandMeters(std::span<BandMeter> out) override
    {
        const int count = std::min(kBands, static_cast<int>(out.size()));

        for (int band = 0; band < count; ++band)
        {
            const auto index = static_cast<std::size_t>(band);
            BandMeter& meter = out[index];
            meter.lowHz = band > 0 ? mMeterSplitHz[index - 1].load(std::memory_order_relaxed) : 0.0;
            meter.highHz = band < kSplits ? mMeterSplitHz[index].load(std::memory_order_relaxed) : 0.0;
            meter.gainReductionDb = mMeterReductionDb[index].exchange(0.0f, std::memory_order_relaxed);
            meter.levelDb = mMeterLevelDb[index].load(std::memory_order_relaxed);
            meter.thresholdDb = Value(multiband_compressor::BandParam(band, multiband_compressor::kBandThreshold));
        }

        return count;
    }

    /// The gain reduction band `band` of `channel` is at now, in dB. For tests.
    [[nodiscard]] double BandGainReductionDb(int channel, int band) const
    {
        return mChannels[static_cast<std::size_t>(channel)].reductionDb[static_cast<std::size_t>(band)];
    }

    /// The crossovers in use, after ordering. For tests.
    [[nodiscard]] std::array<double, kSplits> SplitsInUse() const
    {
        return mSplitHz;
    }

  private:
    struct Channel
    {
        multiband_compressor::Crossover crossover;
        std::array<double, kBands> level{};       ///< each band's smoothed mean square
        std::array<double, kBands> reductionDb{}; ///< smoothed by Attack and Release
        std::array<float, kBands> gain{};         ///< applied, ramping by step to the next target
        std::array<float, kBands> step{};

        [[nodiscard]] bool IsFinite() const
        {
            for (std::size_t band = 0; band < kBands; ++band)
            {
                if (!guitarfx::IsFinite(level[band]) || !guitarfx::IsFinite(reductionDb[band]) ||
                    !guitarfx::IsFinite(gain[band]))
                {
                    return false;
                }
            }

            return crossover.IsFinite();
        }
    };

    /// The parameters as this block uses them.
    struct Settings
    {
        std::array<double, kBands> thresholdDb{};
        std::array<double, kBands> ratio{};
        std::array<double, kBands> kneeDb{};
        std::array<double, kBands> makeup{}; ///< linear
        std::array<double, kBands> on{};     ///< 1 compressing, 0 passing untouched
        std::array<double, kBands> heard{};  ///< 0 while another band is soloed and this one is not
        std::array<double, kBands> attackCoef{};
        std::array<double, kBands> releaseCoef{};
        double mix = 1.0;
        double output = 1.0; ///< linear
        bool linked = true;
    };

    /// Makeup, Mix, Output, On and Solo glide over this, so a knob turn or a preset change does not step
    /// the level mid-note and click (as the VCA compressor's do).
    static constexpr double kLevelSmoothingMs = 20.0;

    void PrepareRate()
    {
        mSplitHz = CurrentSplits();
        mDesign.Prepare(mSplitHz, mSampleRate);
        PublishSplits();

        for (std::size_t band = 0; band < kBands; ++band)
        {
            mDetectorCoef[band] = OnePole(multiband_compressor::kDetectorMs[band], 1.0);
        }

        mLevelCoef = OnePole(kLevelSmoothingMs, multiband_compressor::kControlInterval);
    }

    /// A one-pole's coefficient for a time constant, stepped every `interval` samples.
    [[nodiscard]] double OnePole(double milliseconds, double interval) const
    {
        return 1.0 - std::exp(-interval / (std::max(milliseconds, 0.001) * 0.001 * mSampleRate));
    }

    [[nodiscard]] float Value(multiband_compressor::Param param) const
    {
        return mValues[param].load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::array<double, kSplits> CurrentSplits() const
    {
        return multiband_compressor::OrderedSplits({Value(multiband_compressor::kCrossover1),
                                                    Value(multiband_compressor::kCrossover2),
                                                    Value(multiband_compressor::kCrossover3)},
                                                   0.45 * mSampleRate);
    }

    void PublishSplits()
    {
        for (std::size_t split = 0; split < kSplits; ++split)
        {
            mMeterSplitHz[split].store(mSplitHz[split], std::memory_order_relaxed);
        }
    }

    /// Redesigns any crossover that has moved since the last block.
    void UpdateSplits()
    {
        const auto splits = CurrentSplits();
        bool moved = false;

        for (std::size_t split = 0; split < kSplits; ++split)
        {
            if (splits[split] != mSplitHz[split])
            {
                mDesign.PrepareSplit(static_cast<int>(split), splits[split], mSampleRate);
                moved = true;
            }
        }

        if (moved)
        {
            mSplitHz = splits;
            PublishSplits();
        }
    }

    [[nodiscard]] Settings LoadSettings() const
    {
        using namespace multiband_compressor;
        Settings settings;
        const double attackMs = Value(kAttack);
        const double releaseMs = Value(kRelease);
        bool anySolo = false;

        for (int band = 0; band < kBands; ++band)
        {
            anySolo = anySolo || Value(BandParam(band, kBandSolo)) >= 0.5f;
        }

        for (std::size_t band = 0; band < kBands; ++band)
        {
            const int index = static_cast<int>(band);
            settings.thresholdDb[band] = Value(BandParam(index, kBandThreshold));
            settings.ratio[band] = Value(BandParam(index, kBandRatio));
            settings.kneeDb[band] = Value(BandParam(index, kBandKnee));
            settings.makeup[band] = std::pow(10.0, Value(BandParam(index, kBandGain)) / 20.0);
            settings.on[band] = Value(BandParam(index, kBandOn)) >= 0.5f ? 1.0 : 0.0;
            settings.heard[band] = !anySolo || Value(BandParam(index, kBandSolo)) >= 0.5f ? 1.0 : 0.0;
            settings.attackCoef[band] = OnePole(attackMs * kTimeScale[band], kControlInterval);
            settings.releaseCoef[band] = OnePole(releaseMs * kTimeScale[band], kControlInterval);
        }

        settings.mix = Value(kMix);
        settings.output = std::pow(10.0, Value(kOutput) / 20.0);
        settings.linked = !mDualMono.load(std::memory_order_relaxed);
        return settings;
    }

    /// One non-finite sample would poison the filters and levels for good. Checked once a block.
    void GuardState()
    {
        for (const auto& channel : mChannels)
        {
            if (!channel.IsFinite())
            {
                Reset();
                return;
            }
        }
    }

    /// Smooths `reductionDb` toward what `level` asks for, one control step. A band turned off
    /// lets go at its release and meters none, since none of it is heard.
    static void UpdateReduction(const Settings& settings, const std::array<double, kBands>& level,
                                std::array<double, kBands>& reductionDb, std::array<float, kBands>& blockReductionDb)
    {
        for (std::size_t band = 0; band < kBands; ++band)
        {
            const bool on = settings.on[band] > 0.0;
            const double levelDb = 10.0 * std::log10(std::max(level[band], 1.0e-12));
            const double target =
                on ? multiband_compressor::GainReductionDb(levelDb - settings.thresholdDb[band], settings.ratio[band],
                                                           settings.kneeDb[band])
                   : 0.0;
            double& reduction = reductionDb[band];
            reduction +=
                (target > reduction ? settings.attackCoef[band] : settings.releaseCoef[band]) * (target - reduction);

            if (on)
            {
                blockReductionDb[band] = std::max(blockReductionDb[band], static_cast<float>(reduction));
            }
        }
    }

    /// Sets the channel's gains ramping, over the next control interval, to what its gain
    /// reduction, Makeup, Mix, Output, On and Solo now make.
    void SetRamps(Channel& channel) const
    {
        for (std::size_t band = 0; band < kBands; ++band)
        {
            const double compressed = std::pow(10.0, -channel.reductionDb[band] / 20.0) * mMakeup[band];
            const double processed = mMix * compressed + (1.0 - mMix);
            const double on = mOn[band];
            const auto gain = static_cast<float>(mOutput * mHeard[band] * (on * processed + (1.0 - on)));

            if (!mLevelsPrimed)
            {
                channel.gain[band] = gain;
                channel.step[band] = 0.0f;
            }
            else
            {
                channel.step[band] =
                    (gain - channel.gain[band]) / static_cast<float>(multiband_compressor::kControlInterval);
            }
        }
    }

    /// Makeup, Mix, Output, On and Solo one control step closer to what the controls say.
    void GlideLevels(const Settings& settings)
    {
        if (!mLevelsPrimed)
        {
            mMakeup = settings.makeup;
            mOn = settings.on;
            mHeard = settings.heard;
            mMix = settings.mix;
            mOutput = settings.output;
            return;
        }

        for (std::size_t band = 0; band < kBands; ++band)
        {
            mMakeup[band] += mLevelCoef * (settings.makeup[band] - mMakeup[band]);
            mOn[band] += mLevelCoef * (settings.on[band] - mOn[band]);
            mHeard[band] += mLevelCoef * (settings.heard[band] - mHeard[band]);
        }

        mMix += mLevelCoef * (settings.mix - mMix);
        mOutput += mLevelCoef * (settings.output - mOutput);
    }

    template <int kChannels>
    void Render(std::array<const float*, 2> inputs, std::array<float*, 2> outputs, int numSamples)
    {
        GuardState();
        UpdateSplits();

        const Settings settings = LoadSettings();
        const bool linked = kChannels == 1 || settings.linked;
        std::array<std::array<double, kBands>, kChannels> bands{};
        std::array<float, kBands> blockReductionDb{};

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < kChannels; ++ch)
            {
                Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                auto& split = bands[static_cast<std::size_t>(ch)];
                channel.crossover.Split(mDesign, inputs[static_cast<std::size_t>(ch)][i], split);

                for (std::size_t band = 0; band < kBands; ++band)
                {
                    channel.level[band] += mDetectorCoef[band] * (split[band] * split[band] - channel.level[band]);
                }
            }

            if (--mControlCountdown <= 0)
            {
                mControlCountdown = multiband_compressor::kControlInterval;
                GlideLevels(settings);

                if (linked)
                {
                    UpdateReduction(settings, LinkedLevel<kChannels>(), mChannels[0].reductionDb, blockReductionDb);

                    if constexpr (kChannels == 2)
                    {
                        mChannels[1].reductionDb = mChannels[0].reductionDb;
                    }
                }
                else
                {
                    for (int ch = 0; ch < kChannels; ++ch)
                    {
                        Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                        UpdateReduction(settings, channel.level, channel.reductionDb, blockReductionDb);
                    }
                }

                for (int ch = 0; ch < kChannels; ++ch)
                {
                    SetRamps(mChannels[static_cast<std::size_t>(ch)]);
                }

                mLevelsPrimed = true;
            }

            for (int ch = 0; ch < kChannels; ++ch)
            {
                Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                const auto& split = bands[static_cast<std::size_t>(ch)];
                double sum = 0.0;

                for (std::size_t band = 0; band < kBands; ++band)
                {
                    channel.gain[band] += channel.step[band];
                    sum += channel.gain[band] * split[band];
                }

                if (float* out = outputs[static_cast<std::size_t>(ch)])
                {
                    out[i] = static_cast<float>(sum);
                }
            }
        }

        PublishMeters<kChannels>(blockReductionDb);
    }

    /// The deepest reduction this block, held until the panel reads it, and each band's level.
    template <int kChannels> void PublishMeters(const std::array<float, kBands>& blockReductionDb)
    {
        const auto level = LinkedLevel<kChannels>();

        for (std::size_t band = 0; band < kBands; ++band)
        {
            auto& held = mMeterReductionDb[band];
            float current = held.load(std::memory_order_relaxed);

            while (blockReductionDb[band] > current &&
                   !held.compare_exchange_weak(current, blockReductionDb[band], std::memory_order_relaxed))
            {
            }

            mMeterLevelDb[band].store(static_cast<float>(10.0 * std::log10(std::max(level[band], 1.0e-12))),
                                      std::memory_order_relaxed);
        }
    }

    /// Each band's level as the linked detector reads it: the louder side's.
    template <int kChannels> [[nodiscard]] std::array<double, kBands> LinkedLevel() const
    {
        if constexpr (kChannels == 1)
        {
            return mChannels[0].level;
        }
        else
        {
            std::array<double, kBands> level{};

            for (std::size_t band = 0; band < kBands; ++band)
            {
                level[band] = std::max(mChannels[0].level[band], mChannels[1].level[band]);
            }

            return level;
        }
    }

    // Parameters, written from any thread and read on the audio thread.
    std::array<std::atomic<float>, multiband_compressor::kParamCount> mValues;
    std::atomic<bool> mDualMono{false};

    // Meters, written by the audio thread once a block and read by the message thread.
    std::array<std::atomic<double>, kSplits> mMeterSplitHz{};
    std::array<std::atomic<float>, kBands> mMeterReductionDb{};
    std::array<std::atomic<float>, kBands> mMeterLevelDb{};

    // Derived from the sample rate by PrepareRate().
    std::array<double, kBands> mDetectorCoef{};
    double mLevelCoef = 1.0;

    // Audio thread only.
    multiband_compressor::CrossoverDesign mDesign;
    std::array<double, kSplits> mSplitHz{};
    std::array<Channel, 2> mChannels;
    std::array<double, kBands> mMakeup{};
    std::array<double, kBands> mOn{};
    std::array<double, kBands> mHeard{};
    double mMix = 1.0;
    double mOutput = 1.0;
    bool mLevelsPrimed = false;
    int mControlCountdown = 0;
    ChannelLayout mLayout = ChannelLayout::Mono;
};

inline void RegisterMultibandCompressorEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kMultibandCompressor;
    info.aliases = {"dynamics_multiband_compressor"};
    info.displayName = "Multi-band Compressor";
    info.category = "dynamics";
    info.description = "Four-band compressor: each band has its own threshold, ratio, knee and makeup";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(multiband_compressor::kParams);

    const factory_presets::Builder b(info.parameters, {});
    // A band at 1:1 passes untouched, so a preset that fixes one thing sets the others to 1:1.
    info.presets = {
        b.Defaults("standard", "Standard"),
        // Holds down the low strings' boom without touching anything above it.
        b.Make("tame-the-boom", "Tame the Boom",
               {{"threshold1", -30.0},
                {"ratio1", 4.0},
                {"ratio2", 1.0},
                {"ratio3", 1.0},
                {"ratio4", 1.0},
                {"attack", 20.0},
                {"release", 150.0}}),
        // High-gain palm mutes: the lows clamp fast and let go fast, so chugs stay tight.
        b.Make("tight-low-end", "Tight Low End",
               {{"crossover1", 150.0},
                {"threshold1", -28.0},
                {"ratio1", 6.0},
                {"gain1", 1.0},
                {"threshold2", -24.0},
                {"ratio2", 2.0},
                {"ratio3", 1.0},
                {"ratio4", 1.0},
                {"attack", 5.0},
                {"release", 80.0}}),
        // Every band a little: a clean part's quiet notes come up to its loud ones.
        b.Make("even-out", "Even Out",
               {{"threshold1", -30.0},
                {"threshold2", -30.0},
                {"threshold3", -30.0},
                {"threshold4", -30.0},
                {"ratio1", 2.5},
                {"ratio2", 2.5},
                {"ratio3", 2.5},
                {"ratio4", 2.5},
                {"gain1", 3.0},
                {"gain2", 3.0},
                {"gain3", 3.0},
                {"gain4", 3.0},
                {"attack", 15.0},
                {"release", 200.0}}),
        // Fizz above 3.5 kHz pulled down when it flares, so a bright amp does not bite.
        b.Make("smooth-top", "Smooth Top",
               {{"crossover3", 3500.0},
                {"ratio1", 1.0},
                {"ratio2", 1.0},
                {"ratio3", 1.0},
                {"threshold4", -32.0},
                {"ratio4", 4.0},
                {"attack", 2.0},
                {"release", 60.0}}),
        // The body held steady and the top left open, so chords ring bright and even.
        b.Make("glassy-clean", "Glassy Clean",
               {{"threshold1", -30.0},
                {"ratio1", 4.0},
                {"threshold2", -28.0},
                {"ratio2", 3.0},
                {"gain2", 1.5},
                {"threshold3", -20.0},
                {"ratio3", 1.5},
                {"ratio4", 1.0},
                {"gain4", 2.0},
                {"attack", 12.0},
                {"release", 180.0}}),
        // Every band squashed hard and blended in under the dry, which keeps its attack.
        b.Make("parallel-squash", "Parallel Squash",
               {{"threshold1", -40.0},
                {"threshold2", -40.0},
                {"threshold3", -40.0},
                {"threshold4", -40.0},
                {"ratio1", 8.0},
                {"ratio2", 8.0},
                {"ratio3", 8.0},
                {"ratio4", 8.0},
                {"gain1", 8.0},
                {"gain2", 8.0},
                {"gain3", 8.0},
                {"gain4", 8.0},
                {"attack", 5.0},
                {"release", 100.0},
                {"mix", 0.5}}),
        b.Make("gentle-glue", "Gentle Glue",
               {{"threshold1", -18.0},
                {"threshold2", -18.0},
                {"threshold3", -18.0},
                {"threshold4", -18.0},
                {"ratio1", 1.8},
                {"ratio2", 1.8},
                {"ratio3", 1.8},
                {"ratio4", 1.8},
                {"gain1", 1.0},
                {"gain2", 1.0},
                {"gain3", 1.0},
                {"gain4", 1.0},
                {"attack", 30.0},
                {"release", 250.0}}),
    };

    EffectRegistry::Instance().Register(info.type, info,
                                        []() { return std::make_unique<MultibandCompressorEffect>(); });
}
} // namespace guitarfx
