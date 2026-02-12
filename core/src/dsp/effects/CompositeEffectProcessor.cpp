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
    mInnerPrepared = true;
    ApplyPendingResourceOverrides();
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

  bool CompositeEffectProcessor::LoadResources(const std::vector<ResourceRef> &refs,
                                               const std::vector<std::filesystem::path> & /*paths*/)
  {
    mPendingResourceOverrides.clear();

    if (refs.empty() || mDefinition.exposedResources.empty())
    {
      return false;
    }

    const std::size_t count = std::min(refs.size(), mDefinition.exposedResources.size());
    for (std::size_t i = 0; i < count; ++i)
    {
      const auto &surface = mDefinition.exposedResources[i];
      ResourceRef mapped = refs[i];

      if (mapped.resourceType.empty())
        mapped.resourceType = surface.resourceType;

      if (mapped.parameterId.empty() && !surface.parameterId.empty())
        mapped.parameterId = surface.parameterId;

      if (!mapped.parameterValue.has_value() && surface.parameterValue.has_value())
        mapped.parameterValue = *surface.parameterValue;

      if (!mapped.parameterId.empty() && mapped.parameterValue.has_value())
        mapped.parameters[mapped.parameterId] = *mapped.parameterValue;

      mPendingResourceOverrides.push_back({surface.nodeId, std::move(mapped)});
    }

    if (mInnerPrepared)
    {
      ApplyPendingResourceOverrides();
    }

    return !mPendingResourceOverrides.empty();
  }

  std::string CompositeEffectProcessor::GetType() const
  {
    return mDefinition.GetEffectTypeId();
  }

  std::string CompositeEffectProcessor::GetCategory() const
  {
    return mDefinition.category;
  }

  void CompositeEffectProcessor::ApplyPendingResourceOverrides()
  {
    if (!mInnerPrepared || mPendingResourceOverrides.empty())
      return;

    for (const auto &entry : mPendingResourceOverrides)
    {
      mInnerExecutor.LoadNodeResource(entry.nodeId, entry.ref);
    }
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
          CompositeEffectDefinition def = DeserializeCompositeEffectDefinition(j);

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
      nlohmann::json j = SerializeCompositeEffectDefinition(def);

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
