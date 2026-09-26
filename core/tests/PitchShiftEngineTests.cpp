/**
 * @file PitchShiftEngineTests.cpp
 * @brief Pitch Shift's two engines and the crossfades between its paths.
 *
 *   - the Low Latency engine (TimeDomainPitchShifter) is heard at a new shift within a few
 *     milliseconds, where High Quality (Signalsmith Stretch) takes about 40
 *   - it holds pitch within a few cents from -12 to +12 st, at 44.1, 48 and 96 kHz
 *   - at 0 st the node passes the input through untouched and reports no latency, and at Mix 0
 *     it plays the input delayed by exactly the latency it reports
 *   - entering and leaving the 0 st bypass, and switching engine, never clicks: the output
 *     never moves faster than the tone itself, where a hard switch jumped by the full swing
 *   - a re-engaged Low Latency engine does not replay audio from before its bypass
 *   - none of it allocates on the audio thread, including the engine switch and the splices
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/PitchShiftEffect.h"
#include "helpers/AudioThreadAllocations.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;

using guitarfx::PitchShiftEffect;

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

std::vector<float> Sine(double frequency, double seconds, double amplitude, double sampleRate = kSampleRate)
{
    std::vector<float> out(static_cast<size_t>(seconds * sampleRate));

    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
    }

    return out;
}

/// Six harmonics falling as 1/k: enough for a correlation splice to have a real waveform to match.
std::vector<float> Harmonic(double frequency, double seconds, double sampleRate)
{
    std::vector<float> out(static_cast<size_t>(seconds * sampleRate));

    for (size_t i = 0; i < out.size(); ++i)
    {
        double v = 0.0;

        for (int h = 1; h <= 6; ++h)
        {
            v += std::sin(2.0 * kPi * frequency * h * static_cast<double>(i) / sampleRate + h * 0.7) / h;
        }

        out[i] = static_cast<float>(0.2 * v);
    }

    return out;
}

/// Renders a mono signal through the effect as stereo, in blocks, calling `beforeBlock` with each
/// block's first sample index so a test can move parameters where MIDI would: between blocks.
std::vector<float> Render(PitchShiftEffect& effect, const std::vector<float>& input,
                          const std::function<void(size_t)>& beforeBlock = {})
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);

    for (size_t pos = 0; pos + kBlockSize <= input.size(); pos += kBlockSize)
    {
        if (beforeBlock)
        {
            beforeBlock(pos);
        }

        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inL.begin());
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inR.begin());
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        effect.Process(inputs, outputs, kBlockSize);
        std::copy_n(outL.begin(), kBlockSize, out.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    return out;
}

void PrepareFree(PitchShiftEffect& effect, double engine, double sampleRate = kSampleRate)
{
    effect.Prepare(sampleRate, kBlockSize);
    effect.SetParam("engine", engine);
    effect.SetParam("stepMode", 0.0);
    effect.SetParam("mix", 1.0);
}

double Goertzel(const float* x, int n, double frequency, double sampleRate = kSampleRate)
{
    const double w = 2.0 * kPi * frequency / sampleRate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / n);
        const double s0 = x[i] * window + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/// The strongest frequency within +/-60 cents of `expected`, in cents from it.
double PitchErrorCents(const float* x, int n, double expected, double sampleRate)
{
    double best = 0.0;
    double bestCents = 0.0;

    for (double cents = -60.0; cents <= 60.0; cents += 0.25)
    {
        const double energy = Goertzel(x, n, expected * std::pow(2.0, cents / 1200.0), sampleRate);

        if (energy > best)
        {
            best = energy;
            bestCents = cents;
        }
    }

    return bestCents;
}

/// Mean time from a step (-5 to +7 st and back, on a 440 Hz sine, at every phase of the block
/// grid) until half the output's energy is at the new pitch.
double MeanStepLagMs(double engine)
{
    PitchShiftEffect effect;
    PrepareFree(effect, engine);
    effect.SetParam("semitones", -5.0);
    effect.Reset();

    const auto input = Sine(440.0, 6.0, 0.3);
    const double lowHz = 440.0 * std::pow(2.0, -5.0 / 12.0);
    const double highHz = 440.0 * std::pow(2.0, 7.0 / 12.0);
    std::vector<size_t> steps;
    std::vector<bool> stepUp;

    for (size_t k = 0;; ++k)
    {
        const size_t at = (static_cast<size_t>(kSampleRate * 0.5) + k * 14437) / kBlockSize * kBlockSize;

        if (at + static_cast<size_t>(kSampleRate * 0.2) >= input.size())
        {
            break;
        }

        steps.push_back(at);
        stepUp.push_back(k % 2 == 0);
    }

    size_t next = 0;
    const auto out = Render(effect, input, [&](size_t pos) {
        if (next < steps.size() && pos == steps[next])
        {
            effect.SetParam("semitones", stepUp[next] ? 7.0 : -5.0);
            ++next;
        }
    });

    constexpr int kWindow = 256;
    double total = 0.0;
    int counted = 0;

    for (size_t k = 0; k < steps.size(); ++k)
    {
        const double newHz = stepUp[k] ? highHz : lowHz;
        const double oldHz = stepUp[k] ? lowHz : highHz;

        for (size_t centre = steps[k] - kWindow; centre < steps[k] + static_cast<size_t>(kSampleRate * 0.19);
             centre += 8)
        {
            const float* window = out.data() + centre - kWindow / 2;
            const double eNew = Goertzel(window, kWindow, newHz);
            const double eOld = Goertzel(window, kWindow, oldHz);

            if (eNew >= eOld)
            {
                total += (static_cast<double>(centre) - static_cast<double>(steps[k])) * 1000.0 / kSampleRate;
                ++counted;
                break;
            }
        }
    }

    return counted > 0 ? total / counted : 1.0e9;
}

void TestStepLag()
{
    std::cout << "\n--- A new shift is heard at once on Low Latency ---\n";
    const double lowLatency = MeanStepLagMs(1.0);
    const double highQuality = MeanStepLagMs(0.0);

    // Free mode, so this includes the glide over a controller's steps.
    Check(lowLatency < 5.0, "Low Latency: half the energy at the new pitch within 5 ms", Num(lowLatency) + " ms");
    // Not a requirement, a control: it shows the measurement sees the lag this engine removes.
    Check(highQuality > 25.0, "High Quality still takes its synthesis window (control)", Num(highQuality) + " ms");
}

void TestPitchAccuracy()
{
    std::cout << "\n--- Low Latency holds pitch ---\n";

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        double worst = 0.0;
        std::string worstCase;

        for (const double note : {82.41, 196.0, 329.63})
        {
            for (const double shift : {-12.0, -5.0, -1.0, 2.0, 7.0, 12.0})
            {
                PitchShiftEffect effect;
                PrepareFree(effect, 1.0, sampleRate);
                effect.SetParam("semitones", shift);
                effect.Reset();

                const auto out = Render(effect, Harmonic(note, 1.0, sampleRate));
                const int n = sampleRate > 60000.0 ? 32768 : 16384;
                const double expected = note * std::pow(2.0, shift / 12.0);
                const double error =
                    std::abs(PitchErrorCents(out.data() + out.size() - n - 512, n, expected, sampleRate));

                if (error > worst)
                {
                    worst = error;
                    worstCase = Num(note, 1) + " Hz at " + Num(shift, 0) + " st";
                }
            }
        }

        Check(worst <= 3.0, "Within 3 cents at " + Num(sampleRate / 1000.0, 1) + " kHz",
              "worst " + Num(worst) + " cents, " + worstCase);
    }
}

void TestTransparencyAndAlignment()
{
    std::cout << "\n--- 0 st is transparent, Mix 0 is aligned ---\n";

    {
        PitchShiftEffect effect;
        PrepareFree(effect, 1.0);
        effect.SetParam("semitones", 5.0);
        effect.Reset();
        const auto input = Harmonic(146.83, 1.0, kSampleRate);
        const size_t settle = static_cast<size_t>(kSampleRate * 0.3) / kBlockSize * kBlockSize;
        const auto out = Render(effect, input, [&](size_t pos) {
            if (pos == settle)
            {
                effect.SetParam("semitones", 0.0);
            }
        });

        // The fade back to the bypass is 10 ms; from 20 ms on the output is the input itself.
        const size_t from = settle + static_cast<size_t>(kSampleRate * 0.02);
        bool identical = true;

        for (size_t i = from; i < out.size() - kBlockSize; ++i)
        {
            identical = identical && out[i] == input[i];
        }

        Check(identical, "Settled at 0 st, the output is the input, bit for bit");
        Check(effect.GetLatencySamples() == 0, "0 st reports no latency", std::to_string(effect.GetLatencySamples()));
    }

    {
        PitchShiftEffect effect;
        PrepareFree(effect, 1.0);
        effect.SetParam("semitones", -7.0);
        effect.SetParam("mix", 0.0);
        effect.Reset();
        const int latency = effect.GetLatencySamples();
        const auto input = Harmonic(110.0, 0.5, kSampleRate);
        const auto out = Render(effect, input);
        bool aligned = latency > 0;

        for (size_t i = static_cast<size_t>(latency) + 1000; i < out.size() - kBlockSize; ++i)
        {
            aligned = aligned && out[i] == input[i - static_cast<size_t>(latency)];
        }

        Check(aligned, "Mix 0 plays the input delayed by exactly the reported latency", std::to_string(latency));
        // Between the shortest and longest delays the tap sits at, rather than a number for a host.
        Check(latency >= static_cast<int>(kSampleRate * 0.005) && latency <= static_cast<int>(kSampleRate * 0.015),
              "Low Latency reports 5-15 ms", Num(latency * 1000.0 / kSampleRate, 1) + " ms");
        effect.SetParam("semitones", 12.0);
        Check(effect.GetLatencySamples() == latency, "and the same at every shift");
        effect.SetParam("engine", 0.0);
        Check(effect.GetLatencySamples() >= static_cast<int>(kSampleRate * 0.08), "High Quality reports its 80 ms",
              std::to_string(effect.GetLatencySamples()));
    }
}

/// The largest sample-to-sample step in the output while semitones and engine flip under a sine.
double LargestStep(const std::vector<std::pair<double, double>>& schedule)
{
    PitchShiftEffect effect;
    PrepareFree(effect, schedule.front().second);
    effect.SetParam("semitones", schedule.front().first);
    effect.Reset();

    const auto input = Sine(220.0, 4.0, 0.5);
    const size_t every = static_cast<size_t>(kSampleRate * 0.11) / kBlockSize * kBlockSize;
    size_t index = 0;
    const auto out = Render(effect, input, [&](size_t pos) {
        if (pos > 0 && pos % every == 0)
        {
            index = (index + 1) % schedule.size();
            effect.SetParam("semitones", schedule[index].first);
            effect.SetParam("engine", schedule[index].second);
        }
    });

    double largest = 0.0;

    // Past High Quality's first warm-up, which starts from an empty engine.
    for (size_t i = static_cast<size_t>(kSampleRate * 0.2); i < out.size(); ++i)
    {
        largest = std::max(largest, static_cast<double>(std::fabs(out[i] - out[i - 1])));
    }

    return largest;
}

void TestNoClicks()
{
    std::cout << "\n--- Changing path crossfades ---\n";
    // A 0.5 sine at 220 Hz shifted +5 st moves at most 0.019 a sample; a hard switch between two
    // copies of it out of step moves up to 1.0.
    constexpr double kLimit = 0.04;
    const double bypass = LargestStep({{0.0, 1.0}, {5.0, 1.0}});
    Check(bypass < kLimit, "Low Latency in and out of the 0 st bypass", "largest step " + Num(bypass, 4));
    const double bypassHighQuality = LargestStep({{0.0, 0.0}, {5.0, 0.0}});
    Check(bypassHighQuality < kLimit, "High Quality in and out of the 0 st bypass",
          "largest step " + Num(bypassHighQuality, 4));
    const double engines = LargestStep({{5.0, 0.0}, {5.0, 1.0}, {-3.0, 1.0}, {0.0, 0.0}, {-3.0, 0.0}});
    Check(engines < kLimit, "Switching engine, with the bypass in between", "largest step " + Num(engines, 4));
}

void TestReEngageIsClean()
{
    std::cout << "\n--- A re-engaged Low Latency engine plays nothing stale ---\n";
    PitchShiftEffect effect;
    PrepareFree(effect, 1.0);
    effect.SetParam("semitones", -5.0);
    effect.Reset();

    const auto loud = Sine(220.0, 1.0, 0.5);
    const std::vector<float> silence(static_cast<size_t>(kSampleRate * 0.5), 0.0f);
    (void)Render(effect, loud);
    effect.SetParam("semitones", 0.0);
    (void)Render(effect, silence);
    effect.SetParam("semitones", -5.0);
    const auto out = Render(effect, silence);
    double peak = 0.0;

    for (float v : out)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }

    Check(peak < 1.0e-3, "Silence after the bypass stays silent", "peak " + Num(peak, 6));
}

void TestNoAudioThreadAllocations()
{
    std::cout << "\n--- Nothing allocates on the audio thread ---\n";
    PitchShiftEffect effect;
    PrepareFree(effect, 1.0);
    effect.SetParam("semitones", 3.0);
    effect.Reset();
    const auto input = Harmonic(110.0, 3.0, kSampleRate);
    // Warm both engines once, off the audio thread: the first High Quality engage is where a
    // lazily sized buffer would show.
    (void)Render(effect, input, [&](size_t pos) { effect.SetParam("engine", (pos / 4800) % 2 ? 0.0 : 1.0); });

    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);
    // Made here, as automation holds its bound parameter ids: a Debug std::string allocates an
    // iterator-debugging proxy when it is constructed, which would be the test's allocation.
    const std::string engineKey = "engine";
    const std::string semitonesKey = "semitones";

    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        for (size_t pos = 0; pos + kBlockSize <= input.size(); pos += kBlockSize)
        {
            if (pos % 3200 == 0)
            {
                const auto step = pos / 3200;
                effect.SetParam(engineKey, step % 3 == 0 ? 0.0 : 1.0);
                effect.SetParam(semitonesKey, static_cast<double>(static_cast<int>(step % 7) * 4 - 12));
            }

            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inL.begin());
            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inR.begin());
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            effect.Process(inputs, outputs, kBlockSize);
        }
    });

    Check(allocations.count == 0, "Engine switches, shifts and splices",
          audio_thread_allocations::Describe(allocations));
}

void TestRegistration()
{
    std::cout << "\n--- The Engine parameter ---\n";
    guitarfx::RegisterAllEffects();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kPitchShift);
    const guitarfx::ParameterDef* engine = nullptr;

    if (info)
    {
        for (const auto& param : info->parameters)
        {
            if (param.id == "engine")
            {
                engine = &param;
            }
        }
    }

    Check(engine != nullptr && engine->defaultValue == 0.0 && engine->labels.size() == 2,
          "Registered, defaulting to High Quality so older presets sound the same");

    PitchShiftEffect effect;
    Check(effect.GetParam("engine") == 0.0, "A fresh node is on High Quality");
    effect.SetParam("engine", 1.0);
    Check(effect.GetParam("engine") == 1.0, "and reads back Low Latency once set");
}
} // namespace

int main()
{
    std::cout << "Pitch Shift engine tests\n";
    TestRegistration();
    TestStepLag();
    TestPitchAccuracy();
    TestTransparencyAndAlignment();
    TestNoClicks();
    TestReEngageIsClean();
    TestNoAudioThreadAllocations();

    std::cout << "\n" << (gFailures == 0 ? "All passed" : std::to_string(gFailures) + " failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
