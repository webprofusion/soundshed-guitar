#include "dsp/EffectRegistry.h"
#include "dsp/EffectProcessor.h"

namespace namguitar
{
  EffectRegistry& EffectRegistry::Instance()
  {
    static EffectRegistry instance;
    return instance;
  }

  void EffectRegistry::Register(const std::string& type, const EffectTypeInfo& info, EffectFactory factory)
  {
    mTypeInfo[type] = info;
    mFactories[type] = std::move(factory);
  }

  void EffectRegistry::Unregister(const std::string& type)
  {
    mTypeInfo.erase(type);
    mFactories.erase(type);
  }

  std::unique_ptr<EffectProcessor> EffectRegistry::Create(const std::string& type) const
  {
    auto it = mFactories.find(type);
    if (it != mFactories.end())
    {
      return it->second();
    }
    // Return passthrough for unknown types
    return std::make_unique<PassthroughProcessor>();
  }

  std::vector<EffectTypeInfo> EffectRegistry::GetAllTypes() const
  {
    std::vector<EffectTypeInfo> result;
    result.reserve(mTypeInfo.size());
    for (const auto& [type, info] : mTypeInfo)
    {
      result.push_back(info);
    }
    return result;
  }

  std::vector<EffectTypeInfo> EffectRegistry::GetTypesByCategory(const std::string& category) const
  {
    std::vector<EffectTypeInfo> result;
    for (const auto& [type, info] : mTypeInfo)
    {
      if (info.category == category)
      {
        result.push_back(info);
      }
    }
    return result;
  }

  std::vector<std::string> EffectRegistry::GetCategories() const
  {
    std::vector<std::string> categories;
    for (const auto& [type, info] : mTypeInfo)
    {
      if (std::find(categories.begin(), categories.end(), info.category) == categories.end())
      {
        categories.push_back(info.category);
      }
    }
    return categories;
  }

  bool EffectRegistry::HasType(const std::string& type) const
  {
    return mFactories.count(type) > 0;
  }

  std::optional<EffectTypeInfo> EffectRegistry::GetTypeInfo(const std::string& type) const
  {
    auto it = mTypeInfo.find(type);
    if (it != mTypeInfo.end())
    {
      return it->second;
    }
    return std::nullopt;
  }

} // namespace namguitar
