#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

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
}

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
    if (!audioModelsJson.is_array())
    {
      recordError("audiofx-models.json is not an array");
    }
    else
    {
      for (const auto& entry : audioModelsJson)
      {
        const std::string id = entry.value("id", "");
        const std::string relPath = entry.value("filePath", "");
        if (id.empty() || relPath.empty())
        {
          recordError("AudioFX model entry is missing id or filePath");
          continue;
        }

        const fs::path modelPath = resourcesDir / relPath;
        if (!fs::exists(modelPath))
        {
          recordError("AudioFX model file missing: " + id + " -> " + Describe(modelPath));
        }

        modelLibrary.emplace(id, LibraryEntry{modelPath, entry.value("title", id)});
      }
    }

    // Load IR library
    const auto irLibraryJson = LoadJson(dataDir / "ir-library.json");
    std::unordered_map<std::string, LibraryEntry> irLibrary;
    if (!irLibraryJson.is_array())
    {
      recordError("ir-library.json is not an array");
    }
    else
    {
      for (const auto& entry : irLibraryJson)
      {
        const std::string id = entry.value("id", "");
        const std::string relPath = entry.value("filePath", "");
        if (id.empty() || relPath.empty())
        {
          recordError("IR entry is missing id or filePath");
          continue;
        }

        const fs::path irPath = resourcesDir / relPath;
        if (!fs::exists(irPath))
        {
          recordError("IR file missing: " + id + " -> " + Describe(irPath));
        }

        irLibrary.emplace(id, LibraryEntry{irPath, entry.value("title", id)});
      }
    }

    // Validate default presets
    const auto presetsJson = LoadJson(dataDir / "default-presets.json");
    if (!presetsJson.is_array())
    {
      recordError("default-presets.json is not an array");
    }
    else
    {
      for (const auto& preset : presetsJson)
      {
        const std::string presetId = preset.value("id", "<unnamed>");

        const std::string modelId = preset.value("audioFxModelId", "");
        if (modelId.empty())
        {
          recordError("Preset " + presetId + " is missing audioFxModelId");
        }
        else if (!modelLibrary.contains(modelId))
        {
          recordError("Preset " + presetId + " references unknown AudioFX model id " + modelId);
        }

        const std::string irId = preset.value("irId", "");
        if (irId.empty())
        {
          recordError("Preset " + presetId + " is missing irId");
        }
        else if (!irLibrary.contains(irId))
        {
          recordError("Preset " + presetId + " references unknown IR id " + irId);
        }
      }
    }

    if (!errors.empty())
    {
      std::cerr << "Preset integrity test failed with " << errors.size() << " issue(s):\n";
      for (const auto& error : errors)
      {
        std::cerr << " - " << error << '\n';
      }
      return 1;
    }

    std::cout << "All default presets reference valid models and IRs." << std::endl;
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Preset integrity test encountered a fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
