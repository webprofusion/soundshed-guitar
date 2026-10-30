// A swap between chains of different latency: the incoming chain warms up before it is faded
// in, and the crossfade spans the latency difference. A player heard a glitch switching scenes
// to and from one with a pitch shifter and a chorus. See PresetVoicePool::TimeSwap.

#include "dsp/MultiPresetMixer.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace guitarfx;

namespace
{
constexpr double kTestSampleRate = 48000.0;
constexpr int kTestBlockSize = 64;

bool gAllPassed = true;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        gAllPassed = false;
    }
}

/// input -> gain(gainDb) -> output. Stateless and exactly predictable, so a crossfade
/// between two of these has a closed-form expected value at every sample.
Preset MakeGainPreset(const std::string& id, double gainDb)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    in.enabled = true;

    GraphNode gain;
    gain.id = "g";
    gain.type = EffectGuids::kGain;
    gain.enabled = true;
    gain.params["gainDb"] = gainDb;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;
    out.enabled = true;

    preset.graph.nodes = {in, gain, out};
    preset.graph.edges = {{in.id, gain.id, 0, 0, 1.0}, {gain.id, out.id, 0, 0, 1.0}};
    return preset;
}

/// Runs blocks of DC input through the mixer and appends the left output to `captured`.
void PumpDc(MultiPresetMixer& mixer, int blocks, std::vector<float>& captured, float dc = 1.0f)
{
    std::vector<float> inL(static_cast<size_t>(kTestBlockSize), dc);
    std::vector<float> inR(static_cast<size_t>(kTestBlockSize), dc);
    std::vector<float> outL(static_cast<size_t>(kTestBlockSize), 0.0f);
    std::vector<float> outR(static_cast<size_t>(kTestBlockSize), 0.0f);

    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    for (int b = 0; b < blocks; ++b)
    {
        mixer.Process(inputs, outputs, kTestBlockSize);
        captured.insert(captured.end(), outL.begin(), outL.end());
    }
}

/// input -> pitch shift (+12 st, half mix) -> chorus -> output: the scene a player reported a
/// glitch switching to and from. The pitch shift's latency is the point: 16 ms on Low Latency
/// (engine 1), 80 ms on High Quality (engine 0).
Preset MakePitchChorusPreset(const std::string& id, double engine)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    in.enabled = true;

    GraphNode pitch;
    pitch.id = "ps";
    pitch.type = EffectGuids::kPitchShift;
    pitch.enabled = true;
    pitch.params["semitones"] = 12.0;
    pitch.params["mix"] = 0.5;
    pitch.params["engine"] = engine;

    GraphNode chorus;
    chorus.id = "ch";
    chorus.type = EffectGuids::kChorus;
    chorus.enabled = true;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;
    out.enabled = true;

    preset.graph.nodes = {in, pitch, chorus, out};
    preset.graph.edges = {
        {in.id, pitch.id, 0, 0, 1.0}, {pitch.id, chorus.id, 0, 0, 1.0}, {chorus.id, out.id, 0, 0, 1.0}};
    return preset;
}

/// Plays a steady G3 with a little third harmonic through `from`, swaps to `to` as a scene
/// switch does (same id), and checks the 200 ms after the switch against the steady state
/// either side: no click (the second difference no larger than either side's own) and no gap
/// (no 5 ms window quieter than half the quieter side).
void CheckLatentSwapIsSeamless(const std::string& label, const Preset& from, const Preset& to)
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);
    Check(mixer.AddActivePreset(from, from.id, from.id), label + ": add the outgoing scene");

    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
    constexpr std::size_t kWindow = 9600; // 200 ms
    const double increment = kTwoPi * 196.0 / kTestSampleRate;
    double phase = 0.0;
    std::vector<float> output;

    const auto pump = [&](int blocks) {
        std::vector<float> inL(static_cast<size_t>(kTestBlockSize), 0.0f);
        std::vector<float> inR(static_cast<size_t>(kTestBlockSize), 0.0f);
        std::vector<float> outL(static_cast<size_t>(kTestBlockSize), 0.0f);
        std::vector<float> outR(static_cast<size_t>(kTestBlockSize), 0.0f);
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kTestBlockSize; ++i, phase += increment)
            {
                const auto sample = 0.4f * static_cast<float>(std::sin(phase) + 0.3 * std::sin(3.0 * phase));
                inL[static_cast<size_t>(i)] = sample;
                inR[static_cast<size_t>(i)] = sample;
            }

            mixer.Process(inputs, outputs, kTestBlockSize);
            output.insert(output.end(), outL.begin(), outL.end());
        }
    };

    pump(300);
    const std::size_t switchAt = output.size();
    mixer.PreparePresetSwap(to, to.id, to.id);
    mixer.CommitPresetSwap();
    pump(450);

    const auto secondDifference = [&](std::size_t from, std::size_t to) {
        float worst = 0.0f;

        for (std::size_t i = std::max<std::size_t>(from, 2); i < to; ++i)
        {
            worst = std::max(worst, std::fabs(output[i] - 2.0f * output[i - 1] + output[i - 2]));
        }

        return worst;
    };
    const auto rms = [&](std::size_t from, std::size_t count) {
        double sum = 0.0;

        for (std::size_t i = from; i < from + count; ++i)
        {
            sum += static_cast<double>(output[i]) * output[i];
        }

        return std::sqrt(sum / static_cast<double>(count));
    };

    const float steady = std::max(secondDifference(switchAt - kWindow, switchAt),
                                  secondDifference(switchAt + kWindow, switchAt + 2 * kWindow));
    const float during = secondDifference(switchAt, switchAt + kWindow);
    Check(during <= steady * 1.25f, label + ": no click across the switch (second difference " +
                                        std::to_string(during) + ", steady " + std::to_string(steady) + ")");

    const double quieterSide = std::min(rms(switchAt - 4800, 4800), rms(switchAt + kWindow, 4800));
    double quietest = quieterSide;

    for (std::size_t at = switchAt; at + 240 <= switchAt + kWindow; at += 240)
    {
        quietest = std::min(quietest, rms(at, 240));
    }

    Check(quietest >= 0.5 * quieterSide, label + ": no gap across the switch (quietest 5 ms " +
                                             std::to_string(quietest) + ", quieter side " +
                                             std::to_string(quieterSide) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// A swap into a chain with latency waits for it to fill before fading it in (a fresh pitch
// shifter is silent for its latency, which used to leave a gap and then a step), and a swap
// out of one crossfades across the latency difference rather than skipping it.
// ─────────────────────────────────────────────────────────────────────────────
void TestSwapsLineUpWithChainLatency()
{
    CheckLatentSwapIsSeamless("dry -> low-latency pitch", MakeGainPreset("A", 0.0), MakePitchChorusPreset("A", 1.0));
    CheckLatentSwapIsSeamless("dry -> high-quality pitch", MakeGainPreset("A", 0.0), MakePitchChorusPreset("A", 0.0));
    CheckLatentSwapIsSeamless("low-latency pitch -> dry", MakePitchChorusPreset("A", 1.0), MakeGainPreset("A", 0.0));
    CheckLatentSwapIsSeamless("high-quality pitch -> dry", MakePitchChorusPreset("A", 0.0), MakeGainPreset("A", 0.0));

    // The outgoing 80 ms chain is still fading 60 ms on: the fade spans the difference.
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);
    Check(mixer.AddActivePreset(MakePitchChorusPreset("A", 0.0), "A", "A"), "add the high-quality pitch scene");
    std::vector<float> ignored;
    PumpDc(mixer, 4, ignored, 0.1f);
    mixer.PreparePresetSwap(MakeGainPreset("A", 0.0), "A", "A");
    mixer.CommitPresetSwap();
    PumpDc(mixer, 45, ignored, 0.1f);
    Check(mixer.GetRetiringPresetCount() == 1, "the outgoing chain's fade spans its extra latency");
    PumpDc(mixer, 20, ignored, 0.1f);
    Check(mixer.GetRetiringPresetCount() == 0, "and then ends");
}
} // namespace

int main()
{
    // Without this the registry hands out a PassthroughProcessor for every unknown type.
    RegisterAllEffects();

    TestSwapsLineUpWithChainLatency();

    if (gAllPassed)
    {
        std::cout << "SwapLatencyAlignmentTests passed" << std::endl;
    }

    return gAllPassed ? 0 : 1;
}
