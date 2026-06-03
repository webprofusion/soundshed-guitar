#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

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
#ifndef GUITARFX_TEST_RESOURCES_DIR
#error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif
  try
  {
    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "data";

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

    // Helper to extract resource IDs from graph nodes
    auto extractResourceIds = [](const nlohmann::json& preset, const std::string& nodeType, const std::string& resourceType)
        -> std::vector<std::string> {
      std::vector<std::string> ids;
      if (!preset.contains("graph") || !preset["graph"].contains("nodes"))
        return ids;
      for (const auto& node : preset["graph"]["nodes"])
      {
        if (node.value("type", "") == nodeType && node.contains("resource"))
        {
          const auto& resource = node["resource"];
          if (resource.value("type", "") == resourceType)
          {
            const std::string id = resource.value("id", "");
            if (!id.empty())
              ids.push_back(id);
          }
        }
      }
      return ids;
    };

    for (const auto& preset : presetsJson)
    {
      const std::string presetId = preset.value("id", "<unnamed>");
      const std::string presetName = preset.value("name", presetId);

      ++presetsTested;

      bool presetValid = true;

      // Parse the preset JSON to a Preset struct
      guitarfx::Preset presetStruct;
      try
      {
        auto presetOpt = guitarfx::PresetStorage::DeserializeFromJson(preset.dump());
        if (!presetOpt)
        {
          recordError("Preset '" + presetName + "': failed to parse JSON");
          continue;
        }
        presetStruct = std::move(*presetOpt);
      }
      catch (const std::exception& ex)
      {
        recordError("Preset '" + presetName + "': failed to parse JSON - " + std::string(ex.what()));
        continue;
      }

      // Extract resource IDs from graph nodes for validation
      auto modelIds = extractResourceIds(preset, "amp_nam", "nam");
      auto irIds = extractResourceIds(preset, "cab_ir", "ir");

      if (modelIds.empty())
      {
        recordError("Preset '" + presetName + "' has no amp_nam nodes with NAM resources");
        continue;
      }
      if (irIds.empty())
      {
        recordError("Preset '" + presetName + "' has no cab_ir nodes with IR resources");
        continue;
      }

      // Validate that all referenced resources exist
      for (const auto& modelId : modelIds)
      {
        if (!modelLibrary.contains(modelId))
        {
          recordError("Preset '" + presetName + "' references unknown model: " + modelId);
          presetValid = false;
          continue;
        }

        const fs::path modelPath = modelLibrary.at(modelId).filePath;
        if (!fs::exists(modelPath))
        {
          recordError("Preset '" + presetName + "': model file does not exist: " + Describe(modelPath));
          presetValid = false;
          continue;
        }
      }

      for (const auto& irId : irIds)
      {
        if (!irLibrary.contains(irId))
        {
          recordError("Preset '" + presetName + "' references unknown IR: " + irId);
          presetValid = false;
          continue;
        }

        const fs::path irPath = irLibrary.at(irId).filePath;
        if (!fs::exists(irPath))
        {
          recordError("Preset '" + presetName + "': IR file does not exist: " + Describe(irPath));
          presetValid = false;
          continue;
        }
      }

      if (!presetValid)
      {
        continue;
      }

      // Register effects once
      static bool effectsRegistered = false;
      if (!effectsRegistered)
      {
        guitarfx::RegisterAllEffects();
        effectsRegistered = true;
      }

      // Create ResourceLibrary and SignalGraphExecutor
      auto resourceLibrary = std::make_unique<guitarfx::ResourceLibrary>();
      
      // Register resource library paths
      for (const auto& [id, entry] : modelLibrary)
      {
        guitarfx::LibraryResource resource;
        resource.type = "nam";
        resource.id = id;
        resource.name = entry.title;
        resource.filePath = entry.filePath;
        resourceLibrary->AddResource(resource);
      }
      for (const auto& [id, entry] : irLibrary)
      {
        guitarfx::LibraryResource resource;
        resource.type = "ir";
        resource.id = id;
        resource.name = entry.title;
        resource.filePath = entry.filePath;
        resourceLibrary->AddResource(resource);
      }

      // Create executor and load graph
      guitarfx::SignalGraphExecutor executor;
      executor.SetResourceLibrary(resourceLibrary.get());
      executor.SetGraph(presetStruct.graph);

      // Prepare DSP
      executor.Prepare(kTestSampleRate, kTestBlockSize);

      // Verify graph was loaded
      if (!executor.IsValid())
      {
        recordError("Preset '" + presetName + "': graph is not valid");
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
