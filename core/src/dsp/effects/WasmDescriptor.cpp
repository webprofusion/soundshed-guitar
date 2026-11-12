// WasmDescriptor.cpp - A Custom Effect's description of itself, read and checked: see
// WasmDescriptor.h. WasmEffect.cpp runs the module it describes.

#include "dsp/effects/WasmDescriptor.h"

#include "dsp/FiniteCheck.h"
#include "resources/CustomEffectLibrary.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace guitarfx::wasm_descriptor
{
namespace
{
std::string LimitDescriptorText(const std::string& value)
{
    return value.size() > kMaxDescriptorTextLength ? value.substr(0, kMaxDescriptorTextLength) : value;
}

std::string TrimDescriptorToken(std::string value)
{
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool ParseBoolLike(const std::string& value)
{
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

std::optional<double> ParseDoubleLike(const std::string& value)
{
    try
    {
        return std::stod(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::vector<std::string> SplitLabels(const std::string& value)
{
    std::vector<std::string> labels;
    std::string current;
    std::istringstream stream(value);

    while (std::getline(stream, current, '|'))
    {
        labels.push_back(TrimDescriptorToken(current));
    }

    return labels;
}

// Params and resources each in the order their ids first appear, which is their order in the UI.
struct IdKeyedBlocks
{
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> blocks;
    std::unordered_map<std::string, std::size_t> order;

    void Set(const std::string& id, const std::string& field, const std::string& value)
    {
        auto [orderIt, inserted] = order.try_emplace(id, blocks.size());

        if (inserted)
        {
            blocks.emplace_back(id, std::map<std::string, std::string>{});
        }

        blocks[orderIt->second].second[field] = value;
    }
};

// <prefix>.<id>.<field>, as ABI 2 keys params and resources; an id has no dots, so the field is
// everything after the second. The key's id and field, or nothing with the reason in error: a
// numbered ABI 1 block, a bad id, no field, or one of the fields ABI 1 numbered blocks with.
std::optional<std::pair<std::string, std::string>> ParseIdKeyedDescriptorKey(const std::string& key,
                                                                             std::string_view prefix,
                                                                             std::string& error)
{
    const std::string noun(prefix);
    const std::string_view rest = std::string_view(key).substr(prefix.size() + 1);
    const std::size_t fieldSeparator = rest.find('.');
    const std::string_view id = rest.substr(0, fieldSeparator);

    if (!id.empty() && std::all_of(id.begin(), id.end(), IsAsciiDigit))
    {
        error = "WASM descriptor key '" + key + "' numbers its " + noun + ", as ABI 1 did; ABI 2 keys " + noun +
                "s by id: " + noun + ".<id>.<field>." + kAbiMigrationHint;
        return std::nullopt;
    }

    if (!WasmEffect::IsValidGuestId(id))
    {
        error = "WASM descriptor " + noun + " id '" + std::string(id) +
                "' must be a letter followed by up to 31 letters, digits or underscores.";
        return std::nullopt;
    }

    if (fieldSeparator == std::string_view::npos || fieldSeparator + 1 >= rest.size())
    {
        error = "WASM descriptor key '" + key + "' names no field: use " + noun + ".<id>.<field>.";
        return std::nullopt;
    }

    std::string field(rest.substr(fieldSeparator + 1));

    if (field == "slot" || field == "id")
    {
        error = "WASM descriptor key '" + key + "': ABI 2 " + noun + "s have no '" + field +
                "', the id in the key is all there is." + kAbiMigrationHint;
        return std::nullopt;
    }

    return std::make_pair(std::string(id), std::move(field));
}
} // namespace

std::optional<WasmModuleDescriptor> BuildDescriptorFromEntries(const std::vector<WasmMetadataEntry>& entries,
                                                               std::string* error)
{
    WasmModuleDescriptor descriptor;

    const auto fail = [error](std::string message) -> std::optional<WasmModuleDescriptor> {
        if (error)
        {
            *error = std::move(message);
        }

        return std::nullopt;
    };

    IdKeyedBlocks parameterFields;
    IdKeyedBlocks resourceFields;
    std::string thumbnailBase64;
    std::string thumbnailMimeType = "image/png";

    for (const auto& rawEntry : entries)
    {
        // Every value is the module's own word, bound for UI markup and saved presets.
        const WasmMetadataEntry entry{LimitDescriptorText(rawEntry.key), rawEntry.key.rfind("effect.thumbnail", 0) == 0
                                                                             ? rawEntry.value
                                                                             : LimitDescriptorText(rawEntry.value)};

        // The thumbnail keys are replaced by one checked effect.thumbnailDataUrl below; the UI reads
        // these entries again from the node config, so they are kept only as sanitised here.
        if (entry.key.rfind("effect.thumbnail", 0) != 0)
        {
            descriptor.entries.push_back(entry.key == "effect.category"
                                             ? WasmMetadataEntry{entry.key, SanitizeCustomEffectCategory(entry.value)}
                                             : entry);
        }

        if (entry.key == "effect.name" || entry.key == "effect.title")
        {
            descriptor.displayName = entry.value;
            continue;
        }

        if (entry.key == "effect.description")
        {
            descriptor.description = entry.value;
            continue;
        }

        if (entry.key == "effect.version")
        {
            descriptor.version = entry.value;
            continue;
        }

        if (entry.key == "effect.category")
        {
            descriptor.category = SanitizeCustomEffectCategory(entry.value);
            continue;
        }

        if (entry.key == "effect.thumbnailDataUrl")
        {
            descriptor.thumbnailDataUrl = entry.value;
            continue;
        }

        if (entry.key == "effect.thumbnailBase64")
        {
            thumbnailBase64 = entry.value;
            continue;
        }

        if (entry.key == "effect.thumbnailMimeType")
        {
            thumbnailMimeType = entry.value.empty() ? "image/png" : entry.value;
            continue;
        }

        for (const std::string_view prefix : {std::string_view("param"), std::string_view("resource")})
        {
            if (entry.key.size() <= prefix.size() || entry.key.compare(0, prefix.size(), prefix) != 0 ||
                entry.key[prefix.size()] != '.')
            {
                continue;
            }

            std::string keyError;
            const auto parsed = ParseIdKeyedDescriptorKey(entry.key, prefix, keyError);

            if (!parsed)
            {
                return fail(std::move(keyError));
            }

            (prefix == "param" ? parameterFields : resourceFields).Set(parsed->first, parsed->second, entry.value);
        }
    }

    if (parameterFields.blocks.size() > WasmEffect::kMaxGuestParams)
    {
        return fail("WASM descriptor declares " + std::to_string(parameterFields.blocks.size()) +
                    " params; the limit is " + std::to_string(WasmEffect::kMaxGuestParams) + ".");
    }

    if (resourceFields.blocks.size() > WasmEffect::kMaxGuestResources)
    {
        return fail("WASM descriptor declares " + std::to_string(resourceFields.blocks.size()) +
                    " resources; the limit is " + std::to_string(WasmEffect::kMaxGuestResources) + ".");
    }

    for (const auto& [id, fields] : parameterFields.blocks)
    {
        if (WasmEffect::IsHostParamId(id))
        {
            return fail("WASM descriptor param id '" + id +
                        "' is the id of one of the host's own controls; give the param another.");
        }

        WasmGuestParameterDescriptor paramDescriptor;
        paramDescriptor.definition.id = id;

        if (const auto titleIt = fields.find("title"); titleIt != fields.end() && !titleIt->second.empty())
        {
            paramDescriptor.definition.displayName = titleIt->second;
        }
        else if (const auto nameIt = fields.find("name"); nameIt != fields.end() && !nameIt->second.empty())
        {
            paramDescriptor.definition.displayName = nameIt->second;
        }
        else
        {
            paramDescriptor.definition.displayName = id;
        }

        if (const auto defaultIt = fields.find("default"); defaultIt != fields.end())
        {
            paramDescriptor.definition.defaultValue = ParseDoubleLike(defaultIt->second).value_or(0.0);
        }

        if (const auto minIt = fields.find("min"); minIt != fields.end())
        {
            paramDescriptor.definition.minValue = ParseDoubleLike(minIt->second).value_or(0.0);
        }

        if (const auto maxIt = fields.find("max"); maxIt != fields.end())
        {
            paramDescriptor.definition.maxValue = ParseDoubleLike(maxIt->second).value_or(1.0);
        }

        if (paramDescriptor.definition.maxValue < paramDescriptor.definition.minValue)
        {
            return fail("WASM descriptor param '" + id + "' has max < min.");
        }

        if (const auto unitIt = fields.find("unit"); unitIt != fields.end())
        {
            paramDescriptor.definition.unit = unitIt->second;
        }

        if (const auto groupIt = fields.find("group"); groupIt != fields.end())
        {
            paramDescriptor.definition.group = groupIt->second;
        }

        if (const auto advancedIt = fields.find("advanced"); advancedIt != fields.end())
        {
            paramDescriptor.definition.advanced = ParseBoolLike(advancedIt->second);
        }

        if (const auto stepIt = fields.find("step"); stepIt != fields.end())
        {
            paramDescriptor.definition.step = ParseDoubleLike(stepIt->second).value_or(0.0);
        }

        if (const auto labelsIt = fields.find("labels"); labelsIt != fields.end())
        {
            paramDescriptor.definition.labels = SplitLabels(labelsIt->second);
        }

        if (!paramDescriptor.definition.labels.empty() && paramDescriptor.definition.unit.empty())
        {
            paramDescriptor.definition.unit = "enum";
        }

        if (!paramDescriptor.definition.labels.empty() && paramDescriptor.definition.step == 0.0)
        {
            paramDescriptor.definition.step = 1.0;
        }

        descriptor.parameters.push_back(std::move(paramDescriptor));
    }

    for (const auto& [id, fields] : resourceFields.blocks)
    {
        if (id == WasmEffect::kModuleResourceId)
        {
            return fail("WASM descriptor resource id '" + id +
                        "' is the module's own resource; give the resource another.");
        }

        ExposedResource resource;
        resource.resourceId = id;

        if (const auto titleIt = fields.find("title"); titleIt != fields.end() && !titleIt->second.empty())
        {
            resource.displayName = titleIt->second;
        }
        else if (const auto nameIt = fields.find("name"); nameIt != fields.end() && !nameIt->second.empty())
        {
            resource.displayName = nameIt->second;
        }
        else
        {
            resource.displayName = id;
        }

        resource.resourceType = fields.contains("type") ? fields.at("type") : "blob";
        // The node resource the UI's picker writes. Presets keep each file with its id
        // (kResourceIdsConfigKey), so this place is only where the controller puts it.
        resource.resourceIndex = static_cast<int>(descriptor.exposedResources.size()) + 1;
        resource.allowBrowseFile = !fields.contains("allowBrowseFile") || ParseBoolLike(fields.at("allowBrowseFile"));

        if (const auto parameterIdIt = fields.find("parameterId"); parameterIdIt != fields.end())
        {
            resource.parameterId = parameterIdIt->second;
        }

        if (const auto parameterValueIt = fields.find("parameterValue"); parameterValueIt != fields.end())
        {
            resource.parameterValue = ParseDoubleLike(parameterValueIt->second).value_or(0.0);
        }

        descriptor.exposedResources.push_back(std::move(resource));
    }

    if (descriptor.thumbnailDataUrl.empty() && !thumbnailBase64.empty())
    {
        descriptor.thumbnailDataUrl = "data:" + thumbnailMimeType + ";base64," + thumbnailBase64;
    }

    descriptor.thumbnailDataUrl = SanitizeCustomEffectThumbnail(descriptor.thumbnailDataUrl);

    if (!descriptor.thumbnailDataUrl.empty())
    {
        descriptor.entries.push_back({"effect.thumbnailDataUrl", descriptor.thumbnailDataUrl});
    }

    return descriptor;
}

std::optional<WasmModuleDescriptor> ParseDescriptorBlob(const std::string& text, std::string* error)
{
    std::vector<WasmMetadataEntry> entries;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        line = TrimDescriptorToken(line);

        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const std::size_t separator = line.find('=');

        if (separator == std::string::npos)
        {
            if (error)
            {
                *error = "WASM descriptor line is missing '=': " + line;
            }

            return std::nullopt;
        }

        WasmMetadataEntry entry;
        entry.key = TrimDescriptorToken(line.substr(0, separator));
        entry.value = TrimDescriptorToken(line.substr(separator + 1));

        if (entry.key.empty())
        {
            if (error)
            {
                *error = "WASM descriptor contains an empty key.";
            }

            return std::nullopt;
        }

        entries.push_back(std::move(entry));
    }

    return BuildDescriptorFromEntries(entries, error);
}
} // namespace guitarfx::wasm_descriptor
