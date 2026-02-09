#include "dsp/effects/CompositeEffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetTypesJson.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace guitarfx
{
  // ─────────────────────────────────────────────────────────────
  // CompositeEffectProcessor
  // ─────────────────────────────────────────────────────────────

  CompositeEffectProcessor::CompositeEffectProcessor(const CompositeEffectDefinition &definition)
      : mDefinition(definition)
  {
    BuildParamMap();
    mInnerExecutor.SetGraph(mDefinition.innerGraph);
  }

  void CompositeEffectProcessor::BuildParamMap()
  {
    mParamMap.clear();
    for (const auto &ep : mDefinition.exposedParams)
    {
      mParamMap[ep.paramId] = &ep;
    }
  }

  void CompositeEffectProcessor::SetResourceLibrary(ResourceLibrary *library)
  {
    mInnerExecutor.SetResourceLibrary(library);
  }

  void CompositeEffectProcessor::Prepare(double sampleRate, int maxBlockSize)
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mInnerExecutor.Prepare(sampleRate, maxBlockSize);
  }

  void CompositeEffectProcessor::Reset()
  {
    mInnerExecutor.Reset();
  }

  void CompositeEffectProcessor::Process(float **inputs, float **outputs, int numSamples)
  {
    if (!mEnabled)
    {
      // Bypass: copy input to output
      for (int ch = 0; ch < 2; ++ch)
      {
        if (inputs[ch] && outputs[ch])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            outputs[ch][i] = inputs[ch][i];
          }
        }
      }
      return;
    }

    mInnerExecutor.Process(inputs, outputs, numSamples);
  }

  void CompositeEffectProcessor::SetParam(const std::string &key, double value)
  {
    auto it = mParamMap.find(key);
    if (it != mParamMap.end())
    {
      const ExposedParameter *ep = it->second;

      // Clamp to exposed range
      double clamped = std::clamp(value, ep->minValue, ep->maxValue);

      // Route to inner node
      mInnerExecutor.SetNodeParam(ep->nodeId, ep->nodeParamKey, clamped);
    }
  }

  void CompositeEffectProcessor::SetConfig(const std::string &key, const std::string &value)
  {
    // Config values are not currently exposed on composites.
    // Could be extended if needed to route to inner node configs.
    (void)key;
    (void)value;
  }

  double CompositeEffectProcessor::GetParam(const std::string &key) const
  {
    auto it = mParamMap.find(key);
    if (it != mParamMap.end())
    {
      return it->second->defaultValue;
    }
    return 0.0;
  }

  std::string CompositeEffectProcessor::GetConfig(const std::string &key) const
  {
    (void)key;
    return "";
  }

  std::string CompositeEffectProcessor::GetType() const
  {
    return mDefinition.GetEffectTypeId();
  }

  std::string CompositeEffectProcessor::GetCategory() const
  {
    return mDefinition.category;
  }

  // ─────────────────────────────────────────────────────────────
  // CompositeEffectLibrary
  // ─────────────────────────────────────────────────────────────

  void CompositeEffectLibrary::AddDefinition(const CompositeEffectDefinition &def)
  {
    if (!def.IsValid())
      return;

    // Replace existing definition with same ID
    auto it = std::find_if(mDefinitions.begin(), mDefinitions.end(),
                           [&](const auto &d) { return d.id == def.id; });

    if (it != mDefinitions.end())
    {
      UnregisterDefinition(it->id);
      *it = def;
    }
    else
    {
      mDefinitions.push_back(def);
    }

    RegisterDefinition(def);
  }

  void CompositeEffectLibrary::RemoveDefinition(const std::string &id)
  {
    UnregisterDefinition(id);
    mDefinitions.erase(
        std::remove_if(mDefinitions.begin(), mDefinitions.end(),
                       [&](const auto &d) { return d.id == id; }),
        mDefinitions.end());
  }

  const CompositeEffectDefinition *CompositeEffectLibrary::GetDefinition(const std::string &id) const
  {
    for (const auto &def : mDefinitions)
    {
      if (def.id == id)
        return &def;
    }
    return nullptr;
  }

  bool CompositeEffectLibrary::HasDefinition(const std::string &id) const
  {
    return GetDefinition(id) != nullptr;
  }

  void CompositeEffectLibrary::RegisterAll()
  {
    for (const auto &def : mDefinitions)
    {
      RegisterDefinition(def);
    }
  }

  void CompositeEffectLibrary::RegisterDefinition(const CompositeEffectDefinition &def)
  {
    auto &registry = EffectRegistry::Instance();
    const std::string typeId = def.GetEffectTypeId();

    // Don't re-register if already present
    if (registry.HasType(typeId))
      return;

    EffectTypeInfo info;
    info.type = typeId;
    info.displayName = def.name;
    info.category = def.category;
    info.description = def.description;
    info.requiresResource = false;

    // Build parameter definitions from exposed parameters
    for (const auto &ep : def.exposedParams)
    {
      info.parameters.push_back({ep.paramId, ep.displayName, ep.defaultValue,
                                 ep.minValue, ep.maxValue, ep.unit});
    }

    // Capture definition by value for the factory lambda
    CompositeEffectDefinition capturedDef = def;
    ResourceLibrary *resLib = mResourceLibrary;

    registry.Register(typeId, info, [capturedDef, resLib]()
                      {
      auto processor = std::make_unique<CompositeEffectProcessor>(capturedDef);
      if (resLib)
        processor->SetResourceLibrary(resLib);
      return processor; });
  }

  void CompositeEffectLibrary::UnregisterDefinition(const std::string &id)
  {
    // Build the type ID from the definition ID
    std::string typeId = "composite:" + id;
    EffectRegistry::Instance().Unregister(typeId);
  }

  void CompositeEffectLibrary::LoadFromDirectory(const std::filesystem::path &dir)
  {
    if (!std::filesystem::exists(dir))
      return;

    auto loadDir = [this](const std::filesystem::path &subdir)
    {
      if (!std::filesystem::exists(subdir))
        return;

      for (const auto &entry : std::filesystem::directory_iterator(subdir))
      {
        if (!entry.is_regular_file())
          continue;
        if (entry.path().extension() != ".json")
          continue;

        try
        {
          std::ifstream file(entry.path());
          if (!file.is_open())
            continue;

          nlohmann::json j = nlohmann::json::parse(file);
          CompositeEffectDefinition def;

          def.id = j.value("id", "");
          def.name = j.value("name", "");
          def.category = j.value("category", "");
          def.description = j.value("description", "");
          def.author = j.value("author", "");
          def.version = j.value("version", 1);
          def.createdAt = j.value("createdAt", "");
          def.modifiedAt = j.value("modifiedAt", "");

          if (j.contains("tags") && j["tags"].is_array())
          {
            for (const auto &tag : j["tags"])
            {
              if (tag.is_string())
                def.tags.push_back(tag.get<std::string>());
            }
          }

          if (j.contains("innerGraph") && j["innerGraph"].is_object())
          {
            def.innerGraph = DeserializeSignalGraph(j["innerGraph"]);
          }

          if (j.contains("exposedParams") && j["exposedParams"].is_array())
          {
            for (const auto &epJson : j["exposedParams"])
            {
              ExposedParameter ep;
              ep.paramId = epJson.value("paramId", "");
              ep.displayName = epJson.value("displayName", "");
              ep.nodeId = epJson.value("nodeId", "");
              ep.nodeParamKey = epJson.value("nodeParamKey", "");
              ep.minValue = epJson.value("minValue", 0.0);
              ep.maxValue = epJson.value("maxValue", 1.0);
              ep.defaultValue = epJson.value("defaultValue", 0.0);
              ep.unit = epJson.value("unit", "");
              ep.curve = epJson.value("curve", "linear");
              def.exposedParams.push_back(ep);
            }
          }

          if (j.contains("layout"))
          {
            def.layoutJson = j["layout"].dump();
          }

          if (def.IsValid())
          {
            AddDefinition(def);
          }
        }
        catch (const std::exception &)
        {
          // Skip malformed definition files
          continue;
        }
      }
    };

    loadDir(dir / "factory");
    loadDir(dir / "user");
  }

  bool CompositeEffectLibrary::SaveDefinition(const CompositeEffectDefinition &def,
                                              const std::filesystem::path &userDir)
  {
    if (!def.IsValid())
      return false;

    try
    {
      std::filesystem::create_directories(userDir);

      nlohmann::json j;
      j["id"] = def.id;
      j["name"] = def.name;
      j["category"] = def.category;
      j["description"] = def.description;
      j["author"] = def.author;
      j["version"] = def.version;
      j["createdAt"] = def.createdAt;
      j["modifiedAt"] = def.modifiedAt;

      if (!def.tags.empty())
      {
        j["tags"] = def.tags;
      }

      j["innerGraph"] = SerializeSignalGraph(def.innerGraph);

      j["exposedParams"] = nlohmann::json::array();
      for (const auto &ep : def.exposedParams)
      {
        nlohmann::json epJson;
        epJson["paramId"] = ep.paramId;
        epJson["displayName"] = ep.displayName;
        epJson["nodeId"] = ep.nodeId;
        epJson["nodeParamKey"] = ep.nodeParamKey;
        epJson["minValue"] = ep.minValue;
        epJson["maxValue"] = ep.maxValue;
        epJson["defaultValue"] = ep.defaultValue;
        if (!ep.unit.empty())
          epJson["unit"] = ep.unit;
        if (ep.curve != "linear")
          epJson["curve"] = ep.curve;
        j["exposedParams"].push_back(epJson);
      }

      if (!def.layoutJson.empty())
      {
        try
        {
          j["layout"] = nlohmann::json::parse(def.layoutJson);
        }
        catch (...)
        {
          // Store as raw string if parse fails
        }
      }

      // Use the definition ID as filename
      std::string filename = def.id + ".json";
      std::filesystem::path filePath = userDir / filename;

      std::ofstream file(filePath);
      if (!file.is_open())
        return false;

      file << j.dump(2);
      return true;
    }
    catch (const std::exception &)
    {
      return false;
    }
  }

  bool CompositeEffectLibrary::DeleteDefinition(const std::string &id,
                                                const std::filesystem::path &userDir)
  {
    std::filesystem::path filePath = userDir / (id + ".json");
    if (std::filesystem::exists(filePath))
    {
      std::filesystem::remove(filePath);
    }
    RemoveDefinition(id);
    return true;
  }

} // namespace guitarfx
