#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/DrivePedal.h"
#include "dsp/effects/DriveStages.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace guitarfx
{
namespace overdrive
{
enum Param : std::size_t
{
    kModel,
    kDrive,
    kTone,
    kBass,
    kClipping,
    kLevel,
    kMix,
    kParamCount
};

/// Index order is stored in presets: append new models, never reorder.
enum class Model
{
    TubeScreamer,
    Centaur,
    Bluesbreaker,
    Timmy,
    Fulldrive,
    Lpb1,
    TrebleBooster,
    Count
};

inline constexpr const char* kModelLabels[] = {"TS-808",    "Centaur", "Bluesbreaker", "Timmy",
                                               "Fulldrive", "LPB-1",   "Rangemaster"};
static_assert(std::size(kModelLabels) == static_cast<std::size_t>(Model::Count));

inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"model", "Model", 0.0, 0.0, 6.0, "enum", "Pedal", false, 1.0, kModelLabels},
    {"drive", "Drive", 0.5, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"tone", "Tone", 0.5, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"bass", "Bass", 0.5, 0.0, 1.0, "amount", "Voicing", false, 0.0},
    {"clipping", "Clipping", 0.0, 0.0, 6.0, "enum", "Voicing", false, 1.0, drive::kClipChoiceLabels},
    {"level", "Level", 0.0, -24.0, 24.0, "dB", "Output", false, 0.0},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0},
}};

/// How a model's Tone knob works. Each is a one-pole split at a corner, `low x LP + high x HP`.
enum class ToneStyle
{
    TubeScreamer, ///< fixed 723 Hz low-pass with the treble blended back in, cut to boost
    Shelf,        ///< a treble shelf, cut to boost, flat at noon (the Centaur's Treble)
    Tilt,         ///< lows and highs see-saw about the corner, flat at noon
    LowPass       ///< a treble roll-off that sweeps from the corner up by `toneRange`
};

/**
 * One classic overdrive, as the handful of things that make it sound like itself.
 *
 * All of them are one topology: a clean path, plus a gain path that high-passes, amplifies,
 * low-passes and clips. What differs is where the diodes sit and the values around them:
 *
 *     y = clean x + dirty LP( clip( LP( G HP(x) ) ) )
 *
 * The gain path is an op-amp stage (drive::ProcessOpAmpStage); where it reaches its rails
 * before the diodes, as the Centaur does, the rails and the stage's own feedback shape it too.
 *
 * - TS-808: op-amp gain 1 + (51k + 500k Drive) / 4.7k. The 4.7k to ground goes through
 *   47 nF, so only the mids and highs above 720 Hz are amplified into the 1N914 pair across
 *   the feedback loop, and the dry signal sums at unity: the bass stays clean and the pedal
 *   never squares off. 51 pF across the feedback resistor rolls the gain path off at 5.7 kHz
 *   at full drive. The tone stage is a fixed 723 Hz low-pass (1k and 220 nF) with the treble
 *   blended back by the Tone pot.
 * - Centaur: clean and clipped paths summed, the clean one turned down as gain rises (the
 *   dual-gang gain pot). The gain climbs steeply early in the pot's travel, from well below
 *   the diodes at 0, where the pedal is a clean boost, to about 25 dB. Germanium diodes to
 *   ground clip the gain path, which is focused on the upper mids and rolled off at 1.6 kHz
 *   after the diodes; the Treble control is a shelf, a little under flat at noon. At high
 *   gain the op-amp reaches its 9 V rails, which sit unevenly about its bias: even harmonics.
 *   Fitted to five captures of a Behringer Centaur, Gain 0 to 10 at Treble noon.
 * - Bluesbreaker: the TS layout with the bass left in (46 Hz) and a lower gain range: the
 *   original "amp in a box" overdrive. Its clipped path is rolled off at 1.75 kHz after the
 *   diodes, and the tone control is a gentle tilt about 630 Hz. Symmetric: captures of an
 *   original and a reissue put their 2nd harmonic 50 dB down or more. Fitted to both.
 * - Timmy: no clean path; silicon diodes to ground after a low-gain stage whose bass cut is
 *   low, so it stays flat and transparent. Its Treble is a cut-only roll-off.
 * - Fulldrive: a TS with more gain and the bass corner halved ("flat mids"), and a treble
 *   roll-off in place of the TS tone stage.
 * - LPB-1: one transistor, up to +24 dB of full-range boost. It only clips, asymmetrically,
 *   when pushed near its 9 V supply. Drive is the boost; there is no makeup.
 * - Rangemaster: a germanium treble booster. Its small input capacitor feeds the transistor
 *   only the upper mids and treble, which it boosts by up to 26 dB, so the bass passes at
 *   about its own level while the top end is pushed into the amp, and into the transistor's
 *   soft, lopsided germanium saturation. Modelled as the clean signal plus that boosted,
 *   clipped treble, so at Drive 0 it is the bypass sound. Bass moves the treble corner two
 *   octaves either way about 1.8 kHz, as the common "range" modification does. A boost, so
 *   there is no makeup.
 */
struct Voicing
{
    double minGainDb;
    double maxGainDb;
    double gainTaper;      ///< the gain pot's law: dB across its travel go as amount^gainTaper
    double gainHighPassHz; ///< the Bass knob moves this two octaves either way
    double lowPassAtMinGainHz;
    double lowPassAtMaxGainHz;
    double cleanAtMinDrive;
    double cleanAtMaxDrive;
    double dirty;
    double railPositive; ///< where the gain stage's op-amp pins, volts from its bias (0: never)
    double railNegative;
    double dirtyLowPassHz; ///< after the diodes, on the gain path only (0: none)
    drive::ClipCurve stockClip;
    double clipLevelExponent; ///< see ClipLevelCompensation
    ToneStyle tone;
    double toneHz;
    double toneRange;  ///< Shelf and Tilt: dB either way; LowPass: the sweep's frequency ratio
    double toneNoonDb; ///< Shelf: where it sits at noon
    drive::TrimTable trimDb;
};

// clang-format off
// *INDENT-OFF*
inline constexpr std::array<Voicing, static_cast<std::size_t>(Model::Count)> kVoicings = {{
    // TS-808
    {20.7, 41.4, 1.0, 720.0,  61000.0, 5660.0, 1.0, 1.0,  1.0, 0.0, 0.0, 0.0, {0.55, 0.55, drive::Knee::Soft, drive::Knee::Soft}, 0.73,
     ToneStyle::TubeScreamer, 723.0, 0.0, 0.0, {-2.4, -2.7, -2.9, -3.1, -3.2, -3.3, -3.4, -3.5, -3.5}},
    // Centaur
    {-18.0, 25.0, 0.3, 320.0,  4000.0, 2800.0, 1.0, 0.33, 2.8, 4.2, 3.6, 1600.0, {0.32, 0.32, drive::Knee::Gradual, drive::Knee::Gradual}, 0.63,
     ToneStyle::Shelf, 1100.0, 10.0, -2.5, {0.7, -4.0, -5.0, -5.4, -5.7, -5.8, -5.8, -5.7, -5.6}},
    // Bluesbreaker
    {6.0,  34.0, 1.0, 46.0,   20000.0, 11900.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1750.0, {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.55,
     ToneStyle::Tilt, 630.0, 5.7, 0.0, {-4.0, -4.6, -5.2, -5.7, -6.1, -6.4, -6.6, -6.7, -6.8}},
    // Timmy
    {0.0,  34.0, 1.0, 90.0,   20000.0, 8000.0, 0.0, 0.0,  1.0, 0.0, 0.0, 0.0, {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.66,
     ToneStyle::LowPass, 700.0, 30.0, 0.0, {4.0, 1.4, -0.8, -2.4, -3.5, -4.4, -5.0, -5.3, -5.6}},
    // Fulldrive
    {20.0, 46.0, 1.0, 350.0, 30000.0, 6000.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.77,
     ToneStyle::LowPass, 900.0, 14.0, 0.0, {-7.0, -7.3, -7.5, -7.7, -7.8, -7.9, -8.0, -8.0, -8.0}},
    // LPB-1
    {0.0, 24.0, 1.0, 25.0, 60000.0, 60000.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, {4.0, 3.2, drive::Knee::Soft, drive::Knee::Soft}, 0.46,
     ToneStyle::Tilt, 1000.0, 6.0, 0.0, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    // Rangemaster
    {-30.0, 26.0, 0.35, 1800.0, 14000.0, 9000.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, {1.6, 0.9, drive::Knee::Gradual, drive::Knee::Gradual}, 0.3,
     ToneStyle::Tilt, 1000.0, 6.0, 0.0, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
}};
// *INDENT-ON*
// clang-format on

inline constexpr double kInputHighPassHz = 20.0;
inline constexpr double kDcBlockHz = 8.0;

struct Traits
{
    static constexpr const char* kTypeName = "overdrive";
    static constexpr const auto& kParams = overdrive::kParams;
    static constexpr std::size_t kLevel = overdrive::kLevel;
    static constexpr std::size_t kMix = overdrive::kMix;

    struct Coefficients
    {
        double inputHighPass = 0.0;
        drive::OpAmpStageCoefficients gainStage;
        bool hasDirtyLowPass = false;
        double dirtyLowPass = 1.0;
        double clean = 1.0;
        double dirty = 1.0;
        drive::ClipCurve clip;
        double toneCoefficient = 0.0;
        double toneLow = 1.0;
        double toneHigh = 1.0;
        double dcBlock = 0.0;
        double trim = 1.0;
    };

    struct State
    {
        drive::OnePole input;
        drive::OpAmpStageState gainStage;
        drive::OnePole dirtyLowPass;
        drive::OnePole tone;
        drive::OnePole dcBlock;
        drive::AntialiasMemory clip;

        void Reset() noexcept
        {
            *this = State{};
        }
    };

    static void Design(Coefficients& c, const std::array<double, kParamCount>& values, double rate, double osRate)
    {
        const auto modelIndex = static_cast<std::size_t>(
            std::clamp(static_cast<int>(values[kModel]), 0, static_cast<int>(Model::Count) - 1));
        const Voicing& v = kVoicings[modelIndex];
        const double amount = values[kDrive];
        const double tone = values[kTone];

        c.inputHighPass = drive::OnePoleCoefficient(kInputHighPassHz, rate);
        // The gain stage: its leg's corner (the Bass knob moves it), the gain the pot gives
        // along its law, the feedback capacitor, and the op-amp's rails where it reaches them.
        drive::OpAmpStageCoefficients& stage = c.gainStage;
        const double gainDb = v.minGainDb + (v.maxGainDb - v.minGainDb) * std::pow(amount, v.gainTaper);
        stage.legs[0] = {drive::OnePoleCoefficient(v.gainHighPassHz * drive::KnobOctaves(values[kBass], -2.0), osRate),
                         drive::DbToGain(gainDb)};
        stage.unity = false;
        // The feedback capacitor's corner moves inversely with the feedback resistance, so it
        // is geometric in the gain.
        const double lowPassHz = v.lowPassAtMinGainHz * std::pow(v.lowPassAtMaxGainHz / v.lowPassAtMinGainHz, amount);
        stage.feedback = drive::OnePoleCoefficient(lowPassHz, osRate);
        stage.hasRails = v.railPositive > 0.0;
        stage.railPull = drive::OnePoleCoefficient(drive::kRailPullHz, osRate);
        stage.rails = {v.railPositive, v.railNegative, drive::Knee::Hard, drive::Knee::Hard};
        c.hasDirtyLowPass = v.dirtyLowPassHz > 0.0;
        c.dirtyLowPass = drive::OnePoleCoefficient(c.hasDirtyLowPass ? v.dirtyLowPassHz : osRate, osRate);
        c.clean = v.cleanAtMinDrive + (v.cleanAtMaxDrive - v.cleanAtMinDrive) * amount;
        c.dirty = v.dirty;

        const auto choice = static_cast<drive::ClipChoice>(
            std::clamp(static_cast<int>(values[kClipping]), 0, static_cast<int>(drive::ClipChoice::Count) - 1));
        c.clip = drive::ClipFor(choice, v.stockClip);

        double toneHz = v.toneHz;
        c.toneLow = 1.0;

        switch (v.tone)
        {
        case ToneStyle::TubeScreamer:
            // About 10 dB of treble cut at noon, a slight lift fully up.
            c.toneHigh = 0.03 + 1.2 * tone * tone;
            break;

        case ToneStyle::Shelf:
            c.toneHigh = drive::DbToGain(v.toneNoonDb + (tone - 0.5) * 2.0 * v.toneRange);
            break;

        case ToneStyle::Tilt:
            c.toneLow = drive::DbToGain(-(tone - 0.5) * v.toneRange);
            c.toneHigh = drive::DbToGain((tone - 0.5) * v.toneRange);
            break;

        case ToneStyle::LowPass:
            toneHz = v.toneHz * std::pow(v.toneRange, tone);
            c.toneHigh = 0.0;
            break;
        }

        c.toneCoefficient = drive::OnePoleCoefficient(toneHz, rate);
        c.dcBlock = drive::OnePoleCoefficient(kDcBlockHz, rate);
        c.trim = drive::DbToGain(drive::InterpolateTrimDb(v.trimDb, amount)) *
                 drive::ClipLevelCompensation(v.stockClip, c.clip, v.clipLevelExponent);
    }

    static double Pre(const Coefficients& c, State& s, double volts) noexcept
    {
        return s.input.HighPass(c.inputHighPass, volts);
    }

    static double Shape(const Coefficients& c, State& s, double x) noexcept
    {
        const double driven = drive::ProcessOpAmpStage(c.gainStage, s.gainStage, x);
        const double clipped = c.clip.Antialiased(driven, s.clip);
        return c.clean * x + c.dirty * (c.hasDirtyLowPass ? s.dirtyLowPass.LowPass(c.dirtyLowPass, clipped) : clipped);
    }

    static double Post(const Coefficients& c, State& s, double y) noexcept
    {
        const double low = s.tone.LowPass(c.toneCoefficient, y);
        const double toned = low * c.toneLow + (y - low) * c.toneHigh;
        return s.dcBlock.HighPass(c.dcBlock, toned) * c.trim;
    }
};
} // namespace overdrive

/**
 * Overdrive pedal with seven classic circuits behind its Model switch. See overdrive::Voicing
 * for what each one models.
 *
 * Drive, Tone and Level are the pedal's own knobs. Bass moves the gain path's bass cut two
 * octaves either way, so noon is the stock pedal. Clipping swaps the diodes where they sit:
 * silicon, the SD-1's asymmetric pair, LEDs, germanium, MOSFETs, or none (the Fulldrive's
 * "comp cut"). Level is calibrated so the pedal at default drive is about as loud as bypass
 * for a guitar at the nominal operating level (the LPB-1 and the Rangemaster excepted: boosting
 * is their job).
 */
class OverdriveEffect : public drive::DrivePedal<overdrive::Traits>
{
};

inline void RegisterOverdriveEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kOverdrive;
    info.aliases = {"overdrive"};
    info.displayName = "Overdrive";
    info.category = "drive";
    info.description =
        "Classic overdrives: TS-808, Centaur, Bluesbreaker, Timmy, Fulldrive, LPB-1 boost and Rangemaster treble booster";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(overdrive::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<OverdriveEffect>(); });
}
} // namespace guitarfx
