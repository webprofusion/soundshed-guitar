/**
 * @file EffectModeTests.cpp
 * @brief The modes added to existing effects.
 *
 *   - Tremolo: Classic is unchanged; Harmonic moves the lows and highs in opposite phase;
 *     Pan moves the signal between the sides at constant power, and says it is stereo; Slicer
 *     steps through its pattern, a step each cycle, and Sync can make the steps short
 *   - Digital Delay: Reverse plays each slice backwards, at about the forward level
 *   - Ambient Reverb: Shimmer brightens the tail and still lets it die away with every control
 *     up; Freeze holds the tail, ignores what is played over it, and lets go when turned off
 *   - Noise Gate: Swell fades a note in from silence, and again on a new pick while one rings
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/AmbientReverbEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "dsp/effects/TremoloEffect.h"
#include "helpers/AudioThreadAllocations.h"
#include "helpers/GuitarPhraseSynth.h"

namespace
{
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

/// Runs mono `input` into both sides of `fx`, calling `between(sample)` before each block.
template <typename Between> Stereo Run(guitarfx::EffectProcessor& fx, const std::vector<float>& input, Between between)
{
    fx.Prepare(kRate, kBlock);
    Stereo out{std::vector<float>(input.size()), std::vector<float>(input.size())};
    std::vector<float> inL = input;
    std::vector<float> inR = input;

    for (std::size_t start = 0; start + kBlock <= input.size(); start += kBlock)
    {
        between(start);
        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {out.left.data() + start, out.right.data() + start};
        fx.Process(ins, outs, kBlock);
    }

    return out;
}

Stereo Run(guitarfx::EffectProcessor& fx, const std::vector<float>& input)
{
    return Run(fx, input, [](std::size_t) {});
}

std::vector<float> Sine(double hz, double seconds, float amplitude = 0.25f)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
    }

    return out;
}

std::vector<float> Noise(double seconds, float amplitude = 0.1f)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    std::uint32_t state = 12345u;

    for (float& s : out)
    {
        state = state * 1664525u + 1013904223u;
        s = amplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
    }

    return out;
}

/// A picked line, `seconds` long, at about the nominal level.
std::vector<float> Phrase(double seconds)
{
    using guitarfx::test::PhraseNote;
    std::vector<PhraseNote> notes;
    const double pitches[] = {110.0, 146.8, 196.0, 246.9, 329.6, 220.0, 164.8, 392.0};

    for (int i = 0; i < static_cast<int>(seconds / 0.25); ++i)
    {
        PhraseNote note;
        note.start = 0.25 * i;
        note.length = 0.24;
        note.hz = pitches[i % 8];
        note.peak = 0.3;
        notes.push_back(note);
    }

    return guitarfx::test::RenderPhrase(notes, seconds, kRate);
}

double PowerDb(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double squares = 0.0;
    to = std::min(to, x.size());

    for (std::size_t i = from; i < to; ++i)
    {
        squares += static_cast<double>(x[i]) * x[i];
    }

    return 10.0 * std::log10(squares / static_cast<double>(std::max<std::size_t>(1, to - from)) + 1.0e-30);
}

double SecondsDb(const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    return PowerDb(x, static_cast<std::size_t>(fromSeconds * kRate), static_cast<std::size_t>(toSeconds * kRate));
}

/// Power in 10 ms windows over [from, to).
std::vector<double> WindowPowers(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    std::vector<double> powers;

    for (std::size_t start = from; start + 480 <= to; start += 480)
    {
        double squares = 0.0;

        for (std::size_t i = start; i < start + 480; ++i)
        {
            squares += static_cast<double>(x[i]) * x[i];
        }

        powers.push_back(squares / 480.0);
    }

    return powers;
}

double Correlation(const std::vector<double>& a, const std::vector<double>& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    double meanA = 0.0;
    double meanB = 0.0;

    for (std::size_t i = 0; i < n; ++i)
    {
        meanA += a[i];
        meanB += b[i];
    }

    meanA /= static_cast<double>(n);
    meanB /= static_cast<double>(n);
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;

    for (std::size_t i = 0; i < n; ++i)
    {
        ab += (a[i] - meanA) * (b[i] - meanB);
        aa += (a[i] - meanA) * (a[i] - meanA);
        bb += (b[i] - meanB) * (b[i] - meanB);
    }

    return ab / std::sqrt(aa * bb + 1.0e-30);
}

double SwingDb(const std::vector<double>& powers)
{
    const auto [lo, hi] = std::minmax_element(powers.begin(), powers.end());
    return 10.0 * std::log10((*hi + 1.0e-30) / (*lo + 1.0e-30));
}

void TestTremolo()
{
    std::cout << "\nTremolo" << std::endl;
    const auto noise = Noise(2.0);

    // Classic is the effect as it was: the same formula, sample for sample.
    {
        guitarfx::TremoloEffect fx;
        Apply(fx, {{"rate", 5.3}, {"depth", 0.6}, {"shape", 0.4}, {"mix", 0.8}});
        const auto out = Run(fx, noise);
        double phase = 0.0;
        bool same = true;

        for (std::size_t i = 0; i < noise.size(); ++i)
        {
            const float amount = 1.0f + 0.4f * 8.0f;
            const float shaped = std::tanh(static_cast<float>(std::sin(phase)) * amount) / std::tanh(amount);
            const float gain = (1.0f - 0.6f) + 0.6f * (0.5f * (1.0f + shaped));
            const float expected = noise[i] * (1.0f - 0.8f) + noise[i] * gain * 0.8f;
            // Not bit-exact: the Release build's fast math may contract the two differently.
            same = same && std::fabs(out.left[i] - expected) < 1.0e-6f && out.left[i] == out.right[i];
            phase += 2.0 * kPi * 5.3f / kRate;
            phase = phase >= 2.0 * kPi ? std::fmod(phase, 2.0 * kPi) : phase;
        }

        Check(same, "Classic is the tremolo it was, sample for sample, and mono");
    }

    // Harmonic: a low tone and a high one pulse in opposite phase, so together they barely move.
    {
        const auto low = Sine(100.0, 2.0);
        const auto high = Sine(4000.0, 2.0);
        std::vector<float> both(low.size());

        for (std::size_t i = 0; i < both.size(); ++i)
        {
            both[i] = 0.5f * (low[i] + high[i]);
        }

        const Params harmonic = {{"mode", 1.0}, {"depth", 1.0}, {"rate", 3.0}};
        guitarfx::TremoloEffect lowFx;
        guitarfx::TremoloEffect highFx;
        guitarfx::TremoloEffect bothFx;
        guitarfx::TremoloEffect classicFx;
        Apply(lowFx, harmonic);
        Apply(highFx, harmonic);
        Apply(bothFx, harmonic);
        Apply(classicFx, {{"depth", 1.0}, {"rate", 3.0}});
        const auto lowPowers = WindowPowers(Run(lowFx, low).left, 4800, low.size());
        const auto highPowers = WindowPowers(Run(highFx, high).left, 4800, high.size());
        const double correlation = Correlation(lowPowers, highPowers);
        const double harmonicSwing = SwingDb(WindowPowers(Run(bothFx, both).left, 4800, both.size()));
        const double classicSwing = SwingDb(WindowPowers(Run(classicFx, both).left, 4800, both.size()));
        Check(correlation < -0.8, "Harmonic moves the lows and the highs in opposite phase",
              "correlation " + Num(correlation, 3));
        Check(harmonicSwing < 6.0 && classicSwing > 20.0, "so the level barely moves where Classic's swings",
              Num(harmonicSwing, 1) + " dB against " + Num(classicSwing, 1) + " dB");
    }

    // Pan: one side up as the other goes down, the total power steady.
    {
        const auto tone = Sine(440.0, 2.0);
        guitarfx::TremoloEffect fx;
        Apply(fx, {{"mode", 2.0}, {"depth", 1.0}, {"rate", 2.0}});
        const auto out = Run(fx, tone);
        const auto left = WindowPowers(out.left, 4800, tone.size());
        const auto right = WindowPowers(out.right, 4800, tone.size());
        std::vector<double> total(left.size());

        for (std::size_t i = 0; i < total.size(); ++i)
        {
            total[i] = left[i] + right[i];
        }

        Check(Correlation(left, right) < -0.9, "Pan moves the signal from side to side",
              "correlation " + Num(Correlation(left, right), 3));
        Check(SwingDb(total) < 0.5, "at constant power", Num(SwingDb(total), 2) + " dB");

        // Pan can turn a mono input stereo, so the type declares it can widen: a graph keeps what
        // follows a tremolo in stereo, and switching to Pan is heard at once.
        Check(fx.CanWiden(), "a tremolo is declared able to widen, since Pan does");
    }

    // Slicer: Pulse at full Depth, an 8 Hz step: on, off, on, off, with hard edges at Shape 0.
    {
        const auto tone = Sine(1000.0, 2.0);
        guitarfx::TremoloEffect fx;
        Apply(fx, {{"mode", 3.0}, {"depth", 1.0}, {"rate", 8.0}, {"shape", 0.0}});
        const auto out = Run(fx, tone);
        const double toneDb = PowerDb(tone, 0, tone.size());
        double worstOn = 0.0;
        double loudestOff = -300.0;

        for (std::size_t step = 0; step < 16; ++step)
        {
            const std::size_t start = step * 6000;
            const double db = PowerDb(out.left, start + 1500, start + 4500) - toneDb;

            if (step % 2 == 0)
            {
                worstOn = std::max(worstOn, std::fabs(db));
            }
            else
            {
                loudestOff = std::max(loudestOff, db);
            }
        }

        Check(worstOn < 0.2 && loudestOff < -60.0, "Slicer's Pulse plays every other step and cuts the rest",
              "on within " + Num(worstOn, 2) + " dB, off at " + Num(loudestOff, 1) + " dB");

        // Gallop is 1011 four times over.
        guitarfx::TremoloEffect gallop;
        Apply(gallop, {{"mode", 3.0}, {"depth", 1.0}, {"rate", 8.0}, {"pattern", 1.0}});
        const auto galloped = Run(gallop, tone);
        bool follows = true;

        for (std::size_t step = 0; step < 16; ++step)
        {
            const std::size_t start = step * 6000;
            const bool on = PowerDb(galloped.left, start + 2000, start + 4000) - toneDb > -3.0;
            follows = follows && on == (step % 4 != 1);
        }

        Check(follows, "Gallop plays its pattern: on, off, on, on");

        guitarfx::TremoloEffect fast;
        Apply(fast, {{"mode", 3.0}, {"syncMode", 1.0}, {"syncDivision", 13.0}, {"bpm", 300.0}});
        guitarfx::TremoloEffect classicFast;
        Apply(classicFast, {{"syncMode", 1.0}, {"syncDivision", 13.0}, {"bpm", 300.0}});
        Check(std::fabs(fast.GetParam("effectiveRate") - 40.0) < 1.0e-3 &&
                  std::fabs(classicFast.GetParam("effectiveRate") - 12.0) < 1.0e-3,
              "a synced Slicer step can be a 1/32 at 300 bpm; Classic stays under 12 Hz");
    }
}

/// The times of the peaks in `x` over `threshold`, with their signs.
std::vector<std::pair<std::size_t, float>> Peaks(const std::vector<float>& x, float threshold)
{
    std::vector<std::pair<std::size_t, float>> peaks;

    for (std::size_t i = 1; i + 1 < x.size(); ++i)
    {
        const float m = std::fabs(x[i]);

        if (m > threshold && m >= std::fabs(x[i - 1]) && m > std::fabs(x[i + 1]))
        {
            peaks.emplace_back(i, x[i]);
        }
    }

    return peaks;
}

void TestReverseDelay()
{
    std::cout << "\nDigital Delay: Reverse" << std::endl;
    // A positive click and, 10 ms later, a negative one. Played back, each slice runs backwards,
    // so the negative click's echo comes first.
    std::vector<float> clicks(48000, 0.0f);
    clicks[1000] = 0.8f;
    clicks[1480] = -0.8f;

    const auto echoOrder = [&](double direction) {
        guitarfx::DelayEffect fx;
        Apply(fx, {{"time", 100.0},
                   {"feedback", 0.0},
                   {"mix", 1.0},
                   {"highCut", 20000.0},
                   {"lowCut", 20.0},
                   {"direction", direction}});
        const auto peaks = Peaks(Run(fx, clicks).left, 0.05f);
        int forward = 0;
        int backward = 0;

        for (std::size_t i = 0; i + 1 < peaks.size(); ++i)
        {
            const auto gap = peaks[i + 1].first - peaks[i].first;

            if (gap >= 476 && gap <= 484)
            {
                (peaks[i].second > 0.0f ? forward : backward) += 1;
            }
        }

        return std::make_pair(forward, backward);
    };

    const auto [forwardPairsF, backwardPairsF] = echoOrder(0.0);
    const auto [forwardPairsR, backwardPairsR] = echoOrder(1.0);
    Check(forwardPairsF == 1 && backwardPairsF == 0, "Forward repeats the clicks in order");
    Check(backwardPairsR >= 2 && forwardPairsR == 0, "Reverse plays them back last first, once from each head",
          std::to_string(backwardPairsR) + " reversed pairs");

    const auto phrase = Phrase(4.0);
    double levels[2] = {0.0, 0.0};

    for (int direction = 0; direction < 2; ++direction)
    {
        guitarfx::DelayEffect fx;
        Apply(fx, {{"time", 400.0}, {"feedback", 0.0}, {"mix", 1.0}, {"direction", static_cast<double>(direction)}});
        levels[direction] = SecondsDb(Run(fx, phrase).left, 1.0, 4.0);
    }

    Check(std::fabs(levels[1] - levels[0]) < 2.0, "Reverse sits at about Forward's level",
          Num(levels[1] - levels[0], 2) + " dB");

    // The longest slice, Spread and modulation all at once still fit the line.
    guitarfx::DelayEffect longest;
    Apply(longest, {{"time", 2000.0}, {"spread", 50.0}, {"modDepth", 20.0}, {"modRate", 3.0}, {"direction", 1.0}});
    const auto out = Run(longest, Phrase(5.0));
    bool finite = true;

    for (float s : out.right)
    {
        finite = finite && guitarfx::IsFinite(s);
    }

    Check(finite && SecondsDb(out.right, 4.2, 5.0) > -60.0, "the longest reverse slice plays");
}

/// A 2 s phrase then silence, through an ambient reverb that is all wet.
Stereo AmbientTail(const Params& params, double seconds)
{
    auto input = Phrase(2.0);
    input.resize(static_cast<std::size_t>(seconds * kRate), 0.0f);
    guitarfx::AmbientReverbEffect fx;
    fx.SetParam("mix", 1.0);
    Apply(fx, params);
    return Run(fx, input);
}

void TestAmbient()
{
    std::cout << "\nAmbient Reverb: Shimmer and Freeze" << std::endl;
    const auto treble = [](const std::vector<float>& x) {
        const auto highPass = guitarfx::biquad::HighPass(2500.0, guitarfx::biquad::kButterworthQ, kRate);
        guitarfx::biquad::State state;
        std::vector<float> out(x.size());

        for (std::size_t i = 0; i < x.size(); ++i)
        {
            out[i] = static_cast<float>(state.Process(highPass, x[i]));
        }

        return SecondsDb(out, 3.0, 5.0);
    };

    const double plain = treble(AmbientTail({}, 5.0).left);
    const double shimmer = treble(AmbientTail({{"shimmer", 1.0}}, 5.0).left);
    const double down = treble(AmbientTail({{"shimmer", 1.0}, {"shimmerPitch", 3.0}}, 5.0).left);
    Check(shimmer > plain + 6.0, "Shimmer's octaves up brighten the tail",
          "+" + Num(shimmer - plain, 1) + " dB above 2.5 kHz");
    Check(down < shimmer - 6.0, "an octave down does not", Num(down - plain, 1) + " dB");

    // Every control at its maximum, for the two intervals that climb fastest and slowest: the
    // tail still has to die away rather than sustain itself.
    for (const auto& [pitch, name] : {std::make_pair(0.0, "octave up"), std::make_pair(1.0, "fifth up")})
    {
        const auto out = AmbientTail({{"decay", 1.0},
                                      {"tone", 1.0},
                                      {"diffusion", 1.0},
                                      {"space", 1.0},
                                      {"modDepth", 1.0},
                                      {"shimmer", 1.0},
                                      {"shimmerPitch", pitch}},
                                     21.0)
                             .left;
        const double early = SecondsDb(out, 2.0, 3.0);
        const double late = SecondsDb(out, 20.0, 21.0);
        Check(late < early - 12.0, std::string("full Shimmer and Decay, ") + name + ": the tail still dies away",
              Num(early - late, 1) + " dB down over 18 s");
    }

    // Freeze after the phrase, more playing while frozen, then let go.
    const auto frozen = [](bool playOver) {
        auto input = Phrase(2.0);
        input.resize(static_cast<std::size_t>(20.0 * kRate), 0.0f);

        if (playOver)
        {
            const auto more = Phrase(2.0);
            std::copy(more.begin(), more.end(), input.begin() + static_cast<std::ptrdiff_t>(6.0 * kRate));
        }

        guitarfx::AmbientReverbEffect fx;
        fx.SetParam("mix", 1.0);
        return Run(fx, input,
                   [&fx](std::size_t sample) {
                       if (sample == 99 * kBlock * 16)
                       {
                           fx.SetParam("freeze", 1.0);
                       }

                       if (sample == static_cast<std::size_t>(14.0 * kRate))
                       {
                           fx.SetParam("freeze", 0.0);
                       }
                   })
            .left;
    };

    const auto held = frozen(false);
    const auto playedOver = frozen(true);
    const double atThree = SecondsDb(held, 3.0, 4.0);
    const double atTwelve = SecondsDb(held, 12.0, 13.0);
    Check(std::fabs(atTwelve - atThree) < 1.5, "Freeze holds the tail", Num(atTwelve - atThree, 2) + " dB over 9 s");
    Check(std::fabs(SecondsDb(playedOver, 9.0, 13.0) - SecondsDb(held, 9.0, 13.0)) < 0.5,
          "and takes in nothing played over it");
    Check(SecondsDb(held, 18.0, 19.0) < atTwelve - 20.0, "off again, the tail decays",
          Num(SecondsDb(held, 18.0, 19.0) - atTwelve, 1) + " dB");
}

void TestGateSwell()
{
    std::cout << "\nNoise Gate: Swell" << std::endl;
    guitarfx::NoiseGateEffect fresh;
    Check(fresh.GetParam("mode") == 0.0 && fresh.GetParam("swell") == 800.0, "a fresh gate is a gate, Swell at 800 ms");

    // A tone from silence: the note fades in over the Swell time.
    auto input = std::vector<float>(static_cast<std::size_t>(0.5 * kRate), 0.0f);
    const auto tone = Sine(330.0, 2.0, 0.2f);
    input.insert(input.end(), tone.begin(), tone.end());
    guitarfx::NoiseGateEffect swell;
    Apply(swell, {{"mode", 1.0}, {"swell", 800.0}, {"threshold", -50.0}});
    const auto out = Run(swell, input).left;
    const double toneDb = PowerDb(tone, 0, tone.size());
    const double early = SecondsDb(out, 0.58, 0.62) - toneDb;
    const double middle = SecondsDb(out, 0.88, 0.92) - toneDb;
    const double full = SecondsDb(out, 1.4, 1.5) - toneDb;
    Check(early < -20.0 && middle > early + 6.0 && middle < -3.0 && std::fabs(full) < 0.2,
          "Swell fades a note in from silence over its time",
          Num(early, 1) + ", " + Num(middle, 1) + ", " + Num(full, 2) + " dB at 0.1, 0.4 and 1 s");

    // A second pick while the first note still rings starts the swell again.
    using guitarfx::test::PhraseNote;
    PhraseNote first;
    first.start = 0.2;
    first.length = 1.3;
    first.hz = 196.0;
    first.peak = 0.4;
    first.muted = false;
    PhraseNote second = first;
    second.start = 1.5;
    second.hz = 246.9;
    const auto line = guitarfx::test::RenderPhrase({first, second}, 3.0, kRate);
    guitarfx::NoiseGateEffect retrigger;
    Apply(retrigger, {{"mode", 1.0}, {"swell", 600.0}, {"threshold", -60.0}});
    const auto swelled = Run(retrigger, line).left;
    const double beforePick = SecondsDb(swelled, 1.40, 1.48) - SecondsDb(line, 1.40, 1.48);
    const double afterPick = SecondsDb(swelled, 1.53, 1.58) - SecondsDb(line, 1.53, 1.58);
    const double settled = SecondsDb(swelled, 2.3, 2.5) - SecondsDb(line, 2.3, 2.5);
    Check(std::fabs(beforePick) < 0.5 && afterPick < -15.0 && std::fabs(settled) < 0.5,
          "a new pick while the last note rings swells again",
          Num(beforePick, 2) + ", " + Num(afterPick, 1) + ", " + Num(settled, 2) + " dB");

    guitarfx::NoiseGateEffect quiet;
    Apply(quiet, {{"mode", 1.0}});
    quiet.Prepare(kRate, kBlock);
    std::vector<float> inL = line;
    std::vector<float> inR = line;
    std::vector<float> outL(line.size());
    std::vector<float> outR(line.size());
    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        for (std::size_t start = 0; start + kBlock <= line.size(); start += kBlock)
        {
            float* ins[2] = {inL.data() + start, inR.data() + start};
            float* outs[2] = {outL.data() + start, outR.data() + start};
            quiet.Process(ins, outs, kBlock);
            quiet.ProcessMono(inL.data() + start, outL.data() + start, kBlock);
        }
    });
    Check(allocations.count == 0, "Swell allocates nothing on the audio thread",
          audio_thread_allocations::Describe(allocations));
}

void TestNewModesAllocateNothing()
{
    std::cout << "\nThe audio thread" << std::endl;
    const auto phrase = Phrase(2.0);
    guitarfx::TremoloEffect tremolo;
    guitarfx::DelayEffect delay;
    guitarfx::AmbientReverbEffect reverb;
    tremolo.Prepare(kRate, kBlock);
    delay.Prepare(kRate, kBlock);
    reverb.Prepare(kRate, kBlock);
    std::vector<float> inL = phrase;
    std::vector<float> inR = phrase;
    std::vector<float> outL(phrase.size());
    std::vector<float> outR(phrase.size());
    const std::vector<std::string> keys = {"mode",    "pattern",      "crossover", "direction",
                                           "shimmer", "shimmerPitch", "freeze"};

    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        int k = 0;

        for (std::size_t start = 0; start + kBlock <= phrase.size(); start += kBlock)
        {
            const std::string& key = keys[static_cast<std::size_t>(k++) % keys.size()];
            const double value = static_cast<double>(k % 4);
            tremolo.SetParam(key, key == "crossover" ? 300.0 * value : value);
            delay.SetParam(key, value);
            reverb.SetParam(key, value);
            float* ins[2] = {inL.data() + start, inR.data() + start};
            float* outs[2] = {outL.data() + start, outR.data() + start};
            tremolo.Process(ins, outs, kBlock);
            delay.Process(ins, outs, kBlock);
            reverb.Process(ins, outs, kBlock);
        }
    });

    Check(allocations.count == 0, "Tremolo's modes, Reverse, Shimmer and Freeze allocate nothing, switching as they go",
          audio_thread_allocations::Describe(allocations));
}
} // namespace

int main()
{
    try
    {
        std::cout << "Effect mode tests" << std::endl;
        TestTremolo();
        TestReverseDelay();
        TestAmbient();
        TestGateSwell();
        TestNewModesAllocateNothing();
    }
    catch (const std::exception& error)
    {
        std::cout << "  [FAIL] threw: " << error.what() << std::endl;
        ++gFailures;
    }

    std::cout << "\n" << (gFailures == 0 ? "All passed" : std::to_string(gFailures) + " failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
