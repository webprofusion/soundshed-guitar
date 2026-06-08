/**
 * Signal-chain mutation stress test.
 *
 * Rapidly mutates sample rate, mono/stereo mode, preset selection/mix state,
 * and effect node parameters while processing audio blocks.
 *
 * Goal: surface crash-prone transition sequences and report a reproducible trace.
 *
 * Environment overrides:
 *  - GUITARFX_STRESS_SEED=<uint64>
 *  - GUITARFX_STRESS_STEPS=<int>
 *  - GUITARFX_STRESS_BLOCK_SIZE=<int>
 *  - GUITARFX_STRESS_MAX_PRESETS=<int>
 *  - GUITARFX_STRESS_MODE=baseline|resource-rebind
 *  - GUITARFX_STRESS_TRACE_PATH=<path>
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace fs = std::filesystem;

namespace
{
constexpr double kPi = 3.14159265358979323846;

constexpr int kDefaultSteps = 4000;
constexpr int kDefaultBlockSize = 256;
constexpr int kDefaultMaxPresets = 10;
constexpr int kTraceHistoryLimit = 120;

const std::vector<double> kSampleRates = {
    22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0};

struct ParamMutationTarget
{
  std::string presetId;
  std::string nodeId;
  std::string nodeType;
  std::string paramId;
  double minValue = 0.0;
  double maxValue = 1.0;
  double step = 0.0;
};

struct NodeToggleTarget
{
  std::string presetId;
  std::string nodeId;
};

struct ResourceMutationTarget
{
  std::string presetId;
  std::string nodeId;
  std::string resourceType;
};

enum class StressMode
{
  Baseline,
  ResourceRebind,
};

struct ScenarioData
{
  std::vector<guitarfx::Preset> presets;
  std::vector<std::string> presetIds;
  std::vector<ParamMutationTarget> paramTargets;
  std::vector<NodeToggleTarget> toggleTargets;
  std::vector<ResourceMutationTarget> resourceTargets;
};

struct RunConfig
{
  std::uint64_t seed = 0;
  int steps = kDefaultSteps;
  int blockSize = kDefaultBlockSize;
  int maxPresets = kDefaultMaxPresets;
  StressMode mode = StressMode::Baseline;
  fs::path tracePath;
};

std::string ToString(StressMode mode)
{
  if (mode == StressMode::ResourceRebind)
    return "resource-rebind";
  return "baseline";
}

StressMode ParseStressMode(const char* value)
{
  if (!value)
    return StressMode::Baseline;

  std::string mode = value;
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  if (mode == "resource-rebind" || mode == "resource_rebind" || mode == "rebind")
    return StressMode::ResourceRebind;

  return StressMode::Baseline;
}

guitarfx::Preset CreateSyntheticPreset(int index)
{
  guitarfx::Preset preset;
  preset.id = "synthetic-" + std::to_string(index);
  preset.name = "Synthetic " + std::to_string(index);

  guitarfx::SignalGraph graph;
  graph.nodes.push_back({"in", guitarfx::kNodeTypeInput, "", "Input", true});

  graph.nodes.push_back({"gain", "gain", "utility", "Gain", true});
  graph.nodes.back().params["gainDb"] = (index % 2 == 0) ? -2.0 : 1.5;

  graph.nodes.push_back({"eq", "eq_parametric", "eq", "EQ", true});
  graph.nodes.back().params["lowGainDb"] = (index % 3 == 0) ? 2.0 : -1.0;
  graph.nodes.back().params["midGainDb"] = (index % 3 == 1) ? 1.5 : 0.0;
  graph.nodes.back().params["highGainDb"] = (index % 3 == 2) ? 2.0 : -0.5;

  graph.nodes.push_back({"comp", "compressor_vca", "dynamics", "Compressor", true});
  graph.nodes.back().params["thresholdDb"] = -22.0;
  graph.nodes.back().params["ratio"] = 2.5;

  graph.nodes.push_back({"delay", "delay_digital", "delay", "Delay", true});
  graph.nodes.back().params["timeMs"] = 60.0 + static_cast<double>(index * 12);
  graph.nodes.back().params["feedback"] = 0.25;
  graph.nodes.back().params["mix"] = 0.20;

  graph.nodes.push_back({"verb", "reverb_room", "reverb", "Reverb", true});
  graph.nodes.back().params["mix"] = 0.18;
  graph.nodes.back().params["roomSize"] = 0.65;

  graph.nodes.push_back({"out", guitarfx::kNodeTypeOutput, "", "Output", true});

  graph.edges.push_back({"in", "gain", 0, 0, 1.0});
  graph.edges.push_back({"gain", "eq", 0, 0, 1.0});
  graph.edges.push_back({"eq", "comp", 0, 0, 1.0});
  graph.edges.push_back({"comp", "delay", 0, 0, 1.0});
  graph.edges.push_back({"delay", "verb", 0, 0, 1.0});
  graph.edges.push_back({"verb", "out", 0, 0, 1.0});

  preset.graph = std::move(graph);
  return preset;
}

guitarfx::Preset CreateResourceSyntheticPreset(const int index,
                                              const guitarfx::LibraryResource& nam,
                                              const guitarfx::LibraryResource& ir)
{
  guitarfx::Preset preset;
  preset.id = "synthetic-resource-" + std::to_string(index);
  preset.name = "Synthetic Resource " + std::to_string(index);

  guitarfx::SignalGraph graph;
  graph.nodes.push_back({"in", guitarfx::kNodeTypeInput, "", "Input", true});

  guitarfx::GraphNode amp;
  amp.id = "amp";
  amp.type = "amp_nam_optimized";
  amp.category = "amp";
  amp.label = "NAM";
  amp.enabled = true;
  amp.params["inputGain"] = (index % 2 == 0) ? -1.0 : 1.0;
  amp.params["mix"] = 1.0;
  guitarfx::ResourceRef ampRef;
  ampRef.resourceType = nam.type;
  ampRef.resourceId = nam.id;
  amp.resources.push_back(std::move(ampRef));
  graph.nodes.push_back(std::move(amp));

  guitarfx::GraphNode cab;
  cab.id = "cab";
  cab.type = "ir_cab";
  cab.category = "cab";
  cab.label = "Cab";
  cab.enabled = true;
  cab.params["mix"] = 1.0;
  guitarfx::ResourceRef cabRef;
  cabRef.resourceType = ir.type;
  cabRef.resourceId = ir.id;
  cab.resources.push_back(std::move(cabRef));
  graph.nodes.push_back(std::move(cab));

  graph.nodes.push_back({"out", guitarfx::kNodeTypeOutput, "", "Output", true});
  graph.edges.push_back({"in", "amp", 0, 0, 1.0});
  graph.edges.push_back({"amp", "cab", 0, 0, 1.0});
  graph.edges.push_back({"cab", "out", 0, 0, 1.0});

  preset.graph = std::move(graph);
  return preset;
}

nlohmann::json LoadJson(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
  {
    throw std::runtime_error("Failed to open JSON file: " + path.string());
  }

  nlohmann::json document;
  input >> document;
  return document;
}

std::string Describe(const fs::path& path)
{
  fs::path preferred = path;
  preferred.make_preferred();
  return preferred.string();
}

int ReadEnvInt(const char* key, int fallback, int minValue, int maxValue)
{
  const char* value = std::getenv(key);
  if (!value || *value == '\0')
    return fallback;

  try
  {
    const long parsed = std::stol(value);
    return static_cast<int>(std::clamp(parsed, static_cast<long>(minValue), static_cast<long>(maxValue)));
  }
  catch (...)
  {
    return fallback;
  }
}

std::uint64_t ReadEnvUInt64(const char* key, std::uint64_t fallback)
{
  const char* value = std::getenv(key);
  if (!value || *value == '\0')
    return fallback;

  try
  {
    return static_cast<std::uint64_t>(std::stoull(value));
  }
  catch (...)
  {
    return fallback;
  }
}

void LoadLibraryResources(guitarfx::ResourceLibrary& library,
                          const nlohmann::json& modelsJson,
                          const nlohmann::json& irJson,
                          const fs::path& baseDir)
{
  for (const auto& entry : modelsJson)
  {
    guitarfx::LibraryResource resource;
    resource.type = "nam";
    resource.id = entry.value("id", "");
    resource.name = entry.value("title", entry.value("name", resource.id));
    resource.category = entry.value("category", "");
    resource.description = entry.value("description", "");
    resource.filePath = baseDir / entry.value("filePath", "");
    if (!resource.id.empty())
    {
      library.AddResource(resource);
    }
  }

  for (const auto& entry : irJson)
  {
    guitarfx::LibraryResource resource;
    resource.type = "ir";
    resource.id = entry.value("id", "");
    resource.name = entry.value("title", entry.value("name", resource.id));
    resource.category = entry.value("category", "");
    resource.description = entry.value("description", "");
    resource.filePath = baseDir / entry.value("filePath", "");
    if (!resource.id.empty())
    {
      library.AddResource(resource);
    }
  }
}

ScenarioData BuildScenario(const fs::path& resourcesDir,
                           const guitarfx::ResourceLibrary& library,
                           int maxPresets)
{
  const fs::path dataDir = resourcesDir / "data";

  const auto presetsJson = LoadJson(dataDir / "default-presets.json");
  if (!presetsJson.is_array())
  {
    throw std::runtime_error("default-presets.json is not an array");
  }

  ScenarioData scenario;
  scenario.presets.reserve(static_cast<std::size_t>(std::max(1, maxPresets)));

  for (const auto& presetJson : presetsJson)
  {
    if (static_cast<int>(scenario.presets.size()) >= maxPresets)
      break;

    auto parsed = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
    if (!parsed)
      continue;

    scenario.presetIds.push_back(parsed->id.empty() ? ("preset-" + std::to_string(scenario.presets.size())) : parsed->id);
    scenario.presets.push_back(std::move(*parsed));
  }

  if (scenario.presets.empty())
  {
    const int fallbackCount = std::clamp(maxPresets, 3, 8);
    const auto namResources = library.GetResourcesByType("nam");
    const auto irResources = library.GetResourcesByType("ir");
    const bool canUseResourceFallback = !namResources.empty() && !irResources.empty();

    for (int i = 0; i < fallbackCount; ++i)
    {
      guitarfx::Preset preset;
      if (canUseResourceFallback)
      {
        const auto& nam = namResources[static_cast<std::size_t>(i) % namResources.size()];
        const auto& ir = irResources[static_cast<std::size_t>(i) % irResources.size()];
        preset = CreateResourceSyntheticPreset(i + 1, nam, ir);
      }
      else
      {
        preset = CreateSyntheticPreset(i + 1);
      }
      scenario.presetIds.push_back(preset.id);
      scenario.presets.push_back(std::move(preset));
    }
  }

  const auto& registry = guitarfx::EffectRegistry::Instance();
  for (std::size_t presetIndex = 0; presetIndex < scenario.presets.size(); ++presetIndex)
  {
    const auto& preset = scenario.presets[presetIndex];
    const std::string& presetId = scenario.presetIds[presetIndex];

    for (const auto& node : preset.graph.nodes)
    {
      if (node.type == guitarfx::kNodeTypeInput || node.type == guitarfx::kNodeTypeOutput)
        continue;

      scenario.toggleTargets.push_back({presetId, node.id});

      const auto typeInfo = registry.GetTypeInfo(node.type);
      if (!typeInfo)
        continue;

      if (typeInfo->requiresResource && !typeInfo->resourceType.empty())
      {
        scenario.resourceTargets.push_back({presetId, node.id, typeInfo->resourceType});
      }
      else
      {
        for (const auto& ref : node.resources)
        {
          if (!ref.resourceType.empty())
          {
            scenario.resourceTargets.push_back({presetId, node.id, ref.resourceType});
            break;
          }
        }
      }

      for (const auto& parameter : typeInfo->parameters)
      {
        ParamMutationTarget target;
        target.presetId = presetId;
        target.nodeId = node.id;
        target.nodeType = node.type;
        target.paramId = parameter.id;
        target.minValue = parameter.minValue;
        target.maxValue = parameter.maxValue;
        target.step = parameter.step;
        scenario.paramTargets.push_back(std::move(target));
      }
    }
  }

  return scenario;
}

RunConfig MakeRunConfig()
{
  RunConfig cfg;
  const auto nowTicks = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());

  cfg.seed = ReadEnvUInt64("GUITARFX_STRESS_SEED", nowTicks);
  cfg.steps = ReadEnvInt("GUITARFX_STRESS_STEPS", kDefaultSteps, 1, 200000);
  cfg.blockSize = ReadEnvInt("GUITARFX_STRESS_BLOCK_SIZE", kDefaultBlockSize, 16, 4096);
  cfg.maxPresets = ReadEnvInt("GUITARFX_STRESS_MAX_PRESETS", kDefaultMaxPresets, 1, 32);
  cfg.mode = ParseStressMode(std::getenv("GUITARFX_STRESS_MODE"));

  const char* customTrace = std::getenv("GUITARFX_STRESS_TRACE_PATH");
  if (customTrace && *customTrace != '\0')
  {
    cfg.tracePath = fs::path(customTrace);
  }
  else
  {
    cfg.tracePath = fs::current_path() / "SignalChainMutationStressTest-last-trace.log";
  }

  return cfg;
}

std::unordered_map<std::string, std::vector<guitarfx::LibraryResource>> BuildResourcePool(
    const guitarfx::ResourceLibrary& library)
{
  std::unordered_map<std::string, std::vector<guitarfx::LibraryResource>> pool;
  const auto allResources = library.GetAllResources();
  for (const auto& res : allResources)
  {
    if (res.type.empty() || res.id.empty())
      continue;
    pool[res.type].push_back(res);
  }
  return pool;
}

template <typename T>
T PickRandom(const std::vector<T>& values, std::mt19937_64& rng)
{
  std::uniform_int_distribution<std::size_t> dist(0, values.size() - 1);
  return values[dist(rng)];
}

std::size_t PickIndex(std::size_t size, std::mt19937_64& rng)
{
  std::uniform_int_distribution<std::size_t> dist(0, size - 1);
  return dist(rng);
}

float NextInputSample(std::mt19937_64& rng, double phase, double amplitude, bool noisy)
{
  const float sine = static_cast<float>(std::sin(phase) * amplitude);
  if (!noisy)
    return sine;

  std::uniform_real_distribution<float> noiseDist(-0.2f, 0.2f);
  return sine + noiseDist(rng);
}

bool HasInvalidOrExplodingSignal(const std::vector<float>& left,
                                 const std::vector<float>& right,
                                 double& peakOut)
{
  peakOut = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    const double l = static_cast<double>(left[i]);
    const double r = static_cast<double>(right[i]);
    if (std::isnan(l) || std::isnan(r) || std::isinf(l) || std::isinf(r))
      return true;

    peakOut = std::max(peakOut, std::abs(l));
    peakOut = std::max(peakOut, std::abs(r));
  }

  // Wide bound to catch runaway output while allowing transient overs.
  return peakOut > 64.0;
}

std::string TailTrace(const std::deque<std::string>& trace)
{
  std::string out;
  for (const auto& line : trace)
  {
    out += line;
    out.push_back('\n');
  }
  return out;
}

} // namespace

int main()
{
#ifndef GUITARFX_TEST_RESOURCES_DIR
#error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

  try
  {
    const RunConfig cfg = MakeRunConfig();
    std::mt19937_64 rng(cfg.seed);

    guitarfx::RegisterAllEffects();

    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "data";

    const auto modelsJson = LoadJson(dataDir / "audiofx-models.json");
    const auto irJson = LoadJson(dataDir / "ir-library.json");

    guitarfx::ResourceLibrary library;
    LoadLibraryResources(library, modelsJson, irJson, resourcesDir);

    ScenarioData scenario = BuildScenario(resourcesDir, library, cfg.maxPresets);
    const auto resourcePool = BuildResourcePool(library);

    guitarfx::MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.SetMultiThreadedProcessingEnabled(true);
    mixer.Prepare(48000.0, cfg.blockSize);

    for (std::size_t i = 0; i < scenario.presets.size(); ++i)
    {
      if (!mixer.AddActivePreset(scenario.presets[i], scenario.presetIds[i], scenario.presets[i].name))
      {
        throw std::runtime_error("Failed to add active preset: " + scenario.presetIds[i]);
      }
    }

    for (const auto& presetId : scenario.presetIds)
    {
      mixer.SetPresetMix(presetId, 0.0);
      mixer.SetPresetMute(presetId, false);
      mixer.SetPresetSolo(presetId, false);
      mixer.SetPresetPan(presetId, 0.0);
    }
    mixer.SetPresetMix(scenario.presetIds.front(), 1.0);

    std::ofstream traceFile(cfg.tracePath, std::ios::trunc);
    if (!traceFile)
    {
      throw std::runtime_error("Unable to open trace log: " + Describe(cfg.tracePath));
    }

    traceFile << "seed=" << cfg.seed << '\n';
    traceFile << "steps=" << cfg.steps << '\n';
    traceFile << "blockSize=" << cfg.blockSize << '\n';
    traceFile << "presetCount=" << scenario.presets.size() << '\n';
    traceFile.flush();

    std::cout << "Signal chain mutation stress test" << std::endl;
    std::cout << "  seed       : " << cfg.seed << std::endl;
    std::cout << "  steps      : " << cfg.steps << std::endl;
    std::cout << "  block size : " << cfg.blockSize << std::endl;
    std::cout << "  mode       : " << ToString(cfg.mode) << std::endl;
    std::cout << "  presets    : " << scenario.presets.size() << std::endl;
    std::cout << "  param slots: " << scenario.paramTargets.size() << std::endl;
    std::cout << "  rebind slots: " << scenario.resourceTargets.size() << std::endl;
    std::cout << "  trace file : " << Describe(cfg.tracePath) << std::endl;

    std::vector<float> inL(static_cast<std::size_t>(cfg.blockSize), 0.0f);
    std::vector<float> inR(static_cast<std::size_t>(cfg.blockSize), 0.0f);
    std::vector<float> outL(static_cast<std::size_t>(cfg.blockSize), 0.0f);
    std::vector<float> outR(static_cast<std::size_t>(cfg.blockSize), 0.0f);

    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    std::deque<std::string> traceHistory;
    auto appendTrace = [&](const std::string& line) {
      traceFile << line << '\n';
      traceFile.flush();
      traceHistory.push_back(line);
      if (static_cast<int>(traceHistory.size()) > kTraceHistoryLimit)
      {
        traceHistory.pop_front();
      }
    };

    std::uniform_real_distribution<double> unitDist(0.0, 1.0);
    std::uniform_real_distribution<double> panDist(-1.0, 1.0);
    std::uniform_int_distribution<int> inputChannelDist(0, 1);
    std::uniform_int_distribution<int> burstBlocksDist(1, 5);
    std::uniform_real_distribution<double> freqDist(70.0, 1800.0);
    std::uniform_real_distribution<double> ampDist(0.05, 0.7);

    double phaseL = 0.0;
    double phaseR = 0.5;
    double currentSampleRate = 48000.0;

    for (int step = 0; step < cfg.steps; ++step)
    {
      const int mutationType = static_cast<int>(PickIndex(100, rng));
      const bool resourceMode = (cfg.mode == StressMode::ResourceRebind);

      if (mutationType < 15)
      {
        currentSampleRate = PickRandom(kSampleRates, rng);
        mixer.Prepare(currentSampleRate, cfg.blockSize);
        appendTrace("step=" + std::to_string(step) + " action=prepare sampleRate=" + std::to_string(currentSampleRate));
      }
      else if (mutationType < 30)
      {
        const bool monoMode = unitDist(rng) > 0.5;
        const int inputChannel = inputChannelDist(rng);
        mixer.SetMonoMode(monoMode);
        mixer.SetInputChannel(inputChannel);
        appendTrace("step=" + std::to_string(step) + " action=io mono=" + std::to_string(static_cast<int>(monoMode)) +
                    " inputChannel=" + std::to_string(inputChannel));
      }
      else if (mutationType < 48)
      {
        const std::size_t active = PickIndex(scenario.presetIds.size(), rng);
        for (std::size_t i = 0; i < scenario.presetIds.size(); ++i)
        {
          const double mix = (i == active) ? 1.0 : 0.0;
          mixer.SetPresetMix(scenario.presetIds[i], mix);
          mixer.SetPresetMute(scenario.presetIds[i], false);
          mixer.SetPresetSolo(scenario.presetIds[i], false);
        }
        appendTrace("step=" + std::to_string(step) + " action=selectPreset presetId=" + scenario.presetIds[active]);
      }
      else if (mutationType < 65)
      {
        const std::size_t presetIndex = PickIndex(scenario.presetIds.size(), rng);
        const std::string& presetId = scenario.presetIds[presetIndex];
        const double mix = unitDist(rng);
        const double pan = panDist(rng);
        const bool mute = unitDist(rng) < 0.1;
        const bool solo = unitDist(rng) < 0.05;

        mixer.SetPresetMix(presetId, mix);
        mixer.SetPresetPan(presetId, pan);
        mixer.SetPresetMute(presetId, mute);
        mixer.SetPresetSolo(presetId, solo);

        appendTrace("step=" + std::to_string(step) + " action=presetState presetId=" + presetId + " mix=" +
                    std::to_string(mix) + " pan=" + std::to_string(pan) + " mute=" + std::to_string(static_cast<int>(mute)) +
                    " solo=" + std::to_string(static_cast<int>(solo)));
      }
      else if (mutationType < 78 && !scenario.toggleTargets.empty())
      {
        const auto& target = scenario.toggleTargets[PickIndex(scenario.toggleTargets.size(), rng)];
        const bool enabled = unitDist(rng) > 0.35;
        mixer.SetNodeEnabled(target.presetId, target.nodeId, enabled);
        appendTrace("step=" + std::to_string(step) + " action=nodeEnabled presetId=" + target.presetId +
                    " nodeId=" + target.nodeId + " enabled=" + std::to_string(static_cast<int>(enabled)));
      }
      else if (mutationType < (resourceMode ? 82 : 96) && !scenario.paramTargets.empty())
      {
        const auto& target = scenario.paramTargets[PickIndex(scenario.paramTargets.size(), rng)];
        const double t = unitDist(rng);
        double value = target.minValue + (target.maxValue - target.minValue) * t;
        if (target.step > 0.0)
        {
          const double steps = std::round((value - target.minValue) / target.step);
          value = target.minValue + steps * target.step;
          value = std::clamp(value, target.minValue, target.maxValue);
        }

        mixer.SetNodeParam(target.presetId, target.nodeId, target.paramId, value);
        appendTrace("step=" + std::to_string(step) + " action=nodeParam presetId=" + target.presetId +
                    " nodeId=" + target.nodeId + " type=" + target.nodeType +
                    " param=" + target.paramId + " value=" + std::to_string(value));
      }
      else if (!scenario.resourceTargets.empty())
      {
        const auto& target = scenario.resourceTargets[PickIndex(scenario.resourceTargets.size(), rng)];
        const auto poolIt = resourcePool.find(target.resourceType);
        if (poolIt != resourcePool.end() && !poolIt->second.empty())
        {
          const auto& chosen = poolIt->second[PickIndex(poolIt->second.size(), rng)];
          guitarfx::ResourceRef ref;
          ref.resourceType = chosen.type;
          ref.resourceId = chosen.id;
          const bool ok = mixer.LoadNodeResource(target.presetId, target.nodeId, ref);
          appendTrace("step=" + std::to_string(step) + " action=resourceRebind presetId=" + target.presetId +
                      " nodeId=" + target.nodeId + " resourceType=" + target.resourceType +
                      " resourceId=" + chosen.id + " ok=" + std::to_string(static_cast<int>(ok)));
        }
        else
        {
          appendTrace("step=" + std::to_string(step) + " action=resourceRebindSkipped presetId=" + target.presetId +
                      " nodeId=" + target.nodeId + " resourceType=" + target.resourceType + " reason=no-pool");
        }
      }
      else
      {
        appendTrace("step=" + std::to_string(step) + " action=noop");
      }

      const int bursts = burstBlocksDist(rng);
      for (int burst = 0; burst < bursts; ++burst)
      {
        const double frequency = freqDist(rng);
        const double amplitude = ampDist(rng);
        const bool noisy = unitDist(rng) < 0.3;

        const double inc = 2.0 * kPi * frequency / currentSampleRate;
        const double incR = 2.0 * kPi * (frequency * 0.997) / currentSampleRate;

        for (int sample = 0; sample < cfg.blockSize; ++sample)
        {
          inL[static_cast<std::size_t>(sample)] = NextInputSample(rng, phaseL, amplitude, noisy);
          inR[static_cast<std::size_t>(sample)] = NextInputSample(rng, phaseR, amplitude, noisy);
          phaseL += inc;
          phaseR += incR;
          if (phaseL > 2.0 * kPi)
            phaseL -= 2.0 * kPi;
          if (phaseR > 2.0 * kPi)
            phaseR -= 2.0 * kPi;
        }

        try
        {
          mixer.Process(inputs, outputs, cfg.blockSize);
        }
        catch (const std::exception& ex)
        {
          std::cerr << "\nStress test threw at step " << step << ": " << ex.what() << std::endl;
          std::cerr << "Reproduce with: GUITARFX_STRESS_SEED=" << cfg.seed << std::endl;
          std::cerr << "Trace tail:\n" << TailTrace(traceHistory);
          return 1;
        }
        catch (...)
        {
          std::cerr << "\nStress test threw unknown exception at step " << step << std::endl;
          std::cerr << "Reproduce with: GUITARFX_STRESS_SEED=" << cfg.seed << std::endl;
          std::cerr << "Trace tail:\n" << TailTrace(traceHistory);
          return 1;
        }

        double peak = 0.0;
        if (HasInvalidOrExplodingSignal(outL, outR, peak))
        {
          std::cerr << "\nInvalid output at step " << step << " (peak=" << peak << ")" << std::endl;
          std::cerr << "Reproduce with: GUITARFX_STRESS_SEED=" << cfg.seed << std::endl;
          std::cerr << "Trace tail:\n" << TailTrace(traceHistory);
          return 1;
        }
      }

      if ((step + 1) % 250 == 0)
      {
        std::cout << "  progressed " << (step + 1) << "/" << cfg.steps << " steps" << std::endl;
      }
    }

    std::cout << "Stress test completed without invalid output." << std::endl;
    std::cout << "Re-run exact sequence with GUITARFX_STRESS_SEED=" << cfg.seed << std::endl;
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Signal-chain stress test fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
