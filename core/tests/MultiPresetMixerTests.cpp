#include "dsp/MultiPresetMixer.h"
#include "dsp/EffectGuids.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace guitarfx;

static constexpr double kTestSampleRate = 48000.0;
static constexpr int kTestBlockSize = 64;

static Preset MakePassthroughPreset(const std::string& id)
{
  Preset preset;
  preset.id = id;
  preset.name = id;

  GraphNode in;
  in.id = "in";
  in.type = kNodeTypeInput;

  GraphNode out;
  out.id = "out";
  out.type = kNodeTypeOutput;

  GraphEdge e;
  e.from = in.id;
  e.to = out.id;

  preset.graph.nodes = { in, out };
  preset.graph.edges = { e };
  return preset;
}

static Preset MakeLinearPreset(const std::string& id, const std::vector<std::string>& nodeTypes)
{
  Preset preset;
  preset.id = id;
  preset.name = id;

  GraphNode in{ "in", kNodeTypeInput, "", "Input", true };
  GraphNode out{ "out", kNodeTypeOutput, "", "Output", true };

  preset.graph.nodes.push_back(in);
  std::string prevId = in.id;
  int idx = 0;
  for (const auto& type : nodeTypes)
  {
    GraphNode node;
    node.id = "n" + std::to_string(idx++);
    node.type = type;
    node.enabled = true;
    preset.graph.nodes.push_back(node);

    GraphEdge edge{ prevId, node.id, 0, 0, 1.0 };
    preset.graph.edges.push_back(edge);
    prevId = node.id;
  }

  preset.graph.nodes.push_back(out);
  preset.graph.edges.push_back({ prevId, out.id, 0, 0, 1.0 });
  return preset;
}

static bool GraphHasNodeType(const SignalGraph& graph, const std::string& nodeType)
{
  return std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const GraphNode& node)
  {
    return node.type == nodeType;
  });
}

int main()
{
  bool allPassed = true;

  // Pan/mix smoke test
  {
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    auto pL = MakePassthroughPreset("pL");
    auto pR = MakePassthroughPreset("pR");
    const bool okL = mixer.AddActivePreset(pL, "pL", "LeftPreset");
    const bool okR = mixer.AddActivePreset(pR, "pR", "RightPreset");
    if (!okL || !okR)
    {
      std::cerr << "Failed to add passthrough presets" << std::endl;
      return 1;
    }

    mixer.SetPresetPan("pL", -1.0); // hard left
    mixer.SetPresetPan("pR", +1.0); // hard right
    mixer.SetPresetMix("pL", 0.5);
    mixer.SetPresetMix("pR", 0.5);

    std::vector<float> inL(static_cast<size_t>(kTestBlockSize), 1.0f);
    std::vector<float> inR(static_cast<size_t>(kTestBlockSize), 1.0f);
    std::vector<float> outL(static_cast<size_t>(kTestBlockSize), 0.0f);
    std::vector<float> outR(static_cast<size_t>(kTestBlockSize), 0.0f);

    float *inputs[2] = {inL.data(), inR.data()};
    float *outputs[2] = {outL.data(), outR.data()};

    mixer.Process(inputs, outputs, kTestBlockSize);

    for (int i = 0; i < kTestBlockSize; ++i)
    {
      if (std::fabs(outL[static_cast<size_t>(i)] - 0.5f) > 1e-4f ||
          std::fabs(outR[static_cast<size_t>(i)] - 0.5f) > 1e-4f)
      {
        std::cerr << "Pan/mix mismatch at sample " << i << ": L=" << outL[static_cast<size_t>(i)]
                  << " R=" << outR[static_cast<size_t>(i)] << std::endl;
        allPassed = false;
        break;
      }
    }

    if (allPassed)
    {
      std::cout << "MultiPresetMixer pan/mix test passed" << std::endl;
    }
  }

  // Switching processing mode should not change output for the same input block
  {
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    auto pL = MakePassthroughPreset("threadL");
    auto pR = MakePassthroughPreset("threadR");
    if (!mixer.AddActivePreset(pL, "threadL", "ThreadLeft")
        || !mixer.AddActivePreset(pR, "threadR", "ThreadRight"))
    {
      std::cerr << "Failed to add threading-mode presets" << std::endl;
      allPassed = false;
    }

    mixer.SetPresetPan("threadL", -1.0);
    mixer.SetPresetPan("threadR", +1.0);
    mixer.SetPresetMix("threadL", 0.5);
    mixer.SetPresetMix("threadR", 0.5);

    std::vector<float> inL(static_cast<size_t>(kTestBlockSize), 0.25f);
    std::vector<float> inR(static_cast<size_t>(kTestBlockSize), 0.25f);
    std::vector<float> outMtL(static_cast<size_t>(kTestBlockSize), 0.0f);
    std::vector<float> outMtR(static_cast<size_t>(kTestBlockSize), 0.0f);
    std::vector<float> outStL(static_cast<size_t>(kTestBlockSize), 0.0f);
    std::vector<float> outStR(static_cast<size_t>(kTestBlockSize), 0.0f);

    float *inputs[2] = {inL.data(), inR.data()};
    float *outputsMt[2] = {outMtL.data(), outMtR.data()};
    float *outputsSt[2] = {outStL.data(), outStR.data()};

    mixer.SetMultiThreadedProcessingEnabled(true);
    mixer.Process(inputs, outputsMt, kTestBlockSize);

    mixer.SetMultiThreadedProcessingEnabled(false);
    mixer.Process(inputs, outputsSt, kTestBlockSize);

    for (int i = 0; i < kTestBlockSize; ++i)
    {
      if (std::fabs(outMtL[static_cast<size_t>(i)] - outStL[static_cast<size_t>(i)]) > 1e-6f
          || std::fabs(outMtR[static_cast<size_t>(i)] - outStR[static_cast<size_t>(i)]) > 1e-6f)
      {
        std::cerr << "Processing mode output mismatch at sample " << i
                  << ": MT(L=" << outMtL[static_cast<size_t>(i)]
                  << ", R=" << outMtR[static_cast<size_t>(i)]
                  << ") ST(L=" << outStL[static_cast<size_t>(i)]
                  << ", R=" << outStR[static_cast<size_t>(i)] << ")" << std::endl;
        allPassed = false;
        break;
      }
    }

    if (allPassed)
    {
      std::cout << "MultiPresetMixer processing mode switch test passed" << std::endl;
    }
  }

  // Multiple signal-path replacements should leave only the final nodes in the mixer DSP
  {
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    const std::string presetId = "slotA";
    const auto pathA = MakeLinearPreset("pathA", {"amp_nam", "ir_cab"});
    const auto pathB = MakeLinearPreset("pathB", {"amp_nam"});
    const auto pathC = MakeLinearPreset("pathC", {"eq_parametric"});

    if (!mixer.AddActivePreset(pathA, presetId, "PathA"))
    {
      std::cerr << "Failed to add initial preset instance" << std::endl;
      allPassed = false;
    }

    mixer.RemoveActivePreset(presetId);
    if (!mixer.AddActivePreset(pathB, presetId, "PathB"))
    {
      std::cerr << "Failed to add second preset instance" << std::endl;
      allPassed = false;
    }

    mixer.RemoveActivePreset(presetId);
    if (!mixer.AddActivePreset(pathC, presetId, "PathC"))
    {
      std::cerr << "Failed to add final preset instance" << std::endl;
      allPassed = false;
    }

    const auto nodeTypes = mixer.GetPresetNodeTypes(presetId);
    std::vector<std::string> expected = {kNodeTypeInput, "eq_parametric", kNodeTypeOutput};
    auto sortedActual = nodeTypes;
    auto sortedExpected = expected;
    std::sort(sortedActual.begin(), sortedActual.end());
    std::sort(sortedExpected.begin(), sortedExpected.end());

    if (sortedActual != sortedExpected)
    {
      std::cerr << "Final mixer node types mismatch. Expected {input, eq_parametric, output} but got: ";
      for (const auto &t : nodeTypes)
      {
        std::cerr << t << ' ';
      }
      std::cerr << std::endl;
      allPassed = false;
    }
    else
    {
      std::cout << "MultiPresetMixer signal-path replacement test passed" << std::endl;
    }
  }

  // Global user input calibration should apply a fixed gain before the chain
  {
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    auto preset = MakePassthroughPreset("pCal");
    if (!mixer.AddActivePreset(preset, "pCal", "CalibrationPreset"))
    {
      std::cerr << "Failed to add calibration preset" << std::endl;
      allPassed = false;
    }

    mixer.SetUserInputCalibrationGainDb(6.0);

    std::vector<float> inL(static_cast<size_t>(kTestBlockSize), 0.25f);
    std::vector<float> inR(static_cast<size_t>(kTestBlockSize), 0.25f);
    std::vector<float> outL(static_cast<size_t>(kTestBlockSize), 0.0f);
    std::vector<float> outR(static_cast<size_t>(kTestBlockSize), 0.0f);

    float *inputs[2] = {inL.data(), inR.data()};
    float *outputs[2] = {outL.data(), outR.data()};

    mixer.Process(inputs, outputs, kTestBlockSize);

    const float expected = 0.25f
      * static_cast<float>(std::pow(10.0, 6.0 / 20.0))
      * static_cast<float>(std::sqrt(0.5));
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      if (std::fabs(outL[static_cast<size_t>(i)] - expected) > 1e-3f
          || std::fabs(outR[static_cast<size_t>(i)] - expected) > 1e-3f)
      {
        std::cerr << "Global user input calibration mismatch at sample " << i
                  << ": expected=" << expected
                  << " got L=" << outL[static_cast<size_t>(i)]
                  << " R=" << outR[static_cast<size_t>(i)] << std::endl;
        allPassed = false;
        break;
      }
    }

    if (allPassed)
    {
      std::cout << "MultiPresetMixer user input calibration gain test passed" << std::endl;
    }
  }

  // Loading a preset without embedded global chain settings must keep global transpose usable
  {
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kTestSampleRate, kTestBlockSize);

    auto config = GlobalSignalChainConfig::CreateDefault();
    config.preChainGraph = SignalGraph{};
    config.preChainGraph.nodes.push_back(GraphNode{"__input__", kNodeTypeInput, "utility", "Input", true});
    config.preChainGraph.nodes.push_back(GraphNode{"__output__", kNodeTypeOutput, "utility", "Output", true});
    config.preChainGraph.edges.push_back(GraphEdge{"__input__", "__output__", 0, 0, 1.0});
    mixer.SetGlobalChainConfig(config);

    // Simulate preset load path: preset itself has no globalSignalChain override
    auto preset = MakePassthroughPreset("pNoGlobals");
    if (!mixer.AddActivePreset(preset, "pNoGlobals", "NoGlobals"))
    {
      std::cerr << "Failed to add no-globals preset" << std::endl;
      allPassed = false;
    }

    mixer.SetTranspose(3);

    const auto normalized = mixer.GetGlobalChainConfig();
    if (!GraphHasNodeType(normalized.preChainGraph, EffectGuids::kDynamicsGate)
        || !GraphHasNodeType(normalized.preChainGraph, EffectGuids::kTranspose))
    {
      std::cerr << "Global pre-chain lost required gate/transpose nodes" << std::endl;
      allPassed = false;
    }
    else
    {
      std::cout << "MultiPresetMixer optional-global-chain regression test passed" << std::endl;
    }
  }

  return allPassed ? 0 : 1;
}
