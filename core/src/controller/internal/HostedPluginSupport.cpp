#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace guitarfx::controller_detail
{
std::string NormalizeHostedPluginIdentityToken(std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());

    bool lastWasSeparator = false;

    for (const char raw : value)
    {
        const unsigned char ch = static_cast<unsigned char>(raw);

        if (std::isalnum(ch))
        {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
            lastWasSeparator = false;
            continue;
        }

        if (!normalized.empty() && !lastWasSeparator)
        {
            normalized.push_back('-');
            lastWasSeparator = true;
        }
    }

    while (!normalized.empty() && normalized.back() == '-')
    {
        normalized.pop_back();
    }

    return normalized;
}

std::string BuildHostedPluginStableId(std::string_view manufacturer, std::string_view pluginName)
{
    const std::string normalizedManufacturer = NormalizeHostedPluginIdentityToken(manufacturer);
    const std::string normalizedName = NormalizeHostedPluginIdentityToken(pluginName);

    if (!normalizedManufacturer.empty() && !normalizedName.empty())
    {
        return normalizedManufacturer + "." + normalizedName;
    }

    if (!normalizedName.empty())
    {
        return normalizedName;
    }

    return normalizedManufacturer;
}

bool IsHostedPluginNode(const GraphNode& node)
{
    return EffectRegistry::Instance().Resolve(node.type) == EffectGuids::kPluginHost;
}

/**
 * Which plugin *resource* a node points at.
 *
 * Swap detection uses this rather than the config identity keys below: those are published
 * by the plugin once it has loaded, so at the moment the user picks a different plugin they
 * still describe the previous one and would report "no change".
 */
std::string HostedPluginResourceKey(const GraphNode& node)
{
    for (const auto& resource : node.resources)
    {
        if (resource.resourceType != "plugin")
        {
            continue;
        }

        if (!resource.resourceId.empty())
        {
            return "res:" + resource.resourceId;
        }

        if (!resource.filePath.empty())
        {
            return "path:" + util::PathToUtf8(resource.filePath);
        }
    }

    return {};
}

/**
 * Identity of the plugin a node is configured to host.
 *
 * A state chunk is only meaningful to the plugin it came from. Comparing this key before
 * carrying state across a rebuild, a scene, or a session restore is what stops one
 * plugin's chunk being handed to another.
 *
 * Returns an empty string for nodes whose plugin has never been resolved — callers treat
 * "unknown identity" as "do not carry state across", except where both sides are unknown.
 */
std::string HostedPluginIdentityKey(const GraphNode& node)
{
    const auto read = [&node](const char* key) -> std::string {
        const auto it = node.config.find(key);
        return it != node.config.end() ? it->second : std::string{};
    };

    // Stable id first: it survives a plugin being reinstalled at a different path.
    if (auto stableId = read(kHostedPluginStableIdConfigKey); !stableId.empty())
    {
        return "stable:" + stableId;
    }

    if (auto identifier = read(kHostedPluginIdentifierConfigKey); !identifier.empty())
    {
        return "id:" + identifier;
    }

    // Nothing authoritative — fall back to the resource, which is all an as-yet-unloaded
    // node has to go on.
    return HostedPluginResourceKey(node);
}

/// True when a state chunk stored against `from` may be applied to `to`.
bool HostedPluginIdentityMatches(const GraphNode& from, const GraphNode& to)
{
    const auto fromKey = HostedPluginIdentityKey(from);
    const auto toKey = HostedPluginIdentityKey(to);

    // Both unknown: the node has never resolved a plugin on either side, so there is no
    // evidence of a swap and the state is the best guess available.
    if (fromKey.empty() && toKey.empty())
    {
        return true;
    }

    return fromKey == toKey;
}

std::string GetHostedPluginNodeState(const GraphNode& node)
{
    const auto it = node.config.find(kHostedPluginStateConfigKey);
    return it != node.config.end() ? it->second : std::string{};
}

void ScrubHostedPluginStateForUi(SignalGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (!IsHostedPluginNode(node))
        {
            continue;
        }

        const auto stateIt = node.config.find(kHostedPluginStateConfigKey);

        if (stateIt == node.config.end())
        {
            continue;
        }

        node.config[kHostedPluginStateLengthConfigKey] = std::to_string(stateIt->second.size());
        node.config.erase(stateIt);
    }
}

nlohmann::json SerializePresetForUi(const Preset& preset)
{
    Preset uiPreset = preset;
    ScrubHostedPluginStateForUi(uiPreset.graph);

    for (auto& scene : uiPreset.scenes)
    {
        ScrubHostedPluginStateForUi(scene.graph);
    }

    return nlohmann::json::parse(PresetStorage::SerializeToJson(uiPreset));
}

bool GraphHasScrubbedHostedPluginState(const SignalGraph& graph)
{
    for (const auto& node : graph.nodes)
    {
        if (!IsHostedPluginNode(node))
        {
            continue;
        }

        const auto stateIt = node.config.find(kHostedPluginStateConfigKey);
        const auto lengthIt = node.config.find(kHostedPluginStateLengthConfigKey);

        if (lengthIt != node.config.end() && (stateIt == node.config.end() || stateIt->second.empty()))
        {
            return true;
        }
    }

    return false;
}

bool PresetHasScrubbedHostedPluginState(const Preset& preset)
{
    if (GraphHasScrubbedHostedPluginState(preset.graph))
    {
        return true;
    }

    for (const auto& scene : preset.scenes)
    {
        if (GraphHasScrubbedHostedPluginState(scene.graph))
        {
            return true;
        }
    }

    return false;
}

void AppendHostedPluginGraphSummary(const SignalGraph& graph, const std::string& scopeLabel,
                                    std::vector<std::string>& entries)
{
    for (const auto& node : graph.nodes)
    {
        if (!IsHostedPluginNode(node))
        {
            continue;
        }

        const auto stateIt = node.config.find(kHostedPluginStateConfigKey);
        const auto lengthIt = node.config.find(kHostedPluginStateLengthConfigKey);
        const std::string stateLength =
            stateIt != node.config.end() ? std::to_string(stateIt->second.size()) : std::string{"0"};
        const std::string stateHash =
            stateIt != node.config.end() ? HashStringForLog(stateIt->second) : std::string{"<none>"};
        const std::string scrubbedLength = lengthIt != node.config.end() ? lengthIt->second : std::string{"0"};
        entries.push_back(scopeLabel + "/" + node.id + ":state=" + stateLength + ",hash=" + stateHash +
                          ",scrubbed=" + scrubbedLength);
    }
}

std::string SummarizeHostedPluginState(const Preset& preset)
{
    std::vector<std::string> entries;
    AppendHostedPluginGraphSummary(preset.graph, "graph", entries);

    for (const auto& scene : preset.scenes)
    {
        AppendHostedPluginGraphSummary(scene.graph, "scene:" + scene.id, entries);
    }

    if (entries.empty())
    {
        return "no hosted plugin nodes";
    }

    std::ostringstream summary;

    for (size_t index = 0; index < entries.size(); ++index)
    {
        if (index > 0)
        {
            summary << "; ";
        }

        summary << entries[index];
    }

    return summary.str();
}
} // namespace guitarfx::controller_detail
