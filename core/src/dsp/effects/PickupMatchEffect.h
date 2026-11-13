#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/LinearRamp.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/PickupDesign.h"
#include "dsp/effects/PickupModels.generated.h"
#include "dsp/effects/PickupSensing.generated.h"
#include "dsp/effects/PickupStringComb.h"
#include "dsp/effects/PickupTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <vector>

/**
 * Pickup Match: makes one kind of pickup sound like another, a single coil like a humbucker
 * or the other way round.
 *
 * A passive pickup is close to a linear filter: its inductance resonates with the winding,
 * cable and pot load into a second-order low-pass with a peak. The pickup's own resonance is
 * already in the incoming signal, and turning it into another's is a ratio of two such
 * responses:
 *
 *     H(s) = (s^2/ws^2 + s/(ws Qs) + 1) / (s^2/wt^2 + s/(wt Qt) + 1)
 *
 * The zeros sit on the source pickup's poles and cancel its peak, and the poles put the
 * target's in. That is one biquad, unity at DC, plus the difference in output level.
 *
 * Either side can be a generic class or one of 250 measured pickups. Each measured pickup is a
 * second-order fit to its response (28 Hz to 20 kHz, into 10 ft of low-capacitance cable and a
 * 500K pot; tools/gen-pickup-models.mjs writes PickupModels.generated.h). The fit reaches a median
 * 0.37 dB RMS up to 12 kHz. Active and some noiseless pickups fit worse, up to 5 dB, because their
 * curves carry their own EQ; their fitErrorDb says so. The classes are medians of those fits.
 *
 * The measurements are electrical. Where a pickup sits on the string and how wide it senses
 * matter as much: a bridge pickup barely hears the low harmonics, which hardly move the string
 * there, and a humbucker's two coils take the upper ones down. Those comb the harmonics by
 * harmonic number, so by note; String Sensing applies their average over the notes a guitar
 * plays (tools/gen-pickup-sensing.mjs, PickupSensing.generated.h), which holds for chords too.
 * It runs as the target's low shelf, peak and high shelf with the source's undone.
 *
 * String Comb (a prototype, off by default) adds the comb one string really gets from the
 * target's position, as detail over that average: see PickupStringComb.h.
 */
namespace guitarfx
{
namespace pickup_match
{
/// The generic classes, which come first in the catalogue so their indices never move.
enum class Pickup : std::size_t
{
    SingleCoilNeck,
    SingleCoilMiddle,
    SingleCoilBridge,
    P90,
    HumbuckerNeck,
    HumbuckerBridge,
    ActiveHumbucker,
    Count
};

/// Medians per class of the measured models' fits. The active class is the roughest: actives
/// vary with their preamp, and the level is a typical one rather than any particular unit's.
inline constexpr std::array<PickupModel, static_cast<std::size_t>(Pickup::Count)> kClasses = {{
    {"single-coil-neck", "Single Coil (Neck)", "Generic", PickupFamily::SingleCoil, PickupPosition::Neck, 3551.0, 1.33,
     -0.9, 0.0},
    {"single-coil-middle", "Single Coil (Middle)", "Generic", PickupFamily::SingleCoil, PickupPosition::Middle, 3551.0,
     1.38, 1.7, 0.0},
    {"single-coil-bridge", "Single Coil (Bridge)", "Generic", PickupFamily::SingleCoil, PickupPosition::Bridge, 3217.0,
     1.23, 2.1, 0.0},
    {"p90", "P-90", "Generic", PickupFamily::P90, PickupPosition::Any, 1577.0, 1.09, 6.6, 0.0},
    {"humbucker-neck", "Humbucker (Neck)", "Generic", PickupFamily::Humbucker, PickupPosition::Neck, 2800.0, 1.01, 7.5,
     0.0},
    {"humbucker-bridge", "Humbucker (Bridge)", "Generic", PickupFamily::Humbucker, PickupPosition::Bridge, 2343.0, 0.97,
     9.7, 0.0},
    {"active-humbucker", "Active Humbucker", "Generic", PickupFamily::Active, PickupPosition::Any, 4973.0, 0.63, 11.1,
     0.0},
}};

/// Where "Custom Design" sits: straight after the classes and ahead of the measured models, so
/// the generated table can keep growing at the end without moving it.
inline constexpr std::size_t kCustomPickup = kClasses.size();

/// The catalogue's stand-in for a designed pickup. A designed pickup's numbers come from the
/// design parameters (ResolvePickup), never from these.
inline constexpr PickupModel kCustomModel = {
    "custom-design", "Custom Design", "Custom", PickupFamily::Custom, PickupPosition::Any, 3400.0, 1.4, 0.4, 0.0};

inline constexpr std::size_t kPickupCount = kClasses.size() + 1 + kMeasuredModels.size();

/// Every pickup a choice can name: the classes, the custom design, then the measured models in
/// their generated (append-only) order. A preset stores the index, so this order is part of
/// the format.
inline constexpr std::array<PickupModel, kPickupCount> kPickups = [] {
    std::array<PickupModel, kPickupCount> all{};
    std::size_t next = 0;

    for (const auto& pickup : kClasses)
    {
        all[next++] = pickup;
    }

    all[next++] = kCustomModel;

    for (const auto& pickup : kMeasuredModels)
    {
        all[next++] = pickup;
    }

    return all;
}();

inline constexpr std::array<const char*, kPickupCount> kPickupLabels = [] {
    std::array<const char*, kPickupCount> labels{};

    for (std::size_t i = 0; i < kPickupCount; ++i)
    {
        labels[i] = kPickups[i].label;
    }

    return labels;
}();

/// What happens to the level difference between the two pickups.
enum class LevelMode
{
    Pickup = 0,  ///< a hotter target comes out hotter, and drives what follows harder
    Matched = 1, ///< the level stays where it was, for an even comparison
    Count
};

inline constexpr const char* kLevelModeLabels[] = {"Pickup Output", "Matched"};
static_assert(std::size(kLevelModeLabels) == static_cast<std::size_t>(LevelMode::Count));

// A custom design's choices, in the order of PickupDesign.h's enums.
inline constexpr const char* kDesignTypeLabels[] = {kDesignTypes[0].label, kDesignTypes[1].label,
                                                    kDesignTypes[2].label};
static_assert(std::size(kDesignTypeLabels) == kDesignTypes.size());
inline constexpr const char* kGaugeLabels[] = {"42 AWG", "43 AWG", "44 AWG"};
static_assert(std::size(kGaugeLabels) == kMaxGauge - kMinGauge + 1);
inline constexpr const char* kMagnetLabels[] = {"Alnico 2", "Alnico 3", "Alnico 5", "Ceramic"};
static_assert(std::size(kMagnetLabels) == static_cast<std::size_t>(DesignMagnet::Count));
inline constexpr const char* kCoverLabels[] = {"None", "Nickel Silver", "Brass"};
static_assert(std::size(kCoverLabels) == static_cast<std::size_t>(DesignCover::Count));
inline constexpr const char* kWiringLabels[] = {"Series", "Parallel", "Single Coil"};
static_assert(std::size(kWiringLabels) == static_cast<std::size_t>(DesignWiring::Count));

enum Param : std::size_t
{
    kSource,
    kTarget,
    kAmount,
    kLevelMode,
    kMaxBoost,
    kOutput,
    // A custom design per side, read only while that side is set to Custom Design. The two
    // runs are laid out alike: DesignSpecFor reads one from its first entry.
    kSourceDesignType,
    kSourceResistance,
    kSourceGauge,
    kSourceMagnet,
    kSourceCover,
    kSourceWiring,
    kSourceCapacitanceTrim,
    kTargetDesignType,
    kTargetResistance,
    kTargetGauge,
    kTargetMagnet,
    kTargetCover,
    kTargetWiring,
    kTargetCapacitanceTrim,
    // String sensing: where each pickup sits on the string and how wide it senses.
    kSensing,
    kSourcePosition,
    kTargetPosition,
    // String Comb: the per-string comb in place of String Sensing's average (PickupStringComb.h).
    kComb,
    kCombString,
    kParamCount
};

inline constexpr std::size_t kDesignParamCount = kTargetDesignType - kSourceDesignType;
static_assert(kTargetCapacitanceTrim + 1 - kTargetDesignType == kDesignParamCount);

/// A side's position: from the chosen pickup (its catalogue position, or for a type alone the
/// usual one), or set by hand.
enum class PositionChoice
{
    FromPickup,
    Neck,
    Middle,
    Bridge,
    Count
};

inline constexpr const char* kPositionLabels[] = {"From Pickup", "Neck", "Middle", "Bridge"};
static_assert(std::size(kPositionLabels) == static_cast<std::size_t>(PositionChoice::Count));

/// The string the comb runs for: the one the tracked pitch is played on, or a fixed one.
inline constexpr const char* kCombStringLabels[] = {"Follow Pitch", "Low E", "A", "D", "G", "B", "High E"};
static_assert(std::size(kCombStringLabels) == kOpenStringHz.size() + 1);

inline constexpr double kLastPickup = static_cast<double>(kPickupCount - 1);

/// Amount past 1 carries on past the target, the same way and in proportion: a caricature of
/// the difference, for when the real one is too subtle to hear through a drive and a cab.
inline constexpr double kMaxAmount = 3.0;

// What extrapolating may reach. Each widens to take in both pickups, so nothing up to Amount 1
// is ever held back: only the caricature is.
inline constexpr double kMinExaggeratedHz = 300.0;
inline constexpr double kMinExaggeratedQ = 0.3;
inline constexpr double kMaxExaggeratedQ = 6.0;
inline constexpr double kMaxExaggeratedLevelDb = 18.0;
inline constexpr double kMaxSensingBandDb = 18.0;

/// The one place parameter ranges live: registration and SetParam's clamping both read it.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"source", "Your Pickup", static_cast<double>(Pickup::SingleCoilBridge), 0.0, kLastPickup, "enum", "Pickups", false,
     1.0, kPickupLabels},
    {"target", "Sound Like", static_cast<double>(Pickup::HumbuckerBridge), 0.0, kLastPickup, "enum", "Pickups", false,
     1.0, kPickupLabels},
    {"amount", "Amount", 1.0, 0.0, kMaxAmount, "amount", "Pickups", false, 0.0},
    {"levelMode", "Level", 0.0, 0.0, 1.0, "enum", "Output", false, 1.0, kLevelModeLabels},
    {"maxBoost", "Max Treble Boost", 12.0, 0.0, 24.0, "dB", "Output", true, 0.0},
    {"output", "Output", 0.0, -12.0, 12.0, "dB", "Output", false, 0.0},
    {"sourceDesignType", "Type", static_cast<double>(DesignType::StratSingleCoil), 0.0, 2.0, "enum",
     "Your Pickup Design", true, 1.0, kDesignTypeLabels},
    {"sourceResistance", "DC Resistance", 6.5, 3.0, 20.0, "kΩ", "Your Pickup Design", true, 0.05},
    {"sourceGauge", "Wire Gauge", 42.0, kMinGauge, kMaxGauge, "enum", "Your Pickup Design", true, 1.0, kGaugeLabels},
    {"sourceMagnet", "Magnet", static_cast<double>(DesignMagnet::Alnico5), 0.0, 3.0, "enum", "Your Pickup Design", true,
     1.0, kMagnetLabels},
    {"sourceCover", "Cover", static_cast<double>(DesignCover::None), 0.0, 2.0, "enum", "Your Pickup Design", true, 1.0,
     kCoverLabels},
    {"sourceWiring", "Coil Wiring", static_cast<double>(DesignWiring::Series), 0.0, 2.0, "enum", "Your Pickup Design",
     true, 1.0, kWiringLabels},
    {"sourceCapacitanceTrim", "Capacitance Trim", 0.0, -50.0, 50.0, "%", "Your Pickup Design", true, 1.0},
    {"targetDesignType", "Type", static_cast<double>(DesignType::PafHumbucker), 0.0, 2.0, "enum", "Sound Like Design",
     true, 1.0, kDesignTypeLabels},
    {"targetResistance", "DC Resistance", 8.2, 3.0, 20.0, "kΩ", "Sound Like Design", true, 0.05},
    {"targetGauge", "Wire Gauge", 42.0, kMinGauge, kMaxGauge, "enum", "Sound Like Design", true, 1.0, kGaugeLabels},
    {"targetMagnet", "Magnet", static_cast<double>(DesignMagnet::Alnico5), 0.0, 3.0, "enum", "Sound Like Design", true,
     1.0, kMagnetLabels},
    {"targetCover", "Cover", static_cast<double>(DesignCover::None), 0.0, 2.0, "enum", "Sound Like Design", true, 1.0,
     kCoverLabels},
    {"targetWiring", "Coil Wiring", static_cast<double>(DesignWiring::Series), 0.0, 2.0, "enum", "Sound Like Design",
     true, 1.0, kWiringLabels},
    {"targetCapacitanceTrim", "Capacitance Trim", 0.0, -50.0, 50.0, "%", "Sound Like Design", true, 1.0},
    {"sensing", "String Sensing", 1.0, 0.0, 1.0, "amount", "String Sensing", false, 0.0},
    {"sourcePosition", "Your Pickup Position", 0.0, 0.0, 3.0, "enum", "String Sensing", false, 1.0, kPositionLabels},
    {"targetPosition", "Sound Like Position", 0.0, 0.0, 3.0, "enum", "String Sensing", false, 1.0, kPositionLabels},
    {"comb", "String Comb", 0.0, 0.0, 1.0, "amount", "String Sensing", false, 0.0},
    {"combString", "Comb String", 0.0, 0.0, 6.0, "enum", "String Sensing", false, 1.0, kCombStringLabels},
}};

using Values = std::array<double, kParamCount>;

inline constexpr Values kDefaultValues = DefaultParamValues(kParams);

/// The electrical section, then the target's three sensing bands, then the source's undone.
inline constexpr std::size_t kSectionCount = 7;

/// What the effect runs: its sections and a gain.
struct Design
{
    std::array<BiquadCoefficients, kSectionCount> sections{}; ///< the identity when there is nothing to change
    double gain = 1.0;
    CombSettings comb; ///< String Comb, at strength 0 unless it is turned up
};

/// The pickup's loaded response, s^2/w^2 + s/(w Q) + 1, after the bilinear transform
/// prewarped at its own resonance and scaled by (1 + z^-1)^2: {c0, c1, c2}. Each polynomial is
/// warped at its own peak, so both peaks land where they belong and DC stays at exactly 1 (each
/// sums to 4 at z = 1).
[[nodiscard]] inline std::array<double, 3> Polynomial(double resonanceHz, double q, double sampleRate) noexcept
{
    const double t = std::tan(biquad::kPi * ClampBiquadFrequency(resonanceHz, sampleRate) / sampleRate);
    const double a = 1.0 / (t * t);
    const double b = 1.0 / (t * q);
    return {a + b + 1.0, 2.0 - 2.0 * a, a - b + 1.0};
}

/// The section that turns a pickup resonating at (sourceHz, sourceQ) into one at
/// (targetHz, targetQ). Both polynomials have their roots inside the unit circle, so the
/// section is stable and minimum-phase.
[[nodiscard]] inline BiquadCoefficients MatchSection(double sourceHz, double sourceQ, double targetHz, double targetQ,
                                                     double sampleRate) noexcept
{
    if (sourceHz == targetHz && sourceQ == targetQ)
    {
        return {};
    }

    const auto n = Polynomial(sourceHz, sourceQ, sampleRate);
    const auto d = Polynomial(targetHz, targetQ, sampleRate);
    const double invD0 = 1.0 / d[0];
    return {n[0] * invD0, n[1] * invD0, n[2] * invD0, d[1] * invD0, d[2] * invD0};
}

[[nodiscard]] inline std::size_t PickupIndex(double value) noexcept
{
    return static_cast<std::size_t>(std::clamp(std::lround(value), 0L, static_cast<long>(kLastPickup)));
}

/// The design a side's run of design parameters describes, from its first entry.
[[nodiscard]] inline DesignSpec DesignSpecFor(const Values& values, std::size_t first) noexcept
{
    const auto choice = [&](std::size_t offset, std::size_t count) {
        return static_cast<std::size_t>(
            std::clamp(std::lround(values[first + offset]), 0L, static_cast<long>(count) - 1));
    };

    DesignSpec spec;
    spec.type = static_cast<DesignType>(choice(0, kDesignTypes.size()));
    spec.resistanceOhms = values[first + 1] * 1000.0;
    spec.gauge = static_cast<int>(std::lround(values[first + 2]));
    spec.magnet = static_cast<DesignMagnet>(choice(3, static_cast<std::size_t>(DesignMagnet::Count)));
    spec.cover = static_cast<DesignCover>(choice(4, static_cast<std::size_t>(DesignCover::Count)));
    spec.wiring = static_cast<DesignWiring>(choice(5, static_cast<std::size_t>(DesignWiring::Count)));
    spec.capacitanceTrim = values[first + 6] / 100.0;
    return spec;
}

[[nodiscard]] inline std::size_t DesignParamsFor(Param side) noexcept
{
    return side == kSource ? kSourceDesignType : kTargetDesignType;
}

/// The pickup a side names: a catalogue entry, or for Custom Design the one its design
/// parameters build. Arithmetic only, so it is safe wherever SetParam runs.
[[nodiscard]] inline PickupModel ResolvePickup(const Values& values, Param side) noexcept
{
    const std::size_t index = PickupIndex(values[side]);

    if (index != kCustomPickup)
    {
        return kPickups[index];
    }

    const DesignResult designed = DesignPickup(DesignSpecFor(values, DesignParamsFor(side)));
    PickupModel model = kCustomModel;
    model.resonanceHz = designed.resonanceHz;
    model.q = designed.q;
    model.levelDb = designed.levelDb;
    return model;
}

/// Which of kSensingFits a side senses like: its family's geometry (single coil, P-90 or
/// humbucker; actives are humbuckers, anything else a single coil) at its position. The position
/// is the pickup's own, or the side's Position when set; a pickup without one (a P-90 or active
/// class, a model measured on its own, a custom design) is taken where that kind usually sits:
/// a single coil in the middle, anything else at the bridge.
[[nodiscard]] inline std::size_t SensingProfileIndex(const Values& values, Param side) noexcept
{
    const std::size_t index = PickupIndex(values[side]);
    PickupFamily family = kPickups[index].family;
    PickupPosition position = kPickups[index].position;

    if (index == kCustomPickup)
    {
        const auto spec = DesignSpecFor(values, DesignParamsFor(side));
        family = kDesignTypes[static_cast<std::size_t>(spec.type)].humbucker ? PickupFamily::Humbucker
                                                                             : PickupFamily::SingleCoil;
        position = PickupPosition::Any;
    }

    const auto choice = static_cast<PositionChoice>(
        std::clamp(std::lround(values[side == kSource ? kSourcePosition : kTargetPosition]), 0L, 3L));

    if (choice != PositionChoice::FromPickup)
    {
        position = choice == PositionChoice::Neck     ? PickupPosition::Neck
                   : choice == PositionChoice::Middle ? PickupPosition::Middle
                                                      : PickupPosition::Bridge;
    }

    const std::size_t geometry = family == PickupFamily::P90                                           ? 1
                                 : family == PickupFamily::Humbucker || family == PickupFamily::Active ? 2
                                                                                                       : 0;

    if (position == PickupPosition::Any)
    {
        position = geometry == 0 ? PickupPosition::Middle : PickupPosition::Bridge;
    }

    const std::size_t place = position == PickupPosition::Neck ? 0 : position == PickupPosition::Middle ? 1 : 2;
    return geometry * 3 + place;
}

/// One band of a sensing profile at `scale` of its gain: a low shelf, a peak, a high shelf.
[[nodiscard]] inline BiquadCoefficients SensingSection(std::size_t band, const SensingBand& spec, double scale,
                                                       double sampleRate) noexcept
{
    const double gainDb = std::clamp(spec.gainDb * scale, -kMaxSensingBandDb, kMaxSensingBandDb);

    switch (band)
    {
    case 0:
        return biquad::LowShelf(spec.frequencyHz, spec.q, gainDb, sampleRate);
    case 1:
        return biquad::Peaking(spec.frequencyHz, spec.q, gainDb, sampleRate);
    default:
        return biquad::HighShelf(spec.frequencyHz, spec.q, gainDb, sampleRate);
    }
}

/// The design for a set of parameter values.
///
/// Amount moves the target's resonance and Q from the source's towards the chosen pickup's by
/// ratio, and its level by dB, so 0 is exactly the identity and 1 is the chosen pickup. Past 1 it
/// carries on the same way (see kMaxAmount), held to a resonance of at least 300 Hz, a Q of
/// 0.3-6 and a level change of 18 dB, or the pickups' own where those go further, so a caricature
/// stays a stable filter at a usable level. Max Treble Boost caps how far the
/// target's peak may sit above the source's. Above both peaks the analogue section's gain
/// settles at (ft / fs)^2; after prewarping, the digital one reaches (tan(pi ft / sr) /
/// tan(pi fs / sr))^2 at Nyquist, which is larger and grows without bound as ft nears Nyquist
/// (+66 dB from a P-90 to an active pickup at 8 kHz). So the cap is on the tangents: it holds
/// the boost at every frequency, at every sample rate. It matters going from a low, damped
/// pickup to a bright one, where restoring the missing top would raise hiss and pick noise as
/// much as the strings.
[[nodiscard]] inline Design BuildDesign(const Values& values, double sampleRate) noexcept
{
    const PickupModel source = ResolvePickup(values, kSource);
    const PickupModel target = ResolvePickup(values, kTarget);
    const double amount = std::clamp(values[kAmount], 0.0, kMaxAmount);

    double targetHz = source.resonanceHz * std::pow(target.resonanceHz / source.resonanceHz, amount);
    targetHz = std::max(targetHz, std::min({kMinExaggeratedHz, source.resonanceHz, target.resonanceHz}));
    const double targetQ =
        std::clamp(source.q * std::pow(target.q / source.q, amount), std::min({kMinExaggeratedQ, source.q, target.q}),
                   std::max({kMaxExaggeratedQ, source.q, target.q}));

    if (targetHz > source.resonanceHz)
    {
        const double maxRatio = std::pow(10.0, std::max(0.0, values[kMaxBoost]) / 40.0);
        const double sourceTan =
            std::tan(biquad::kPi * ClampBiquadFrequency(source.resonanceHz, sampleRate) / sampleRate);
        const double capHz = std::atan(sourceTan * maxRatio) * sampleRate / biquad::kPi;
        targetHz = std::max(source.resonanceHz, std::min(targetHz, capHz));
    }

    double levelDb = values[kOutput];

    if (std::lround(values[kLevelMode]) == static_cast<long>(LevelMode::Pickup))
    {
        const double gapDb = target.levelDb - source.levelDb;
        const double limitDb = std::max(kMaxExaggeratedLevelDb, std::abs(gapDb));
        levelDb += std::clamp(amount * gapDb, -limitDb, limitDb);
    }

    Design design;
    design.sections[0] = MatchSection(source.resonanceHz, source.q, targetHz, targetQ, sampleRate);
    design.gain = std::pow(10.0, levelDb / 20.0);

    // String sensing: the target's profile, and the source's undone. A shelf or peak with its
    // gain negated is the exact inverse of itself (RBJ, fixed Q), so the pair cancels when the
    // two are alike, and the same pickup on both sides runs nothing at all.
    const std::size_t sourceProfile = SensingProfileIndex(values, kSource);
    const std::size_t targetProfile = SensingProfileIndex(values, kTarget);
    const double strength = std::clamp(values[kSensing], 0.0, 1.0) * amount;

    // String Comb is detail over the average, which stays whole. It follows Amount up to 1 and
    // stops there; a comb has no caricature.
    design.comb = {sourceProfile != targetProfile ? std::clamp(values[kComb], 0.0, 1.0) * std::min(amount, 1.0) : 0.0,
                   sourceProfile, targetProfile};

    if (strength > 0.0 && sourceProfile != targetProfile)
    {
        const auto& from = kSensingFits[sourceProfile].bands;
        const auto& to = kSensingFits[targetProfile].bands;

        // Max Treble Boost holds here too: the top end the target's width adds over the source's.
        const double topBoostDb = strength * (to[2].gainDb - from[2].gainDb);
        const double maxBoostDb = std::max(0.0, values[kMaxBoost]);
        const double held = topBoostDb > maxBoostDb ? maxBoostDb / topBoostDb : 1.0;
        design.sections[3] = SensingSection(2, to[2], strength * held, sampleRate);
        design.sections[6] = SensingSection(2, from[2], -strength * held, sampleRate);

        for (std::size_t band = 0; band < 2; ++band)
        {
            design.sections[1 + band] = SensingSection(band, to[band], strength, sampleRate);
            design.sections[4 + band] = SensingSection(band, from[band], -strength, sampleRate);
        }
    }

    return design;
}
} // namespace pickup_match

class PickupMatchEffect : public EffectProcessor
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

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    PickupMatchEffect()
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
        for (auto& channel : mState)
        {
            for (auto& state : channel)
            {
                state.Reset();
            }
        }

        for (auto& section : mSections)
        {
            section.Finish();
        }

        mGain.Finish();
        mRampSamplesRemaining = 0;
        mTracker.Reset();
        mTrackedString = pickup_match::kFallbackString;
        mSeenDetections = 0;
        mComb.Clear();
        mComb.SetGoal(CombGoal());
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
            const bool comb = mComb.Engaged();

            if (comb)
            {
                const float left = inputs[0] ? inputs[0][i] : 0.0f;
                AdvanceComb(inputs[0] && inputs[1] ? 0.5f * (left + inputs[1][i]) : left);
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                if (inputs[ch] && outputs[ch])
                {
                    outputs[ch][i] = ProcessSample(inputs[ch][i], mState[ch], static_cast<std::size_t>(ch), comb);
                }
            }
        }
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

        if (!mEnabled)
        {
            std::copy_n(input, numSamples, output);
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            AdvanceRamp();
            const bool comb = mComb.Engaged();

            if (comb)
            {
                AdvanceComb(input[i]);
            }

            output[i] = ProcessSample(input[i], mState[0], 0, comb);
        }
    }

    /// May run on the audio thread (automation, MIDI): the redesign is a handful of arithmetic
    /// and allocates nothing.
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

        const std::size_t index = FindParamSpec(pickup_match::kParams, key);

        if (index == pickup_match::kParamCount)
        {
            return;
        }

        const double normalised = NormaliseParamValue(pickup_match::kParams[index], value);

        if (mValues[index] == normalised)
        {
            return;
        }

        mValues[index] = normalised;
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

        const std::size_t index = FindParamSpec(pickup_match::kParams, key);
        return index == pickup_match::kParamCount ? 0.0 : mValues[index];
    }

    [[nodiscard]] bool GetFrequencyResponse(std::span<const double> frequenciesHz,
                                            std::span<double> magnitudesDb) const override
    {
        if (frequenciesHz.size() != magnitudesDb.size())
        {
            return false;
        }

        const pickup_match::Design design = pickup_match::BuildDesign(mValues, mSampleRate);
        const double gainDb = 20.0 * std::log10(design.gain);

        for (std::size_t i = 0; i < frequenciesHz.size(); ++i)
        {
            double magnitude = 1.0;

            for (const auto& section : design.sections)
            {
                magnitude *= std::abs(biquad::Response(section, frequenciesHz[i], mSampleRate));
            }

            magnitudesDb[i] = 20.0 * std::log10(std::max(magnitude, 1.0e-12)) + gainDb;

            // The comb as one string gets it: the chosen string, or while following the pitch
            // the D, so the curve holds still.
            if (design.comb.strength > 0.0)
            {
                const std::size_t fixed = FixedString();
                const auto comb = pickup_match::CombValuesFor(
                    design.comb, fixed < pickup_match::kOpenStringHz.size() ? fixed : pickup_match::kFallbackString,
                    mSampleRate);
                magnitudesDb[i] += pickup_match::CombResponseDb(comb, frequenciesHz[i], mSampleRate);
            }
        }

        return true;
    }

    /// Each side's resonance, Q and level; for a custom design also what it is built as: its
    /// wire length, inductance, resistance and capacitance (pickup_match::DesignPickup).
    [[nodiscard]] std::vector<std::pair<std::string, double>> DescribeSettings() const override
    {
        std::vector<std::pair<std::string, double>> figures;

        for (const auto side : {pickup_match::kSource, pickup_match::kTarget})
        {
            const std::string prefix = side == pickup_match::kSource ? "source" : "target";
            const auto pickup = pickup_match::ResolvePickup(mValues, side);
            figures.emplace_back(prefix + "PeakHz", pickup.resonanceHz);
            figures.emplace_back(prefix + "Q", pickup.q);
            figures.emplace_back(prefix + "LevelDb", pickup.levelDb);

            if (pickup_match::PickupIndex(mValues[side]) == pickup_match::kCustomPickup)
            {
                const auto designed = pickup_match::DesignPickup(
                    pickup_match::DesignSpecFor(mValues, pickup_match::DesignParamsFor(side)));
                figures.emplace_back(prefix + "WireLengthM", designed.wireLengthM);
                figures.emplace_back(prefix + "InductanceH", designed.inductanceH);
                figures.emplace_back(prefix + "ResistanceKohm", designed.resistanceOhms / 1000.0);
                figures.emplace_back(prefix + "CapacitancePf", designed.capacitancePf);
            }
        }

        return figures;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "pickup_match";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "eq";
    }

  private:
    /// Coefficient and gain changes glide over this long.
    static constexpr double kRampSeconds = 0.015;

    void Configure(double sampleRate)
    {
        mSampleRate = sampleRate;
        mRampSamples = std::max(1, static_cast<int>(std::lround(sampleRate * kRampSeconds)));
        mComb.Prepare(sampleRate);
        mTracker.Prepare(sampleRate);
        UpdateDesign();
        Reset();
    }

    void UpdateDesign() noexcept
    {
        const pickup_match::Design design = pickup_match::BuildDesign(mValues, mSampleRate);

        for (std::size_t i = 0; i < pickup_match::kSectionCount; ++i)
        {
            mSections[i].target = design.sections[i];
            mSections[i].Begin(mRampSamples);
        }

        mGain.target = design.gain;
        mRampSamplesRemaining = mRampSamples;
        mGain.Begin(mRampSamples);

        // Coming on, the comb starts from the fallback string with nothing tracked.
        if (!mComb.Engaged() && design.comb.strength > 0.0)
        {
            mTracker.Reset();
            mTrackedString = pickup_match::kFallbackString;
            mSeenDetections = 0;
        }

        mCombSettings = design.comb;
        mComb.SetGoal(CombGoal());
    }

    /// The string Comb String fixes, or kOpenStringHz.size() while it follows the pitch.
    [[nodiscard]] std::size_t FixedString() const noexcept
    {
        const long choice = std::lround(mValues[pickup_match::kCombString]);
        return choice >= 1 ? static_cast<std::size_t>(choice - 1) : pickup_match::kOpenStringHz.size();
    }

    [[nodiscard]] pickup_match::CombValues CombGoal() const noexcept
    {
        const std::size_t fixed = FixedString();
        return pickup_match::CombValuesFor(
            mCombSettings, fixed < pickup_match::kOpenStringHz.size() ? fixed : mTrackedString, mSampleRate);
    }

    /// Once per sample while the comb runs: follows the pitch to its string, and glides the comb.
    void AdvanceComb(float monitor) noexcept
    {
        if (FixedString() == pickup_match::kOpenStringHz.size())
        {
            mTracker.Push(IsFinite(monitor) ? monitor : 0.0f);

            if (const auto detections = mTracker.DetectionCount(); detections != mSeenDetections)
            {
                mSeenDetections = detections;

                if (const double hz = mTracker.FrequencyHz(); hz > 0.0)
                {
                    const std::size_t string = pickup_match::StringForPitch(hz, mTrackedString);

                    if (string != mTrackedString)
                    {
                        mTrackedString = string;
                        mComb.SetGoal(CombGoal());
                    }
                }
            }
        }

        mComb.Advance();
    }

    void AdvanceRamp() noexcept
    {
        if (mRampSamplesRemaining <= 0)
        {
            return;
        }

        if (--mRampSamplesRemaining == 0)
        {
            for (auto& section : mSections)
            {
                section.Finish();
            }

            mGain.Finish();
            return;
        }

        for (auto& section : mSections)
        {
            section.Advance();
        }

        mGain.Advance();
    }

    using ChannelState = std::array<biquad::State, pickup_match::kSectionCount>;

    [[nodiscard]] float ProcessSample(float input, ChannelState& states, std::size_t channel, bool comb) noexcept
    {
        // A NaN or infinity would stay in the filters' memory, so it plays as silence.
        // IsFinite, because the fast-math builds fold std::isfinite away.
        double wet = IsFinite(input) ? static_cast<double>(input) : 0.0;

        for (std::size_t i = 0; i < pickup_match::kSectionCount; ++i)
        {
            wet = states[i].Process(mSections[i].current, wet);
        }

        if (comb)
        {
            wet = mComb.Process(channel, wet);
        }

        wet *= mGain.current;

        if (!IsFinite(wet))
        {
            for (auto& state : states)
            {
                state.Reset();
            }

            return 0.0f;
        }

        return static_cast<float>(wet);
    }

    pickup_match::Values mValues = pickup_match::kDefaultValues;
    std::array<biquad::Ramp, pickup_match::kSectionCount> mSections{};
    LinearRamp<double> mGain{1.0, 1.0, 0.0};
    std::array<ChannelState, 2> mState{};
    int mRampSamples = 1;
    int mRampSamplesRemaining = 0;

    pickup_match::StringComb mComb;
    pickup_match::CombSettings mCombSettings;
    PitchTracker mTracker; ///< fed only while the comb follows the pitch
    std::size_t mTrackedString = pickup_match::kFallbackString;
    std::uint64_t mSeenDetections = 0;
};

namespace pickup_match
{
[[nodiscard]] inline EffectPresetDefinition MakePreset(const char* id, const char* displayName, Pickup source,
                                                       Pickup target, bool isDefault = false)
{
    EffectPresetDefinition preset;
    preset.id = id;
    preset.displayName = displayName;
    preset.isDefault = isDefault;
    preset.parameters = {{"source", static_cast<double>(source)},
                         {"target", static_cast<double>(target)},
                         {"amount", 1.0},
                         {"levelMode", static_cast<double>(LevelMode::Pickup)},
                         {"maxBoost", kParams[kMaxBoost].defaultValue},
                         {"output", 0.0}};

    for (const auto& spec : kParams)
    {
        preset.parameterOrder.emplace_back(spec.id);
    }

    return preset;
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    return {
        MakePreset("single-to-humbucker", "Single Coil to Humbucker", Pickup::SingleCoilBridge, Pickup::HumbuckerBridge,
                   true),
        MakePreset("neck-single-to-humbucker", "Neck Single Coil to Humbucker", Pickup::SingleCoilNeck,
                   Pickup::HumbuckerNeck),
        MakePreset("humbucker-to-single", "Humbucker to Single Coil", Pickup::HumbuckerBridge,
                   Pickup::SingleCoilBridge),
        MakePreset("neck-humbucker-to-single", "Neck Humbucker to Single Coil", Pickup::HumbuckerNeck,
                   Pickup::SingleCoilNeck),
        MakePreset("single-to-p90", "Single Coil to P-90", Pickup::SingleCoilBridge, Pickup::P90),
        MakePreset("humbucker-to-p90", "Humbucker to P-90", Pickup::HumbuckerBridge, Pickup::P90),
    };
}

[[nodiscard]] inline const char* FamilyName(PickupFamily family) noexcept
{
    switch (family)
    {
    case PickupFamily::SingleCoil:
        return "Single Coil";
    case PickupFamily::P90:
        return "P-90";
    case PickupFamily::Humbucker:
        return "Humbucker";
    case PickupFamily::Active:
        return "Active";
    case PickupFamily::Custom:
        return "Custom";
    case PickupFamily::Other:
        break;
    }

    return "Other";
}

/// The picture the picker draws for a family; the UI knows these names (pickerIcons.ts).
[[nodiscard]] inline const char* FamilyIcon(PickupFamily family) noexcept
{
    switch (family)
    {
    case PickupFamily::SingleCoil:
        return "pickup-single";
    case PickupFamily::P90:
        return "pickup-p90";
    case PickupFamily::Humbucker:
        return "pickup-humbucker";
    case PickupFamily::Active:
        return "pickup-active";
    case PickupFamily::Custom:
        return "pickup-custom";
    case PickupFamily::Other:
        break;
    }

    return "pickup-other";
}

/// One line under a pickup's name: where it sits, its peak, and its output against a generic
/// neck single coil. A fit that misses the measurement by more than 1.5 dB says it is approximate.
[[nodiscard]] inline std::string DescribePickup(const PickupModel& pickup)
{
    if (pickup.family == PickupFamily::Custom)
    {
        return "Built from its type, wire, magnet and cover, under Design";
    }

    std::string detail;

    switch (pickup.position)
    {
    case PickupPosition::Neck:
        detail = "Neck · ";
        break;
    case PickupPosition::Middle:
        detail = "Middle · ";
        break;
    case PickupPosition::Bridge:
        detail = "Bridge · ";
        break;
    case PickupPosition::Any:
        break;
    }

    const double relativeDb = pickup.levelDb - kClasses[0].levelDb;
    char text[96];
    std::snprintf(text, sizeof(text), "peak %.1f kHz · output %+.0f dB%s", pickup.resonanceHz / 1000.0, relativeDb,
                  pickup.fitErrorDb > 1.5 ? " · approximate" : "");
    return detail + text;
}

/// The picker's view of every pickup, in index order.
[[nodiscard]] inline std::vector<ParamLabelInfo> PickupLabelInfo()
{
    std::vector<ParamLabelInfo> infos;
    infos.reserve(kPickups.size());

    for (const auto& pickup : kPickups)
    {
        infos.push_back({FamilyName(pickup.family), pickup.brand, FamilyIcon(pickup.family), DescribePickup(pickup)});
    }

    return infos;
}
} // namespace pickup_match

inline void RegisterPickupMatchEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kPickupMatch;
    info.aliases = {"pickup_match"};
    info.displayName = "Pickup Match";
    info.category = "eq";
    info.description = "Makes your pickup sound like another kind: a single coil like a humbucker, or a humbucker "
                       "like a single coil. Put it first, before any drive or amp, so they hear the new pickup.";
    info.requiresResource = false;
    info.presets = pickup_match::FactoryPresets();
    info.parameters = BuildParameterDefs(pickup_match::kParams);

    // 257 choices are too many for a knob: both sides open a picker grouped by family and brand.
    const auto labelInfo = pickup_match::PickupLabelInfo();

    for (auto index : {pickup_match::kSource, pickup_match::kTarget})
    {
        info.parameters[index].labelInfo = labelInfo;
        info.parameters[index].control = "picker";
    }

    // A design's type picks its shape, so it gets the pictures too.
    for (auto index : {pickup_match::kSourceDesignType, pickup_match::kTargetDesignType})
    {
        auto& def = info.parameters[index];
        def.control = "picker";

        for (const auto& type : pickup_match::kDesignTypes)
        {
            const auto family =
                type.humbucker ? pickup_match::PickupFamily::Humbucker : pickup_match::PickupFamily::SingleCoil;
            def.labelInfo.push_back(
                {pickup_match::FamilyName(family), "", pickup_match::FamilyIcon(family), type.detail});
        }
    }

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<PickupMatchEffect>(); });
}
} // namespace guitarfx
