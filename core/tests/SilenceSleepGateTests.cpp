// SilenceSleepGate: the mixer stops running its chains after a stretch of silence in and out,
// and picks up again on the first block anything is played, with nothing lost.

#include "dsp/EffectGuids.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/SilenceSleepGate.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

Preset MakeLinearPreset(const std::string& id, const std::vector<std::string>& nodeTypes)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};
    preset.graph.nodes.push_back(in);
    std::string previous = in.id;
    int index = 0;

    for (const auto& type : nodeTypes)
    {
        GraphNode node;
        node.id = "n" + std::to_string(index++);
        node.type = type;
        node.enabled = true;
        preset.graph.nodes.push_back(node);
        preset.graph.edges.push_back({previous, node.id, 0, 0, 1.0});
        previous = node.id;
    }

    preset.graph.nodes.push_back(out);
    preset.graph.edges.push_back({previous, out.id, 0, 0, 1.0});
    return preset;
}

struct Rig
{
    MultiPresetMixer mixer;
    ResourceLibrary library;
    std::vector<float> inL = std::vector<float>(kBlock, 0.0f);
    std::vector<float> inR = std::vector<float>(kBlock, 0.0f);
    std::vector<float> outL = std::vector<float>(kBlock, 0.0f);
    std::vector<float> outR = std::vector<float>(kBlock, 0.0f);

    explicit Rig(const Preset& preset)
    {
        mixer.SetResourceLibrary(&library);
        mixer.Prepare(kSampleRate, kBlock);
        mixer.AddActivePreset(preset, preset.id, preset.name);
    }

    void Fill(float value)
    {
        std::fill(inL.begin(), inL.end(), value);
        std::fill(inR.begin(), inR.end(), value);
    }

    /// Runs `seconds` of blocks of the current input; returns the loudest output sample.
    float Run(double seconds)
    {
        float peak = 0.0f;
        const int blocks = static_cast<int>(std::ceil(seconds * kSampleRate / kBlock));

        for (int b = 0; b < blocks; ++b)
        {
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            mixer.Process(inputs, outputs, kBlock);
            peak = std::max(peak, SilenceSleepGate::PeakOf(outL.data(), outR.data(), kBlock));
        }

        return peak;
    }
};

void TestGateOnItsOwn()
{
    SilenceSleepGate gate;
    gate.Prepare(kSampleRate);

    // Quiet in and out for just under the hold: still awake.
    const int holdBlocks = static_cast<int>(SilenceSleepGate::kHoldSeconds * kSampleRate / kBlock);

    for (int b = 0; b < holdBlocks - 1; ++b)
    {
        Check(!gate.ShouldSkip(0.0f, true), "awake before the hold has passed");
        gate.NoteProcessed(0.0f, 0.0f, kBlock, false);
    }

    Check(!gate.IsAsleep(), "not asleep one block short of the hold");
    gate.NoteProcessed(0.0f, 0.0f, kBlock, false);
    gate.NoteProcessed(0.0f, 0.0f, kBlock, false);
    Check(gate.IsAsleep(), "asleep after the hold");
    Check(gate.ShouldSkip(0.0f, true), "skips silence while asleep");
    Check(!gate.ShouldSkip(0.01f, true), "playing wakes it");
    Check(!gate.IsAsleep(), "awake after playing");

    // Audible output (an amp turning input hiss into hiss) keeps it awake however long it lasts.
    gate.Wake();

    for (int b = 0; b < holdBlocks * 3; ++b)
    {
        gate.NoteProcessed(5.0e-5f, 1.0e-3f, kBlock, false);
    }

    Check(!gate.IsAsleep(), "audible output keeps it awake");

    // Asleep over input hiss: the same hiss does not wake it, 6 dB more does.
    gate.Wake();

    for (int b = 0; b < holdBlocks + 2; ++b)
    {
        gate.NoteProcessed(5.0e-5f, 0.0f, kBlock, false);
    }

    Check(gate.IsAsleep(), "sleeps over quiet hiss when the output is silent");
    Check(gate.ShouldSkip(5.0e-5f, true), "the same hiss does not wake it");
    Check(!gate.ShouldSkip(1.1e-4f, true), "hiss 6 dB louder wakes it");

    // A graph that can ring out waits the longer hold.
    gate.Wake();

    for (int b = 0; b < holdBlocks + 2; ++b)
    {
        gate.NoteProcessed(0.0f, 0.0f, kBlock, true);
    }

    Check(!gate.IsAsleep(), "a ring-out graph waits past the ordinary hold");

    // Not allowed: never asleep, and the count starts again.
    for (int b = 0; b < holdBlocks * 6; ++b)
    {
        gate.NoteProcessed(0.0f, 0.0f, kBlock, true);
    }

    Check(gate.IsAsleep(), "a ring-out graph sleeps after the long hold");
    Check(!gate.ShouldSkip(0.0f, false), "not allowed wakes it");
    Check(!gate.IsAsleep(), "awake once not allowed");
}

void TestClassification()
{
    Check(!ClassifyGraphForSleep({kNodeTypeInput, kNodeTypeOutput}).canRingOut, "a passthrough cannot ring out");
    Check(ClassifyGraphForSleep({EffectGuids::kDelayDigital}).canRingOut, "a delay can ring out");
    Check(ClassifyGraphForSleep({EffectGuids::kReverbRoom}).canRingOut, "a reverb can ring out");
    Check(ClassifyGraphForSleep({EffectGuids::kPluginHost}).keepsAwake, "a hosted plugin keeps it awake");
    Check(ClassifyGraphForSleep({EffectGuids::kGuitarToMidi}).keepsAwake, "Guitar to MIDI keeps it awake");
    Check(!ClassifyGraphForSleep({EffectGuids::kDelayDigital}).keepsAwake, "a delay does not keep it awake");
}

void TestBypassedDelaysAndReverbsDoNotRingOut()
{
    SignalGraphExecutor executor;
    executor.SetGraph(MakeLinearPreset("rev", {EffectGuids::kReverbRoom}).graph);
    Check(executor.GetSleepTraits().canRingOut && !executor.AllRingOutNodesBypassed(), "a reverb that is on rings out");
    executor.SetNodeEnabled("n0", false);
    Check(executor.AllRingOutNodesBypassed(), "a reverb switched off rings nothing out");
    executor.SetNodeEnabled("n0", true);
    Check(!executor.AllRingOutNodesBypassed(), "switched back on, it rings out again");

    SignalGraphExecutor plain;
    plain.SetGraph(MakeLinearPreset("pass", {}).graph);
    Check(!plain.AllRingOutNodesBypassed(), "a graph with nothing to ring out is not 'all bypassed'");

    // The default global post-chain's Doubler, a delay, is off, so a plain chain sleeps after
    // the short hold, not the ring-out one.
    Rig rig(MakeLinearPreset("pass", {}));
    rig.Fill(0.0f);
    rig.Run(SilenceSleepGate::kHoldSeconds + 0.5);
    Check(rig.mixer.IsSleepingOnSilence(), "with the global Doubler off, the short hold applies");
}

void TestMixerSleepsAndWakesCleanly()
{
    // The same chain twice, one never sleeping: after a sleep, playing must come out exactly
    // as it does from a chain that ran all along, from the very block that wakes it.
    Rig rig(MakeLinearPreset("pass", {}));
    Rig awake(MakeLinearPreset("pass", {}));
    awake.mixer.SetSleepOnSilenceEnabled(false);

    rig.Fill(0.0f);
    awake.Fill(0.0f);
    rig.Run(0.5);
    awake.Run(0.5);
    Check(!rig.mixer.IsSleepingOnSilence(), "mixer awake after half a second of silence");
    // The long hold covers either hold.
    rig.Run(SilenceSleepGate::kRingOutHoldSeconds);
    awake.Run(SilenceSleepGate::kRingOutHoldSeconds);
    Check(rig.mixer.IsSleepingOnSilence(), "mixer asleep after the hold of silence");

    float worst = 0.0f;
    float loudest = 0.0f;

    for (int b = 0; b < 8; ++b)
    {
        // A burst of a sine, so the comparison covers more than a constant.
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.25f * static_cast<float>(std::sin(0.05 * (b * kBlock + i)));
            rig.inL[i] = rig.inR[i] = awake.inL[i] = awake.inR[i] = s;
        }

        rig.Run(kBlock / kSampleRate);
        awake.Run(kBlock / kSampleRate);

        for (int i = 0; i < kBlock; ++i)
        {
            worst = std::max(worst, std::fabs(rig.outL[i] - awake.outL[i]));
            worst = std::max(worst, std::fabs(rig.outR[i] - awake.outR[i]));
            loudest = std::max(loudest, std::fabs(rig.outL[i]));
        }
    }

    Check(!rig.mixer.IsSleepingOnSilence(), "playing wakes the mixer");
    Check(loudest > 0.01f, "the woken chain passes signal");
    Check(worst < 1e-5f,
          "output after waking matches a chain that never slept (max diff " + std::to_string(worst) + ")");
}

void TestMixerGateOff()
{
    Rig rig(MakeLinearPreset("pass", {}));
    rig.mixer.SetSleepOnSilenceEnabled(false);
    rig.Fill(0.0f);
    rig.Run(SilenceSleepGate::kRingOutHoldSeconds + 1.0);
    Check(!rig.mixer.IsSleepingOnSilence(), "never sleeps with the gate off");
}

void TestMixerHeldAwake()
{
    // A noise gate learning the noise floor holds the mixer awake: the quiet it listens to
    // would otherwise stop the chains, and the gate would hear nothing.
    Rig rig(MakeLinearPreset("pass", {}));
    rig.mixer.SetHoldAwake(true);
    rig.Fill(0.0f);
    rig.Run(SilenceSleepGate::kRingOutHoldSeconds + 1.0);
    Check(!rig.mixer.IsSleepingOnSilence(), "never sleeps while held awake");

    rig.mixer.SetHoldAwake(false);
    rig.Run(SilenceSleepGate::kRingOutHoldSeconds + 1.0);
    Check(rig.mixer.IsSleepingOnSilence(), "sleeps again once released");
}

void TestDelayRepeatsAreNotCut()
{
    Rig rig(MakeLinearPreset("delay", {EffectGuids::kDelayDigital}));

    // One loud block, then silence. The repeats must all be heard: the gate may not sleep
    // in a gap between them, and once it does the output must stay silent.
    rig.Fill(0.5f);
    rig.Run(kBlock / kSampleRate);
    rig.Fill(0.0f);

    rig.Run(1.5);
    Check(!rig.mixer.IsSleepingOnSilence(), "awake while a delay may still repeat");

    rig.Run(60.0);
    Check(rig.mixer.IsSleepingOnSilence(), "asleep once the repeats have died away");
}
} // namespace

int main()
{
    RegisterAllEffects();
    TestGateOnItsOwn();
    TestClassification();
    TestBypassedDelaysAndReverbsDoNotRingOut();
    TestMixerSleepsAndWakesCleanly();
    TestMixerGateOff();
    TestMixerHeldAwake();
    TestDelayRepeatsAreNotCut();

    if (gFailures != 0)
    {
        std::cerr << gFailures << " SilenceSleepGate check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "SilenceSleepGate tests passed" << std::endl;
    return 0;
}
