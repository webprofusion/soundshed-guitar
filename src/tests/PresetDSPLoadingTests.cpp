#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/NAMDSPManager.h"

// Force factory registration by referencing factory functions directly.
// This ensures the translation units with static factory::Helper registrations
// are not stripped by the linker.
#include "NAM/wavenet.h"
#include "NAM/lstm.h"
#include "NAM/convnet.h"

namespace
{
// Touch factory symbols to prevent dead-stripping
[[maybe_unused]] volatile auto force_wavenet = &nam::wavenet::Factory;
[[maybe_unused]] volatile auto force_lstm = &nam::lstm::Factory;
[[maybe_unused]] volatile auto force_convnet = &nam::convnet::Factory;
}

namespace fs = std::filesystem;

namespace
{
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

struct LibraryEntry
{
  fs::path filePath;
  std::string title;
};

std::string Describe(const fs::path& path)
{
  fs::path preferred = path;
  preferred.make_preferred();
  return preferred.string();
}
} // namespace

int main()
{
#ifndef NAMGUITAR_TEST_RESOURCES_DIR
#error "NAMGUITAR_TEST_RESOURCES_DIR must be defined"
#endif
  try
  {
    const fs::path resourcesDir = fs::path(NAMGUITAR_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "ui" / "data";

    std::vector<std::string> errors;
    const auto recordError = [&errors](std::string message) {
      errors.push_back(std::move(message));
    };

    // Load model library
    const auto audioModelsJson = LoadJson(dataDir / "audiofx-models.json");
    std::unordered_map<std::string, LibraryEntry> modelLibrary;
    if (audioModelsJson.is_array())
    {
      for (const auto& entry : audioModelsJson)
      {
        const std::string id = entry.value("id", "");
        const std::string relPath = entry.value("filePath", "");
        if (!id.empty() && !relPath.empty())
        {
          modelLibrary.emplace(id, LibraryEntry{resourcesDir / relPath, entry.value("title", id)});
        }
      }
    }

    // Load IR library
    const auto irLibraryJson = LoadJson(dataDir / "ir-library.json");
    std::unordered_map<std::string, LibraryEntry> irLibrary;
    if (irLibraryJson.is_array())
    {
      for (const auto& entry : irLibraryJson)
      {
        const std::string id = entry.value("id", "");
        const std::string relPath = entry.value("filePath", "");
        if (!id.empty() && !relPath.empty())
        {
          irLibrary.emplace(id, LibraryEntry{resourcesDir / relPath, entry.value("title", id)});
        }
      }
    }

    // Load and test each preset
    const auto presetsJson = LoadJson(dataDir / "default-presets.json");
    if (!presetsJson.is_array())
    {
      throw std::runtime_error("default-presets.json is not an array");
    }

    constexpr double kTestSampleRate = 48000.0;
    constexpr int kTestBlockSize = 512;

    int presetsLoaded = 0;
    int presetsTested = 0;

    for (const auto& preset : presetsJson)
    {
      const std::string presetId = preset.value("id", "<unnamed>");
      const std::string presetName = preset.value("name", presetId);
      const std::string modelId = preset.value("audioFxModelId", "");
      const std::string irId = preset.value("irId", "");

      ++presetsTested;

      // Skip if model or IR not found in library (already caught by integrity test)
      if (!modelLibrary.contains(modelId))
      {
        recordError("Preset '" + presetName + "' references unknown model: " + modelId);
        continue;
      }
      if (!irLibrary.contains(irId))
      {
        recordError("Preset '" + presetName + "' references unknown IR: " + irId);
        continue;
      }

      const fs::path modelPath = modelLibrary.at(modelId).filePath;
      const fs::path irPath = irLibrary.at(irId).filePath;

      // Create a fresh DSP manager for each preset
      namguitar::NAMDSPManager dsp;
      dsp.Prepare(kTestSampleRate, kTestBlockSize);

      // Test model loading
      if (!fs::exists(modelPath))
      {
        recordError("Preset '" + presetName + "': model file does not exist: " + Describe(modelPath));
        continue;
      }

      if (!dsp.LoadModel(modelPath))
      {
        recordError("Preset '" + presetName + "': failed to load model from " + Describe(modelPath));
        continue;
      }

      if (!dsp.HasModel())
      {
        recordError("Preset '" + presetName + "': model loaded but HasModel() returns false");
        continue;
      }

      // Test IR loading
      if (!fs::exists(irPath))
      {
        recordError("Preset '" + presetName + "': IR file does not exist: " + Describe(irPath));
        continue;
      }

      if (!dsp.LoadImpulseResponse(irPath))
      {
        recordError("Preset '" + presetName + "': failed to load IR from " + Describe(irPath));
        continue;
      }

      if (!dsp.HasImpulseResponse())
      {
        recordError("Preset '" + presetName + "': IR loaded but HasImpulseResponse() returns false");
        continue;
      }

      ++presetsLoaded;
      std::cout << "  [OK] " << presetName << std::endl;
    }

    std::cout << "\nDSP Loading Results: " << presetsLoaded << "/" << presetsTested << " presets loaded successfully.\n";

    if (!errors.empty())
    {
      std::cerr << "\nDSP loading test failed with " << errors.size() << " issue(s):\n";
      for (const auto& error : errors)
      {
        std::cerr << " - " << error << '\n';
      }
      return 1;
    }

    std::cout << "All default presets loaded their models and IRs successfully." << std::endl;
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "DSP loading test encountered a fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
