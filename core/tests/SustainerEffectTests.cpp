/**
 * @file SustainerEffectTests.cpp
 * @brief Tests for the Sustainer.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - a held note blooms into a tone at its pitch, at Amount times the pick's level, and that
 *     tone carries on after the string has faded, for Hold and no longer
 *   - nothing is added before Bloom, so short notes are untouched
 *   - Harmonic puts the tone an octave or more up
 *   - a new pick lets the old tone go; a slur carries it to the new pitch
 *   - silence stays silent, Amount at zero passes the input, the mono path matches the stereo one
 *   - non-finite input never reaches the output, and it is bounded at any sample rate
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "LoFiTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/SustainerEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::SustainerEffect;

/// A plucked note: a sine at `hz` that starts at `peak` and dies away with time constant `decay`.
std::vector<float> Note(double hz, double peak, double decay, double seconds, double sampleRate = kSampleRate)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const double t = static_cast<double>(n) / sampleRate;
        signal[n] = static_cast<float>(peak * std::exp(-t / decay) * std::sin(2.0 * kPi * hz * t));
    }

    return signal;
}

/// Amplitude of the component at `hz` in `x` over [from, from + length) seconds.
double AmplitudeAt(const std::vector<float>& x, double hz, double from, double length = 0.5)
{
    const auto start = static_cast<std::size_t>(from * kSampleRate);
    const auto count = static_cast<std::size_t>(length * kSampleRate);
    double re = 0.0;
    double im = 0.0;

    for (std::size_t n = 0; n < count && start + n < x.size(); ++n)
    {
        // Hann window, so a neighbouring component leaks little into this one.
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / static_cast<double>(count));
        const double w = 2.0 * kPi * hz * static_cast<double>(n) / kSampleRate;
        re += window * x[start + n] * std::cos(w);
        im -= window * x[start + n] * std::sin(w);
    }

    return 4.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

std::vector<float> Difference(const std::vector<float>& a, const std::vector<float>& b)
{
    std::vector<float> out(a.size());

    for (std::size_t n = 0; n < a.size(); ++n)
    {
        out[n] = a[n] - b[n];
    }

    return out;
}

std::vector<float> Play(const Params& params, const std::vector<float>& input)
{
    SustainerEffect effect;
    Configure(effect, params);
    return Run(effect, input, input).left;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kSustainer);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("sustainer") == guitarfx::EffectGuids::kSustainer, "the alias resolves");
    Check(registry.Create("sustainer")->GetType() == "sustainer", "the registry creates one");

    if (!info)
    {
        return;
    }

    const std::vector<std::string> expected = {"amount", "bloom", "hold", "harmonic", "sensitivity"};
    bool ordered = info->parameters.size() == expected.size();

    for (std::size_t i = 0; ordered && i < expected.size(); ++i)
    {
        ordered = info->parameters[i].id == expected[i];
    }

    Check(ordered && info->category == "dynamics", "five parameters, in order, listed with the dynamics");
    Check(info->presets.size() == 5 && info->presets.front().isDefault, "five factory presets, the first the default");

    SustainerEffect probe;
    probe.Prepare(kSampleRate, kBlockSize);

    for (const auto& param : info->parameters)
    {
        Check(std::fabs(probe.GetParam(param.id) - param.defaultValue) < 1.0e-6,
              param.id + " starts at its declared default");
    }

    for (const auto& preset : info->presets)
    {
        bool inRange = preset.parameters.size() == info->parameters.size();

        for (const auto& param : info->parameters)
        {
            const auto it = preset.parameters.find(param.id);
            inRange = inRange && it != preset.parameters.end() && it->second >= param.minValue &&
                      it->second <= param.maxValue;
        }

        Check(inRange, preset.id + " sets every parameter within range");
    }

    probe.SetParam("hold", 500.0);
    probe.SetParam("harmonic", 9.0);
    probe.SetParam("amount", std::numeric_limits<double>::quiet_NaN());
    Check(probe.GetParam("hold") == 20.0 && probe.GetParam("harmonic") == 3.0 && probe.GetParam("amount") == 0.5,
          "clamps, and ignores a non-finite value");
}

void TestBloomAndHold()
{
    std::cout << "\nBloom and hold" << std::endl;
    // Under the -50 dB sensitivity after about 3.7 s.
    const auto note = Note(220.0, 0.3, 0.8, 12.0);
    const auto out = Play({{"amount", 0.5}, {"bloom", 0.7}, {"hold", 4.0}}, note);
    const auto added = Difference(out, note);

    Check(ToDb(AmplitudeAt(added, 220.0, 0.1, 0.4)) < -80.0, "nothing is added before Bloom",
          Num(ToDb(AmplitudeAt(added, 220.0, 0.1, 0.4)), 1) + " dB");
    const double bloomed = AmplitudeAt(added, 220.0, 2.5);
    Check(std::fabs(ToDb(bloomed / (0.5 * 0.3))) < 2.0, "once held, the tone is at Amount times the pick's level",
          Num(ToDb(bloomed / 0.15), 2) + " dB from it");
    const double carried = AmplitudeAt(out, 220.0, 6.0);
    Check(std::fabs(ToDb(carried / (0.5 * 0.3))) < 2.0 && ToDb(AmplitudeAt(note, 220.0, 6.0)) < -70.0,
          "and carries on after the string has faded",
          "tone " + Num(ToDb(carried), 1) + " dB, string " + Num(ToDb(AmplitudeAt(note, 220.0, 6.0)), 1) + " dB");
    Check(ToDb(AmplitudeAt(out, 220.0, 9.0)) < -80.0, "for Hold, and then it lets go",
          Num(ToDb(AmplitudeAt(out, 220.0, 9.0)), 1) + " dB at 9 s");

    const auto noHold = Play({{"amount", 0.5}, {"bloom", 0.7}, {"hold", 0.0}}, note);
    const double left = ToDb(AmplitudeAt(Difference(noHold, note), 220.0, 5.0));
    Check(left < -80.0, "with no Hold it goes when the string does", Num(left, 1) + " dB of tone left at 5 s");

    // A short note: over before Bloom.
    auto staccato = Note(220.0, 0.3, 0.8, 4.0);
    std::fill(staccato.begin() + 12000, staccato.end(), 0.0f);
    const auto shortOut = Play({{"bloom", 0.7}}, staccato);
    Check(shortOut == staccato, "a note shorter than Bloom is left exactly as it was");

    // The tone swells in rather than switching on.
    double worst = 0.0;

    for (std::size_t n = 1; n < added.size(); ++n)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(added[n] - added[n - 1])));
    }

    const double toneStep = 0.15 * 2.0 * kPi * 220.0 / kSampleRate;
    Check(worst < 1.5 * toneStep, "the tone comes and goes without a click",
          Num(worst / toneStep, 2) + "x its own step");
}

void TestHarmonic()
{
    std::cout << "\nHarmonic" << std::endl;
    const auto note = Note(220.0, 0.3, 2.0, 4.0);
    const std::vector<std::pair<double, double>> cases = {{0.0, 220.0}, {1.0, 440.0}, {2.0, 660.0}, {3.0, 880.0}};

    for (const auto& [setting, hz] : cases)
    {
        const auto added = Difference(Play({{"harmonic", setting}, {"bloom", 0.5}}, note), note);
        double strongest = 0.0;
        double strongestHz = 0.0;

        for (const auto& candidate : cases)
        {
            const double amplitude = AmplitudeAt(added, candidate.second, 2.5);

            if (amplitude > strongest)
            {
                strongest = amplitude;
                strongestHz = candidate.second;
            }
        }

        Check(strongestHz == hz && ToDb(strongest) > -30.0,
              guitarfx::sustainer::HarmonicLabels()[static_cast<std::size_t>(setting)] + " puts the tone at " +
                  Num(hz, 0) + " Hz",
              Num(strongestHz, 0) + " Hz, " + Num(ToDb(strongest), 1) + " dB");
    }
}

void TestNewNotes()
{
    std::cout << "\nNew notes" << std::endl;
    // A note, then another picked at a different pitch.
    auto phrase = Note(220.0, 0.3, 1.5, 3.0);
    const auto second = Note(330.0, 0.3, 1.5, 4.0);
    std::fill(phrase.end() - 960, phrase.end(), 0.0f);
    phrase.insert(phrase.end(), second.begin(), second.end());
    const auto added = Difference(Play({{"bloom", 0.6}, {"hold", 6.0}}, phrase), phrase);
    Check(ToDb(AmplitudeAt(added, 220.0, 2.0)) > -30.0, "the first note blooms");
    Check(ToDb(AmplitudeAt(added, 220.0, 3.25, 0.3)) < -60.0, "a new pick lets its tone go at once",
          Num(ToDb(AmplitudeAt(added, 220.0, 3.25, 0.3)), 1) + " dB");
    Check(ToDb(AmplitudeAt(added, 330.0, 5.5)) > -30.0 && ToDb(AmplitudeAt(added, 220.0, 5.5)) < -60.0,
          "and the new note blooms at its own pitch");

    // A slur: the pitch moves with no new attack, as a hammer-on or a bend does.
    std::vector<float> slur(static_cast<std::size_t>(5.0 * kSampleRate));
    double phase = 0.0;

    for (std::size_t n = 0; n < slur.size(); ++n)
    {
        const double t = static_cast<double>(n) / kSampleRate;
        phase += 2.0 * kPi * (t < 2.5 ? 220.0 : 247.0) / kSampleRate;
        slur[n] = static_cast<float>(0.3 * std::exp(-t / 4.0) * std::sin(phase));
    }

    const auto slurred = Difference(Play({{"bloom", 0.6}}, slur), slur);
    Check(ToDb(AmplitudeAt(slurred, 247.0, 3.5)) > -30.0 && ToDb(AmplitudeAt(slurred, 220.0, 3.5)) < -50.0,
          "a slur carries the tone to the new pitch",
          "247 Hz " + Num(ToDb(AmplitudeAt(slurred, 247.0, 3.5)), 1) + " dB, 220 Hz " +
              Num(ToDb(AmplitudeAt(slurred, 220.0, 3.5)), 1) + " dB");
}

void TestTransparency()
{
    std::cout << "\nTransparency" << std::endl;
    const std::vector<float> silence(96000, 0.0f);
    Check(Peak(Play({{"amount", 1.0}, {"hold", 20.0}}, silence)) == 0.0, "silence stays silent");

    const auto note = Note(220.0, 0.3, 0.8, 4.0);
    Check(Play({{"amount", 0.0}}, note) == note, "Amount at zero passes the input exactly");

    // A power chord (root, fifth, octave) is periodic at the octave below its root, which is
    // what a pitch tracker finds. Whatever blooms, it is never down there.
    std::vector<float> chord(static_cast<std::size_t>(4.0 * kSampleRate), 0.0f);

    for (const double hz : {110.0, 165.0, 220.0})
    {
        const auto partial = Note(hz, 0.15, 3.0, 4.0);

        for (std::size_t n = 0; n < chord.size(); ++n)
        {
            chord[n] += partial[n];
        }
    }

    const auto chordAdded = Difference(Play({{"bloom", 0.4}, {"amount", 1.0}}, chord), chord);
    Check(ToDb(AmplitudeAt(chordAdded, 55.0, 2.5)) < -80.0, "a chord never blooms an octave under its root",
          Num(ToDb(AmplitudeAt(chordAdded, 55.0, 2.5)), 1) + " dB at 55 Hz, " +
              Num(ToDb(AmplitudeAt(chordAdded, 110.0, 2.5)), 1) + " dB at its root");

    // And a real low note still does: a seven-string's low B is under the floor, a low D is not.
    const auto lowD = Note(73.4, 0.3, 3.0, 4.0);
    Check(ToDb(Rms(Difference(Play({{"bloom", 0.4}}, lowD), lowD), 96000)) > -40.0, "a low D still blooms");

    // Quiet noise is not a note: nothing blooms on hum or hiss.
    const auto hiss = Noise(4.0, 0.02);
    Check(MaxDifference(Play({{"bloom", 0.3}}, hiss), hiss) == 0.0, "noise with no pitch gets no tone");

    SustainerEffect stereo;
    SustainerEffect mono;
    Configure(stereo, {{"bloom", 0.4}});
    Configure(mono, {{"bloom", 0.4}});
    const auto out = Run(stereo, note, note);
    Check(out.left == out.right && !stereo.CanWiden() && stereo.SupportsMonoProcessing(), "a mono input stays mono");
    Check(MaxDifference(RunMono(mono, note), out.left) < 1.0e-5, "the mono path matches the stereo one");
}

void TestRobustness()
{
    std::cout << "\nRobustness" << std::endl;

    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1.0e30f})
    {
        auto input = Note(220.0, 0.3, 2.0, 4.0);
        std::fill(input.begin() + 48000, input.begin() + 48512, bad);
        const auto out = Play({{"bloom", 0.3}, {"amount", 1.0}}, input);
        Check(AllFinite(out) && Peak(out) < 10.0, "a non-finite or absurd input leaves the output finite and bounded",
              "peak " + Num(Peak(out), 2));
    }

    for (const double sampleRate : {8000.0, 44100.0, 96000.0, 192000.0})
    {
        SustainerEffect effect;
        Configure(effect, {{"amount", 1.0}, {"bloom", 0.2}, {"harmonic", 3.0}, {"hold", 20.0}}, sampleRate);
        const auto input = Note(1200.0, 0.9, 3.0, 3.0, sampleRate);
        const auto out = Run(effect, input, input).left;
        Check(AllFinite(out) && Peak(out) < 2.0, "bounded with everything up at " + Num(sampleRate, 0) + " Hz",
              "peak " + Num(Peak(out), 2));
    }
}
} // namespace

int main()
{
    std::cout << "=== SustainerEffectTests ===" << std::endl;
    guitarfx::RegisterSustainerEffect();

    TestRegistration();
    TestBloomAndHold();
    TestHarmonic();
    TestNewNotes();
    TestTransparency();
    TestRobustness();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
