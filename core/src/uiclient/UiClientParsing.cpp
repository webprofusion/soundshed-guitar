/**
 * UiClientParsing.cpp - Engine payloads into ClientState values.
 */

#include "uiclient/UiClientParsing.h"

#include "presets/PresetStorage.h"

namespace guitarfx::uiclient::parse
{
std::vector<std::string> StringArray(const nlohmann::json& object, const char* key)
{
    std::vector<std::string> result;

    if (!object.is_object() || !object.contains(key) || !object[key].is_array())
    {
        return result;
    }

    for (const auto& value : object[key])
    {
        if (value.is_string())
        {
            result.push_back(value.get<std::string>());
        }
    }

    return result;
}

std::optional<Preset> ParsePreset(const nlohmann::json& preset)
{
    if (!preset.is_object())
    {
        return std::nullopt;
    }

    return PresetStorage::DeserializeFromJson(preset.dump());
}

LibraryResource Resource(const nlohmann::json& entry, const std::string& type)
{
    LibraryResource resource;
    resource.type = type;
    resource.id = entry.value("id", "");
    resource.name = entry.value("name", resource.id);
    resource.category = entry.value("category", "");
    resource.description = entry.value("description", "");
    resource.filePath = entry.value("filePath", "");
    resource.hash = entry.value("hash", "");
    resource.fileMissing = entry.value("fileMissing", false);
    resource.tags = StringArray(entry, "tags");

    if (entry.contains("metadata") && entry["metadata"].is_object())
    {
        resource.metadata = entry["metadata"];
    }

    return resource;
}

std::map<std::string, std::vector<LibraryResource>> ResourceLibrary(const nlohmann::json& library)
{
    std::map<std::string, std::vector<LibraryResource>> result;

    for (const auto& [type, entries] : library.items())
    {
        if (!entries.is_array())
        {
            continue; // "totalCount"
        }

        auto& list = result[type];

        for (const auto& entry : entries)
        {
            if (entry.is_object())
            {
                list.push_back(Resource(entry, type));
            }
        }
    }

    return result;
}

EffectParamInfo EffectParam(const nlohmann::json& param)
{
    EffectParamInfo info;
    info.key = param.value("key", "");
    info.name = param.value("name", info.key);
    info.unit = param.value("unit", "");
    info.group = param.value("group", "");
    info.minValue = param.value("min", 0.0);
    info.maxValue = param.value("max", 1.0);
    info.defaultValue = param.value("default", info.minValue);
    info.step = param.value("step", 0.0);
    info.advanced = param.value("advanced", false);
    info.logTaper = param.value("taper", std::string{}) == "log";
    info.labels = StringArray(param, "labels");
    return info;
}

EffectPresetInfo EffectPreset(const nlohmann::json& preset, const std::string& defaultSource)
{
    EffectPresetInfo info;
    info.id = preset.value("id", "");
    info.name = preset.value("name", info.id);
    info.source = preset.value("source", defaultSource);

    if (preset.contains("parameters") && preset["parameters"].is_object())
    {
        for (const auto& [key, value] : preset["parameters"].items())
        {
            if (value.is_number())
            {
                info.parameters[key] = value.get<double>();
            }
        }
    }

    return info;
}

std::vector<EffectTypeInfo> EffectCatalog(const nlohmann::json& catalog)
{
    std::vector<EffectTypeInfo> result;

    if (!catalog.is_array())
    {
        return result;
    }

    for (const auto& entry : catalog)
    {
        if (!entry.is_object())
        {
            continue;
        }

        EffectTypeInfo info;
        info.type = entry.value("type", "");
        info.name = entry.value("name", info.type);
        info.category = entry.value("category", "");
        info.requiresResource = entry.value("requiresResource", false);
        info.resourceType = entry.value("resourceType", "");

        if (entry.contains("parameters") && entry["parameters"].is_array())
        {
            for (const auto& param : entry["parameters"])
            {
                if (param.is_object())
                {
                    info.parameters.push_back(EffectParam(param));
                }
            }
        }

        if (entry.contains("presets") && entry["presets"].is_array())
        {
            for (const auto& preset : entry["presets"])
            {
                if (preset.is_object())
                {
                    info.presets.push_back(EffectPreset(preset, "factory"));
                }
            }
        }

        if (!info.type.empty())
        {
            result.push_back(std::move(info));
        }
    }

    return result;
}
} // namespace guitarfx::uiclient::parse
