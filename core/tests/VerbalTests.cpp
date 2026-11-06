/**
 * @file VerbalTests.cpp
 * @brief Verbal, the multi-machine reverb (core/src/dsp/reverb/).
 *
 *   - every machine runs finite, stays silent on silence and dies away; the tank machines' two
 *     sides are uncorrelated from a mono input
 *   - Decay is the measured RT60: the tank machines and Spring within tolerance from 1 to 8 s,
 *     and it answers across the whole knob on every machine that shows it
 *   - the Mix law keeps the dry at unity and adds the wet, with a fully wet top
 *   - Infinite holds the tail and keeps adding to it; Freeze holds it and takes nothing new;
 *     releasing either lets it decay; a held level stays bounded on every machine
 *   - a machine change lets the old tail ring out, without a click, whatever is already ringing
 *   - a tail isn't retired while audio is still in a delay line
 *   - Nonlinear's Feedback dies away at the top of the knob on every Shape
 *   - the Swell fades rather than cuts, doesn't duck a held tail, and hides the pick in Dry
 *   - Shimmer's held tail stays where it is
 *   - stepped and dragged controls don't click
 *   - Reflections lights both sides from a one-sided input
 *   - a non-finite input sample is scrubbed rather than latched
 *   - every machine-only control says which machines show it
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/reverb/VerbalEffect.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double v)
{
    return std::to_string(std::round(v * 1000.0) / 1000.0).substr(0, 7);
}

/// A rendered stereo output, with a callback per block for automation.
struct Render
{
    std::vector<float> l;
    std::vector<float> r;

    [[nodiscard]] double Energy(double fromS, double toS) const
    {
        const auto a = static_cast<std::size_t>(fromS * kSampleRate);
        const auto b = std::min(l.size(), static_cast<std::size_t>(toS * kSampleRate));
        double e = 0.0;

        for (std::size_t i = a; i < b; ++i)
        {
            e += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
        }

        return e / std::max<std::size_t>(1, b - a);
    }

    [[nodiscard]] bool Finite() const
    {
        return std::all_of(l.begin(), l.end(), [](float v) { return guitarfx::IsFinite(v); }) &&
               std::all_of(r.begin(), r.end(), [](float v) { return guitarfx::IsFinite(v); });
    }

    [[nodiscard]] float Peak() const
    {
        float peak = 0.0f;

        for (std::size_t i = 0; i < l.size(); ++i)
        {
            peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
        }

        return peak;
    }

    /// The share of the energy that a 3 kHz first difference lets through: a rough top-end meter.
    [[nodiscard]] double HighShare(double fromS, double toS) const
    {
        const auto a = static_cast<std::size_t>(fromS * kSampleRate);
        const auto b = std::min(l.size(), static_cast<std::size_t>(toS * kSampleRate));
        double all = 1e-30, high = 0.0;

        for (std::size_t i = a + 1; i < b; ++i)
        {
            const double d = static_cast<double>(l[i]) - l[i - 1];
            high += d * d;
            all += static_cast<double>(l[i]) * l[i];
        }

        return high / all;
    }
};

template <typename Input, typename Automation>
Render Run(guitarfx::VerbalEffect& fx, double seconds, Input input, Automation automate)
{
    const auto total = static_cast<std::size_t>(seconds * kSampleRate);
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};
    Render out;
    out.l.reserve(total);
    out.r.reserve(total);

    for (std::size_t start = 0; start < total; start += kBlockSize)
    {
        automate(static_cast<double>(start) / kSampleRate);

        for (int i = 0; i < kBlockSize; ++i)
        {
            const float v = input(start + static_cast<std::size_t>(i));
            inL[i] = v;
            inR[i] = v;
        }

        fx.Process(ins, outs, kBlockSize);
        out.l.insert(out.l.end(), outL.begin(), outL.end());
        out.r.insert(out.r.end(), outR.begin(), outR.end());
    }

    return out;
}

/// A 50 ms noise burst at -12 dBFS rms from `at` seconds, on every call the same.
auto Burst(double at = 0.0, double length = 0.05)
{
    return [at, length](std::size_t n) {
        const auto start = static_cast<std::size_t>(at * kSampleRate);
        const auto end = start + static_cast<std::size_t>(length * kSampleRate);

        if (n < start || n >= end)
        {
            return 0.0f;
        }

        std::uint32_t seed = static_cast<std::uint32_t>(n) * 2654435761u + 12345u;
        seed ^= seed >> 13;
        seed *= 1274126177u;
        return 0.25f * 1.7320508f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    };
}

/// A sine at `hz`, every sample.
auto Tone(double hz, float level)
{
    return [hz, level](std::size_t n) {
        return level * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(n) / kSampleRate));
    };
}

void NoAutomation(double)
{
}

std::unique_ptr<guitarfx::VerbalEffect> Make(const std::map<std::string, double>& params)
{
    auto fx = std::make_unique<guitarfx::VerbalEffect>();
    fx->SetParam("mix", 1.0);

    for (const auto& [key, value] : params)
    {
        fx->SetParam(key, value);
    }

    fx->Prepare(kSampleRate, kBlockSize);
    return fx;
}

/// Schroeder T30 from an impulse response, both channels.
double MeasureT30(const Render& out)
{
    std::vector<double> edc(out.l.size());
    double acc = 0.0;

    for (std::size_t i = out.l.size(); i-- > 0;)
    {
        acc += static_cast<double>(out.l[i]) * out.l[i] + static_cast<double>(out.r[i]) * out.r[i];
        edc[i] = acc;
    }

    const double total = edc[0];
    const auto at = [&](double db) {
        const double target = total * std::pow(10.0, db / 10.0);

        for (std::size_t i = 0; i < edc.size(); ++i)
        {
            if (edc[i] <= target)
            {
                return static_cast<double>(i) / kSampleRate;
            }
        }

        return static_cast<double>(edc.size()) / kSampleRate;
    };

    return 2.0 * (at(-35.0) - at(-5.0));
}

void TestEveryMachine()
{
    std::cout << "\nEvery machine: finite, silent on silence, dies away" << std::endl;

    for (int machine = 0; machine < static_cast<int>(guitarfx::verbal::kMachineCount); ++machine)
    {
        const std::string name = guitarfx::verbal::kMachineLabels[machine];
        auto fx = Make({{"machine", machine}, {"decay", 1.0}, {"echoFeedback", 0.3}});
        const auto silent = Run(*fx, 0.3, [](std::size_t) { return 0.0f; }, NoAutomation);
        Check(silent.Energy(0.0, 0.3) == 0.0, name + ": silence in, silence out");

        fx->Reset();
        const auto out = Run(*fx, 3.0, Burst(), NoAutomation);
        const double early = out.Energy(0.05, 0.4);
        const double late = out.Energy(2.5, 3.0);
        Check(out.Finite() && early > 1e-7, name + ": answers a burst", "early " + Num(10 * std::log10(early)) + " dB");
        Check(late < early * 1e-3, name + ": dies away", Num(10 * std::log10(late / early)) + " dB");

        // The tank machines' two sides: uncorrelated even though both inputs are the same.
        if (machine != static_cast<int>(guitarfx::verbal::Machine::Reflections))
        {
            double lr = 0.0, ll = 0.0, rr = 0.0;

            for (std::size_t i = static_cast<std::size_t>(0.15 * kSampleRate); i < out.l.size(); ++i)
            {
                lr += static_cast<double>(out.l[i]) * out.r[i];
                ll += static_cast<double>(out.l[i]) * out.l[i];
                rr += static_cast<double>(out.r[i]) * out.r[i];
            }

            const double corr = lr / std::sqrt(ll * rr + 1e-30);
            Check(std::abs(corr) < 0.5, name + ": the sides are uncorrelated", "corr " + Num(corr));
        }
    }
}

void TestDecayIsRt60()
{
    std::cout << "\nDecay reads as the measured RT60" << std::endl;

    for (const auto& [machine, label] : {std::pair{1, "Hall"}, std::pair{0, "Room"}, std::pair{2, "Plate"},
                                         std::pair{5, "Bloom"}, std::pair{7, "Choir"}, std::pair{3, "Spring"}})
    {
        for (const double decay : {1.0, 3.0, 8.0})
        {
            // A Bloom's own envelope outlasts a short Decay: about 1.7 s at the default Length.
            if (machine == 5 && decay < 2.0)
            {
                continue;
            }

            auto fx = Make({{"machine", machine}, {"decay", decay}, {"preDelay", 1.0}, {"tone", 0.6}, {"lowEnd", 0.5}});
            const auto out =
                Run(*fx, decay * 1.6 + 1.0, [](std::size_t n) { return n == 0 ? 0.5f : 0.0f; }, NoAutomation);
            const double t30 = MeasureT30(out);
            Check(std::abs(t30 / decay - 1.0) < 0.15, std::string(label) + " Decay " + Num(decay) + " s",
                  "T30 " + Num(t30) + " s");
        }
    }

    // The knob is one range for every machine, so no machine may stop answering part way up it.
    for (const auto& [machine, label] : {std::pair{1, "Hall"}, std::pair{3, "Spring"}, std::pair{9, "Tape Echo"}})
    {
        const auto render = [machine](double decay, double seconds) {
            auto fx = Make({{"machine", machine}, {"decay", decay}, {"preDelay", 1.0}, {"echoFeedback", 0.0}});
            return Run(*fx, seconds, [](std::size_t n) { return n == 0 ? 0.5f : 0.0f; }, NoAutomation);
        };
        Check(MeasureT30(render(0.2, 1.5)) < 0.75 * MeasureT30(render(0.4, 1.5)),
              std::string(label) + ": the bottom of the knob still moves it");
        const auto level = [&render](double decay) { return render(decay, 3.0).Energy(2.5, 3.0); };
        Check(level(30.0) < level(60.0) && level(15.0) < level(30.0),
              std::string(label) + ": the top of the knob still moves it");
    }
}

void TestMixLaw()
{
    std::cout << "\nMix adds the reverb to an untouched dry" << std::endl;
    using guitarfx::VerbalEffect;
    Check(VerbalEffect::WetGain(0.0f) == 0.0f && VerbalEffect::DryGain(0.0f) == 1.0f, "Mix 0 is dry only");
    Check(std::abs(20.0f * std::log10(VerbalEffect::WetGain(0.5f)) + 10.0f) < 0.1f &&
              VerbalEffect::DryGain(0.5f) == 1.0f,
          "half way: the dry at unity, the wet 10 dB down");
    Check(VerbalEffect::WetGain(0.9f) == 1.0f && VerbalEffect::DryGain(0.9f) == 1.0f, "nine tenths: both at unity");
    Check(VerbalEffect::WetGain(1.0f) == 1.0f && VerbalEffect::DryGain(1.0f) == 0.0f, "the top is fully wet");

    bool rising = true;

    for (float m = 0.0f; m < 1.0f; m += 0.01f)
    {
        rising = rising && VerbalEffect::WetGain(m + 0.01f) >= VerbalEffect::WetGain(m);
    }

    Check(rising, "the wet never falls as Mix rises");
}

void TestHold()
{
    std::cout << "\nHold" << std::endl;

    // Infinite: engaged at 0.3 s, a second burst at 1.5 s joins the tail, released at 3.5 s.
    {
        auto fx = Make({{"machine", 1}, {"decay", 1.0}, {"holdMode", 0}});
        auto input = [](std::size_t n) { return Burst(0.0)(n) + Burst(1.5)(n); };
        auto automate = [&fx](double t) { fx->SetParam("hold", t >= 0.3 && t < 3.5 ? 1.0 : 0.0); };
        const auto out = Run(*fx, 6.0, input, automate);
        const double held = out.Energy(1.0, 1.4);
        const double later = out.Energy(3.0, 3.4);
        const double grown = out.Energy(1.8, 2.2);
        Check(out.Finite() && later > held * 0.3, "Infinite holds the tail",
              Num(10 * std::log10(later / held)) + " dB over two seconds");
        Check(grown > held * 1.5, "Infinite adds what is played into it", Num(10 * std::log10(grown / held)) + " dB");
        Check(out.Energy(5.5, 6.0) < later * 1e-2, "released, it decays again");
    }

    // Freeze: engaged at 0.3 s; a louder burst at 1.5 s must not join the tail.
    {
        auto fx = Make({{"machine", 1}, {"decay", 1.0}, {"holdMode", 1}});
        auto input = [](std::size_t n) { return Burst(0.0)(n) + 2.0f * Burst(1.5)(n); };
        auto automate = [&fx](double t) { fx->SetParam("hold", t >= 0.3 ? 1.0 : 0.0); };
        const auto out = Run(*fx, 3.0, input, automate);
        const double before = out.Energy(1.0, 1.4);
        const double after = out.Energy(2.0, 2.4);
        Check(before > 1e-8 && std::abs(10 * std::log10(after / before)) < 2.0, "Freeze holds and takes nothing new",
              Num(10 * std::log10(after / before)) + " dB across the burst");
    }

    // Twenty seconds of hot sustained playing into Infinite, on every machine.
    for (int machine = 0; machine < static_cast<int>(guitarfx::verbal::kMachineCount); ++machine)
    {
        auto fx = Make({{"machine", machine}, {"decay", 4.0}, {"hold", 1.0}, {"holdMode", 0}});
        const auto out = Run(*fx, 20.0, Tone(220.0, 0.9f), NoAutomation);
        Check(out.Finite() && out.Peak() < 4.0f,
              std::string(guitarfx::verbal::kMachineLabels[machine]) + ": held level is bounded",
              "peak " + Num(20 * std::log10(out.Peak())) + " dBFS");
    }

    // Shimmer's held tail must not keep climbing the octaves.
    {
        auto fx = Make({{"machine", 8}, {"decay", 4.0}, {"holdMode", 1}, {"preDelay", 1.0}});
        auto automate = [&fx](double t) { fx->SetParam("hold", t >= 0.5 ? 1.0 : 0.0); };
        const auto out = Run(*fx, 16.0, Burst(0.0, 0.3), automate);
        const double early = out.HighShare(3.0, 4.0);
        const double late = out.HighShare(15.0, 16.0);
        Check(late < early * 2.0, "Shimmer Freeze doesn't climb", "share above 3 kHz x" + Num(late / early));
        Check(std::abs(10 * std::log10(out.Energy(15.0, 16.0) / out.Energy(3.0, 4.0))) < 3.0,
              "Shimmer Freeze holds its level",
              Num(10 * std::log10(out.Energy(15.0, 16.0) / out.Energy(3.0, 4.0))) + " dB");
    }
}

void TestMachineChangeSpillsOver()
{
    std::cout << "\nChanging machine" << std::endl;
    auto fx = Make({{"machine", 1}, {"decay", 4.0}});
    auto automate = [&fx](double t) { fx->SetParam("machine", t >= 0.8 ? 0.0 : 1.0); };
    const auto out = Run(*fx, 1.6, Burst(), automate);
    const auto change = static_cast<std::size_t>(0.8 * kSampleRate);
    double worstStep = 0.0, typicalStep = 0.0;

    for (std::size_t i = change - 4800; i < change + 4800; ++i)
    {
        const double step = std::abs(static_cast<double>(out.l[i]) - out.l[i - 1]);
        (i < change ? typicalStep : worstStep) = std::max(i < change ? typicalStep : worstStep, step);
    }

    const double before = out.Energy(0.7, 0.8);
    const double after = out.Energy(0.8, 0.9);
    Check(after > before * 0.3, "the old tail rings on", Num(10 * std::log10(after / before)) + " dB");
    Check(worstStep < typicalStep * 2.5, "without a click", "worst step x" + Num(worstStep / (typicalStep + 1e-12)));
}

/// Worst second difference within 5 ms of `at`, over the worst across the half second before it.
double ClickIndex(const Render& out, double at)
{
    const auto i = static_cast<std::size_t>(at * kSampleRate);
    const auto worst = [&out](std::size_t a, std::size_t b) {
        double v = 0.0;

        for (std::size_t k = a; k < b; ++k)
        {
            v = std::max(v, std::abs(static_cast<double>(out.l[k]) - 2.0 * out.l[k - 1] + out.l[k - 2]));
            v = std::max(v, std::abs(static_cast<double>(out.r[k]) - 2.0 * out.r[k - 1] + out.r[k - 2]));
        }

        return v;
    };
    return worst(i - 240, i + 240) / (worst(i - 24000, i - 480) + 1e-12);
}

void TestChangingMachineWhileATailRings()
{
    std::cout << "\nChanging machine with a tail already ringing" << std::endl;
    // A sine has hardly any second difference of its own, so a click has nothing to hide behind.
    const auto note = Tone(220.0, 0.25f);

    // The machines that run apart from the tank are the ones an in-place change cut dead.
    for (const auto& [machine, label] : {std::pair{3, "Spring"}, std::pair{9, "Tape Echo"}})
    {
        auto fx = Make({{"machine", 1}, {"decay", 8.0}, {"echoFeedback", 0.5}});
        auto automate = [&fx, machine = machine](double t) {
            fx->SetParam("machine", t >= 3.0 ? 0.0 : t >= 1.5 ? machine : t >= 0.5 ? 2.0 : 1.0);
        };
        const auto out = Run(*fx, 4.0, note, automate);
        Check(out.Finite() && ClickIndex(out, 3.0) < 2.5, std::string(label) + " to Room: no click",
              "x" + Num(ClickIndex(out, 3.0)));
    }

    // A change every 300 ms.
    {
        auto fx = Make({{"machine", 1}, {"decay", 6.0}, {"echoFeedback", 0.5}});
        static constexpr int kOrder[] = {3, 9, 0, 2, 6, 3, 10, 8, 3, 1};
        auto automate = [&fx](double t) {
            const int step = static_cast<int>(t / 0.3) - 3;

            if (step >= 0 && step < 10)
            {
                fx->SetParam("machine", kOrder[step]);
            }
        };
        const auto out = Run(*fx, 4.5, note, automate);
        double worst = 0.0;

        for (int i = 0; i < 10; ++i)
        {
            worst = std::max(worst, ClickIndex(out, 0.9 + 0.3 * i));
        }

        Check(out.Finite() && worst < 2.5, "a change every 300 ms: no click", "worst x" + Num(worst));
    }

    // Spring doesn't run the tank, so a Plate reached through it must not find its old contents.
    {
        auto fx = Make({{"machine", 1}, {"decay", 10.0}, {"preDelay", 1.0}});
        auto automate = [&fx](double t) {
            fx->SetParam("machine", t >= 2.0 ? 2.0 : t >= 1.0 ? 3.0 : t >= 0.5 ? 2.0 : 1.0);
        };
        const auto out = Run(*fx, 3.0, Burst(0.0, 0.7), automate);
        Check(out.Energy(2.1, 2.3) <= out.Energy(1.7, 1.9) * 1.1, "a machine left doesn't come back with its old tail",
              Num(10 * std::log10(out.Energy(2.1, 2.3) / out.Energy(1.7, 1.9))) + " dB");
    }

    // A machine that has heard nothing gives way without disturbing the tail behind it.
    {
        auto fx = Make({{"machine", 1}, {"decay", 6.0}, {"preDelay", 1.0}});
        auto automate = [&fx](double t) { fx->SetParam("machine", t >= 1.0 ? 3.0 : t >= 0.5 ? 2.0 : 1.0); };
        const auto out = Run(*fx, 1.4, Burst(), automate);
        const double before = out.Energy(0.85, 1.0);
        const double after = out.Energy(1.05, 1.2);
        Check(after > before * 0.5, "a silent machine doesn't take the tail with it",
              Num(10 * std::log10(after / before)) + " dB");
    }

    // Hold on the outgoing machine lets go on a ramp.
    {
        auto fx = Make({{"machine", 1}, {"decay", 3.0}, {"hold", 1.0}, {"holdMode", 1}});
        fx->SetParam("hold", 0.0);
        auto automate = [&fx](double t) {
            fx->SetParam("hold", t >= 0.3 ? 1.0 : 0.0);
            fx->SetParam("machine", t >= 1.5 ? 2.0 : 1.0);
        };
        const auto out = Run(*fx, 2.0, [&note](std::size_t n) { return n < 12000 ? note(n) : 0.0f; }, automate);
        Check(ClickIndex(out, 1.5) < 2.5, "Hold on the outgoing machine lets go cleanly",
              "x" + Num(ClickIndex(out, 1.5)));
    }
}

void TestTailInFlight()
{
    std::cout << "\nA tail still inside a delay line" << std::endl;
    const auto impulse = [](std::size_t n) { return n == 0 ? 0.5f : 0.0f; };

    for (const auto& [params, label] :
         {std::pair{std::map<std::string, double>{{"machine", 1}, {"preDelay", 1000.0}}, "Hall, Pre-Delay 1000 ms"},
          std::pair{std::map<std::string, double>{{"machine", 2}, {"preDelay", 500.0}}, "Plate, Pre-Delay 500 ms"},
          std::pair{std::map<std::string, double>{{"machine", 9}, {"echoTime", 1500.0}}, "Tape Echo, 1500 ms"},
          std::pair{std::map<std::string, double>{{"machine", 10}, {"shapeTime", 2000.0}}, "Nonlinear, 2000 ms"}})
    {
        auto left = Make(params);
        const auto stayed = Run(*left, 4.0, impulse, NoAutomation);
        auto moved = Make(params);
        auto automate = [&moved](double t) {
            if (t >= 0.2)
            {
                moved->SetParam("machine", 0.0);
            }
        };
        const auto changed = Run(*moved, 4.0, impulse, automate);
        const double ratio = changed.Energy(0.0, 4.0) / (stayed.Energy(0.0, 4.0) + 1e-30);
        Check(ratio > 0.5, std::string(label) + " is heard after a change",
              Num(10 * std::log10(ratio + 1e-30)) + " dB");
    }
}

void TestNonlinearFeedback()
{
    std::cout << "\nNonlinear Feedback" << std::endl;

    for (int shape = 0; shape < 6; ++shape)
    {
        auto fx =
            Make({{"machine", 10}, {"decay", 1.0}, {"shape", shape}, {"shapeTime", 824.0}, {"shapeFeedback", 1.0}});
        const auto out = Run(*fx, 20.0, [](std::size_t n) { return n == 0 ? 0.5f : 0.0f; }, NoAutomation);
        const double late = out.Energy(18.0, 20.0);
        Check(out.Finite() && late < 1e-7,
              std::string(guitarfx::verbal::kShapeLabels[shape]) + " dies away at Feedback 1",
              Num(10 * std::log10(late + 1e-30)) + " dB");
    }
}

void TestSwell()
{
    std::cout << "\nSwell" << std::endl;
    // Two picks into a ringing note.
    const auto picks = [](std::size_t n) { return Burst(0.0, 0.25)(n) + 2.0f * Burst(1.0, 0.06)(n); };
    const auto pick = static_cast<std::size_t>(1.0 * kSampleRate);

    // Wet: the tail goes down over a few milliseconds, not in one sample.
    {
        auto fx = Make({{"machine", 4}, {"decay", 4.0}, {"preDelay", 1.0}, {"swellMode", 0}, {"riseTime", 0.1}});
        const auto out = Run(*fx, 1.1, picks, NoAutomation);
        double fastest = 0.0;

        for (std::size_t i = pick; i < pick + 960; i += 48)
        {
            const auto level = [&out](std::size_t a) {
                double e = 1e-14;

                for (std::size_t k = a; k < a + 48; ++k)
                {
                    e += static_cast<double>(out.l[k]) * out.l[k] + static_cast<double>(out.r[k]) * out.r[k];
                }

                return 10.0 * std::log10(e);
            };

            // Only the audible part of the fall counts; near silence every step is a big ratio.
            if (level(i) > -70.0)
            {
                fastest = std::max(fastest, level(i) - level(i + 48));
            }
        }

        Check(fastest < 30.0, "Wet fades rather than cuts", Num(fastest) + " dB in the fastest ms");
    }

    // Under a held tail the pick must not duck anything.
    {
        auto fx = Make({{"machine", 4}, {"decay", 4.0}, {"preDelay", 1.0}, {"swellMode", 0}, {"holdMode", 1}});
        auto automate = [&fx](double t) { fx->SetParam("hold", t >= 0.5 ? 1.0 : 0.0); };
        const auto out = Run(*fx, 1.2, picks, automate);
        const double change = 10 * std::log10(out.Energy(1.02, 1.06) / out.Energy(0.9, 0.98));
        Check(std::abs(change) < 3.0, "Wet leaves a frozen tail alone", Num(change) + " dB across the pick");
    }

    // Dry at half Mix: the pick is what it exists to hide.
    {
        auto fx = Make({{"machine", 4}, {"decay", 1.0}, {"preDelay", 1.0}, {"swellMode", 1}, {"riseTime", 0.6}});
        fx->SetParam("mix", 0.5);
        const auto out = Run(*fx, 1.1, [](std::size_t n) { return 2.0f * Burst(1.0, 0.06)(n); }, NoAutomation);
        // The burst is 0.5 rms. Its first 30 ms is the attack, here or 10 ms behind.
        const double leak = out.Energy(1.0, 1.03);
        Check(leak < 0.25 * 0.01, "Dry hides the pick",
              Num(10 * std::log10((leak + 1e-30) / 0.25)) + " dB against the pick");
        Check(fx->GetLatencySamples() == 480, "Dry reports the look-ahead it runs on");
    }

    auto wet = Make({{"machine", 4}, {"swellMode", 0}});
    auto other = Make({{"machine", 1}});
    Check(wet->GetLatencySamples() == 0 && other->GetLatencySamples() == 0, "nothing else has latency");

    // Leaving the Swell part way up a rise doesn't jump the gain.
    {
        auto fx = Make({{"machine", 4}, {"decay", 2.0}, {"preDelay", 1.0}, {"swellMode", 1}, {"riseTime", 2.0}});
        fx->SetParam("mix", 0.5);
        auto automate = [&fx](double t) { fx->SetParam("machine", t >= 1.5 ? 1.0 : 4.0); };
        const auto out = Run(*fx, 2.0, [](std::size_t n) { return Tone(330.0, 0.3f)(n); }, automate);
        Check(ClickIndex(out, 1.5) < 2.5, "leaving the Swell part way up: no click", "x" + Num(ClickIndex(out, 1.5)));
    }
}

void TestControlMoves()
{
    std::cout << "\nMoving controls over a ringing tail" << std::endl;
    const auto sound = Tone(220.0, 0.25f);

    struct Move
    {
        int machine;
        const char* key;
        double from, to;
    };

    static constexpr Move kMoves[] = {
        {1, "tone", 0.2, 0.9},       {1, "mod", 0.1, 0.9},       {1, "lowEnd", 0.2, 0.9},
        {1, "midEq", 0.2, 0.9},      {3, "tone", 0.2, 0.9},      {3, "mod", 0.1, 0.9},
        {6, "mod", 0.1, 0.9},        {6, "diffusion", 0.2, 0.9}, {5, "bloomLength", 0.2, 0.8},
        {9, "heads", 0.0, 2.0},      {9, "spacing", 0.0, 1.0},   {10, "shapeTime", 800.0, 300.0},
        {10, "shape", 3.0, 1.0},     {11, "size", 0.8, 0.3},     {11, "listenerX", 0.2, 0.8},
        {11, "roomShape", 0.0, 2.0},
    };

    for (const Move& m : kMoves)
    {
        auto fx = Make({{"machine", m.machine}, {m.key, m.from}, {"decay", 4.0}, {"preDelay", 1.0}});
        auto automate = [&fx, &m](double t) { fx->SetParam(m.key, t >= 1.5 ? m.to : m.from); };
        const auto out = Run(*fx, 1.8, sound, automate);
        const double index = ClickIndex(out, 1.5);
        Check(out.Finite() && index < 2.5,
              std::string(guitarfx::verbal::kMachineLabels[m.machine]) + " " + m.key + " step: no click",
              "x" + Num(index));
    }
}

void TestReflectionsBalance()
{
    std::cout << "\nReflections from a one-sided input" << std::endl;
    auto fx = Make({{"machine", 11}});
    std::vector<float> inL(kBlockSize, 0.0f), inR(kBlockSize, 0.0f), outL(kBlockSize), outR(kBlockSize);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};
    double left = 0.0, right = 0.0;

    for (int block = 0; block < 100; ++block)
    {
        inL[0] = block == 0 ? 0.5f : 0.0f;
        fx->Process(ins, outs, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i)
        {
            left += static_cast<double>(outL[i]) * outL[i];
            right += static_cast<double>(outR[i]) * outR[i];
        }
    }

    const double gap = 10 * std::log10(left / right);
    Check(std::abs(gap) < 6.0, "both sides answer", Num(gap) + " dB apart");
}

void TestNonFiniteInput()
{
    std::cout << "\nA non-finite input sample" << std::endl;
    auto fx = Make({{"machine", 6}, {"decay", 2.0}});
    const auto out = Run(
        *fx, 1.0,
        [](std::size_t n) {
            // From its bits: under fast math a NaN constant can be folded away.
            return n == 1000 ? std::bit_cast<float>(0x7fc00000u) : Burst(0.1)(n);
        },
        NoAutomation);
    Check(out.Finite() && out.Energy(0.3, 0.6) > 1e-8,
          "is scrubbed: the output stays finite and the reverb keeps working");
}

void TestVisibilityMetadata()
{
    std::cout << "\nControls each machine shows" << std::endl;
    guitarfx::RegisterVerbalEffect();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kReverbVerbal);
    Check(info.has_value(), "registered");

    if (!info)
    {
        return;
    }

    bool ok = true;
    std::string problem;

    for (const auto& def : info->parameters)
    {
        if (def.group == "Machine")
        {
            const bool valid = def.visibleWhenKey == "machine" && !def.visibleWhenValues.empty() &&
                               std::all_of(def.visibleWhenValues.begin(), def.visibleWhenValues.end(),
                                           [](int m) { return m >= 0 && m < 12; });
            ok = ok && valid;
            problem = valid ? problem : def.id;
        }
    }

    Check(ok, "every machine-only control names its machines", problem);
    const auto& machine = info->parameters[guitarfx::verbal::kMachine];
    Check(machine.control == "picker" && machine.labels.size() == 12 && machine.labelInfo.size() == 12,
          "Machine is a picker of twelve, each with a line of detail");
}
} // namespace

int main()
{
    std::cout << "=== VerbalTests ===" << std::endl;
    TestEveryMachine();
    TestDecayIsRt60();
    TestMixLaw();
    TestHold();
    TestMachineChangeSpillsOver();
    TestChangingMachineWhileATailRings();
    TestTailInFlight();
    TestNonlinearFeedback();
    TestSwell();
    TestControlMoves();
    TestReflectionsBalance();
    TestNonFiniteInput();
    TestVisibilityMetadata();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
