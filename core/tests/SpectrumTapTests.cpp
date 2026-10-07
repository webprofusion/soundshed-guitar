/**
 * @file SpectrumTapTests.cpp
 * @brief The spectrum drawn behind an EQ curve: what SpectrumTap measures, and which
 *        signal the executor and mixer hand it.
 *
 * The tap exists because the Signal Analyzer's spectrogram resolves no finer than the
 * host's block size, so the resolution test runs at a 32-sample block on purpose. The
 * executor tests pin down that the display shows the EQ's *input* -- the source the user
 * is EQing -- and that it survives the things that rebuild a node.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/SpectrumTap.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace guitarfx;
using Clock = std::chrono::steady_clock;

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;

bool Report(const std::string& label, bool passed, const std::string& detail = {})
{
    std::cout << "  " << std::left << std::setw(62) << (label + ":") << (passed ? "PASS" : "FAIL");

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";
    return passed;
}

std::string Db(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value << " dB";
    return out.str();
}

/// The display bin whose centre is closest to `hz` on the log axis.
int NearestBin(double hz)
{
    int best = 0;

    for (int bin = 1; bin < SpectrumTap::kBins; ++bin)
    {
        if (std::abs(std::log(SpectrumTap::BinFrequencyHz(bin) / hz)) <
            std::abs(std::log(SpectrumTap::BinFrequencyHz(best) / hz)))
        {
            best = bin;
        }
    }

    return best;
}

int LoudestBin(const SpectrumTap::Bins& bins)
{
    return static_cast<int>(std::max_element(bins.begin(), bins.end()) - bins.begin());
}

/// A continuous sine, handed out a block at a time.
class SineSource
{
  public:
    SineSource(double frequencyHz, double amplitude) : mFrequencyHz(frequencyHz), mAmplitude(amplitude)
    {
    }

    void Fill(std::vector<float>& block)
    {
        for (auto& sample : block)
        {
            sample = static_cast<float>(mAmplitude * std::sin(2.0 * kPi * mFrequencyHz * mTime));
            mTime += 1.0 / kSampleRate;
        }
    }

  private:
    double mFrequencyHz;
    double mAmplitude;
    double mTime = 0.0;
};

/// Pushes `seconds` of a sine into a tap in `blockSize` blocks.
void PushSine(SpectrumTap& tap, SineSource& source, int blockSize, double seconds)
{
    std::vector<float> block(static_cast<std::size_t>(blockSize));
    const int blocks = static_cast<int>(seconds * kSampleRate / blockSize);

    for (int i = 0; i < blocks; ++i)
    {
        source.Fill(block);
        tap.Push(block.data(), nullptr, blockSize);
    }
}

bool TestWindowSizing()
{
    std::cout << "\nWindow sizing\n";
    bool ok = true;
    ok &= Report("44.1 kHz uses 8192", SpectrumTap::FftSizeFor(44100.0) == 8192);
    ok &= Report("48 kHz uses 8192", SpectrumTap::FftSizeFor(48000.0) == 8192);
    ok &= Report("96 kHz uses 16384", SpectrumTap::FftSizeFor(96000.0) == 16384);
    ok &= Report("192 kHz is capped at 16384", SpectrumTap::FftSizeFor(192000.0) == 16384);
    ok &= Report("A nonsense rate still gets a window", SpectrumTap::FftSizeFor(0.0) == 8192);
    ok &= Report("Bins span 20 Hz to 20 kHz",
                 std::abs(SpectrumTap::BinFrequencyHz(0) - 20.0) < 1e-9 &&
                     std::abs(SpectrumTap::BinFrequencyHz(SpectrumTap::kBins - 1) - 20000.0) < 1e-6);
    return ok;
}

bool TestSineLandsInItsBinAtItsLevel()
{
    std::cout << "\nA sine lands in its bin, at its level\n";
    bool ok = true;

    SpectrumTap tap;
    SineSource sine(1000.0, 0.5); // -6 dBFS
    PushSine(tap, sine, 64, 0.5);

    SpectrumTap::Bins bins{};
    tap.Analyze(kSampleRate, bins);

    const int expectedBin = NearestBin(1000.0);
    const int loudest = LoudestBin(bins);
    ok &= Report("Loudest bin is the one nearest 1 kHz", std::abs(loudest - expectedBin) <= 1,
                 "bin " + std::to_string(loudest) + ", expected " + std::to_string(expectedBin));

    // The tilt is 3 dB an octave about 1 kHz, so allow for the bin centre not being exactly there.
    const double tilt =
        SpectrumTap::kTiltDbPerOctave * std::log2(SpectrumTap::BinFrequencyHz(loudest) / SpectrumTap::kTiltPivotHz);
    const double level = bins[static_cast<std::size_t>(loudest)];
    ok &= Report("A -6 dBFS sine reads about -6 dB", std::abs(level - (-6.0 + tilt)) < 1.5, Db(level));

    const double at100 = bins[static_cast<std::size_t>(NearestBin(100.0))];
    const double at10k = bins[static_cast<std::size_t>(NearestBin(10000.0))];
    ok &= Report("An octave-plus away there is nothing", at100 < -60.0 && at10k < -60.0,
                 "100 Hz " + Db(at100) + ", 10 kHz " + Db(at10k));
    return ok;
}

bool TestLowFrequenciesResolveAtTinyBlocks()
{
    std::cout << "\nLow frequencies resolve whatever the block size\n";
    bool ok = true;

    SpectrumTap tap;
    SineSource sine(100.0, 0.5);
    PushSine(tap, sine, 32, 0.5);

    SpectrumTap::Bins bins{};
    tap.Analyze(kSampleRate, bins);

    const int loudest = LoudestBin(bins);
    ok &= Report("A 100 Hz tone in 32-sample blocks peaks at 100 Hz", std::abs(loudest - NearestBin(100.0)) <= 1,
                 "peak at " + std::to_string(static_cast<int>(SpectrumTap::BinFrequencyHz(loudest))) + " Hz");

    const double peak = bins[static_cast<std::size_t>(loudest)];
    const double at400 = bins[static_cast<std::size_t>(NearestBin(400.0))];
    ok &= Report("Two octaves up is at least 40 dB down", peak - at400 >= 40.0, Db(peak) + " vs " + Db(at400));
    return ok;
}

bool TestSilenceAndGaps()
{
    std::cout << "\nSilence, gaps and restarts\n";
    bool ok = true;

    SpectrumTap tap;
    SineSource sine(1000.0, 0.5);
    PushSine(tap, sine, 256, 0.5);

    auto now = Clock::now();
    SpectrumTap::Bins bins{};
    tap.Analyze(kSampleRate, bins, now);
    const std::size_t bin = static_cast<std::size_t>(NearestBin(1000.0));
    const double playing = bins[bin];

    // A pause shorter than a big host block must not read as silence.
    now += std::chrono::milliseconds(100);
    tap.Analyze(kSampleRate, bins, now);
    ok &= Report("A short gap holds the level", std::abs(bins[bin] - playing) < 0.5,
                 Db(playing) + " -> " + Db(bins[bin]));

    for (int i = 0; i < 60; ++i)
    {
        now += std::chrono::milliseconds(33);
        tap.Analyze(kSampleRate, bins, now);
    }

    const bool allFloor =
        std::all_of(bins.begin(), bins.end(), [](float value) { return value < SpectrumTap::kFloorDb + 1.0; });
    ok &= Report("Two seconds without samples falls to the floor", allFloor, Db(bins[bin]));

    // The tone comes back: the display rises within a few 30 Hz frames.
    PushSine(tap, sine, 256, 0.2);

    for (int i = 0; i < 3; ++i)
    {
        now += std::chrono::milliseconds(33);
        tap.Analyze(kSampleRate, bins, now);
    }

    ok &= Report("A returning tone is back within three frames", std::abs(bins[bin] - playing) < 1.5, Db(bins[bin]));

    // Everything pushed before a restart is somebody else's signal.
    tap.Restart();
    tap.Analyze(kSampleRate, bins, now);
    ok &= Report("A restart forgets what was pushed before it", bins[bin] < SpectrumTap::kFloorDb + 1.0, Db(bins[bin]));

    std::vector<float> huge(70000, 0.25f);
    tap.Push(huge.data(), nullptr, static_cast<int>(huge.size()));
    tap.Push(nullptr, nullptr, 64);
    tap.Analyze(kSampleRate, bins, now);
    ok &= Report("A block longer than the ring is taken safely", std::all_of(bins.begin(), bins.end(), [](float v) {
                     return v >= SpectrumTap::kFloorDb && v <= SpectrumTap::kCeilingDb;
                 }));
    return ok;
}

/// in -> eq (1 kHz cut by 12 dB) -> out.
SignalGraph MakeEqGraph()
{
    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"eq", EffectGuids::kEqParametric, "eq", "EQ", true});
    graph.nodes.back().params["lowMidFreq"] = 1000.0;
    graph.nodes.back().params["lowMidGain"] = -12.0;
    graph.nodes.back().params["lowMidQ"] = 1.0;
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "eq", 0, 0, 1.0});
    graph.edges.push_back({"eq", "out", 0, 0, 1.0});
    return graph;
}

void RunExecutor(SignalGraphExecutor& executor, SineSource& sine, double seconds)
{
    constexpr int kBlock = 128;
    std::vector<float> inL(kBlock);
    std::vector<float> inR(kBlock, 0.0f);
    std::vector<float> outL(kBlock);
    std::vector<float> outR(kBlock);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    for (int i = 0; i < static_cast<int>(seconds * kSampleRate / kBlock); ++i)
    {
        sine.Fill(inL);
        executor.Process(inputs, outputs, kBlock);
    }
}

/// Reads as if a second had passed since the last read, so the display has fully settled on
/// whatever the executor last heard. Real reads in a test are microseconds apart.
double ReadLevelAt(SignalGraphExecutor& executor, double hz)
{
    static auto now = Clock::now();
    now += std::chrono::seconds(1);
    SpectrumTap::Bins bins{};
    executor.ReadWatchedSpectrum(bins, now);
    return bins[static_cast<std::size_t>(NearestBin(hz))];
}

bool TestExecutorTapsTheNodeInput()
{
    std::cout << "\nThe executor shows the EQ's input\n";
    bool ok = true;

    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Mono); // a guitar on one input
    executor.SetGraph(MakeEqGraph());
    executor.Prepare(kSampleRate, 128);
    SineSource sine(1000.0, 0.5);

    SpectrumTap::Bins bins{};
    ok &= Report("Nothing is read before a watch", !executor.ReadWatchedSpectrum(bins));
    ok &= Report("An unknown node cannot be watched", !executor.WatchNodeSpectrum("no_such_node"));
    ok &= Report("The EQ node can be watched", executor.WatchNodeSpectrum("eq"));

    RunExecutor(executor, sine, 0.5);
    const double atEq = ReadLevelAt(executor, 1000.0);
    ok &= Report("The EQ's spectrum is its input, before the cut", atEq > -9.0, Db(atEq));

    // The output node's input is the EQ's output, so watching it shows the cut.
    ok &= Report("Watching moves to another node", executor.WatchNodeSpectrum("out"));
    RunExecutor(executor, sine, 0.5);
    const double atOut = ReadLevelAt(executor, 1000.0);
    ok &= Report("After the EQ the tone is about 12 dB lower", std::abs((atEq - atOut) - 12.0) < 2.0, Db(atOut));

    // A bypassed EQ still has a source worth seeing.
    executor.WatchNodeSpectrum("eq");
    executor.SetNodeEnabled("eq", false);
    RunExecutor(executor, sine, 0.5);
    const double bypassed = ReadLevelAt(executor, 1000.0);
    ok &= Report("A bypassed EQ still shows its input", std::abs(bypassed - atEq) < 1.5, Db(bypassed));
    executor.SetNodeEnabled("eq", true);

    // A graph edit rebuilds every node state; the watch has to carry across it.
    executor.SetGraph(MakeEqGraph());
    executor.Prepare(kSampleRate, 128);
    SineSource quiet(1000.0, 0.05); // -26 dBFS
    RunExecutor(executor, quiet, 0.5);
    const double rebuilt = ReadLevelAt(executor, 1000.0);
    ok &=
        Report("A rebuilt graph keeps feeding the watched node", std::abs(rebuilt - (atEq - 20.0)) < 2.0, Db(rebuilt));

    executor.ClearSpectrumWatch();
    ok &= Report("Clearing the watch stops reads", !executor.ReadWatchedSpectrum(bins));

    // A removed node ends the watch.
    executor.WatchNodeSpectrum("eq");
    SignalGraph withoutEq;
    withoutEq.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    withoutEq.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    withoutEq.edges.push_back({"in", "out", 0, 0, 1.0});
    executor.SetGraph(withoutEq);
    ok &= Report("A node that is gone cannot be watched", !executor.WatchNodeSpectrum("eq"));
    return ok;
}

bool TestMixerFindsTheNodeByScope()
{
    std::cout << "\nThe mixer finds the node by scope\n";
    bool ok = true;

    MultiPresetMixer mixer;
    mixer.Prepare(kSampleRate, 128);

    Preset preset;
    preset.id = "presetA";
    preset.name = "presetA";
    preset.graph = MakeEqGraph();
    mixer.AddActivePreset(preset, "presetA", "presetA");

    constexpr int kBlock = 128;
    std::vector<float> inL(kBlock);
    std::vector<float> inR(kBlock, 0.0f);
    std::vector<float> outL(kBlock);
    std::vector<float> outR(kBlock);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    SineSource sine(1000.0, 0.5);

    // The mixer reads on the real clock, so wait out the display's attack before reading.
    const auto run = [&]() {
        for (int i = 0; i < static_cast<int>(0.5 * kSampleRate / kBlock); ++i)
        {
            sine.Fill(inL);
            mixer.Process(inputs, outputs, kBlock);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    };

    SpectrumTap::Bins bins{};
    ok &= Report("A preset node is found by preset id", mixer.ReadNodeSpectrum("preset", "presetA", "eq", bins));
    ok &= Report("An empty preset id means the live preset", mixer.ReadNodeSpectrum("preset", "", "eq", bins));
    ok &= Report("Another preset's id finds nothing", !mixer.ReadNodeSpectrum("preset", "presetB", "eq", bins));
    ok &= Report("An unknown scope finds nothing", !mixer.ReadNodeSpectrum("sideways", "", "eq", bins));

    run();
    mixer.ReadNodeSpectrum("preset", "presetA", "eq", bins);
    const double presetLevel = bins[static_cast<std::size_t>(NearestBin(1000.0))];
    ok &= Report("The preset EQ shows the tone", presetLevel > -30.0, Db(presetLevel));

    // The Global EQ lives in the post chain under a well-known id.
    mixer.ClearSpectrumTaps();
    ok &= Report("The Global EQ is found in the post chain", mixer.ReadNodeSpectrum("post", "", "global_eq", bins));
    run();
    mixer.ReadNodeSpectrum("post", "", "global_eq", bins);
    const double globalLevel = bins[static_cast<std::size_t>(NearestBin(1000.0))];
    ok &= Report("The Global EQ shows the chain's output", globalLevel > -60.0, Db(globalLevel));
    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;

    for (const auto& test : {TestWindowSizing, TestSineLandsInItsBinAtItsLevel, TestLowFrequenciesResolveAtTinyBlocks,
                             TestSilenceAndGaps, TestExecutorTapsTheNodeInput, TestMixerFindsTheNodeByScope})
    {
        if (!test())
        {
            allPassed = false;
        }
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
