/**
 * ExposedResources.cpp - The resource slots a node shows: its type's, then a Custom Effect's
 * descriptor's (see ExposedResources.h).
 */

#include "uiclient/ExposedResources.h"

#include "dsp/EffectGuids.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <string>
#include <utility>

namespace guitarfx::uiclient
{
namespace
{
/// The node config key the engine keeps a Custom Effect's descriptor under (presetV2.ts
/// WASM_GUEST_DESCRIPTOR_CONFIG_KEY): a JSON array of {key, value} strings.
constexpr const char* kDescriptorConfigKey = "wasmGuestDescriptor";

std::string Trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

/// "1", "true", "yes", "on" and their opposites, as the web's parseBooleanDescriptorValue reads them.
bool DescriptorBool(const std::map<std::string, std::string>& fields, const char* name, bool fallback)
{
    const auto it = fields.find(name);

    if (it == fields.end())
    {
        return fallback;
    }

    auto value = Trimmed(it->second);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (value == "1" || value == "true" || value == "yes" || value == "on")
    {
        return true;
    }

    if (value == "0" || value == "false" || value == "no" || value == "off")
    {
        return false;
    }

    return fallback;
}

std::string Field(const std::map<std::string, std::string>& fields, const char* name)
{
    const auto it = fields.find(name);
    return it != fields.end() ? it->second : std::string();
}
} // namespace

std::vector<ExposedResourceInfo> NodeExposedResources(const ClientState& state, const GraphNode& node)
{
    std::vector<ExposedResourceInfo> slots;

    if (const auto* type = state.FindEffectType(node.type))
    {
        slots = type->exposedResources;
    }

    if (node.type != EffectGuids::kWasmHost)
    {
        return slots;
    }

    const auto config = node.config.find(kDescriptorConfigKey);

    if (config == node.config.end() || Trimmed(config->second).empty())
    {
        return slots;
    }

    const auto entries = nlohmann::json::parse(config->second, nullptr, false);

    if (!entries.is_array())
    {
        return slots;
    }

    // resource.<id>.<field>, keyed by id in the order each id first appears (Custom Effect ABI 2).
    static const std::regex kResourceKey(R"(^resource\.([A-Za-z][A-Za-z0-9_]{0,31})\.(.+)$)");
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> groups;

    for (const auto& entry : entries)
    {
        if (!entry.is_object() || !entry.contains("key") || !entry["key"].is_string())
        {
            continue;
        }

        const auto key = entry["key"].get<std::string>();
        const auto value =
            entry.contains("value") && entry["value"].is_string() ? entry["value"].get<std::string>() : std::string();
        std::smatch match;

        if (!std::regex_match(key, match, kResourceKey))
        {
            continue;
        }

        const auto id = match[1].str();
        auto group = std::find_if(groups.begin(), groups.end(), [&id](const auto& g) { return g.first == id; });

        if (group == groups.end())
        {
            groups.emplace_back(id, std::map<std::string, std::string>{});
            group = std::prev(groups.end());
        }

        group->second[match[2].str()] = value;
    }

    // The module is slot 0; the controller keeps each declared file at its place after it, in
    // declaration order. An id the host's own slots take is refused by the engine: never listed.
    int declared = 0;
    std::vector<ExposedResourceInfo> guest;

    for (const auto& [id, fields] : groups)
    {
        if (std::any_of(slots.begin(), slots.end(), [&id](const auto& s) { return s.resourceId == id; }))
        {
            continue;
        }

        ExposedResourceInfo slot;
        slot.resourceId = id;
        slot.displayName = !Field(fields, "title").empty()  ? Field(fields, "title")
                           : !Field(fields, "name").empty() ? Field(fields, "name")
                                                            : id;
        slot.resourceType = !Field(fields, "type").empty() ? Field(fields, "type") : "blob";
        slot.resourceIndex = ++declared;
        slot.allowBrowseFile = DescriptorBool(fields, "allowBrowseFile", true);
        slot.parameterId = Field(fields, "parameterId");

        if (const auto it = fields.find("parameterValue"); it != fields.end())
        {
            try
            {
                slot.parameterValue = std::stod(it->second);
            }
            catch (const std::exception&)
            {
                slot.parameterValue = 0.0;
            }
        }

        guest.push_back(std::move(slot));
    }

    slots.insert(slots.end(), guest.begin(), guest.end());
    return slots;
}
} // namespace guitarfx::uiclient
