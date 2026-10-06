/**
 * @file RotaryVibeEffectTests.cpp
 * @brief The rotary cabinet and the photocell vibe.
 *
 * Rotary:
 *   - each speed's rotors get there at their own pace: the horn in about a second, the drum in
 *     several; Brake stops them
 *   - the horn's Doppler and tremolo grow with speed and Depth, and stop with the rotors
 *   - the mics give a mono input a stereo image, and say so; Spread 0 keeps it mono
 *   - about as loud as bypass at the defaults and across Drive and Depth
 * Vibe:
 *   - Vibrato keeps the level and moves the pitch; Intensity 0 holds it still
 *   - the lamp and cells make the sweep lopsided: light comes on faster than it goes
 *   - Chorus sits near bypass; Sync follows the tempo; the mono path is the stereo path
 * Both: the same output in any block size, nothing allocated on the audio thread, NaN input
 * dropped, and every factory preset in range.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/RotaryEffect.h"
#include "dsp/effects/VibeEffect.h"
#include "helpers/AudioThreadAllocations.h"
#include "helpers/GuitarPhraseSynth.h"

namespace
{
using guitarfx::RotaryEffect;
using guitarfx::VibeEffect;
using Params = std::vector<std::pair<std::string, double>>;

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double v, int precision = 2)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

void Apply(guitarfx::EffectProcessor& fx, const Params& params)
{
    for (const auto& [key, value] : params)
    {
        fx.SetParam(key, value);
    }
}

/// Runs mono `input` into both sides of `fx`, prepared at `rate`, in blocks of `block`.
Stereo Run(guitarfx::EffectProcessor& fx, const std::vector<float>& input, int block = kBlock, double rate = kRate)
{
    fx.Prepare(rate, std::max(block, kBlock));
    Stereo out{std::vector<float>(input.size()), std::vector<float>(input.size())};
    std::vector<float> inL = input;
    std::vector<float> inR = input;

    for (std::size_t start = 0; start < input.size(); start += static_cast<std::size_t>(block))
    {
        const int count =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), input.size() - start));
        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {out.left.data() + start, out.right.data() + start};
        fx.Process(ins, outs, count);
    }

    return out;
}

std::vector<float> Sine(double hz, double seconds, float amplitude = 0.25f, double rate = kRate)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * rate));

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate));
    }

    return out;
}

/// A picked line across the neck at -18 dBFS RMS, the nominal operating level.
std::vector<float> Phrase(double rate = kRate)
{
    using guitarfx::test::PhraseNote;
    std::vector<PhraseNote> notes;
    const double pitches[] = {82.4, 110.0, 146.8, 196.0, 246.9, 329.6, 220.0, 164.8, 392.0, 123.5};

    for (int i = 0; i < 20; ++i)
    {
        PhraseNote note;
        note.start = 0.25 * i;
        note.length = 0.24;
        note.hz = pitches[i % 10];
        note.peak = 0.3;
        notes.push_back(note);
    }

    auto out = guitarfx::test::RenderPhrase(notes, 5.2, rate);
    double squares = 0.0;

    for (float s : out)
    {
        squares += static_cast<double>(s) * s;
    }

    const double gain = std::pow(10.0, -18.0 / 20.0) / std::sqrt(squares / static_cast<double>(out.size()));

    for (float& s : out)
    {
        s = static_cast<float>(s * gain);
    }

    return out;
}

double PowerDb(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double squares = 0.0;

    for (std::size_t i = from; i < to; ++i)
    {
        squares += static_cast<double>(x[i]) * x[i];
    }

    return 10.0 * std::log10(squares / static_cast<double>(to - from) + 1.0e-30);
}

/// Both channels' power over [from, to), against the mono input's, in dB.
double GainDb(const Stereo& out, const std::vector<float>& input, std::size_t from)
{
    const double outDb = 10.0 * std::log10(0.5 * (std::pow(10.0, PowerDb(out.left, from, out.left.size()) / 10.0) +
                                                  std::pow(10.0, PowerDb(out.right, from, out.right.size()) / 10.0)));
    return outDb - PowerDb(input, from, input.size());
}

/// The spread of a tone's frequency, in cents, read from the times between its rising zero
/// crossings over [from, to): 5th to 95th percentile, so a stray crossing cannot decide it.
double PitchSpreadCents(const std::vector<float>& x, std::size_t from, std::size_t to, double nominalHz)
{
    std::vector<double> crossings;

    for (std::size_t i = from + 1; i < to; ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            crossings.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
        }
    }

    // Over a few periods, so the reading is not dominated by one crossing's timing.
    constexpr std::size_t kSpan = 4;
    std::vector<double> cents;

    for (std::size_t i = kSpan; i < crossings.size(); ++i)
    {
        const double hz = kRate * static_cast<double>(kSpan) / (crossings[i] - crossings[i - kSpan]);
        cents.push_back(1200.0 * std::log2(hz / nominalHz));
    }

    if (cents.size() < 20)
    {
        return 0.0;
    }

    std::sort(cents.begin(), cents.end());
    return cents[cents.size() * 95 / 100] - cents[cents.size() * 5 / 100];
}

/// The swing of a tone's level, in dB, between the loudest and quietest 10 ms window over
/// [from, to).
double LevelSwingDb(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    const std::size_t window = 480;
    double lo = 1.0e9;
    double hi = -1.0e9;

    for (std::size_t start = from; start + window <= to; start += window)
    {
        const double db = PowerDb(x, start, start + window);
        lo = std::min(lo, db);
        hi = std::max(hi, db);
    }

    return hi - lo;
}

void TestRotorSpeeds()
{
    std::cout << "\nRotary: the rotors' speeds" << std::endl;
    RotaryEffect fx;
    fx.Prepare(kRate, kBlock);
    Check(std::fabs(fx.GetParam("hornHz") - 0.8) < 1.0e-9 && std::fabs(fx.GetParam("drumHz") - 0.68) < 1.0e-9,
          "a fresh cabinet is already turning at Slow, the drum a little slower than the horn");

    std::vector<float> silenceL(kBlock, 0.0f);
    std::vector<float> silenceR(kBlock, 0.0f);
    float* ins[2] = {silenceL.data(), silenceR.data()};
    float* outs[2] = {silenceL.data(), silenceR.data()};
    const auto runSeconds = [&](double seconds) {
        for (int b = 0; b < static_cast<int>(seconds * kRate / kBlock); ++b)
        {
            fx.Process(ins, outs, kBlock);
        }
    };

    // A block at Slow first: the switch has to come while the cabinet plays.
    fx.Process(ins, outs, kBlock);
    fx.SetParam("speed", 1.0);
    runSeconds(1.0);
    const double horn = fx.GetParam("hornHz");
    const double drum = fx.GetParam("drumHz");
    Check(horn > 0.95 * 6.7, "the horn is at Fast within a second", Num(horn) + " Hz");
    Check(drum < 0.7 * 6.7 * 0.88, "the drum is still spinning up after a second", Num(drum) + " Hz");

    runSeconds(3.0);
    Check(fx.GetParam("drumHz") > 0.95 * 6.7 * 0.88, "the drum is at Fast within four seconds",
          Num(fx.GetParam("drumHz")) + " Hz");

    fx.SetParam("speed", 2.0);
    runSeconds(8.0);
    Check(fx.GetParam("hornHz") < 0.05 && fx.GetParam("drumHz") < 0.05, "Brake stops both rotors",
          Num(fx.GetParam("hornHz"), 3) + ", " + Num(fx.GetParam("drumHz"), 3) + " Hz");

    RotaryEffect quick;
    quick.SetParam("ramp", 0.0);
    quick.Prepare(kRate, kBlock);
    quick.Process(ins, outs, kBlock);
    quick.SetParam("speed", 1.0);

    for (int b = 0; b < static_cast<int>(0.3 * kRate / kBlock); ++b)
    {
        quick.Process(ins, outs, kBlock);
    }

    Check(quick.GetParam("hornHz") > 0.9 * 6.7, "Ramp 0 gets the horn there in a fraction of the time",
          Num(quick.GetParam("hornHz")) + " Hz after 0.3 s");

    // A preset loading onto a fresh node lands after Prepare, before any audio: the cabinet
    // starts at its speed rather than spinning up from Slow.
    RotaryEffect loaded;
    loaded.Prepare(kRate, kBlock);
    loaded.SetParam("speed", 1.0);
    loaded.Process(ins, outs, kBlock);
    Check(loaded.GetParam("hornHz") > 6.69 && loaded.GetParam("drumHz") > 0.999 * 6.7 * 0.88,
          "set to Fast before it plays, it starts at Fast");
}

void TestRotaryHorn()
{
    std::cout << "\nRotary: the horn's Doppler and tremolo" << std::endl;
    // 2 kHz is all horn. Read from the left mic, after the smoothers have settled.
    const auto tone = Sine(2000.0, 3.0);
    const std::size_t from = 24000;
    const std::size_t to = tone.size();

    const auto measure = [&](const Params& params) {
        RotaryEffect fx;
        Apply(fx, params);
        const auto out = Run(fx, tone);
        return std::make_pair(PitchSpreadCents(out.left, from, to, 2000.0), LevelSwingDb(out.left, from, to));
    };

    const auto [slowCents, slowSwing] = measure({{"speed", 0.0}});
    const auto [fastCents, fastSwing] = measure({{"speed", 1.0}});
    const auto [deepCents, deepSwing] = measure({{"speed", 1.0}, {"depth", 1.0}});
    const auto [stillCents, stillSwing] = measure({{"speed", 2.0}});
    const auto [flatCents, flatSwing] = measure({{"speed", 1.0}, {"depth", 0.0}});

    Check(fastCents > 20.0 && fastCents < 120.0, "Fast swings the horn's pitch by tens of cents",
          Num(fastCents, 1) + " cents");
    Check(fastCents > 4.0 * slowCents, "Slow swings it far less", Num(slowCents, 1) + " cents");
    Check(deepCents > fastCents, "more Depth, more Doppler", Num(deepCents, 1) + " cents");
    Check(fastSwing > 4.0 && deepSwing > fastSwing, "the horn's tremolo is several dB, and grows with Depth",
          Num(fastSwing, 1) + ", " + Num(deepSwing, 1) + " dB");
    Check(stillCents < 2.0 && stillSwing < 0.5, "a stopped horn moves nothing",
          Num(stillCents, 2) + " cents, " + Num(stillSwing, 2) + " dB");
    Check(flatCents < 2.0 && flatSwing < 0.5, "Depth 0 hears nothing move",
          Num(flatCents, 2) + " cents, " + Num(flatSwing, 2) + " dB");
}

void TestRotaryStereoAndLevel()
{
    std::cout << "\nRotary: stereo and level" << std::endl;
    const auto phrase = Phrase();

    RotaryEffect stereo;
    const auto out = Run(stereo, phrase);
    double difference = 0.0;

    for (std::size_t i = 0; i < phrase.size(); ++i)
    {
        difference = std::max(difference, static_cast<double>(std::fabs(out.left[i] - out.right[i])));
    }

    Check(stereo.ProducesStereoOutput() && difference > 1.0e-3, "the two mics make a mono input stereo",
          "max |L-R| " + Num(difference, 4));

    RotaryEffect mono;
    mono.SetParam("spread", 0.0);
    const auto monoOut = Run(mono, phrase);
    Check(!mono.ProducesStereoOutput() && monoOut.left == monoOut.right, "Mic Spread 0 is one mic: mono");

    double worst = 0.0;
    std::string where;

    for (const Params& params : {Params{}, Params{{"drive", 0.0}}, Params{{"drive", 1.0}}, Params{{"depth", 0.0}},
                                 Params{{"depth", 1.0}}, Params{{"speed", 1.0}}})
    {
        RotaryEffect fx;
        Apply(fx, params);
        const double db = GainDb(Run(fx, phrase), phrase, 24000);

        if (std::fabs(db) > std::fabs(worst))
        {
            worst = db;
            where = params.empty() ? "defaults" : params.front().first + " " + Num(params.front().second, 1);
        }
    }

    Check(std::fabs(worst) < 2.0, "as loud as bypass on a guitar, across Drive, Depth and Speed",
          "worst " + Num(worst, 2) + " dB at " + where);
}

void TestVibe()
{
    std::cout << "\nVibe" << std::endl;
    const auto tone = Sine(1000.0, 3.0);
    const std::size_t from = 24000;

    VibeEffect vibrato;
    Apply(vibrato, {{"mode", 1.0}, {"intensity", 1.0}});
    const auto wobble = Run(vibrato, tone);
    const double cents = PitchSpreadCents(wobble.left, from, tone.size(), 1000.0);
    const double swing = LevelSwingDb(wobble.left, from, tone.size());
    Check(cents > 8.0, "Vibrato moves the pitch", Num(cents, 1) + " cents");
    Check(swing < 0.2, "and keeps the level: the stages are all-pass", Num(swing, 3) + " dB");

    VibeEffect still;
    Apply(still, {{"mode", 1.0}, {"intensity", 0.0}});
    const double stillCents = PitchSpreadCents(Run(still, tone).left, from, tone.size(), 1000.0);
    Check(stillCents < 1.0, "Intensity 0 holds the sweep still", Num(stillCents, 2) + " cents");

    // The lamp lights faster than it dims and the cells darken slowly, so the light spends
    // less of each cycle rising than falling.
    VibeEffect throb;
    Apply(throb, {{"rate", 2.0}, {"intensity", 1.0}, {"throb", 1.0}});
    throb.Prepare(kRate, kBlock);
    std::vector<float> silence(kBlock, 0.0f);
    float* ins[2] = {silence.data(), silence.data()};
    float* outs[2] = {silence.data(), silence.data()};
    double previous = throb.GetParam("light");
    int rising = 0;
    int falling = 0;

    for (int b = 0; b < static_cast<int>(4.0 * kRate / kBlock); ++b)
    {
        throb.Process(ins, outs, kBlock);
        const double light = throb.GetParam("light");

        if (b > static_cast<int>(kRate / kBlock))
        {
            (light > previous ? rising : falling) += 1;
        }

        previous = light;
    }

    const double risingShare = static_cast<double>(rising) / static_cast<double>(rising + falling);
    Check(risingShare < 0.45, "the sweep is lopsided: the light rises for under half of each cycle",
          Num(100.0 * risingShare, 1) + "%");

    const auto phrase = Phrase();
    double worst = 0.0;

    for (double intensity : {0.0, 0.5, 1.0})
    {
        VibeEffect chorus;
        chorus.SetParam("intensity", intensity);
        const double db = GainDb(Run(chorus, phrase), phrase, 24000);
        worst = std::fabs(db) > std::fabs(worst) ? db : worst;
    }

    Check(std::fabs(worst) < 2.5, "Chorus sits near bypass on a guitar at any Intensity",
          "worst " + Num(worst, 2) + " dB");

    VibeEffect synced;
    Apply(synced, {{"syncMode", 1.0}, {"syncDivision", 4.0}, {"bpm", 90.0}});
    Check(std::fabs(synced.GetParam("effectiveRate") - 1.5) < 1.0e-9, "Sync runs a quarter note at the tempo",
          Num(synced.GetParam("effectiveRate"), 3) + " Hz at 90 bpm");

    // The mono path is the stereo path's left channel, sample for sample.
    VibeEffect stereoPath;
    VibeEffect monoPath;
    const auto stereoOut = Run(stereoPath, phrase);
    monoPath.Prepare(kRate, kBlock);
    std::vector<float> monoOut(phrase.size());

    for (std::size_t start = 0; start + kBlock <= phrase.size(); start += kBlock)
    {
        monoPath.ProcessMono(const_cast<float*>(phrase.data() + start), monoOut.data() + start, kBlock);
    }

    bool same = monoPath.SupportsMonoProcessing() && !monoPath.ProducesStereoOutput();

    for (std::size_t i = 0; i + kBlock <= phrase.size(); ++i)
    {
        same = same && monoOut[i] == stereoOut.left[i] && stereoOut.left[i] == stereoOut.right[i];
    }

    Check(same, "it runs mono, and the mono path matches the stereo one exactly");
}

/// Common to both effects: block size, the audio thread, NaN, other rates, factory presets.
void TestSafety(const char* name, const char* type,
                const std::function<std::unique_ptr<guitarfx::EffectProcessor>()>& make, const Params& busy)
{
    std::cout << "\n" << name << ": safety" << std::endl;
    const auto phrase = Phrase();

    auto reference = make();
    Apply(*reference, busy);
    const auto expected = Run(*reference, phrase, kBlock);
    bool sameInAnyBlock = true;

    for (int block : {1, 17, 512})
    {
        auto fx = make();
        Apply(*fx, busy);
        const auto out = Run(*fx, phrase, block);
        sameInAnyBlock = sameInAnyBlock && out.left == expected.left && out.right == expected.right;
    }

    Check(sameInAnyBlock, "the same output in blocks of 1, 17, 64 and 512");

    auto fx = make();
    Apply(*fx, busy);
    fx->Prepare(kRate, kBlock);
    std::vector<float> inL = phrase;
    std::vector<float> inR = phrase;
    std::vector<float> outL(phrase.size());
    std::vector<float> outR(phrase.size());
    const std::vector<std::string> keys = {"speed", "depth", "rate", "intensity", "mix", "level", "mode"};

    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        int k = 0;

        for (std::size_t start = 0; start + kBlock <= phrase.size(); start += kBlock)
        {
            fx->SetParam(keys[static_cast<std::size_t>(k++) % keys.size()], static_cast<double>(k % 3) * 0.4);
            float* ins[2] = {inL.data() + start, inR.data() + start};
            float* outs[2] = {outL.data() + start, outR.data() + start};
            fx->Process(ins, outs, kBlock);
        }
    });

    Check(allocations.count == 0, "nothing allocates on the audio thread, parameters moving",
          audio_thread_allocations::Describe(allocations));

    auto poisoned = make();
    Apply(*poisoned, busy);
    std::vector<float> nanInput = phrase;
    nanInput[30000] = std::strtof("nan", nullptr);
    nanInput[30001] = std::strtof("inf", nullptr);
    nanInput[30002] = -std::strtof("inf", nullptr);
    poisoned->SetParam("depth", std::strtod("nan", nullptr));
    const auto poisonedOut = Run(*poisoned, nanInput);
    bool finite = true;

    for (std::size_t i = 0; i < phrase.size(); ++i)
    {
        finite = finite && guitarfx::IsFinite(poisonedOut.left[i]) && guitarfx::IsFinite(poisonedOut.right[i]);
    }

    const double after = PowerDb(poisonedOut.left, 48000 * 3, phrase.size());
    Check(finite && after > -40.0, "NaN and infinite input never reach the output, and it plays on",
          Num(after, 1) + " dB after");

    bool otherRates = true;

    for (double rate : {44100.0, 96000.0})
    {
        auto atRate = make();
        Apply(*atRate, busy);
        const auto source = Phrase(rate);
        const auto out = Run(*atRate, source, kBlock, rate);
        otherRates = otherRates && std::fabs(GainDb(out, source, static_cast<std::size_t>(rate / 2))) < 3.0;
    }

    Check(otherRates, "the same level at 44.1 and 96 kHz");

    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(type);
    bool presetsInRange = info.has_value() && !info->presets.empty() && info->presets.front().isDefault;

    if (info)
    {
        for (const auto& preset : info->presets)
        {
            for (const auto& def : info->parameters)
            {
                // Division is the player's, unless the preset is tempo-synced.
                const auto found = preset.parameters.find(def.id);
                const bool leftOut = def.id == "syncDivision" && found == preset.parameters.end();
                presetsInRange =
                    presetsInRange && (leftOut || (found != preset.parameters.end() && found->second >= def.minValue &&
                                                   found->second <= def.maxValue));
            }
        }

        for (const auto& def : info->parameters)
        {
            auto fresh = make();
            presetsInRange = presetsInRange && fresh->GetParam(def.id) == def.defaultValue;
        }
    }

    Check(presetsInRange, "the default preset comes first, every preset sets every control in range, and a fresh "
                          "effect starts at the declared defaults");
}
} // namespace

int main()
{
    try
    {
        std::cout << "Rotary and Vibe tests" << std::endl;
        guitarfx::RegisterRotaryEffect();
        guitarfx::RegisterVibeEffect();

        TestRotorSpeeds();
        TestRotaryHorn();
        TestRotaryStereoAndLevel();
        TestVibe();
        TestSafety("Rotary", guitarfx::EffectGuids::kRotary, [] { return std::make_unique<RotaryEffect>(); },
                   {{"speed", 1.0}, {"drive", 0.6}, {"balance", 0.3}});
        TestSafety("Vibe", guitarfx::EffectGuids::kVibe, [] { return std::make_unique<VibeEffect>(); },
                   {{"rate", 5.0}, {"intensity", 0.9}, {"throb", 0.8}});
    }
    catch (const std::exception& error)
    {
        std::cout << "  [FAIL] threw: " << error.what() << std::endl;
        ++gFailures;
    }

    std::cout << "\n" << (gFailures == 0 ? "All passed" : std::to_string(gFailures) + " failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
