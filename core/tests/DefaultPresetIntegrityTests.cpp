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

        // Validate that preset uses graph-based resource config (v2 format)
        if (!preset.contains("graph"))
        {
          recordError("Preset " + presetId + " is missing 'graph' (formatVersion 2 required)");
          continue;
        }

        const auto& graph = preset["graph"];
        if (!graph.contains("nodes") || !graph["nodes"].is_array())
        {
          recordError("Preset " + presetId + " has invalid graph structure (missing nodes array)");
          continue;
        }

        // Validate each node's resource references
        for (const auto& node : graph["nodes"])
        {
          const std::string nodeId = node.value("id", "<unnamed>");
          const std::string nodeType = node.value("type", "");

          // Check if this node type requires a resource
          if (nodeType == "amp_nam")
          {
            if (!node.contains("resource"))
            {
              recordError("Preset " + presetId + " node " + nodeId + " (amp_nam) is missing resource config");
              continue;
            }
            const auto& resource = node["resource"];
            const std::string resourceType = resource.value("type", "");
            const std::string resourceId = resource.value("id", "");

            if (resourceType != "nam")
            {
              recordError("Preset " + presetId + " node " + nodeId + " has invalid resource type: " + resourceType);
            }
            else if (resourceId.empty())
            {
              recordError("Preset " + presetId + " node " + nodeId + " is missing resource id");
            }
            else if (!modelLibrary.contains(resourceId))
            {
              recordError("Preset " + presetId + " node " + nodeId + " references unknown NAM model: " + resourceId);
            }
          }
          else if (nodeType == "cab_ir")
          {
            if (!node.contains("resource"))
            {
              recordError("Preset " + presetId + " node " + nodeId + " (cab_ir) is missing resource config");
              continue;
            }
            const auto& resource = node["resource"];
            const std::string resourceType = resource.value("type", "");
            const std::string resourceId = resource.value("id", "");

            if (resourceType != "ir")
            {
              recordError("Preset " + presetId + " node " + nodeId + " has invalid resource type: " + resourceType);
            }
            else if (resourceId.empty())
            {
              recordError("Preset " + presetId + " node " + nodeId + " is missing resource id");
            }
            else if (!irLibrary.contains(resourceId))
            {
              recordError("Preset " + presetId + " node " + nodeId + " references unknown IR: " + resourceId);
            }
          }
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

    std::cout << "All default presets have valid graph-based resource references." << std::endl;
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Preset integrity test encountered a fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
