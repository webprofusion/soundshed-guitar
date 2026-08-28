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

    if (gAllPassed)
    {
        std::cout << "GaplessSwitchingTests passed" << std::endl;
    }
    return gAllPassed ? 0 : 1;
}
