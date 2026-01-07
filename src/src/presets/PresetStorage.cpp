#include "presets/PresetStorage.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

namespace namguitar
{
  namespace
  {
    nlohmann::json SerializeResourceRef(const ResourceRef& ref)
    {
      nlohmann::json json;
      if (!ref.resourceType.empty())
        json["resourceType"] = ref.resourceType;
      if (!ref.resourceId.empty())
        json["resourceId"] = ref.resourceId;
      if (!ref.filePath.empty())
        json["filePath"] = ref.filePath.string();
      if (!ref.embeddedId.empty())
        json["embeddedId"] = ref.embeddedId;
      return json;
    }

    ResourceRef DeserializeResourceRef(const nlohmann::json& json)
    {
      ResourceRef ref;
      // Support both long-form (resourceType/resourceId) and short-form (type/id)
      ref.resourceType = json.value("resourceType", json.value("type", ""));
      ref.resourceId = json.value("resourceId", json.value("id", ""));
      ref.filePath = json.value("filePath", "");
      ref.embeddedId = json.value("embeddedId", "");
      return ref;
    }

    nlohmann::json SerializeNode(const GraphNode& node)
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

      if (node.resource && node.resource->IsValid())
      {
        json["resource"] = SerializeResourceRef(*node.resource);
      }

      return json;
    }

    GraphNode DeserializeNode(const nlohmann::json& json)
    {
      GraphNode node;
      node.id = json.value("id", "");
      node.type = json.value("type", "");
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

      if (json.contains("resource") && json["resource"].is_object())
      {
        node.resource = DeserializeResourceRef(json["resource"]);
      }

      return node;
    }

    nlohmann::json SerializeEdge(const GraphEdge& edge)
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

    GraphEdge DeserializeEdge(const nlohmann::json& json)
    {
      GraphEdge edge;
      edge.from = json.value("from", "");
      edge.to = json.value("to", "");
      edge.fromPort = json.value("fromPort", 0);
      edge.toPort = json.value("toPort", 0);
      edge.gain = json.value("gain", 1.0);
      return edge;
    }

    nlohmann::json SerializeEmbeddedResource(const EmbeddedResource& res)
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
        json["originalPath"] = res.originalPath.string();
      return json;
    }

    EmbeddedResource DeserializeEmbeddedResource(const nlohmann::json& json)
    {
      EmbeddedResource res;
      res.id = json.value("id", "");
      res.type = json.value("type", "");
      res.name = json.value("name", "");
      res.hash = json.value("hash", "");
      res.data = json.value("data", "");
      res.originalPath = json.value("originalPath", "");
      return res;
    }
  } // namespace

  std::string PresetStorage::SerializeToJson(const Preset& preset)
  {
    nlohmann::json json;

    // Metadata
    json["id"] = preset.id;
    json["name"] = preset.name;
    json["version"] = preset.version;
    if (!preset.author.empty())
      json["author"] = preset.author;
    if (!preset.category.empty())
      json["category"] = preset.category;
    if (!preset.description.empty())
      json["description"] = preset.description;
    if (!preset.createdAt.empty())
      json["createdAt"] = preset.createdAt;
    if (!preset.modifiedAt.empty())
      json["modifiedAt"] = preset.modifiedAt;
    if (!preset.tags.empty())
      json["tags"] = preset.tags;

    // Global settings
    nlohmann::json global;
    global["inputTrim"] = preset.global.inputTrim;
    global["outputTrim"] = preset.global.outputTrim;
    global["transpose"] = preset.global.transpose;
    json["global"] = global;

    // Signal graph
    nlohmann::json graph;
    graph["nodes"] = nlohmann::json::array();
    for (const auto& node : preset.graph.nodes)
    {
      graph["nodes"].push_back(SerializeNode(node));
    }
    graph["edges"] = nlohmann::json::array();
    for (const auto& edge : preset.graph.edges)
    {
      graph["edges"].push_back(SerializeEdge(edge));
    }
    json["graph"] = graph;

    // Embedded resources
    if (!preset.embeddedResources.empty())
    {
      json["embeddedResources"] = nlohmann::json::array();
      for (const auto& res : preset.embeddedResources)
      {
        json["embeddedResources"].push_back(SerializeEmbeddedResource(res));
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
        preset.global.transpose = globalJson.value("transpose", 0);
      }

      // Signal graph
      if (json.contains("graph") && json["graph"].is_object())
      {
        const auto& graph = json["graph"];

        if (graph.contains("nodes") && graph["nodes"].is_array())
        {
          for (const auto& nodeJson : graph["nodes"])
          {
            preset.graph.nodes.push_back(DeserializeNode(nodeJson));
          }
        }

        if (graph.contains("edges") && graph["edges"].is_array())
        {
          for (const auto& edgeJson : graph["edges"])
          {
            preset.graph.edges.push_back(DeserializeEdge(edgeJson));
          }
        }
      }

      // Embedded resources
      if (json.contains("embeddedResources") && json["embeddedResources"].is_array())
      {
        for (const auto& resJson : json["embeddedResources"])
        {
          preset.embeddedResources.push_back(DeserializeEmbeddedResource(resJson));
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
      file << SerializeToJson(preset);
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
      std::stringstream buffer;
      buffer << file.rdbuf();
      return DeserializeFromJson(buffer.str());
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
      SaveToFile(preset, directory / filename);
    }
  }

} // namespace namguitar
