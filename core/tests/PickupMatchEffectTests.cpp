/**
 * @file PickupMatchEffectTests.cpp
 * @brief Tests for Pickup Match.
 *
 * These measure what the effect claims to do:
 *   - registration, the parameter table and the factory presets
 *   - Amount 0, or the same pickup on both sides, passes audio through bit for bit
 *   - the response is the ratio of the two pickups' second-order responses, at DC and across
 *     the band, and the audio through the effect has the response it reports
 *   - single coil to humbucker and back again cancels out
 *   - Level follows the pickups' output difference, or holds it at Matched
 *   - Max Treble Boost caps the shelf going to a brighter pickup
 *   - the mono path matches the stereo one sample for sample
 *   - String Comb: the audio has the comb the curve reports, a notch where the string says, and
 *     Follow Pitch lands on the string a note is played on
 *   - parameter sweeps stay finite and bounded; NaN input recovers; low sample rates hold
 */

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectAnalysis.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/PickupMatchEffect.h"

namespace
{
using guitarfx::PickupMatchEffect;
using guitarfx::pickup_match::Pickup;
using Params = std::vector<std::pair<std::string, double>>;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (!condition)
    {
        ++gFailures;
    }

    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

/// The value that selects Custom Design.
const double kCustomChoice = static_cast<double>(guitarfx::pickup_match::kCustomPickup);

double P(Pickup pickup)
{
    return static_cast<double>(pickup);
}

std::unique_ptr<PickupMatchEffect> Make(const Params& params, double sampleRate = kSampleRate)
{
    auto effect = std::make_unique<PickupMatchEffect>();
    effect->Prepare(sampleRate, kBlock);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    // Land on the settings, not partway through the glide towards them.
    effect->Reset();
    return effect;
}

std::vector<float> Run(PickupMatchEffect& effect, const std::vector<float>& input)
{
    std::vector<float> left = input;
    std::vector<float> right = input;
    std::vector<float> outLeft(input.size());
    std::vector<float> outRight(input.size());

    for (std::size_t start = 0; start < input.size(); start += kBlock)
    {
        const int count = static_cast<int>(std::min<std::size_t>(kBlock, input.size() - start));
        float* in[2] = {left.data() + start, right.data() + start};
        float* out[2] = {outLeft.data() + start, outRight.data() + start};
        effect.Process(in, out, count);
    }

    return outLeft;
}

std::vector<float> Noise(std::size_t length, float amplitude = 0.25f, std::uint32_t seed = 7)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    std::vector<float> noise(length);

    for (auto& sample : noise)
    {
        sample = dist(rng);
    }

    return noise;
}

/// The analogue pickup's loaded response, 1 / (1 - x^2 + j x / Q).
std::complex<double> Analogue(const guitarfx::pickup_match::PickupModel& pickup, double frequencyHz)
{
    const double x = frequencyHz / pickup.resonanceHz;
    return 1.0 / std::complex<double>(1.0 - x * x, x / pickup.q);
}

double ResponseDb(const PickupMatchEffect& effect, double frequencyHz)
{
    const double frequencies[] = {frequencyHz};
    double magnitudes[] = {0.0};
    return effect.GetFrequencyResponse(frequencies, magnitudes) ? magnitudes[0] : 1.0e9;
}

/// The gain in dB a steady sine at `frequencyHz` comes through the effect with.
double MeasuredDb(PickupMatchEffect& effect, double frequencyHz)
{
    const int settle = static_cast<int>(kSampleRate * 0.2);
    const int measure = static_cast<int>(kSampleRate * 0.5);
    std::vector<float> sine(settle + measure);

    for (std::size_t i = 0; i < sine.size(); ++i)
    {
        sine[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * frequencyHz * i / kSampleRate));
    }

    effect.Reset();
    const auto out = Run(effect, sine);
    double in = 0.0;
    double wet = 0.0;

    for (int i = settle; i < settle + measure; ++i)
    {
        in += static_cast<double>(sine[i]) * sine[i];
        wet += static_cast<double>(out[i]) * out[i];
    }

    return 10.0 * std::log10(wet / in);
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kPickupMatch);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("pickup_match") == guitarfx::EffectGuids::kPickupMatch, "the alias resolves to it");
    Check(registry.Create("pickup_match")->GetType() == "pickup_match", "the registry creates one from the alias");

    if (!info)
    {
        return;
    }

    Check(info->category == "eq", "an EQ effect");
    bool ordered = info->parameters.size() == guitarfx::pickup_match::kParamCount;

    for (std::size_t i = 0; ordered && i < info->parameters.size(); ++i)
    {
        ordered = info->parameters[i].id == guitarfx::pickup_match::kParams[i].id;
    }

    Check(ordered, "parameters registered in table order");
    Check(info->parameters[guitarfx::pickup_match::kSource].labels.size() == guitarfx::pickup_match::kPickupCount,
          "every class and measured model is a Your Pickup choice");

    for (auto index : {guitarfx::pickup_match::kSource, guitarfx::pickup_match::kTarget})
    {
        const auto& def = info->parameters[index];
        const auto json = guitarfx::controller_detail::SerializeEffectParameter(def);
        const bool described = def.control == "picker" && def.labelInfo.size() == def.labels.size() &&
                               json.value("control", std::string{}) == "picker" && json.contains("labelInfo") &&
                               json["labelInfo"].size() == def.labels.size();
        Check(described, def.id + " is a picker, with a group, brand, picture and detail per pickup");
    }

    const auto& info0 = info->parameters[guitarfx::pickup_match::kSource].labelInfo;
    Check(!info0.empty() && info0.front().group == "Single Coil" && info0.front().subgroup == "Generic" &&
              info0.front().icon == "pickup-single" && info0.front().detail.find("peak 3.6 kHz") != std::string::npos,
          "the first pickup reads as a generic neck single coil peaking at 3.6 kHz",
          info0.empty() ? "" : info0.front().detail);

    int defaults = 0;
    bool inRange = true;

    for (const auto& preset : info->presets)
    {
        defaults += preset.isDefault ? 1 : 0;

        for (const auto& [key, value] : preset.parameters)
        {
            const std::size_t index = guitarfx::FindParamSpec(guitarfx::pickup_match::kParams, key);
            inRange = inRange && index < guitarfx::pickup_match::kParamCount &&
                      value >= guitarfx::pickup_match::kParams[index].minValue &&
                      value <= guitarfx::pickup_match::kParams[index].maxValue;
        }
    }

    Check(defaults == 1, "exactly one default preset");
    Check(inRange, "every preset value is a known parameter, in range");

    PickupMatchEffect effect;
    effect.SetParam("amount", 7.0);
    effect.SetParam("source", 9999.0);
    Check(effect.GetParam("amount") == guitarfx::pickup_match::kMaxAmount &&
              effect.GetParam("source") == guitarfx::pickup_match::kLastPickup,
          "out-of-range values clamp");
}

void TestCatalogue()
{
    std::cout << "\nCatalogue" << std::endl;
    using guitarfx::pickup_match::kPickups;
    std::set<std::string> ids;
    std::set<std::string> labels;
    bool sane = true;

    for (const auto& pickup : kPickups)
    {
        ids.insert(pickup.id);
        labels.insert(pickup.label);
        sane = sane && pickup.resonanceHz > 500.0 && pickup.resonanceHz < 20000.0 && pickup.q > 0.1 &&
               pickup.q < 10.0 && std::abs(pickup.levelDb) < 30.0 && pickup.fitErrorDb >= 0.0;
    }

    Check(ids.size() == kPickups.size() && labels.size() == kPickups.size(), "ids and labels are unique",
          std::to_string(kPickups.size()) + " pickups");
    Check(sane, "every pickup's resonance, Q and level are plausible");

    // Presets store the index: the classes and the first measured models must stay put.
    Check(std::string(kPickups[0].id) == "single-coil-neck" && std::string(kPickups[6].id) == "active-humbucker" &&
              std::string(kPickups[7].id) == "custom-design" &&
              std::string(kPickups[8].id) == "ampeg-amg100bld-switch-position-1",
          "the classes, then Custom Design, then the measured models in their pinned order");
    Check(guitarfx::pickup_match::kCustomPickup == 7, "Custom Design is index 7");

    // Every measured pickup into every class and back must be a stable, finite filter.
    bool stable = true;
    const auto noise = Noise(2048);

    for (std::size_t i = guitarfx::pickup_match::kClasses.size(); i < kPickups.size(); ++i)
    {
        for (Pickup other : {Pickup::SingleCoilNeck, Pickup::HumbuckerBridge, Pickup::ActiveHumbucker, Pickup::P90})
        {
            for (bool forward : {true, false})
            {
                auto effect = Make({{"source", forward ? static_cast<double>(i) : P(other)},
                                    {"target", forward ? P(other) : static_cast<double>(i)},
                                    {"maxBoost", 24.0}});
                const auto out = Run(*effect, noise);
                stable = stable && std::all_of(out.begin(), out.end(),
                                               [](float s) { return guitarfx::IsFinite(s) && std::abs(s) < 40.0f; });
            }
        }
    }

    Check(stable, "every measured pickup converts to and from the classes, finite and bounded");
}

void TestIdentity()
{
    std::cout << "\nIdentity" << std::endl;
    const auto noise = Noise(8192);

    auto zero = Make({{"amount", 0.0}});
    Check(Run(*zero, noise) == noise, "Amount 0 passes audio through bit for bit");

    auto same = Make({{"source", P(Pickup::HumbuckerNeck)}, {"target", P(Pickup::HumbuckerNeck)}});
    Check(Run(*same, noise) == noise, "the same pickup on both sides passes audio through bit for bit");
}

void TestResponse()
{
    std::cout << "\nResponse" << std::endl;
    using guitarfx::pickup_match::kClasses;
    const auto& single = kClasses[static_cast<std::size_t>(Pickup::SingleCoilBridge)];
    const auto& humbucker = kClasses[static_cast<std::size_t>(Pickup::HumbuckerBridge)];

    auto effect =
        Make({{"sensing", 0.0}, {"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::HumbuckerBridge)}});
    const double expectedDc = humbucker.levelDb - single.levelDb;
    Check(std::abs(ResponseDb(*effect, 20.0) - expectedDc) < 0.05, "single coil to humbucker gains the level gap",
          std::to_string(ResponseDb(*effect, 20.0)) + " dB, expected " + std::to_string(expectedDc));

    // The ratio of the two analogue responses. The bilinear transform bends it near Nyquist,
    // so compare across the band a guitar pickup speaks in.
    double worst = 0.0;

    for (double f = 50.0; f <= 10000.0; f *= 1.1)
    {
        const double expected = 20.0 * std::log10(std::abs(Analogue(humbucker, f) / Analogue(single, f))) + expectedDc;
        worst = std::max(worst, std::abs(ResponseDb(*effect, f) - expected));
    }

    Check(worst < 0.5, "follows the analogue ratio to 10 kHz", "worst " + std::to_string(worst) + " dB");
    Check(ResponseDb(*effect, 3217.0) < ResponseDb(*effect, 200.0) - 3.0,
          "the single coil's peak is taken down relative to the lows");

    double worstMeasured = 0.0;

    for (double f : {100.0, 1000.0, 2343.0, 3217.0, 6000.0, 12000.0})
    {
        worstMeasured = std::max(worstMeasured, std::abs(MeasuredDb(*effect, f) - ResponseDb(*effect, f)));
    }

    Check(worstMeasured < 0.1, "the audio has the response it reports",
          "worst " + std::to_string(worstMeasured) + " dB; at 3217 Hz " + std::to_string(MeasuredDb(*effect, 3217.0)) +
              " dB measured, " + std::to_string(ResponseDb(*effect, 3217.0)) + " reported");
}

/// The response at `hz` relative to 1 kHz: the shape, with the level taken out.
double ShapeDb(const PickupMatchEffect& effect, double hz)
{
    return ResponseDb(effect, hz) - ResponseDb(effect, 1000.0);
}

void TestSensing()
{
    std::cout << "\nString sensing" << std::endl;
    const double matched = static_cast<double>(guitarfx::pickup_match::LevelMode::Matched);

    // Bridge to neck on the same kind of pickup: the neck hears the low harmonics far more.
    auto neck =
        Make({{"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::SingleCoilNeck)}, {"levelMode", matched}});
    auto neckElectrical = Make({{"source", P(Pickup::SingleCoilBridge)},
                                {"target", P(Pickup::SingleCoilNeck)},
                                {"levelMode", matched},
                                {"sensing", 0.0}});
    Check(ShapeDb(*neck, 100.0) > 6.0 && std::abs(ShapeDb(*neckElectrical, 100.0)) < 1.5,
          "bridge to neck fills out the lows, which the electrics alone barely touch",
          std::to_string(ShapeDb(*neck, 100.0)) + " vs " + std::to_string(ShapeDb(*neckElectrical, 100.0)) +
              " dB at 100 Hz");

    // The same pickup moved to the neck by hand: sensing only.
    auto moved = Make({{"source", P(Pickup::SingleCoilBridge)},
                       {"target", P(Pickup::SingleCoilBridge)},
                       {"targetPosition", static_cast<double>(guitarfx::pickup_match::PositionChoice::Neck)}});
    Check(ShapeDb(*moved, 100.0) > 6.0 && std::abs(ResponseDb(*moved, 1000.0)) < 1.0,
          "Sound Like Position moves the same pickup to the neck",
          std::to_string(ShapeDb(*moved, 100.0)) + " dB at 100 Hz");

    // Single coil to humbucker at the neck: the two coils take the top down beyond the electrics.
    auto hum =
        Make({{"source", P(Pickup::SingleCoilNeck)}, {"target", P(Pickup::HumbuckerNeck)}, {"levelMode", matched}});
    auto humElectrical = Make({{"source", P(Pickup::SingleCoilNeck)},
                               {"target", P(Pickup::HumbuckerNeck)},
                               {"levelMode", matched},
                               {"sensing", 0.0}});
    Check(ShapeDb(*hum, 6300.0) < ShapeDb(*humElectrical, 6300.0) - 2.5,
          "a humbucker's two coils take the top down beyond its electrics",
          std::to_string(ShapeDb(*hum, 6300.0)) + " vs " + std::to_string(ShapeDb(*humElectrical, 6300.0)) +
              " dB at 6.3 kHz");

    // Strength scales the sensing part by dB.
    auto half = Make({{"source", P(Pickup::SingleCoilBridge)},
                      {"target", P(Pickup::SingleCoilBridge)},
                      {"targetPosition", static_cast<double>(guitarfx::pickup_match::PositionChoice::Neck)},
                      {"sensing", 0.5}});
    Check(std::abs(ShapeDb(*half, 100.0) - 0.5 * ShapeDb(*moved, 100.0)) < 0.3,
          "String Sensing at half moves half the dB");

    // Undoing the source is exact: there and back again is flat, sensing and all.
    auto there = Make({{"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::HumbuckerNeck)}});
    auto back = Make({{"source", P(Pickup::HumbuckerNeck)}, {"target", P(Pickup::SingleCoilBridge)}});
    double worst = 0.0;

    for (double f = 30.0; f <= 20000.0; f *= 1.2)
    {
        worst = std::max(worst, std::abs(ResponseDb(*there, f) + ResponseDb(*back, f)));
    }

    Check(worst < 1.0e-6, "bridge single coil to neck humbucker and back is flat",
          "worst " + std::to_string(worst) + " dB");

    // Max Treble Boost holds the top a narrower pickup adds over a wider one.
    auto capped = Make({{"source", P(Pickup::HumbuckerBridge)},
                        {"target", P(Pickup::HumbuckerBridge)},
                        {"targetPosition", static_cast<double>(guitarfx::pickup_match::PositionChoice::Bridge)},
                        {"sourcePosition", static_cast<double>(guitarfx::pickup_match::PositionChoice::Bridge)},
                        {"maxBoost", 0.0}});
    auto widthUp =
        Make({{"source", P(Pickup::HumbuckerBridge)}, {"target", P(Pickup::SingleCoilBridge)}, {"maxBoost", 0.0}});
    auto widthUpOpen =
        Make({{"source", P(Pickup::HumbuckerBridge)}, {"target", P(Pickup::SingleCoilBridge)}, {"maxBoost", 24.0}});
    Check(ResponseDb(*widthUp, 10000.0) < ResponseDb(*widthUpOpen, 10000.0) - 2.0 &&
              std::abs(ResponseDb(*capped, 10000.0)) < 1.0e-9,
          "Max Treble Boost holds the top a single coil's narrower window adds",
          std::to_string(ResponseDb(*widthUp, 10000.0)) + " vs " + std::to_string(ResponseDb(*widthUpOpen, 10000.0)) +
              " dB at 10 kHz");

    // A design is sensed where its type sits: a custom PAF like the generic bridge humbucker.
    auto designed = Make({{"target", kCustomChoice}, {"targetDesignType", 2.0}});
    auto generic = Make({{"target", P(Pickup::HumbuckerBridge)}});
    Check(std::abs(ShapeDb(*designed, 100.0) - ShapeDb(*generic, 100.0)) < 1.5,
          "a custom humbucker is sensed at the bridge, like the generic one");
}

/// A plucked-string stand-in: a sawtooth's first harmonics, so the tracker has a clear pitch.
std::vector<float> Tone(double frequencyHz, double seconds)
{
    std::vector<float> tone(static_cast<std::size_t>(seconds * kSampleRate));

    for (std::size_t i = 0; i < tone.size(); ++i)
    {
        double sample = 0.0;

        for (int harmonic = 1; harmonic <= 8; ++harmonic)
        {
            sample += std::sin(2.0 * kPi * frequencyHz * harmonic * i / kSampleRate) / harmonic;
        }

        tone[i] = static_cast<float>(0.1 * sample);
    }

    return tone;
}

void TestStringComb()
{
    std::cout << "\nString Comb" << std::endl;
    using guitarfx::pickup_match::StringForPitch;
    const double neck = static_cast<double>(guitarfx::pickup_match::PositionChoice::Neck);
    const double matched = static_cast<double>(guitarfx::pickup_match::LevelMode::Matched);
    // A bridge single coil moved to the neck: no electrical change, so the comb is all there is.
    const Params moved = {{"source", P(Pickup::SingleCoilBridge)},
                          {"target", P(Pickup::SingleCoilBridge)},
                          {"targetPosition", neck},
                          {"levelMode", matched},
                          {"comb", 1.0}};
    const auto with = [&](double string) {
        Params params = moved;
        params.emplace_back("combString", string);
        return Make(params);
    };

    Check(StringForPitch(82.41, 2) == 0 && StringForPitch(110.0, 0) == 1 && StringForPitch(150.0, 0) == 2 &&
              StringForPitch(200.0, 0) == 3 && StringForPitch(250.0, 0) == 4 && StringForPitch(330.0, 0) == 5 &&
              StringForPitch(1200.0, 0) == 5 && StringForPitch(60.0, 3) == 0,
          "a pitch goes to the highest string whose open note is at or below it");
    Check(StringForPitch(104.0, 1) == 1 && StringForPitch(104.0, 0) == 0 && StringForPitch(98.0, 1) == 0,
          "a note just under an open string stays on the string it was on");

    // The low E under a neck single coil (160 mm of 648): notches every 648 / 160 x 82.41 Hz.
    auto lowE = with(1.0);
    const double notchHz = 648.0 / 160.0 * 82.41;
    Check(ResponseDb(*lowE, notchHz) < ResponseDb(*lowE, 0.5 * notchHz) - 8.0 &&
              ResponseDb(*lowE, 2.0 * notchHz) < ResponseDb(*lowE, 1.5 * notchHz) - 8.0,
          "the neck's notches sit where the low E's half-waves fit",
          std::to_string(ResponseDb(*lowE, notchHz)) + " dB at the notch, " +
              std::to_string(ResponseDb(*lowE, 0.5 * notchHz)) + " dB between");

    double worst = 0.0;

    for (double f : {90.0, 167.0, 250.0, 334.0, 500.0, 900.0, 1700.0, 3000.0})
    {
        worst = std::max(worst, std::abs(MeasuredDb(*lowE, f) - ResponseDb(*lowE, f)));
    }

    Check(worst < 0.5, "the audio has the comb the curve reports", "worst " + std::to_string(worst) + " dB");

    // On the high E the same move puts the first notch four times higher. The comb alone is
    // what the curve has over the same settings without it.
    auto highE = with(6.0);
    Params plain = moved;
    plain.back().second = 0.0;
    auto flat = Make(plain);
    const auto combDb = [&](auto& effect, double f) { return ResponseDb(effect, f) - ResponseDb(*flat, f); };
    Check(std::abs(combDb(*highE, 4.0 * notchHz) - combDb(*lowE, notchHz)) < 0.5, "each string has its own comb",
          std::to_string(combDb(*highE, 4.0 * notchHz)) + " vs " + std::to_string(combDb(*lowE, notchHz)) + " dB");

    // The notches do not run to the top of the band: far above the first few, the comb is gone.
    double ripple = 0.0;

    for (double f = 8000.0; f < 12000.0; f += 20.0)
    {
        ripple = std::max(ripple, std::abs(combDb(*lowE, f) - combDb(*lowE, 8000.0)));
    }

    Check(ripple < 1.5 && combDb(*lowE, 20.0) > -2.5,
          "the comb fades out above its first notches and below its first peak",
          std::to_string(ripple) + " dB of ripple at 8-12 kHz, " + std::to_string(combDb(*lowE, 20.0)) +
              " dB at 20 Hz");

    // Two pickups in nearly the same place (a bridge single coil and a bridge humbucker) have
    // nearly the same comb, so there is little to add.
    auto near = Make({{"source", P(Pickup::SingleCoilBridge)},
                      {"target", P(Pickup::HumbuckerBridge)},
                      {"levelMode", matched},
                      {"comb", 1.0},
                      {"combString", 1.0}});
    auto average =
        Make({{"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::HumbuckerBridge)}, {"levelMode", matched}});
    double most = 0.0;

    for (double f = 60.0; f < 12000.0; f *= 1.03)
    {
        most = std::max(most, std::abs(ResponseDb(*near, f) - ResponseDb(*average, f)));
    }

    Check(most > 0.2 && most < 2.0, "pickups in nearly the same place get only a shallow comb",
          std::to_string(most) + " dB at most");

    // The same pickup in the same place: nothing to do, comb or not.
    auto same = Make({{"source", P(Pickup::HumbuckerBridge)}, {"target", P(Pickup::HumbuckerBridge)}, {"comb", 1.0}});
    const auto noise = Noise(4096);
    Check(Run(*same, noise) == noise, "the same pickup on both sides still passes audio bit for bit");

    // Follow Pitch: a note on the A string comes out as it does with the A string chosen.
    const auto note = Tone(123.47, 0.6); // B2, the A string's second fret
    auto follow = with(0.0);
    auto fixedA = with(2.0);
    const auto followed = Run(*follow, note);
    const auto fixed = Run(*fixedA, note);
    double difference = 0.0;
    double level = 0.0;

    for (std::size_t i = note.size() / 2; i < note.size(); ++i)
    {
        difference += std::pow(static_cast<double>(followed[i]) - fixed[i], 2.0);
        level += static_cast<double>(fixed[i]) * fixed[i];
    }

    Check(difference < 1.0e-4 * level, "Follow Pitch settles on the string the note is played on",
          std::to_string(10.0 * std::log10(std::max(difference, 1.0e-30) / level)) + " dB apart");

    // Random comb settings every block, on a note that keeps changing string.
    auto swept = with(0.0);
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    bool bounded = true;

    for (int block = 0; block < 300; ++block)
    {
        swept->SetParam("comb", unit(rng));
        swept->SetParam("combString", std::floor(unit(rng) * 7.0));
        swept->SetParam("source", std::floor(unit(rng) * 7.0));
        swept->SetParam("target", std::floor(unit(rng) * 7.0));
        const auto out = Run(*swept, Tone(82.41 * std::pow(2.0, std::floor(unit(rng) * 36.0) / 12.0), 0.02));
        bounded = bounded && std::all_of(out.begin(), out.end(),
                                         [](float s) { return guitarfx::IsFinite(s) && std::abs(s) < 20.0f; });
    }

    Check(bounded, "random comb settings every block stay finite and bounded");

    for (double rate : {22050.0, 44100.0, 192000.0})
    {
        Params params = moved;
        params.emplace_back("combString", 6.0);
        auto effect = Make(params, rate);
        const auto out = Run(*effect, Noise(8192));
        Check(std::all_of(out.begin(), out.end(), [](float s) { return guitarfx::IsFinite(s) && std::abs(s) < 20.0f; }),
              "the comb is stable at " + std::to_string(static_cast<int>(rate)) + " Hz");
    }
}

void TestExaggerate()
{
    std::cout << "\nAmount past 1" << std::endl;
    const double matched = static_cast<double>(guitarfx::pickup_match::LevelMode::Matched);
    const auto at = [&](double amount) {
        return Make({{"source", P(Pickup::SingleCoilBridge)},
                     {"target", P(Pickup::HumbuckerBridge)},
                     {"levelMode", matched},
                     {"amount", amount}});
    };
    const auto one = at(1.0);
    const auto two = at(2.0);
    const auto three = at(3.0);

    // Single coil to humbucker darkens: further at 2, further still at 3.
    Check(ShapeDb(*two, 3000.0) < ShapeDb(*one, 3000.0) - 2.0 && ShapeDb(*three, 3000.0) < ShapeDb(*two, 3000.0) - 2.0,
          "the humbucker's darkening grows past Amount 1",
          std::to_string(ShapeDb(*one, 3000.0)) + " / " + std::to_string(ShapeDb(*two, 3000.0)) + " / " +
              std::to_string(ShapeDb(*three, 3000.0)) + " dB at 3 kHz");

    // String sensing scales the same way: bridge to neck fills out twice as much at 2.
    const auto neck = [&](double amount) {
        return Make({{"source", P(Pickup::SingleCoilBridge)},
                     {"target", P(Pickup::SingleCoilBridge)},
                     {"targetPosition", static_cast<double>(guitarfx::pickup_match::PositionChoice::Neck)},
                     {"amount", amount}});
    };
    const auto neckOne = neck(1.0);
    const auto neckTwo = neck(2.0);
    // Close to double: each band stops at 18 dB, which the bridge's low shelf (-9.2 dB) just passes.
    Check(ShapeDb(*neckTwo, 100.0) > 1.85 * ShapeDb(*neckOne, 100.0) &&
              ShapeDb(*neckTwo, 100.0) <= 2.0 * ShapeDb(*neckOne, 100.0) + 0.1,
          "String Sensing about doubles at Amount 2, each band held to 18 dB",
          std::to_string(ShapeDb(*neckOne, 100.0)) + " -> " + std::to_string(ShapeDb(*neckTwo, 100.0)) +
              " dB at 100 Hz");

    // The level gap scales too, but no further than 18 dB (or the pickups' own gap).
    const auto louder =
        Make({{"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::HumbuckerBridge)}, {"amount", 3.0}});
    Check(std::abs(ResponseDb(*louder, 20.0) - 18.0) < 0.6, "Pickup Output's gap stops at 18 dB",
          std::to_string(ResponseDb(*louder, 20.0)) + " dB");

    // Up to Amount 1 nothing is held back: the measured models' widest gap still comes through.
    using guitarfx::pickup_match::kPickups;
    std::size_t quietest = 0;
    std::size_t loudest = 0;

    for (std::size_t i = 0; i < kPickups.size(); ++i)
    {
        quietest = kPickups[i].levelDb < kPickups[quietest].levelDb ? i : quietest;
        loudest = kPickups[i].levelDb > kPickups[loudest].levelDb ? i : loudest;
    }

    auto widest =
        Make({{"source", static_cast<double>(quietest)}, {"target", static_cast<double>(loudest)}, {"sensing", 0.0}});
    const double gap = kPickups[loudest].levelDb - kPickups[quietest].levelDb;
    Check(gap > 18.0 && std::abs(ResponseDb(*widest, 20.0) - gap) < 0.5, "Amount 1 keeps a gap wider than 18 dB",
          std::to_string(gap) + " dB");

    // Pushed to 3 between the most different pickups, every pairing stays a finite, bounded filter.
    bool stable = true;
    const auto noise = Noise(4096);

    for (Pickup from : {Pickup::P90, Pickup::ActiveHumbucker, Pickup::SingleCoilNeck, Pickup::HumbuckerBridge})
    {
        for (Pickup to : {Pickup::P90, Pickup::ActiveHumbucker, Pickup::SingleCoilNeck, Pickup::HumbuckerBridge})
        {
            for (double rate : {8000.0, 48000.0})
            {
                auto effect = Make({{"source", P(from)}, {"target", P(to)}, {"amount", 3.0}, {"maxBoost", 24.0}}, rate);
                const auto out = Run(*effect, noise);
                stable = stable && std::all_of(out.begin(), out.end(),
                                               [](float s) { return guitarfx::IsFinite(s) && std::abs(s) < 40.0f; });
            }
        }
    }

    Check(stable, "Amount 3 stays finite and bounded between the most different pickups, at 8 and 48 kHz");
}

void TestRoundTrip()
{
    std::cout << "\nRound trip" << std::endl;
    auto there = Make({{"source", P(Pickup::SingleCoilNeck)}, {"target", P(Pickup::HumbuckerNeck)}});
    auto back = Make({{"source", P(Pickup::HumbuckerNeck)}, {"target", P(Pickup::SingleCoilNeck)}});

    double worst = 0.0;

    for (double f = 30.0; f <= 20000.0; f *= 1.2)
    {
        worst = std::max(worst, std::abs(ResponseDb(*there, f) + ResponseDb(*back, f)));
    }

    Check(worst < 1.0e-6, "single coil to humbucker and back is flat", "worst " + std::to_string(worst) + " dB");

    const auto noise = Noise(16384);
    const auto out = Run(*back, Run(*there, noise));
    double error = 0.0;

    for (std::size_t i = 0; i < noise.size(); ++i)
    {
        error = std::max(error, static_cast<double>(std::abs(out[i] - noise[i])));
    }

    Check(error < 1.0e-4, "and the audio comes back", "max error " + std::to_string(error));
}

void TestLevelMode()
{
    std::cout << "\nLevel" << std::endl;
    auto matched = Make({{"sensing", 0.0},
                         {"source", P(Pickup::SingleCoilBridge)},
                         {"target", P(Pickup::HumbuckerBridge)},
                         {"levelMode", static_cast<double>(guitarfx::pickup_match::LevelMode::Matched)}});
    Check(std::abs(ResponseDb(*matched, 20.0)) < 0.05, "Matched holds the lows where they were");

    auto half = Make({{"sensing", 0.0},
                      {"source", P(Pickup::SingleCoilBridge)},
                      {"target", P(Pickup::HumbuckerBridge)},
                      {"amount", 0.5}});
    auto full =
        Make({{"sensing", 0.0}, {"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::HumbuckerBridge)}});
    Check(std::abs(ResponseDb(*half, 20.0) - 0.5 * ResponseDb(*full, 20.0)) < 0.05,
          "Amount moves the level by dB, half way at 0.5");

    auto trimmed = Make({{"amount", 0.0}, {"output", -6.0}});
    Check(std::abs(ResponseDb(*trimmed, 1000.0) + 6.0) < 1.0e-9, "Output trims on its own");
}

void TestMaxBoost()
{
    std::cout << "\nMax Treble Boost" << std::endl;

    for (double cap : {0.0, 6.0, 12.0})
    {
        auto effect = Make({{"sensing", 0.0},
                            {"source", P(Pickup::P90)},
                            {"target", P(Pickup::ActiveHumbucker)},
                            {"levelMode", static_cast<double>(guitarfx::pickup_match::LevelMode::Matched)},
                            {"maxBoost", cap}});
        const double top = ResponseDb(*effect, 16000.0);
        Check(top <= cap + 1.0,
              "P-90 to active holds the top to about " + std::to_string(static_cast<int>(cap)) + " dB",
              std::to_string(top) + " dB at 16 kHz");
    }

    auto open = Make({{"sensing", 0.0},
                      {"source", P(Pickup::P90)},
                      {"target", P(Pickup::ActiveHumbucker)},
                      {"levelMode", static_cast<double>(guitarfx::pickup_match::LevelMode::Matched)},
                      {"maxBoost", 24.0}});
    Check(ResponseDb(*open, 16000.0) > 15.0, "a wide cap lets the full boost through");

    auto down = Make({{"sensing", 0.0},
                      {"source", P(Pickup::SingleCoilBridge)},
                      {"target", P(Pickup::HumbuckerBridge)},
                      {"maxBoost", 0.0}});
    auto downOpen =
        Make({{"sensing", 0.0}, {"source", P(Pickup::SingleCoilBridge)}, {"target", P(Pickup::HumbuckerBridge)}});
    Check(std::abs(ResponseDb(*down, 8000.0) - ResponseDb(*downOpen, 8000.0)) < 1.0e-9,
          "it does nothing going to a darker pickup");
}

void TestMono()
{
    std::cout << "\nMono path" << std::endl;
    const Params params = {{"source", P(Pickup::HumbuckerBridge)}, {"target", P(Pickup::SingleCoilMiddle)}};
    auto stereo = Make(params);
    auto mono = Make(params);
    auto noise = Noise(4096);
    const auto expected = Run(*stereo, noise);
    std::vector<float> out(noise.size());

    for (std::size_t start = 0; start < noise.size(); start += kBlock)
    {
        mono->ProcessMono(noise.data() + start, out.data() + start, kBlock);
    }

    Check(out == expected, "mono matches stereo sample for sample");
}

guitarfx::pickup_match::DesignResult Designed(guitarfx::pickup_match::DesignSpec spec)
{
    return guitarfx::pickup_match::DesignPickup(spec);
}

double Semitones(double a, double b)
{
    return 12.0 * std::log2(a / b);
}

void TestDesign()
{
    std::cout << "\nCustom design" << std::endl;
    using namespace guitarfx::pickup_match;
    using guitarfx::pickup_match::kClasses;

    // A median design of each type, built from its measured median wire length at 42 AWG.
    const auto median = [](DesignType type) {
        DesignSpec spec;
        spec.type = type;
        spec.resistanceOhms = kDesignTypes[static_cast<std::size_t>(type)].medianWireLengthM * OhmsPerMetre(42);
        return spec;
    };
    const auto strat = Designed(median(DesignType::StratSingleCoil));
    const auto tele = Designed(median(DesignType::TeleSingleCoil));
    const auto paf = Designed(median(DesignType::PafHumbucker));
    const double humbuckerHz = std::sqrt(kClasses[4].resonanceHz * kClasses[5].resonanceHz);

    // The calibration: a median design lands on the catalogue's matching generic class.
    Check(std::abs(Semitones(strat.resonanceHz, kClasses[0].resonanceHz)) < 1.0 &&
              std::abs(Semitones(tele.resonanceHz, kClasses[2].resonanceHz)) < 1.0 &&
              std::abs(Semitones(paf.resonanceHz, humbuckerHz)) < 1.0,
          "median designs land within a semitone of the generic classes",
          std::to_string(strat.resonanceHz) + " / " + std::to_string(tele.resonanceHz) + " / " +
              std::to_string(paf.resonanceHz) + " Hz");
    Check(std::abs(strat.q / kClasses[0].q - 1.0) < 0.15 && std::abs(paf.q / kClasses[5].q - 1.0) < 0.15,
          "and their Q within 15%", std::to_string(strat.q) + " / " + std::to_string(paf.q));
    Check(std::abs(strat.levelDb - kDesignTypes[0].levelDb) < 1.0e-9 &&
              std::abs(paf.levelDb - kDesignTypes[2].levelDb) < 1.0e-9,
          "and at their types' calibrated level");
    // The measured medians: 2.68 H, 2.75 H and 4.84 H. These are medians over every pickup of the
    // type, many with no gauge recorded, while the design is the fit at the median wire length of
    // those with one, so they differ by a few percent (PAF 8%). A transcription slip in the
    // coefficients would be far off. The fit's own accuracy is the back-test's 7.5% (PickupDesign.h).
    Check(std::abs(strat.inductanceH / 2.677 - 1.0) < 0.1 && std::abs(tele.inductanceH / 2.75 - 1.0) < 0.1 &&
              std::abs(paf.inductanceH / 4.835 - 1.0) < 0.1,
          "median inductances within 10% of the measured medians",
          std::to_string(strat.inductanceH) + " / " + std::to_string(tele.inductanceH) + " / " +
              std::to_string(paf.inductanceH) + " H");

    DesignSpec base = median(DesignType::StratSingleCoil);
    const auto with = [&](auto change) {
        DesignSpec spec = base;
        change(spec);
        return Designed(spec);
    };
    const auto hotter = with([](DesignSpec& s) { s.resistanceOhms *= 1.4; });
    Check(hotter.resonanceHz < strat.resonanceHz && hotter.levelDb > strat.levelDb + 2.0,
          "more wire: a lower peak and more output");
    const auto thinner = with([](DesignSpec& s) { s.gauge = 43; });
    Check(thinner.wireLengthM < strat.wireLengthM && thinner.resonanceHz > strat.resonanceHz &&
              thinner.levelDb < strat.levelDb,
          "the same resistance in thinner wire: less wire, a higher peak and less output");
    const auto ceramic = with([](DesignSpec& s) { s.magnet = DesignMagnet::Ceramic; });
    const auto alnico2 = with([](DesignSpec& s) { s.magnet = DesignMagnet::Alnico2; });
    Check(ceramic.inductanceH > alnico2.inductanceH && alnico2.inductanceH > strat.inductanceH &&
              ceramic.resonanceHz < strat.resonanceHz,
          "ceramic raises inductance most, then A2, over A5");
    const auto brass = with([](DesignSpec& s) { s.cover = DesignCover::Brass; });
    const auto nickel = with([](DesignSpec& s) { s.cover = DesignCover::NickelSilver; });
    Check(brass.q < strat.q * 0.7 && nickel.q == strat.q, "a brass cover damps the peak; nickel silver does not",
          std::to_string(brass.q) + " vs " + std::to_string(strat.q));
    const auto moreC = with([](DesignSpec& s) { s.capacitanceTrim = 0.5; });
    Check(moreC.resonanceHz < strat.resonanceHz && moreC.capacitancePf > strat.capacitancePf,
          "more winding capacitance lowers the peak");
    const auto wired = with([](DesignSpec& s) { s.wiring = DesignWiring::Parallel; });
    Check(wired.resonanceHz == strat.resonanceHz && wired.levelDb == strat.levelDb,
          "coil wiring leaves a single coil alone");

    base = median(DesignType::PafHumbucker);
    const auto split = with([](DesignSpec& s) { s.wiring = DesignWiring::SingleCoil; });
    const auto parallel = with([](DesignSpec& s) { s.wiring = DesignWiring::Parallel; });
    Check(split.resonanceHz > paf.resonanceHz && parallel.resonanceHz > split.resonanceHz,
          "split and parallel humbuckers peak higher than series, parallel highest",
          std::to_string(paf.resonanceHz) + " / " + std::to_string(split.resonanceHz) + " / " +
              std::to_string(parallel.resonanceHz) + " Hz");
    Check(split.levelDb < paf.levelDb - 5.0 && parallel.levelDb < paf.levelDb - 5.0, "and come out about 6 dB lower");

    // The same design on both sides is no change at all.
    Params same = {{"source", kCustomChoice}, {"target", kCustomChoice}};

    for (const char* side : {"source", "target"})
    {
        const std::string s = side;
        same.emplace_back(s + "DesignType", 2.0);
        same.emplace_back(s + "Resistance", 9.1);
        same.emplace_back(s + "Magnet", 3.0);
    }

    auto identity = Make(same);
    const auto noise = Noise(4096);
    Check(Run(*identity, noise) == noise, "the same custom design on both sides passes audio bit for bit");

    // Custom Design follows its own side's parameters, not the other's.
    auto custom = Make({{"target", kCustomChoice}, {"targetDesignType", 2.0}, {"sourceDesignType", 0.0}});
    auto generic = Make({{"target", P(Pickup::HumbuckerBridge)}});
    Check(std::abs(ResponseDb(*custom, 4000.0) - ResponseDb(*generic, 4000.0)) < 3.0,
          "a custom PAF converts much like the generic humbucker");

    const auto figures = custom->DescribeSettings();
    const auto has = [&](const std::string& name) {
        return std::any_of(figures.begin(), figures.end(), [&](const auto& f) { return f.first == name; });
    };
    Check(has("targetInductanceH") && has("targetCapacitancePf") && has("targetPeakHz") && has("sourcePeakHz") &&
              !has("sourceInductanceH"),
          "it describes a custom side's construction, and only a custom side's");

    // The engine answers the panel's curve request from an instance built this way, and sends
    // these figures with the curve (HandleGetEffectResponseRequest).
    const auto analysed = guitarfx::effect_analysis::CreateConfigured(
        guitarfx::EffectGuids::kPickupMatch, {{"source", kCustomChoice}, {"sourceResistance", 7.0}});
    const auto analysedFigures = analysed ? analysed->DescribeSettings() : decltype(figures){};
    const auto resistance = std::find_if(analysedFigures.begin(), analysedFigures.end(),
                                         [](const auto& f) { return f.first == "sourceResistanceKohm"; });
    Check(resistance != analysedFigures.end() && std::abs(resistance->second - 7.0) < 1.0e-9,
          "the analysis instance carries the design the panel sent");
}

void TestSweepsAndRecovery()
{
    std::cout << "\nSweeps, recovery and sample rates" << std::endl;
    auto effect = Make({});
    const auto noise = Noise(kBlock);
    std::vector<float> left(kBlock);
    std::vector<float> right(kBlock);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    bool finite = true;
    float peak = 0.0f;

    for (int block = 0; block < 400; ++block)
    {
        effect->SetParam("source", std::floor(unit(rng) * 7.0));
        effect->SetParam("target", std::floor(unit(rng) * 7.0));
        effect->SetParam("amount", unit(rng) * guitarfx::pickup_match::kMaxAmount);
        effect->SetParam("maxBoost", unit(rng) * 24.0);

        // Designs anywhere in their ranges, on either side.
        for (const char* side : {"source", "target"})
        {
            const std::string s = side;
            effect->SetParam(s + "DesignType", std::floor(unit(rng) * 3.0));
            effect->SetParam(s + "Resistance", 3.0 + unit(rng) * 17.0);
            effect->SetParam(s + "Gauge", 42.0 + std::floor(unit(rng) * 3.0));
            effect->SetParam(s + "Magnet", std::floor(unit(rng) * 4.0));
            effect->SetParam(s + "Cover", std::floor(unit(rng) * 3.0));
            effect->SetParam(s + "Wiring", std::floor(unit(rng) * 3.0));
            effect->SetParam(s + "CapacitanceTrim", -50.0 + unit(rng) * 100.0);
        }

        if (unit(rng) < 0.3)
        {
            effect->SetParam(unit(rng) < 0.5 ? "source" : "target", kCustomChoice);
        }

        auto in = noise;
        float* inputs[2] = {in.data(), in.data()};
        float* outputs[2] = {left.data(), right.data()};
        effect->Process(inputs, outputs, kBlock);

        for (float sample : left)
        {
            finite = finite && guitarfx::IsFinite(sample);
            peak = std::max(peak, std::abs(sample));
        }
    }

    Check(finite && peak < 20.0f, "random settings every block stay finite and bounded",
          "peak " + std::to_string(peak));

    auto nan = Make({});
    std::vector<float> bad(kBlock, std::nanf(""));
    Run(*nan, bad);
    const auto after = Run(*nan, Noise(kBlock));
    Check(std::all_of(after.begin(), after.end(), [](float s) { return guitarfx::IsFinite(s); }),
          "recovers from NaN input");

    for (double rate : {8000.0, 22050.0, 44100.0, 96000.0, 192000.0})
    {
        auto low = Make({{"source", P(Pickup::P90)}, {"target", P(Pickup::ActiveHumbucker)}, {"maxBoost", 24.0}}, rate);
        const auto out = Run(*low, Noise(8192));
        const bool ok =
            std::all_of(out.begin(), out.end(), [](float s) { return guitarfx::IsFinite(s) && std::abs(s) < 20.0f; });
        Check(ok, "stable at " + std::to_string(static_cast<int>(rate)) + " Hz");
    }
}
} // namespace

int main()
{
    std::cout << "=== PickupMatchEffectTests ===" << std::endl;
    guitarfx::RegisterPickupMatchEffect();

    TestRegistration();
    TestCatalogue();
    TestDesign();
    TestIdentity();
    TestResponse();
    TestRoundTrip();
    TestExaggerate();
    TestSensing();
    TestStringComb();
    TestLevelMode();
    TestMaxBoost();
    TestMono();
    TestSweepsAndRecovery();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
