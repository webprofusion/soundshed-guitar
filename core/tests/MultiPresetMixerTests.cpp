#include "dsp/MultiPresetMixer.h"
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

  return allPassed ? 0 : 1;
}
