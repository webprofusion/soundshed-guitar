#include "presets/PresetStorage.h"
#include "presets/PresetTypesJson.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

namespace guitarfx
{
  namespace
  {
    bool HasUnsafeRelativeSegments(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute())
        return false;

      for (const auto& segment : path)
      {
        if (segment == "..")
          return true;
      }

      return false;
    }

    std::filesystem::path ResolveStoredPathForRuntime(const std::filesystem::path& storedPath,
                                                      const std::optional<std::filesystem::path>& baseDirectory)
    {
      if (storedPath.empty() || storedPath.is_absolute() || !baseDirectory.has_value())
        return storedPath;

      if (HasUnsafeRelativeSegments(storedPath))
        return storedPath;

      return (baseDirectory.value() / storedPath).lexically_normal();
    }

    std::filesystem::path BuildPathForStorage(const std::filesystem::path& runtimePath,
                                              const std::optional<std::filesystem::path>& baseDirectory)
    {
      if (runtimePath.empty() || !baseDirectory.has_value())
        return runtimePath;

      std::error_code ec;
      auto normalizedRuntimePath = std::filesystem::weakly_canonical(runtimePath, ec);
      if (ec)
        normalizedRuntimePath = runtimePath.lexically_normal();

      ec.clear();
      auto normalizedBase = std::filesystem::weakly_canonical(baseDirectory.value(), ec);
      if (ec)
        normalizedBase = baseDirectory.value().lexically_normal();

      const auto relativePath = normalizedRuntimePath.lexically_relative(normalizedBase);
      if (!relativePath.empty() && !relativePath.is_absolute() && !HasUnsafeRelativeSegments(relativePath))
        return relativePath;

      return runtimePath;
    }

    nlohmann::json SerializePresetResourceRef(const ResourceRef& ref,
                                              const std::optional<std::filesystem::path>& baseDirectory)
    {
      nlohmann::json json;
      if (!ref.resourceType.empty())
        json["resourceType"] = ref.resourceType;
      if (!ref.resourceId.empty())
        json["resourceId"] = ref.resourceId;
      if (!ref.filePath.empty())
        json["filePath"] = BuildPathForStorage(ref.filePath, baseDirectory).generic_string();
      if (!ref.embeddedId.empty())
        json["embeddedId"] = ref.embeddedId;
      if (!ref.parameterId.empty())
        json["parameterId"] = ref.parameterId;
      if (ref.parameterValue.has_value())
        json["parameterValue"] = *ref.parameterValue;
      if (!ref.parameters.empty())
      {
        json["parameters"] = nlohmann::json::object();
        for (const auto& [key, value] : ref.parameters)
        {
          json["parameters"][key] = value;
        }
      }
      return json;
    }

    ResourceRef DeserializePresetResourceRef(const nlohmann::json& json,
                                             const std::optional<std::filesystem::path>& baseDirectory)
    {
      ResourceRef ref;
      // Support both long-form (resourceType/resourceId) and short-form (type/id)
      ref.resourceType = json.value("resourceType", json.value("type", ""));
      ref.resourceId = json.value("resourceId", json.value("id", ""));
      ref.filePath = ResolveStoredPathForRuntime(
        std::filesystem::path(json.value("filePath", "")),
        baseDirectory);
      ref.embeddedId = json.value("embeddedId", "");
      ref.parameterId = json.value("parameterId", "");
      if (json.contains("parameterValue") && json["parameterValue"].is_number())
      {
        ref.parameterValue = json["parameterValue"].get<double>();
      }
      if (json.contains("parameters") && json["parameters"].is_object())
      {
        for (const auto& [key, value] : json["parameters"].items())
        {
          if (value.is_number())
          {
            ref.parameters[key] = value.get<double>();
          }
        }
      }
      return ref;
    }

    nlohmann::json SerializeGraphNode(const GraphNode& node,
                                      const std::optional<std::filesystem::path>& baseDirectory)
    {
      nlohmann::json json;
      json["id"] = node.id;
      json["type"] = node.type;
      if (!node.category.empty())
        json["category"] = node.category;
      if (!node.label.empty())
        json["label"] = node.label;
      if (!node.enabled)
        json["enabled"] = node.enabled;

      if (!node.params.empty())
      {
        json["params"] = nlohmann::json::object();
        for (const auto& [key, value] : node.params)
        {
          json["params"][key] = value;
        }
      }

      if (!node.config.empty())
      {
        json["config"] = nlohmann::json::object();
        for (const auto& [key, value] : node.config)
        {
          json["config"][key] = value;
        }
      }

      const bool isBlendRef = node.type == EffectGuids::kAmpNamBlend
        && node.config.find("blendId") != node.config.end();
      if (!node.resources.empty() && !isBlendRef)
      {
        json["resources"] = nlohmann::json::array();
        for (const auto& res : node.resources)
        {
          if (res.IsValid())
          {
            json["resources"].push_back(SerializePresetResourceRef(res, baseDirectory));
          }
        }
      }
      return json;
    }

    GraphNode DeserializeGraphNode(const nlohmann::json& json,
                                   const std::optional<std::filesystem::path>& baseDirectory)
    {
      GraphNode node;
      node.id = json.value("id", "");
      // Resolve legacy string IDs to canonical UUIDs for backward compatibility
      node.type = EffectRegistry::Instance().Resolve(json.value("type", ""));
      node.category = json.value("category", "");
      // Support both "label" and "displayName"
      node.label = json.value("label", json.value("displayName", ""));
      // Support both "enabled" and "bypassed" (inverted)
      if (json.contains("enabled"))
      {
        node.enabled = json.value("enabled", true);
      }
      else if (json.contains("bypassed"))
      {
        node.enabled = !json.value("bypassed", false);
      }
      else
      {
        node.enabled = true;
      }

      if (json.contains("params") && json["params"].is_object())
      {
        for (const auto& [key, value] : json["params"].items())
        {
          if (value.is_number())
          {
            node.params[key] = value.get<double>();
          }
        }
      }

      if (json.contains("config") && json["config"].is_object())
      {
        for (const auto& [key, value] : json["config"].items())
        {
          if (value.is_string())
          {
            node.config[key] = value.get<std::string>();
          }
        }
      }

      if (json.contains("resources") && json["resources"].is_array())
      {
        for (const auto& resJson : json["resources"])
        {
          if (resJson.is_object())
          {
            node.resources.push_back(DeserializePresetResourceRef(resJson, baseDirectory));
          }
        }
      }
      return node;
    }

    nlohmann::json SerializeGraphEdge(const GraphEdge& edge)
    {
      nlohmann::json json;
      json["from"] = edge.from;
      json["to"] = edge.to;
      if (edge.fromPort != 0)
        json["fromPort"] = edge.fromPort;
      if (edge.toPort != 0)
        json["toPort"] = edge.toPort;
      if (edge.gain != 1.0)
        json["gain"] = edge.gain;
      return json;
    }

    GraphEdge DeserializeGraphEdge(const nlohmann::json& json)
    {
      GraphEdge edge;
      edge.from = json.value("from", "");
      edge.to = json.value("to", "");
      edge.fromPort = json.value("fromPort", 0);
      edge.toPort = json.value("toPort", 0);
      edge.gain = json.value("gain", 1.0);
      return edge;
    }

    nlohmann::json SerializeEmbeddedResource(const EmbeddedResource& res,
                                             const std::optional<std::filesystem::path>& baseDirectory)
    {
      nlohmann::json json;
      json["id"] = res.id;
      json["type"] = res.type;
      json["name"] = res.name;
      if (!res.hash.empty())
        json["hash"] = res.hash;
      if (!res.data.empty())
        json["data"] = res.data;
      if (!res.originalPath.empty())
        json["originalPath"] = BuildPathForStorage(res.originalPath, baseDirectory).generic_string();
      return json;
    }

    EmbeddedResource DeserializeEmbeddedResource(const nlohmann::json& json,
                                                 const std::optional<std::filesystem::path>& baseDirectory)
    {
      EmbeddedResource res;
      res.id = json.value("id", "");
      res.type = json.value("type", "");
      res.name = json.value("name", "");
      res.hash = json.value("hash", "");
      res.data = json.value("data", "");
      res.originalPath = ResolveStoredPathForRuntime(
        std::filesystem::path(json.value("originalPath", "")),
        baseDirectory);
      return res;
    }

    nlohmann::json SerializePresetScene(const PresetScene& scene,
                                        const std::optional<std::filesystem::path>& baseDirectory)
    {
      nlohmann::json json;
      json["id"] = scene.id;
      if (!scene.title.empty())
        json["title"] = scene.title;

      nlohmann::json graph;
      graph["nodes"] = nlohmann::json::array();
      for (const auto& node : scene.graph.nodes)
      {
        graph["nodes"].push_back(SerializeGraphNode(node, baseDirectory));
      }
      graph["edges"] = nlohmann::json::array();
      for (const auto& edge : scene.graph.edges)
      {
        graph["edges"].push_back(SerializeGraphEdge(edge));
      }
      json["graph"] = std::move(graph);
      return json;
    }

    PresetScene DeserializePresetScene(const nlohmann::json& json,
                                       const std::optional<std::filesystem::path>& baseDirectory)
    {
      PresetScene scene;
      scene.id = json.value("id", "");
      scene.title = json.value("title", "");
      if (json.contains("graph") && json["graph"].is_object())
      {
        const auto& graph = json["graph"];
        if (graph.contains("nodes") && graph["nodes"].is_array())
        {
          for (const auto& nodeJson : graph["nodes"])
          {
            scene.graph.nodes.push_back(DeserializeGraphNode(nodeJson, baseDirectory));
          }
        }
        if (graph.contains("edges") && graph["edges"].is_array())
        {
          for (const auto& edgeJson : graph["edges"])
          {
            scene.graph.edges.push_back(DeserializeGraphEdge(edgeJson));
          }
        }
      }
      EnsurePresetBoundaryGainNodes(scene.graph);
      return scene;
    }
  } // namespace

  std::string PresetStorage::SerializeToJson(const Preset& preset)
  {
    const std::optional<std::filesystem::path> noBaseDirectory;
    nlohmann::json json;
    Preset normalizedPreset = preset;
    NormalizePresetScenes(normalizedPreset);

    // Metadata
    json["id"] = normalizedPreset.id;
    json["name"] = normalizedPreset.name;
    json["version"] = normalizedPreset.version;
    if (!normalizedPreset.author.empty())
      json["author"] = normalizedPreset.author;
    if (!normalizedPreset.category.empty())
      json["category"] = normalizedPreset.category;
    if (!normalizedPreset.description.empty())
      json["description"] = normalizedPreset.description;
    if (!normalizedPreset.createdAt.empty())
      json["createdAt"] = normalizedPreset.createdAt;
    if (!normalizedPreset.modifiedAt.empty())
      json["modifiedAt"] = normalizedPreset.modifiedAt;
    if (!normalizedPreset.tags.empty())
      json["tags"] = normalizedPreset.tags;
    if (normalizedPreset.designedPeakInputDbfs.has_value())
      json["designedPeakInputDbfs"] = normalizedPreset.designedPeakInputDbfs.value();

    // Global settings
    nlohmann::json global;
    global["inputTrim"] = normalizedPreset.global.inputTrim;
    global["outputTrim"] = normalizedPreset.global.outputTrim;
    global["outputVolume"] = normalizedPreset.global.outputVolume;
    global["autoLevelInput"] = normalizedPreset.global.autoLevelInput;
    global["autoLevelOutput"] = normalizedPreset.global.autoLevelOutput;
    global["transpose"] = normalizedPreset.global.transpose;
    json["global"] = global;

    if (normalizedPreset.globalSignalChain.has_value())
      json["globalSignalChain"] = *normalizedPreset.globalSignalChain;

    if (!normalizedPreset.scenes.empty())
    {
      json["scenes"] = nlohmann::json::array();
      for (const auto& scene : normalizedPreset.scenes)
      {
        json["scenes"].push_back(SerializePresetScene(scene, noBaseDirectory));
      }
    }
    else
    {
      nlohmann::json graph;
      graph["nodes"] = nlohmann::json::array();
      for (const auto& node : normalizedPreset.graph.nodes)
      {
        graph["nodes"].push_back(SerializeGraphNode(node, noBaseDirectory));
      }
      graph["edges"] = nlohmann::json::array();
      for (const auto& edge : normalizedPreset.graph.edges)
      {
        graph["edges"].push_back(SerializeGraphEdge(edge));
      }
      json["graph"] = graph;
    }

    // Embedded resources
    if (!normalizedPreset.embeddedResources.empty())
    {
      json["embeddedResources"] = nlohmann::json::array();
      for (const auto& res : normalizedPreset.embeddedResources)
      {
        json["embeddedResources"].push_back(SerializeEmbeddedResource(res, noBaseDirectory));
      }
    }

    return json.dump(2);
  }

  std::optional<Preset> PresetStorage::DeserializeFromJson(const std::string& jsonStr)
  {
    try
    {
      nlohmann::json json = nlohmann::json::parse(jsonStr);

      Preset preset;

      // Metadata
      preset.id = json.value("id", "");
      preset.name = json.value("name", "");
      // version can be an int or a string like "1.0"
      if (json.contains("version"))
      {
        if (json["version"].is_number())
        {
          preset.version = json["version"].get<int>();
        }
        else if (json["version"].is_string())
        {
          // Parse string version like "1.0" to int
          preset.version = std::stoi(json["version"].get<std::string>());
        }
      }
      // Also check formatVersion as an alternative
      if (json.contains("formatVersion") && json["formatVersion"].is_number())
      {
        preset.version = json["formatVersion"].get<int>();
      }
      preset.author = json.value("author", "");
      preset.category = json.value("category", "");
      preset.description = json.value("description", "");
      preset.createdAt = json.value("createdAt", "");
      preset.modifiedAt = json.value("modifiedAt", "");

      if (json.contains("tags") && json["tags"].is_array())
      {
        for (const auto& tag : json["tags"])
        {
          preset.tags.push_back(tag.get<std::string>());
        }
      }
      if (json.contains("designedPeakInputDbfs") && json["designedPeakInputDbfs"].is_number())
        preset.designedPeakInputDbfs = json["designedPeakInputDbfs"].get<double>();

      // Global settings (support both "global" and "globals" for compatibility)
      nlohmann::json globalJson;
      if (json.contains("global") && json["global"].is_object())
      {
        globalJson = json["global"];
      }
      else if (json.contains("globals") && json["globals"].is_object())
      {
        globalJson = json["globals"];
      }
      
      if (!globalJson.is_null())
      {
        preset.global.inputTrim = globalJson.value("inputTrim", 0.0);
        preset.global.outputTrim = globalJson.value("outputTrim", 0.0);
        preset.global.outputVolume = globalJson.value("outputVolume", 1.0);
        preset.global.autoLevelInput = globalJson.value("autoLevelInput", false);
        preset.global.autoLevelOutput = globalJson.value("autoLevelOutput", false);
        preset.global.transpose = globalJson.value("transpose", 0);
      }

      if (json.contains("globalSignalChain") && json["globalSignalChain"].is_object())
      {
        preset.globalSignalChain = json["globalSignalChain"].get<GlobalSignalChainConfig>();
      }

      // Signal graph
      if (json.contains("graph") && json["graph"].is_object())
      {
        const auto& graph = json["graph"];

        if (graph.contains("nodes") && graph["nodes"].is_array())
        {
          for (const auto& nodeJson : graph["nodes"])
          {
            preset.graph.nodes.push_back(DeserializeGraphNode(nodeJson, std::nullopt));
          }
        }

        if (graph.contains("edges") && graph["edges"].is_array())
        {
          for (const auto& edgeJson : graph["edges"])
          {
            preset.graph.edges.push_back(DeserializeGraphEdge(edgeJson));
          }
        }
      }

      if (json.contains("scenes") && json["scenes"].is_array())
      {
        for (const auto& sceneJson : json["scenes"])
        {
          if (sceneJson.is_object())
            preset.scenes.push_back(DeserializePresetScene(sceneJson, std::nullopt));
        }
      }

      NormalizePresetScenes(preset);

      // Embedded resources
      if (json.contains("embeddedResources") && json["embeddedResources"].is_array())
      {
        for (const auto& resJson : json["embeddedResources"])
        {
          preset.embeddedResources.push_back(DeserializeEmbeddedResource(resJson, std::nullopt));
        }
      }

      return preset;
    }
    catch (const std::exception&)
    {
      return std::nullopt;
    }
  }

  bool PresetStorage::SaveToFile(const Preset& preset, const std::filesystem::path& path)
  {
    try
    {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream file(path);
      if (!file.is_open())
      {
        return false;
      }
      const std::optional<std::filesystem::path> baseDirectory = path.parent_path();
      Preset normalizedPreset = preset;
      NormalizePresetScenes(normalizedPreset);

      nlohmann::json json;

      json["id"] = normalizedPreset.id;
      json["name"] = normalizedPreset.name;
      json["version"] = normalizedPreset.version;
      if (!normalizedPreset.author.empty())
        json["author"] = normalizedPreset.author;
      if (!normalizedPreset.category.empty())
        json["category"] = normalizedPreset.category;
      if (!normalizedPreset.description.empty())
        json["description"] = normalizedPreset.description;
      if (!normalizedPreset.createdAt.empty())
        json["createdAt"] = normalizedPreset.createdAt;
      if (!normalizedPreset.modifiedAt.empty())
        json["modifiedAt"] = normalizedPreset.modifiedAt;
      if (!normalizedPreset.tags.empty())
        json["tags"] = normalizedPreset.tags;

      nlohmann::json global;
      global["inputTrim"] = normalizedPreset.global.inputTrim;
      global["outputTrim"] = normalizedPreset.global.outputTrim;
      global["outputVolume"] = normalizedPreset.global.outputVolume;
      global["autoLevelInput"] = normalizedPreset.global.autoLevelInput;
      global["autoLevelOutput"] = normalizedPreset.global.autoLevelOutput;
      global["transpose"] = normalizedPreset.global.transpose;
      json["global"] = global;

      if (normalizedPreset.globalSignalChain.has_value())
        json["globalSignalChain"] = *normalizedPreset.globalSignalChain;

      if (!normalizedPreset.scenes.empty())
      {
        json["scenes"] = nlohmann::json::array();
        for (const auto& scene : normalizedPreset.scenes)
        {
          json["scenes"].push_back(SerializePresetScene(scene, baseDirectory));
        }
      }
      else
      {
        nlohmann::json graph;
        graph["nodes"] = nlohmann::json::array();
        for (const auto& node : normalizedPreset.graph.nodes)
        {
          graph["nodes"].push_back(SerializeGraphNode(node, baseDirectory));
        }
        graph["edges"] = nlohmann::json::array();
        for (const auto& edge : normalizedPreset.graph.edges)
        {
          graph["edges"].push_back(SerializeGraphEdge(edge));
        }
        json["graph"] = graph;
      }

      if (!normalizedPreset.embeddedResources.empty())
      {
        json["embeddedResources"] = nlohmann::json::array();
        for (const auto& res : normalizedPreset.embeddedResources)
        {
          json["embeddedResources"].push_back(SerializeEmbeddedResource(res, baseDirectory));
        }
      }

      file << json.dump(2);
      return true;
    }
    catch (const std::exception&)
    {
      return false;
    }
  }

  std::optional<Preset> PresetStorage::LoadFromFile(const std::filesystem::path& path)
  {
    try
    {
      std::ifstream file(path);
      if (!file.is_open())
      {
        return std::nullopt;
      }

      nlohmann::json json = nlohmann::json::parse(file);
      Preset preset;

      preset.id = json.value("id", "");
      preset.name = json.value("name", "");
      if (json.contains("version"))
      {
        if (json["version"].is_number())
        {
          preset.version = json["version"].get<int>();
        }
        else if (json["version"].is_string())
        {
          preset.version = std::stoi(json["version"].get<std::string>());
        }
      }
      if (json.contains("formatVersion") && json["formatVersion"].is_number())
      {
        preset.version = json["formatVersion"].get<int>();
      }
      preset.author = json.value("author", "");
      preset.category = json.value("category", "");
      preset.description = json.value("description", "");
      preset.createdAt = json.value("createdAt", "");
      preset.modifiedAt = json.value("modifiedAt", "");

      if (json.contains("tags") && json["tags"].is_array())
      {
        for (const auto& tag : json["tags"])
        {
          preset.tags.push_back(tag.get<std::string>());
        }
      }

      nlohmann::json globalJson;
      if (json.contains("global") && json["global"].is_object())
      {
        globalJson = json["global"];
      }
      else if (json.contains("globals") && json["globals"].is_object())
      {
        globalJson = json["globals"];
      }

      if (!globalJson.is_null())
      {
        preset.global.inputTrim = globalJson.value("inputTrim", 0.0);
        preset.global.outputTrim = globalJson.value("outputTrim", 0.0);
        preset.global.outputVolume = globalJson.value("outputVolume", 1.0);
        preset.global.autoLevelInput = globalJson.value("autoLevelInput", false);
        preset.global.autoLevelOutput = globalJson.value("autoLevelOutput", false);
        preset.global.transpose = globalJson.value("transpose", 0);
      }

      if (json.contains("globalSignalChain") && json["globalSignalChain"].is_object())
      {
        preset.globalSignalChain = json["globalSignalChain"].get<GlobalSignalChainConfig>();
      }

      const std::optional<std::filesystem::path> baseDirectory = path.parent_path();
      if (json.contains("graph") && json["graph"].is_object())
      {
        const auto& graph = json["graph"];

        if (graph.contains("nodes") && graph["nodes"].is_array())
        {
          for (const auto& nodeJson : graph["nodes"])
          {
            preset.graph.nodes.push_back(DeserializeGraphNode(nodeJson, baseDirectory));
          }
        }

        if (graph.contains("edges") && graph["edges"].is_array())
        {
          for (const auto& edgeJson : graph["edges"])
          {
            preset.graph.edges.push_back(DeserializeGraphEdge(edgeJson));
          }
        }
      }

      if (json.contains("scenes") && json["scenes"].is_array())
      {
        for (const auto& sceneJson : json["scenes"])
        {
          if (sceneJson.is_object())
            preset.scenes.push_back(DeserializePresetScene(sceneJson, baseDirectory));
        }
      }

      NormalizePresetScenes(preset);

      if (json.contains("embeddedResources") && json["embeddedResources"].is_array())
      {
        for (const auto& resJson : json["embeddedResources"])
        {
          preset.embeddedResources.push_back(DeserializeEmbeddedResource(resJson, baseDirectory));
        }
      }

      return preset;
    }
    catch (const std::exception&)
    {
      return std::nullopt;
    }
  }

  std::vector<Preset> PresetStorage::LoadAllFromDirectory(const std::filesystem::path& directory)
  {
    std::vector<Preset> presets;

    if (!std::filesystem::exists(directory))
    {
      return presets;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
      if (entry.is_regular_file() && entry.path().extension() == ".json")
      {
        auto preset = LoadFromFile(entry.path());
        if (preset)
        {
          presets.push_back(*preset);
        }
      }
    }

    return presets;
  }

  void PresetStorage::SaveAllToDirectory(const std::vector<Preset>& presets, const std::filesystem::path& directory)
  {
    std::filesystem::create_directories(directory);

    for (const auto& preset : presets)
    {
      auto filename = preset.id + ".json";
      (void)SaveToFile(preset, directory / filename);
    }
  }

} // namespace guitarfx
