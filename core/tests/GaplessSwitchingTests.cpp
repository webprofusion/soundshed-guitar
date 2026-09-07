// Phase 1 gapless-switching coverage: a preset swap must crossfade rather than cut, must
// stay hidden from every query while the outgoing instance rings down, and must not rebuild
// the global chains when nothing about them changed.
//
// See docs/plans/gapless-preset-switching.md.

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

// A single instance at the default centre pan is scaled by the equal-power pan law.
const float kCentrePanGain = static_cast<float>(std::cos(3.14159265358979323846 * 0.25));

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

/// input -> delay(mix 1.0, heavy feedback) -> output. The wet signal keeps circulating long
/// after the input stops, which is exactly the thing a preset switch must not cut off.
Preset MakeDelayPreset(const std::string& id, double timeMs, double feedback)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    in.enabled = true;

    GraphNode delay;
    delay.id = "d";
    delay.type = EffectGuids::kDelayDigital;
    delay.enabled = true;
    delay.params["time"] = timeMs;
    delay.params["feedback"] = feedback;
    delay.params["mix"] = 1.0;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;
    out.enabled = true;

    preset.graph.nodes = {in, delay, out};
    preset.graph.edges = {{in.id, delay.id, 0, 0, 1.0}, {delay.id, out.id, 0, 0, 1.0}};
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

/// Same, with a 440 Hz sine instead. `phase` carries across calls so the tone is continuous.
void PumpSine(MultiPresetMixer& mixer, int blocks, double& phase, float amplitude = 0.5f)
{
    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
    const double increment = kTwoPi * 440.0 / kTestSampleRate;

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
            inL[static_cast<size_t>(i)] = amplitude * static_cast<float>(std::sin(phase));
            inR[static_cast<size_t>(i)] = inL[static_cast<size_t>(i)];
        }

        mixer.Process(inputs, outputs, kTestBlockSize);
    }
}

float MaxAbs(const std::vector<float>& samples, std::size_t from)
{
    float peak = 0.0f;

    for (std::size_t i = from; i < samples.size(); ++i)
    {
        peak = std::max(peak, std::fabs(samples[i]));
    }

    return peak;
}

float MaxAbsDelta(const std::vector<float>& samples, std::size_t& atIndex)
{
    float worst = 0.0f;
    atIndex = 0;

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const float delta = std::fabs(samples[i] - samples[i - 1]);

        if (delta > worst)
        {
            worst = delta;
            atIndex = i;
        }
    }

    return worst;
}

// ─────────────────────────────────────────────────────────────────────────────
// A preset swap crossfades: no step discontinuity, no silent block, and the output
// settles on the incoming preset's level.
// ─────────────────────────────────────────────────────────────────────────────
void TestSwapCrossfadesWithoutDiscontinuity()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    Check(mixer.AddActivePreset(MakeGainPreset("A", 0.0), "A", "A"), "add preset A");

    std::vector<float> warmup;
    PumpDc(mixer, 4, warmup);
    const float steadyA = warmup.back();
    Check(std::fabs(steadyA - kCentrePanGain) < 1e-4f, "preset A settles at unity through the centre pan law");

    // -20 dB => 0.1 linear, an easily distinguished target level.
    mixer.PreparePresetSwap(MakeGainPreset("B", -20.0), "B", "B");
    mixer.CommitPresetSwap();

    std::vector<float> transition;
    PumpDc(mixer, 40, transition); // 2560 samples, comfortably past the 1024-sample ramp

    // The whole captured stream, warm-up included, must be free of steps.
    std::vector<float> stream = warmup;
    stream.insert(stream.end(), transition.begin(), transition.end());

    const float expectedFinal = kCentrePanGain * 0.1f;
    // Largest legitimate per-sample change is the ramp slope: (1.0 - 0.1) / 1024.
    const float rampSlope = kCentrePanGain * (1.0f - 0.1f) / 1024.0f;
    const float tolerance = rampSlope * 4.0f;

    std::size_t worstIndex = 0;
    const float worstDelta = MaxAbsDelta(stream, worstIndex);
    Check(worstDelta <= tolerance, "swap produces no step discontinuity (worst delta " + std::to_string(worstDelta) +
                                       " at sample " + std::to_string(worstIndex) + ", tolerance " +
                                       std::to_string(tolerance) + ")");

    // A hard cut used to drive the output to zero for at least one block.
    const float quietest = *std::min_element(stream.begin(), stream.end());
    Check(quietest > expectedFinal * 0.5f,
          "output never collapses toward silence during the swap (min " + std::to_string(quietest) + ")");

    // Monotone descent from A's level to B's level.
    Check(std::fabs(transition.back() - expectedFinal) < 1e-4f, "output settles on the incoming preset's level (got " +
                                                                    std::to_string(transition.back()) + ", want " +
                                                                    std::to_string(expectedFinal) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// The outgoing instance stays in the DSP graph while it fades, but must be invisible
// to every lookup — its ID often matches the incoming one on a scene switch.
// ─────────────────────────────────────────────────────────────────────────────
void TestRetiringInstanceHiddenFromQueries()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    Check(mixer.AddActivePreset(MakeGainPreset("same", 0.0), "same", "v1"), "add initial preset");

    // Same ID, as a scene switch on the active preset would produce.
    mixer.PreparePresetSwap(MakeGainPreset("same", -6.0), "same", "v2");
    mixer.CommitPresetSwap();

    Check(mixer.GetPresetCount() == 1, "retiring instance is not counted");

    const auto ids = mixer.GetActivePresetIds();
    Check(ids.size() == 1 && ids.front() == "same", "retiring instance does not duplicate its ID in the active list");

    const auto cfg = mixer.GetPresetConfig("same");
    Check(cfg.has_value() && cfg->name == "v2", "lookups resolve to the incoming instance, not the one fading out");

    // Parameter routing must reach the new instance.
    Check(mixer.GetNodeProcessor("same", "g") != nullptr, "node lookup resolves against the incoming instance");
}

// ─────────────────────────────────────────────────────────────────────────────
// Global chain staging: an unchanged config must not stage a rebuild at all.
// ─────────────────────────────────────────────────────────────────────────────
void TestUnchangedGlobalChainSkipsRebuild()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    // Prepare() has already built the chains, so the live config is normalized.
    auto config = mixer.GetGlobalChainConfig();

    Check(!mixer.PrepareGlobalChainSwap(config), "identical global chain config stages no rebuild");
    mixer.CommitGlobalChainSwap();

    // Any real graph change must stage one.
    Check(!config.preChainGraph.nodes.empty(), "default pre-chain graph is populated");
    config.preChainGraph.nodes.front().params["gainDb"] = 3.0;

    Check(mixer.PrepareGlobalChainSwap(config), "a changed global chain graph stages a rebuild");
    mixer.CommitGlobalChainSwap();

    Check(mixer.GetGlobalChainConfig().preChainGraph.nodes.front().params.at("gainDb") == 3.0,
          "committed global chain config is the one that was staged");

    // ...and re-applying it is now a no-op again.
    Check(!mixer.PrepareGlobalChainSwap(mixer.GetGlobalChainConfig()),
          "re-applying the committed config stages no rebuild");
    mixer.CommitGlobalChainSwap();
}

// ─────────────────────────────────────────────────────────────────────────────
// Replacing one slot of a multi-preset mix crossfades that slot and leaves the others
// running untouched.
// ─────────────────────────────────────────────────────────────────────────────
void TestInPlaceReplaceCrossfadesAndKeepsOtherSlots()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    Check(mixer.AddActivePreset(MakeGainPreset("slotA", -20.0), "slotA", "A"), "add slot A");
    Check(mixer.AddActivePreset(MakeGainPreset("slotB", -20.0), "slotB", "B"), "add slot B");

    std::vector<float> warmup;
    PumpDc(mixer, 4, warmup);

    Check(mixer.ReplaceActivePresetInPlace(MakeGainPreset("slotA", -6.0), "slotA", "A2"),
          "in-place replace succeeds for an active slot");

    Check(mixer.GetPresetCount() == 2, "both slots remain live across an in-place replace");
    const auto ids = mixer.GetActivePresetIds();
    Check(std::find(ids.begin(), ids.end(), "slotB") != ids.end(), "the untouched slot is still active");

    std::vector<float> transition;
    PumpDc(mixer, 40, transition);

    std::vector<float> stream = warmup;
    stream.insert(stream.end(), transition.begin(), transition.end());

    // slotA ramps 0.1 -> 0.501; slotB holds at 0.1. Slope is bounded by slotA's change.
    const float rampSlope = kCentrePanGain * 0.402f / 1024.0f;
    std::size_t worstIndex = 0;
    const float worstDelta = MaxAbsDelta(stream, worstIndex);
    Check(worstDelta <= rampSlope * 4.0f, "in-place replace produces no step discontinuity (worst delta " +
                                              std::to_string(worstDelta) + " at sample " + std::to_string(worstIndex) +
                                              ")");

    const float expectedFinal = kCentrePanGain * (0.1f + 0.5012f);
    Check(std::fabs(transition.back() - expectedFinal) < 2e-3f, "mix settles on replaced slot + untouched slot (got " +
                                                                    std::to_string(transition.back()) + ", want " +
                                                                    std::to_string(expectedFinal) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// Hammering the switch must not accumulate instances or produce non-finite output.
// ─────────────────────────────────────────────────────────────────────────────
void TestRapidSwitchingStaysBounded()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    Check(mixer.AddActivePreset(MakeGainPreset("p", 0.0), "p", "p0"), "add initial preset");

    std::vector<float> captured;

    for (int i = 0; i < 24; ++i)
    {
        mixer.PreparePresetSwap(MakeGainPreset("p", (i % 2) ? -12.0 : 0.0), "p", "p" + std::to_string(i));
        mixer.CommitPresetSwap();
        Check(mixer.GetPresetCount() == 1, "live instance count stays at one while switching");
        PumpDc(mixer, 2, captured); // switch again long before the previous ramp finishes
    }

    const bool allFinite = std::all_of(captured.begin(), captured.end(), [](float v) { return std::isfinite(v); });
    Check(allFinite, "rapid switching keeps the output finite");

    const float loudest = *std::max_element(captured.begin(), captured.end());
    Check(loudest <= kCentrePanGain * 1.05f,
          "overlapping fades never sum above unity (peak " + std::to_string(loudest) + ")");

    // Let everything drain, then confirm the mixer converged on the last preset.
    std::vector<float> settled;
    PumpDc(mixer, 40, settled);
    const float expected = kCentrePanGain * static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    Check(std::fabs(settled.back() - expected) < 1e-4f,
          "mixer converges on the final preset after the ramps drain (got " + std::to_string(settled.back()) +
              ", want " + std::to_string(expected) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// Tail spill: the preset you just left keeps ringing over the new one. Same switch,
// run twice, and the only difference is the tail budget.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<float> SwitchAwayFromDelay(double tailSeconds)
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);
    Check(mixer.AddActivePreset(MakeDelayPreset("echo", 120.0, 0.85), "echo", "echo"), "add the delay preset");

    // Half a second of playing: several repeats are circulating by the time we switch.
    double phase = 0.0;
    PumpSine(mixer, 400, phase);

    mixer.SetPresetSwapTailSeconds(tailSeconds);
    // Switch to something that makes no sound of its own, so whatever comes out afterwards
    // is the outgoing preset's tail and nothing else.
    mixer.PreparePresetSwap(MakeGainPreset("silent", -80.0), "silent", "silent");
    mixer.CommitPresetSwap();

    // ...and the player stops. 400 ms of silence in.
    std::vector<float> after;
    PumpDc(mixer, 300, after, 0.0f);
    return after;
}

void TestTailSpillCarriesTheDelayPastTheSwap()
{
    {
        MultiPresetMixer mixer;
        mixer.SetPresetSwapTailSeconds(-1.0);
        Check(mixer.GetPresetSwapTailSeconds() == 0.0, "a negative tail length reads as off");
        mixer.SetPresetSwapTailSeconds(1e6);
        Check(mixer.GetPresetSwapTailSeconds() == MultiPresetMixer::kMaxPresetSwapTailSeconds,
              "an absurd tail length is clamped to the ceiling");
    }

    // Everything past the 1024-sample declick ramp, which both runs share.
    constexpr std::size_t kPastTheCrossfade = 2048;

    const float withSpill = MaxAbs(SwitchAwayFromDelay(1.0), kPastTheCrossfade);
    const float withoutSpill = MaxAbs(SwitchAwayFromDelay(0.0), kPastTheCrossfade);

    Check(withSpill > 0.05f,
          "the delay keeps ringing for 400 ms after the switch (peak " + std::to_string(withSpill) + ")");
    Check(withoutSpill < 1e-4f,
          "with the spill off the same switch cuts it dead (peak " + std::to_string(withoutSpill) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// A preset with nothing to ring must not sit there costing a chain. It never enters
// the ring-out at all — the graph says up front that it has no tail to keep — and one
// that does ring is dropped as soon as it falls silent, long before the budget ends.
// ─────────────────────────────────────────────────────────────────────────────
void TestTailSpillOnlyKeepsWhatCanRing()
{
    const auto retiringAfter = [](const Preset& outgoing, int blocks) {
        MultiPresetMixer mixer;
        ResourceLibrary lib;
        mixer.SetResourceLibrary(&lib);
        mixer.Prepare(kTestSampleRate, kTestBlockSize);
        Check(mixer.AddActivePreset(outgoing, outgoing.id, outgoing.name), "add the outgoing preset");

        double phase = 0.0;
        PumpSine(mixer, 400, phase);

        mixer.SetPresetSwapTailSeconds(2.0);
        mixer.PreparePresetSwap(MakeGainPreset("next", 0.0), "next", "next");
        mixer.CommitPresetSwap();

        std::vector<float> after;
        PumpDc(mixer, blocks, after, 0.0f);
        return mixer.GetRetiringPresetCount();
    };

    // 40 blocks is ~53 ms: past the 1024-sample declick, nowhere near the 2 s budget or
    // even the silence window, so only an up-front decision can have retired it.
    Check(retiringAfter(MakeGainPreset("dry", 0.0), 40) == 0,
          "a preset with no delay or reverb is cut on the declick ramp, not run on");
    Check(retiringAfter(MakeDelayPreset("echo", 120.0, 0.85), 40) == 1, "a preset with a delay is kept ringing");
    // ...and the one that does ring still ends itself early once it decays, rather than
    // riding the whole budget.
    Check(retiringAfter(MakeDelayPreset("shortecho", 40.0, 0.2), 375) == 0,
          "a tail that has decayed is dropped well before the budget runs out");
}

// ─────────────────────────────────────────────────────────────────────────────
// The budget is the backstop for a tail that never decays on its own.
// ─────────────────────────────────────────────────────────────────────────────
void TestTailSpillStopsAtTheEndOfTheBudget()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);
    // Feedback at the top of the range: this one is still going when the budget runs out.
    Check(mixer.AddActivePreset(MakeDelayPreset("runaway", 120.0, 0.95), "runaway", "runaway"),
          "add the runaway delay preset");

    double phase = 0.0;
    PumpSine(mixer, 400, phase);

    mixer.SetPresetSwapTailSeconds(0.25);
    mixer.PreparePresetSwap(MakeGainPreset("next", 0.0), "next", "next");
    mixer.CommitPresetSwap();

    std::vector<float> during;
    PumpDc(mixer, 150, during, 0.0f); // 200 ms: inside the budget
    Check(mixer.GetRetiringPresetCount() == 1, "the tail is still running inside its budget");

    std::vector<float> after;
    PumpDc(mixer, 450, after, 0.0f); // a further 600 ms: budget plus the 250 ms release
    Check(mixer.GetRetiringPresetCount() == 0, "the tail is gone once the budget and its release run out");
}

// ─────────────────────────────────────────────────────────────────────────────
// Tails cost a whole chain each. Hammering the switch must not stack them up.
// ─────────────────────────────────────────────────────────────────────────────
void TestTailSpillStaysBoundedUnderRapidSwitching()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);
    Check(mixer.AddActivePreset(MakeDelayPreset("echo0", 120.0, 0.85), "echo0", "echo0"), "add the first preset");

    mixer.SetPresetSwapTailSeconds(4.0);

    double phase = 0.0;
    std::size_t worstRetiring = 0;

    for (int i = 1; i <= 24; ++i)
    {
        const auto id = "echo" + std::to_string(i);
        mixer.PreparePresetSwap(MakeDelayPreset(id, 120.0, 0.85), id, id);
        mixer.CommitPresetSwap();
        PumpSine(mixer, 2, phase); // switch again long before any tail has decayed
        Check(mixer.GetPresetCount() == 1, "live instance count stays at one while switching");
        worstRetiring = std::max(worstRetiring, mixer.GetRetiringPresetCount());
    }

    Check(worstRetiring <= 3, "simultaneous tails stay bounded (worst " + std::to_string(worstRetiring) + ")");

    std::vector<float> captured;
    PumpDc(mixer, 40, captured, 0.0f);
    const bool allFinite = std::all_of(captured.begin(), captured.end(), [](float v) { return std::isfinite(v); });
    Check(allFinite, "rapid switching with tails keeps the output finite");
}
} // namespace

int main()
{
    // Without this the registry hands out a PassthroughProcessor for every unknown type,
    // so the gain nodes below would silently run at unity and the tests would prove nothing.
    RegisterAllEffects();

    TestSwapCrossfadesWithoutDiscontinuity();
    TestRetiringInstanceHiddenFromQueries();
    TestUnchangedGlobalChainSkipsRebuild();
    TestInPlaceReplaceCrossfadesAndKeepsOtherSlots();
    TestRapidSwitchingStaysBounded();
    TestTailSpillCarriesTheDelayPastTheSwap();
    TestTailSpillOnlyKeepsWhatCanRing();
    TestTailSpillStopsAtTheEndOfTheBudget();
    TestTailSpillStaysBoundedUnderRapidSwitching();

    if (gAllPassed)
    {
        std::cout << "GaplessSwitchingTests passed" << std::endl;
    }

    return gAllPassed ? 0 : 1;
}
