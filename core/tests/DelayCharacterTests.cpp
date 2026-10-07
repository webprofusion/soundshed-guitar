/**
 * @file DelayCharacterTests.cpp
 * @brief Tests for the character delays: `delay_tape` and `delay_analog`.
 *
 * These assert the mechanism each effect claims, not only that it makes sound:
 *   - both: registration and presets, exact repeat timing at three rates, tempo sync,
 *     dry passthrough, Reset, glide that bends pitch without ever reversing, a stored
 *     Time applied before audio that does not glide, loop stability below unity feedback
 *     and a bounded runaway above it, every preset stable on full-scale noise
 *   - analog: repeat bandwidth tracks the BBD clock, which tracks Time and stage count;
 *     the compander is transparent on a steady signal and pumps the noise floor
 *   - tape: wow is a speed error (same pitch swing at any delay length), repeats darken
 *     pass by pass, the heads land at their ratios, hiss is silent at idle and gone once
 *     playing stops, and the head bump cannot push the loop over unity
 *
 * Several of these pin bugs found while building the effects, each noted where it is
 * checked.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/AnalogDelayEffect.h"
#include "dsp/effects/TapeDelayEffect.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 64;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << std::endl;

    if (!condition)
    {
        ++gFailures;
    }
}

std::string Num(double value, int digits = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return buffer;
}

using Params = std::vector<std::pair<std::string, double>>;

struct Subject
{
    const char* name;
    const char* type;
    const char* alias;
    double maxFeedback;
};

const Subject kTape{"tape", guitarfx::EffectGuids::kDelayTape, "delay_tape", 1.10};
const Subject kAnalog{"analog", guitarfx::EffectGuids::kDelayAnalog, "delay_analog", 1.15};

/// A neutral, linear voicing so timing and level tests measure the delay and nothing else.
Params Linear(const Subject& subject)
{
    if (subject.type == kTape.type)
    {
        return {{"wow", 0.0},         {"flutter", 0.0}, {"age", 0.0},      {"saturation", 0.0}, {"headBump", 0.0},
                {"highCut", 16000.0}, {"lowCut", 20.0}, {"feedback", 0.0}, {"mix", 1.0}};
    }

    return {{"compander", 0.0}, {"saturation", 0.0}, {"noise", 0.0}, {"tone", 1.0},
            {"modDepth", 0.0},  {"feedback", 0.0},   {"mix", 1.0}};
}

std::unique_ptr<guitarfx::EffectProcessor> Make(const Subject& subject, double sampleRate, const Params& params)
{
    auto effect = guitarfx::EffectRegistry::Instance().Create(subject.type);
    effect->Prepare(sampleRate, kBlock);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    return effect;
}

/// Runs `input` through in blocks, mono path. Trailing samples short of a block are left 0.
std::vector<float> Run(guitarfx::EffectProcessor& effect, std::vector<float> input)
{
    std::vector<float> output(input.size(), 0.0f);

    for (std::size_t n = 0; n + kBlock <= input.size(); n += kBlock)
    {
        effect.ProcessMono(&input[n], &output[n], kBlock);
    }

    return output;
}

std::vector<float> Sine(double sampleRate, double seconds, double hz, double level, double stopAfter = 1.0e9)
{
    std::vector<float> signal(static_cast<std::size_t>(sampleRate * seconds), 0.0f);
    const auto stop = static_cast<std::size_t>(sampleRate * stopAfter);

    for (std::size_t n = 0; n < std::min(signal.size(), stop); ++n)
    {
        signal[n] = static_cast<float>(level * std::sin(2.0 * kPi * hz * static_cast<double>(n) / sampleRate));
    }

    return signal;
}

double Peak(const std::vector<float>& s, double sampleRate, double fromSeconds, double toSeconds)
{
    const auto from = static_cast<std::size_t>(sampleRate * fromSeconds);
    const auto to = std::min(s.size(), static_cast<std::size_t>(sampleRate * toSeconds));
    double peak = 0.0;

    for (std::size_t n = from; n < to; ++n)
    {
        peak = std::max(peak, std::fabs(static_cast<double>(s[n])));
    }

    return peak;
}

bool AllFinite(const std::vector<float>& s)
{
    return std::all_of(s.begin(), s.end(), [](float v) { return guitarfx::IsFinite(v); });
}

/// Amplitude at one frequency, by correlation.
double ToneLevel(const std::vector<float>& s, double sampleRate, double hz, std::size_t from, std::size_t to)
{
    double re = 0.0;
    double im = 0.0;

    for (std::size_t n = from; n < to; ++n)
    {
        const double phase = 2.0 * kPi * hz * static_cast<double>(n) / sampleRate;
        re += s[n] * std::cos(phase);
        im += s[n] * std::sin(phase);
    }

    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

/// Lowest and highest instantaneous frequency of a sine, from its zero crossings.
std::pair<double, double> PitchRange(const std::vector<float>& s, double sampleRate, double fromSeconds,
                                     double toSeconds)
{
    double lo = 1.0e9;
    double hi = 0.0;
    double last = -1.0;
    const auto from = static_cast<std::size_t>(sampleRate * fromSeconds) + 1;
    const auto to = std::min(s.size(), static_cast<std::size_t>(sampleRate * toSeconds));

    for (std::size_t n = from; n < to; ++n)
    {
        if (s[n - 1] < 0.0f && s[n] >= 0.0f)
        {
            const double cross = static_cast<double>(n - 1) + s[n - 1] / (s[n - 1] - s[n]);

            if (last >= 0.0)
            {
                lo = std::min(lo, sampleRate / (cross - last));
                hi = std::max(hi, sampleRate / (cross - last));
            }

            last = cross;
        }
    }

    return {lo, hi};
}

// ── Both effects ────────────────────────────────────────────────────────────────────

void TestRegistration(const Subject& subject)
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(subject.type);
    Check(info.has_value(), std::string(subject.name) + ": registered under its GUID");

    if (!info)
    {
        return;
    }

    Check(registry.Resolve(subject.alias) == subject.type, std::string(subject.name) + ": legacy id resolves");
    Check(info->category == "delay" && info->requiresTempo,
          std::string(subject.name) + ": delay category, tempo-aware");

    int defaults = 0;
    bool wellFormed = true;
    std::string problem;

    for (const auto& preset : info->presets)
    {
        defaults += preset.isDefault ? 1 : 0;

        for (const auto& [key, value] : preset.parameters)
        {
            const auto def = std::find_if(info->parameters.begin(), info->parameters.end(),
                                          [&](const auto& p) { return p.id == key; });

            if (def == info->parameters.end() || value < def->minValue || value > def->maxValue)
            {
                wellFormed = false;
                problem = preset.id + "." + key;
            }
        }
    }

    Check(defaults == 1, std::string(subject.name) + ": exactly one default preset", std::to_string(defaults));
    Check(wellFormed, std::string(subject.name) + ": every preset value names a real parameter, in range", problem);
}

/// Where the first repeat of an impulse peaks, in samples after the impulse.
double RepeatLag(const Subject& subject, double sampleRate, double timeMs)
{
    auto params = Linear(subject);
    params.emplace_back("time", timeMs);
    auto effect = Make(subject, sampleRate, params);

    std::vector<float> input(static_cast<std::size_t>(sampleRate * (timeMs * 0.001 + 0.2)), 0.0f);
    const std::size_t impulseAt = 3200;
    input[impulseAt] = 0.5f;
    const auto output = Run(*effect, input);
    const auto peak =
        std::max_element(output.begin(), output.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); });
    return static_cast<double>(peak - output.begin()) - static_cast<double>(impulseAt);
}

/// The delay line's contract, which both effects are built on: read before write, and a
/// read at d returns exactly the sample written d samples ago, across buffer laps.
void TestDelayLineContract()
{
    guitarfx::delay_line::FractionalDelayLine line;
    line.Resize(1000);
    double worst = 0.0;

    for (int n = 0; n < 5000; ++n)
    {
        for (int d = 2; d <= 64 && d <= n; ++d)
        {
            worst = std::max(worst, std::fabs(static_cast<double>(line.ReadHermite(d)) - static_cast<double>(n - d)));
        }

        line.Write(static_cast<float>(n));
    }

    Check(worst < 1.0e-3, "delay line: read at d returns the sample written d ago", Num(worst, 6));
}

/// The first repeat lands on Time. Two bugs this pins: writing the line before reading it
/// put every repeat one sample early, and a Time set after Prepare (as the executor applies
/// stored parameters) glided in from the default Time.
///
/// The tape's playback EQ is one-pole filters and a bell, whose impulse responses peak at
/// once, so its repeat is exact. The analog delay's four-pole reconstruction filter has
/// group delay of its own, as a real pedal's does — measured 0.19 ms at 250 ms — so its
/// repeat must be late by that and never early, and the lag far too short to hear. (A
/// spacing test between two Times cannot stand in for exactness here: writing first shifts
/// every repeat by the same sample, which a difference cancels.)
void TestTiming(const Subject& subject)
{
    for (double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        const std::string at = " at " + Num(sampleRate, 0) + " Hz";
        const double lag = RepeatLag(subject, sampleRate, 250.0) - sampleRate * 0.25;

        if (subject.type == kTape.type)
        {
            Check(std::fabs(lag) < 0.5, "tape: repeat exactly on Time" + at, "error " + Num(lag, 1) + " samples");
        }
        else
        {
            const double lagMs = lag * 1000.0 / sampleRate;
            Check(lagMs >= 0.0 && lagMs < 0.5, "analog: repeat on Time plus the filter's lag, never early" + at,
                  Num(lagMs, 3) + " ms");
        }
    }
}

void TestTempoAndPassthrough(const Subject& subject)
{
    auto effect = Make(subject, 48000.0, {{"syncMode", 1.0}, {"syncDivision", 4.0}, {"bpm", 120.0}});
    Check(std::fabs(effect->GetParam("effectiveTimeMs") - 500.0) < 1.0e-6,
          std::string(subject.name) + ": a quarter note at 120 BPM is 500 ms");

    auto dry = Make(subject, 48000.0, {{"mix", 0.0}, {"feedback", 0.5}});
    const auto input = Sine(48000.0, 1.0, 330.0, 0.3);
    const auto output = Run(*dry, input);
    double worst = 0.0;

    for (std::size_t n = 0; n + kBlock <= input.size(); ++n)
    {
        worst = std::max(worst, std::fabs(static_cast<double>(output[n] - input[n])));
    }

    Check(worst < 1.0e-6, std::string(subject.name) + ": Mix 0 is the dry signal", Num(worst, 9));

    auto tail = Make(subject, 48000.0, {{"feedback", 0.8}, {"mix", 1.0}, {"time", 100.0}});
    (void)Run(*tail, Sine(48000.0, 0.5, 440.0, 0.4));
    tail->Reset();
    const auto afterReset = Run(*tail, std::vector<float>(48000, 0.0f));
    Check(Peak(afterReset, 48000.0, 0.0, 1.0) < 1.0e-6, std::string(subject.name) + ": Reset clears the tail");
}

/// A glided Time change bends pitch, but never below half speed and never backwards. An
/// exponential ramp alone moved the read head faster than real time: 300 to 600 ms read
/// down to 28 Hz from a 440 Hz sine. Glide 0 changes Time without any dive at all.
void TestGlide(const Subject& subject)
{
    constexpr double sr = 48000.0;

    for (double glide : {0.0, 400.0})
    {
        auto params = Linear(subject);
        params.emplace_back("time", 300.0);
        params.emplace_back("glide", glide);
        auto effect = Make(subject, sr, params);

        auto input = Sine(sr, 3.0, 440.0, 0.2);
        std::vector<float> output(input.size(), 0.0f);
        const std::size_t changeAt = static_cast<std::size_t>(sr * 1.0) / kBlock * kBlock;

        for (std::size_t n = 0; n + kBlock <= input.size(); n += kBlock)
        {
            if (n == changeAt)
            {
                effect->SetParam("time", 600.0);
            }

            effect->ProcessMono(&input[n], &output[n], kBlock);
        }

        // Glide 0 jumps, so its one discontinuous period is skipped: what it must not do is
        // dive. (At 440 Hz a 300 ms jump is a whole number of cycles, which would hide the
        // discontinuity anyway; the skip keeps the check honest at any test frequency.)
        const double from = (glide == 0.0) ? 1.02 : 1.0;
        const auto [lo, hi] = PitchRange(output, sr, from, 2.9);
        const std::string label = std::string(subject.name) + ": glide " + Num(glide, 0) + " ms";

        if (glide == 0.0)
        {
            Check(lo > 435.0 && hi < 445.0, label + " changes Time without a pitch dive", Num(lo, 1));
        }
        else
        {
            Check(lo < 430.0, label + " bends pitch", Num(lo, 1) + " Hz");
            Check(lo > 210.0 && hi < 450.0, label + " never below half speed or reversed",
                  Num(lo, 1) + ".." + Num(hi, 1));
        }
    }
}

/// Below unity the loop decays; above it the runaway is bounded, and louder the more
/// Feedback there is. On the analog delay, feeding back *after* the expander took
/// Feedback 0.80 to an amplitude of 14; tying saturator drive to Feedback made the runaway
/// *quieter* as Feedback rose.
void TestLoop(const Subject& subject)
{
    constexpr double sr = 48000.0;
    const auto burst = [&](double seconds) { return Sine(sr, seconds, 220.0, 0.4, 0.1); };

    for (double feedback : {0.5, 0.8, 0.95})
    {
        auto effect = Make(subject, sr, {{"time", 350.0}, {"feedback", feedback}, {"mix", 1.0}});
        const auto output = Run(*effect, burst(12.0));
        const double early = Peak(output, sr, 1.0, 2.0);
        const double late = Peak(output, sr, 10.5, 11.5);
        Check(late < early * 0.5 && AllFinite(output),
              std::string(subject.name) + ": Feedback " + Num(feedback, 2) + " decays",
              Num(early, 4) + " -> " + Num(late, 5));
    }

    double previous = 0.0;
    bool rising = true;
    bool bounded = true;

    for (double feedback : {1.03, subject.maxFeedback})
    {
        auto effect = Make(subject, sr, {{"time", 400.0}, {"feedback", feedback}, {"mix", 1.0}});
        const auto output = Run(*effect, burst(14.0));
        const double late = Peak(output, sr, 12.5, 13.9);
        rising = rising && late > previous;
        bounded = bounded && AllFinite(output) && Peak(output, sr, 0.0, 14.0) < 4.0;
        previous = late;
    }

    Check(bounded, std::string(subject.name) + ": the runaway stays finite and bounded");
    Check(rising && previous > 0.01, std::string(subject.name) + ": more Feedback, louder runaway",
          "sustains at " + Num(previous, 4));
}

void TestPresetsStable(const Subject& subject)
{
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(subject.type);
    bool stable = true;
    std::string problem;
    guitarfx::delay_line::Rng rng;

    for (const auto& preset : info->presets)
    {
        Params params(preset.parameters.begin(), preset.parameters.end());
        auto effect = Make(subject, 48000.0, params);
        std::vector<float> noise(48000 * 3);

        for (auto& v : noise)
        {
            v = rng.NextBipolar();
        }

        const auto output = Run(*effect, noise);

        if (!AllFinite(output) || Peak(output, 48000.0, 0.0, 3.0) > 8.0)
        {
            stable = false;
            problem = preset.id;
        }
    }

    Check(stable, std::string(subject.name) + ": every preset stays finite and bounded on full-scale noise", problem);
}

void TestStereo(const Subject& subject)
{
    // A delay can widen, so a graph keeps whatever follows it in stereo from the start and a
    // user raising Spread is heard at once (EffectProcessor::CanWiden).
    auto effect = Make(subject, 48000.0, {});
    Check(effect->CanWiden(), std::string(subject.name) + ": declared able to widen a mono input");

    // And Spread does widen: the same signal on both sides comes out as two.
    auto stereo = Make(subject, 48000.0, {{"feedback", 0.5}, {"spread", 10.0}});
    auto input = Sine(48000.0, 1.0, 523.0, 0.3);
    std::vector<float> right = input;
    std::vector<float> outLeft(input.size()), outRight(input.size());

    for (std::size_t n = 0; n + kBlock <= input.size(); n += kBlock)
    {
        float* ins[2] = {&input[n], &right[n]};
        float* outs[2] = {&outLeft[n], &outRight[n]};
        stereo->Process(ins, outs, kBlock);
    }

    double difference = 0.0;

    for (std::size_t n = 0; n < input.size(); ++n)
    {
        difference = std::max(difference, std::fabs(static_cast<double>(outLeft[n] - outRight[n])));
    }

    Check(difference > 1.0e-3, std::string(subject.name) + ": Spread turns a mono input stereo", Num(difference, 6));
}

// ── Analog ──────────────────────────────────────────────────────────────────────────

double AnalogRepeatGain(double timeMs, double stages, double hz)
{
    auto params = Linear(kAnalog);
    params.emplace_back("time", timeMs);
    params.emplace_back("stages", stages);
    auto effect = Make(kAnalog, 48000.0, params);
    const auto output = Run(*effect, Sine(48000.0, 1.4, hz, 0.1));
    return ToneLevel(output, 48000.0, hz, 48000, 48000 + 14400) / 0.1;
}

double AnalogMinus3Db(double timeMs, double stages)
{
    const double reference = AnalogRepeatGain(timeMs, stages, 100.0);
    double low = 100.0;
    double high = 20000.0;

    for (int iteration = 0; iteration < 10; ++iteration)
    {
        const double mid = std::sqrt(low * high);

        if (AnalogRepeatGain(timeMs, stages, mid) / reference > 0.7079) // -3 dB
        {
            low = mid;
        }
        else
        {
            high = mid;
        }
    }

    return std::sqrt(low * high);
}

/// The headline behaviour: a longer delay is a slower clock is a darker repeat, and fewer
/// stages at the same delay is slower still. The measured -3 dB point sits at 0.8-0.9 of
/// the reconstruction filter's corner, the rest of the loop's filtering taking the rest.
void TestAnalogBandwidth()
{
    guitarfx::AnalogDelayEffect reference;
    reference.Prepare(48000.0, kBlock);
    double previous = 1.0e9;
    bool falling = true;
    bool tracking = true;
    std::string detail;

    for (double timeMs : {50.0, 150.0, 300.0, 600.0})
    {
        const double measured = AnalogMinus3Db(timeMs, 3.0);
        const double ratio = measured / reference.ReconstructionCornerHz(timeMs);
        falling = falling && measured < previous;
        tracking = tracking && ratio > 0.75 && ratio < 0.95;
        detail += Num(timeMs, 0) + " ms " + Num(measured, 0) + " Hz; ";
        previous = measured;
    }

    Check(falling, "analog: repeat bandwidth falls as Time rises", detail);
    Check(tracking, "analog: bandwidth tracks the BBD clock's reconstruction corner");

    const double fewStages = AnalogMinus3Db(300.0, 0.0);
    const double manyStages = AnalogMinus3Db(300.0, 3.0);
    Check(fewStages < manyStages * 0.5, "analog: 1024 stages is far darker than 4096 at 300 ms",
          Num(fewStages, 0) + " vs " + Num(manyStages, 0) + " Hz");
}

/// The compander is transparent on a steady signal at any amount, from -46 to 0 dBFS. The
/// clamps have to be matched for that: with one clamp range for both halves it measured
/// +2.8 dB above 0 dBFS. And it pumps: the noise floor right after a burst stands well
/// above the floor a second later, which it does not do with the compander off.
void TestCompander()
{
    bool flat = true;
    std::string detail;

    for (double amount : {0.5, 1.0})
    {
        for (double level : {0.005, 0.063, 0.5, 1.0})
        {
            auto params = Linear(kAnalog);
            params.emplace_back("compander", amount);
            params.emplace_back("time", 100.0);
            auto effect = Make(kAnalog, 48000.0, params);
            const auto output = Run(*effect, Sine(48000.0, 1.2, 220.0, level));
            const double gainDb = 20.0 * std::log10(Peak(output, 48000.0, 0.9, 1.19) / level);

            if (std::fabs(gainDb) > 0.5)
            {
                flat = false;
                detail = "amount " + Num(amount, 1) + " at " + Num(20.0 * std::log10(level), 0) +
                         " dBFS: " + Num(gainDb, 2) + " dB";
            }
        }
    }

    Check(flat, "analog: compander transparent on a steady signal, -46 to 0 dBFS", detail);

    // A burst ends at 0.5 s and its repeat at 0.8 s; the reconstruction filter's tail is
    // gone by 0.86 s (measured), so the pump window starts there and not sooner — starting
    // at 0.82 s measured the tail, not the noise. The expander's 60 ms release means the
    // pump is over by about 0.96 s, and the settled window is early enough that the idle
    // gate, which follows the input, is still fully open (it holds until about 1.6 s).
    double pump[2] = {0.0, 0.0};
    double idleFloor[2] = {0.0, 0.0};

    for (int on = 0; on < 2; ++on)
    {
        auto effect = Make(kAnalog, 48000.0,
                           {{"time", 300.0},
                            {"feedback", 0.0},
                            {"mix", 1.0},
                            {"compander", on ? 1.0 : 0.0},
                            {"noise", 1.0},
                            {"saturation", 0.0}});
        const auto output = Run(*effect, Sine(48000.0, 2.0, 330.0, 0.4, 0.5));
        idleFloor[on] = std::max(1.0e-12, Peak(output, 48000.0, 1.35, 1.58));
        pump[on] = Peak(output, 48000.0, 0.86, 0.90) / idleFloor[on];
    }

    Check(pump[1] > 3.0 && pump[0] < 1.5, "analog: the compander makes the noise floor pump after a note",
          "on " + Num(pump[1], 1) + "x, off " + Num(pump[0], 2) + "x");

    // And it does the job it is in a BBD pedal for: the noise floor between notes is far
    // lower with it than without. Measured 20 dB.
    Check(idleFloor[1] < 0.25 * idleFloor[0], "analog: the compander lowers the idle noise floor",
          Num(20.0 * std::log10(idleFloor[1] / idleFloor[0]), 1) + " dB");
}

// ── Tape ────────────────────────────────────────────────────────────────────────────

/// Wow is a speed error, so its pitch swing is the same at any delay length. Modelled as a
/// time error instead, flutter came out ten times more violent in pitch than wow.
void TestTapeWow()
{
    double spreads[2] = {0.0, 0.0};
    int slot = 0;

    for (double timeMs : {60.0, 1200.0})
    {
        auto params = Linear(kTape);
        params.emplace_back("time", timeMs);
        params.emplace_back("wow", 1.0);
        auto effect = Make(kTape, 48000.0, params);
        const auto [lo, hi] = PitchRange(Run(*effect, Sine(48000.0, 6.0, 1000.0, 0.2)), 48000.0, 1.5, 5.9);
        spreads[slot++] = 1200.0 * std::log2(hi / lo);
    }

    Check(spreads[0] > 20.0, "tape: full wow is clearly audible", Num(spreads[0], 1) + " cents");
    Check(std::fabs(spreads[0] - spreads[1]) < 0.25 * spreads[0], "tape: wow's pitch swing does not depend on Time",
          Num(spreads[0], 1) + " vs " + Num(spreads[1], 1) + " cents");
}

void TestTapeHeadsAndTone()
{
    auto params = Linear(kTape);
    params.emplace_back("time", 500.0);
    params.emplace_back("headMode", 6.0);
    auto effect = Make(kTape, 48000.0, params);
    std::vector<float> input(48000, 0.0f);
    input[3200] = 0.5f;
    const auto output = Run(*effect, input);
    int found = 0;

    for (double ratio : guitarfx::tape_delay::kHeadRatios)
    {
        const auto expected = 3200 + static_cast<std::size_t>(48000.0 * 0.5 * ratio);
        const auto first = output.begin() + static_cast<std::ptrdiff_t>(expected - 8);
        const auto last = output.begin() + static_cast<std::ptrdiff_t>(expected + 8);
        const auto at = static_cast<std::size_t>(
            std::max_element(first, last, [](float a, float b) { return std::fabs(a) < std::fabs(b); }) -
            output.begin());
        found += (at == expected && std::fabs(output[at]) > 0.1f) ? 1 : 0;
    }

    Check(found == 3, "tape: three heads give three taps exactly at their ratios", std::to_string(found));

    // Successive repeats of a noise burst have a falling spectral centroid. Age is off so no
    // hiss joins each pass; the default voicing's playback EQ does the darkening.
    auto dark = Make(kTape, 48000.0,
                     {{"wow", 0.0}, {"flutter", 0.0}, {"age", 0.0}, {"feedback", 0.7}, {"mix", 1.0}, {"time", 300.0}});
    std::vector<float> burst(48000 * 2, 0.0f);
    guitarfx::delay_line::Rng rng;

    for (std::size_t n = 3200; n < 3200 + 480; ++n)
    {
        burst[n] = 0.3f * rng.NextBipolar();
    }

    const auto repeats = Run(*dark, burst);
    double previous = 1.0e9;
    bool darker = true;
    std::string detail;

    for (int repeat = 1; repeat <= 3; ++repeat)
    {
        const auto start = static_cast<std::size_t>(3200 + 14400 * repeat - 96);
        double weighted = 0.0;
        double total = 0.0;

        for (double hz = 100.0; hz < 16000.0; hz *= 1.06)
        {
            const double level = ToneLevel(repeats, 48000.0, hz, start, start + 768);
            weighted += level * hz;
            total += level;
        }

        const double centroid = weighted / std::max(1.0e-12, total);
        darker = darker && centroid < previous;
        detail += Num(centroid, 0) + " Hz; ";
        previous = centroid;
    }

    Check(darker, "tape: each repeat is darker than the last", detail);
}

/// Silent at idle, and gone once playing stops. The gate follows the input, not the line —
/// on the line, hiss fed back would hold its own gate open — and needs a floor: without
/// one hiss was still measurable four seconds after playing stopped.
void TestTapeHiss()
{
    auto idle = Make(kTape, 48000.0, {{"age", 1.0}});
    Check(Peak(Run(*idle, std::vector<float>(96000, 0.0f)), 48000.0, 0.0, 2.0) == 0.0,
          "tape: a worn tape that has never been played is silent");

    auto played = Make(kTape, 48000.0, {{"age", 1.0}, {"feedback", 0.0}, {"mix", 1.0}, {"time", 200.0}});
    const auto output = Run(*played, Sine(48000.0, 6.0, 440.0, 0.1, 1.0));
    Check(Peak(output, 48000.0, 5.0, 5.9) == 0.0, "tape: hiss is gone once the gate has released");
}

/// Full head bump with Low Cut at its minimum would take the loop over unity at 100 Hz for
/// any Feedback above about 0.64; the feedback tap is normalised by the EQ's measured peak.
void TestTapeBumpStability()
{
    auto effect =
        Make(kTape, 48000.0,
             {{"time", 350.0}, {"feedback", 0.95}, {"headBump", 1.0}, {"lowCut", 20.0}, {"age", 0.0}, {"mix", 1.0}});
    const auto output = Run(*effect, Sine(48000.0, 14.0, 110.0, 0.4, 0.1));
    const double early = Peak(output, 48000.0, 1.0, 2.0);
    const double late = Peak(output, 48000.0, 12.5, 13.5);
    Check(late < early * 0.5, "tape: full head bump cannot push Feedback 0.95 over unity",
          Num(early, 4) + " -> " + Num(late, 5));
}
} // namespace

int main()
{
    std::cout << "=== DelayCharacterTests ===" << std::endl;
    guitarfx::RegisterTapeDelayEffect();
    guitarfx::RegisterAnalogDelayEffect();

    TestDelayLineContract();

    for (const Subject* subject : {&kTape, &kAnalog})
    {
        std::cout << "\n-- " << subject->name << " --" << std::endl;
        TestRegistration(*subject);
        TestTiming(*subject);
        TestTempoAndPassthrough(*subject);
        TestGlide(*subject);
        TestLoop(*subject);
        TestPresetsStable(*subject);
        TestStereo(*subject);
    }

    std::cout << "\n-- analog --" << std::endl;
    TestAnalogBandwidth();
    TestCompander();

    std::cout << "\n-- tape --" << std::endl;
    TestTapeWow();
    TestTapeHeadsAndTone();
    TestTapeHiss();
    TestTapeBumpStability();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
