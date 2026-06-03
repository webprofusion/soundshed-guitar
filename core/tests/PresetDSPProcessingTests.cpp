/**
 * Preset DSP processing smoke test
 *
 * Loads the resource libraries and default presets, processes each preset,
 * and asserts that audio is produced with reasonable, non-uniform peaks.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
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
using Sample = float;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kWarmupBlocks = 3;

struct SignalAnalysis
{
  double peak = 0.0;
  double rms = 0.0;
  bool hasNaN = false;
  bool hasInf = false;
  bool allZeros = true;
  bool allSame = true;
};

nlohmann::json LoadJson(const fs::path& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    throw std::runtime_error("Unable to open JSON file: " + path.string());
  }

  nlohmann::json json;
  file >> json;
  return json;
}

template <typename T>
void GenerateSine(std::vector<T>& buffer, double frequency, double sampleRate, double amplitude)
{
  for (std::size_t i = 0; i < buffer.size(); ++i)
  {
    buffer[i] = static_cast<T>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
  }
}

SignalAnalysis Analyze(const std::vector<Sample>& buffer)
{
  SignalAnalysis result;
  if (buffer.empty())
  {
    return result;
  }

  const double first = static_cast<double>(buffer.front());
  double sumSquares = 0.0;
  for (const auto& sVal : buffer)
  {
    const double s = static_cast<double>(sVal);
    result.hasNaN = result.hasNaN || std::isnan(s);
    result.hasInf = result.hasInf || std::isinf(s);

    result.peak = std::max(result.peak, std::abs(s));
    if (s != 0.0)
    {
      result.allZeros = false;
    }
    if (s != first)
    {
      result.allSame = false;
    }

    sumSquares += s * s;
  }

  result.rms = std::sqrt(sumSquares / static_cast<double>(buffer.size()));
  return result;
}

void LoadLibraryResources(guitarfx::ResourceLibrary& library, const nlohmann::json& modelsJson, const nlohmann::json& irJson, const fs::path& baseDir)
{
  for (const auto& entry : modelsJson)
  {
    guitarfx::LibraryResource res;
    res.type = "nam";
    res.id = entry.value("id", "");
    res.name = entry.value("title", entry.value("name", res.id));
    res.category = entry.value("category", "");
    res.description = entry.value("description", "");
    res.filePath = baseDir / entry.value("filePath", "");
    if (!res.id.empty())
    {
      library.AddResource(res);
    }
  }

  for (const auto& entry : irJson)
  {
    guitarfx::LibraryResource res;
    res.type = "ir";
    res.id = entry.value("id", "");
    res.name = entry.value("title", entry.value("name", res.id));
    res.category = entry.value("category", "");
    res.description = entry.value("description", "");
    res.filePath = baseDir / entry.value("filePath", "");
    if (!res.id.empty())
    {
      library.AddResource(res);
    }
  }
}

struct PresetRunResult
{
  bool success = false;
  std::string error;
  double peak = 0.0;
};

PresetRunResult RunPreset(guitarfx::SignalGraphExecutor& executor, guitarfx::ResourceLibrary& library, const guitarfx::Preset& preset)
{
  PresetRunResult result;

  executor.Reset();
  executor.SetResourceLibrary(&library);
  executor.SetInputTrim(preset.global.inputTrim);
  executor.SetOutputTrim(preset.global.outputTrim);
  executor.SetGraph(preset.graph);
  executor.Prepare(kSampleRate, kBlockSize);

  std::vector<Sample> inL(static_cast<std::size_t>(kBlockSize));
  std::vector<Sample> inR(static_cast<std::size_t>(kBlockSize));
  std::vector<Sample> outL(static_cast<std::size_t>(kBlockSize));
  std::vector<Sample> outR(static_cast<std::size_t>(kBlockSize));

  GenerateSine(inL, 440.0, kSampleRate, 0.5);
  GenerateSine(inR, 440.0, kSampleRate, 0.5);

  float* fInL = new float[kBlockSize];
  float* fInR = new float[kBlockSize];
  float* fOutL = new float[kBlockSize];
  float* fOutR = new float[kBlockSize];
  float* fInputs[2] = {fInL, fInR};
  float* fOutputs[2] = {fOutL, fOutR};

  try
  {
    for (int i = 0; i < kWarmupBlocks; ++i)
    {
      for (int j = 0; j < kBlockSize; ++j)
      {
        fInL[j] = static_cast<float>(inL[j]);
        fInR[j] = static_cast<float>(inR[j]);
      }
      executor.Process(fInputs, fOutputs, kBlockSize);
      for (int j = 0; j < kBlockSize; ++j)
      {
        outL[j] = fOutL[j];
        outR[j] = fOutR[j];
      }
    }
    delete[] fInL;
    delete[] fInR;
    delete[] fOutL;
    delete[] fOutR;
  }
  catch (const std::exception& ex)
  {
    result.error = std::string("Processing threw: ") + ex.what();
    return result;
  }
  catch (...)
  {
    result.error = "Processing threw: unknown";
    return result;
  }

  const auto analysis = Analyze(outL);
  if (analysis.hasNaN)
  {
    result.error = "Output contains NaN";
    return result;
  }
  if (analysis.hasInf)
  {
    result.error = "Output contains Inf";
    return result;
  }
  if (analysis.allZeros)
  {
    result.error = "Output is all zeros";
    return result;
  }
  if (analysis.allSame)
  {
    result.error = "Output is DC";
    return result;
  }

  constexpr double kMaxReasonablePeak = 10.0; // ~20 dBFS headroom
  if (analysis.peak <= 0.0)
  {
    result.error = "Output peak is non-positive";
    return result;
  }
  if (analysis.peak > kMaxReasonablePeak)
  {
    result.error = "Output peak too high: " + std::to_string(analysis.peak);
    return result;
  }

  result.success = true;
  result.peak = analysis.peak;
  return result;
}

} // namespace

int main()
{
#ifndef GUITARFX_TEST_RESOURCES_DIR
#error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

  try
  {
    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "data";

    const auto modelsJson = LoadJson(dataDir / "audiofx-models.json");
    const auto irJson = LoadJson(dataDir / "ir-library.json");
    const auto presetsJson = LoadJson(dataDir / "default-presets.json");

    if (!presetsJson.is_array())
    {
      throw std::runtime_error("default-presets.json is not an array");
    }

    std::vector<guitarfx::Preset> presets;
    presets.reserve(presetsJson.size());
    for (const auto& presetJson : presetsJson)
    {
      auto presetOpt = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
      if (presetOpt)
      {
        presets.push_back(*presetOpt);
      }
      else
      {
        std::cerr << "Failed to parse preset JSON" << std::endl;
        return 1;
      }
    }

    guitarfx::RegisterAllEffects();
    
    guitarfx::ResourceLibrary library;
    LoadLibraryResources(library, modelsJson, irJson, resourcesDir);
    
    guitarfx::SignalGraphExecutor executor;

    std::vector<double> peaks;
    peaks.reserve(presets.size());

    for (const auto& preset : presets)
    {
      auto run = RunPreset(executor, library, preset);
      if (!run.success)
      {
        std::cerr << "Preset '" << preset.name << "' failed: " << run.error << std::endl;
        return 1;
      }

      peaks.push_back(run.peak);
      std::cout << "Preset '" << preset.name << "' peak: " << run.peak << std::endl;
    }

    if (peaks.empty())
    {
      std::cerr << "No presets processed" << std::endl;
      return 1;
    }

    const auto [minIt, maxIt] = std::minmax_element(peaks.begin(), peaks.end());
    const double peakSpread = *maxIt - *minIt;
    constexpr double kPeakTolerance = 1e-4;
    if (peakSpread < kPeakTolerance)
    {
      std::cerr << "All presets produced the same peak (" << *minIt << ")" << std::endl;
      return 1;
    }

    std::cout << "Processed " << peaks.size() << " presets successfully." << std::endl;
    std::cout << "Peak range: min=" << *minIt << ", max=" << *maxIt << std::endl;
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
