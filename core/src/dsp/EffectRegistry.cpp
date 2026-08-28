#include "dsp/EffectRegistry.h"
#include "dsp/EffectProcessor.h"

namespace guitarfx
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

    for (const auto& alias : info.aliases)
    {
        mAliases[alias] = type;
    }
}

void EffectRegistry::Unregister(const std::string& type)
{
    // Remove any aliases that pointed to this type
    for (auto it = mAliases.begin(); it != mAliases.end();)
    {
        if (it->second == type)
        {
            it = mAliases.erase(it);
        }
        else
        {
            ++it;
        }
    }

    mTypeInfo.erase(type);
    mFactories.erase(type);
}

std::string EffectRegistry::Resolve(const std::string& type) const
{
    auto it = mAliases.find(type);
    return (it != mAliases.end()) ? it->second : type;
}

std::unique_ptr<EffectProcessor> EffectRegistry::Create(const std::string& type) const
{
    const std::string canonical = Resolve(type);
    auto it = mFactories.find(canonical);

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
    return mFactories.count(Resolve(type)) > 0;
}

std::optional<EffectTypeInfo> EffectRegistry::GetTypeInfo(const std::string& type) const
{
    auto it = mTypeInfo.find(Resolve(type));

    if (it != mTypeInfo.end())
    {
        return it->second;
    }

    return std::nullopt;
}
} // namespace guitarfx
