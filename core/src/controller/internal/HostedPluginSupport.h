#pragma once

/**
 * HostedPluginSupport.h — Identity and state handling for hosted-plugin nodes.
 *
 * A hosted plugin's state is an opaque chunk that is only meaningful to the
 * plugin it came from. Everything that carries such a chunk across a rebuild,
 * a scene change or a session restore has to agree on what "same plugin"
 * means, and everything that sends a graph to the UI has to strip the chunk
 * first. Both rules are stated once, here.
 */

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace guitarfx
{
struct GraphNode;
struct SignalGraph;
struct Preset;
}

namespace guitarfx::controller_detail
{

// Node config keys describing which plugin a node hosts, and its saved state.
inline constexpr const char* kHostedPluginStableIdConfigKey = "pluginStableId";
inline constexpr const char* kHostedPluginIdentifierConfigKey = "pluginIdentifier";
inline constexpr const char* kHostedPluginNameConfigKey = "pluginName";
inline constexpr const char* kHostedPluginManufacturerConfigKey = "pluginManufacturer";
inline constexpr const char* kHostedPluginFormatConfigKey = "pluginFormat";
inline constexpr const char* kHostedPluginLastErrorCodeConfigKey = "lastErrorCode";
inline constexpr const char* kHostedPluginStateConfigKey = "pluginStateBase64";
inline constexpr const char* kHostedPluginStateLengthConfigKey = "pluginStateBase64Length";

/// Lowercases and strips punctuation so the same plugin spelled differently by
/// two formats still produces one token.
[[nodiscard]] std::string NormalizeHostedPluginIdentityToken(std::string_view value);

/// Builds the reinstall-surviving id for a plugin from its manufacturer and name.
[[nodiscard]] std::string BuildHostedPluginStableId(std::string_view manufacturer,
                                                    std::string_view name);

[[nodiscard]] bool IsHostedPluginNode(const GraphNode& node);

/// Resource the node points at, as a comparable key ("res:"/"path:" prefixed).
[[nodiscard]] std::string HostedPluginResourceKey(const GraphNode& node);

/// Identity of the plugin a node is configured to host. Empty when the plugin
/// has never been resolved; callers treat "unknown" as "do not carry state
/// across", except where both sides are unknown.
[[nodiscard]] std::string HostedPluginIdentityKey(const GraphNode& node);

/// True when a state chunk stored against `from` may be applied to `to`.
[[nodiscard]] bool HostedPluginIdentityMatches(const GraphNode& from, const GraphNode& to);

[[nodiscard]] std::string GetHostedPluginNodeState(const GraphNode& node);

/// Removes plugin state chunks from a graph bound for the UI. They can be
/// megabytes each and the UI has no use for them.
void ScrubHostedPluginStateForUi(SignalGraph& graph);

/// Serializes a preset for the UI with hosted-plugin state scrubbed out.
[[nodiscard]] nlohmann::json SerializePresetForUi(const Preset& preset);

[[nodiscard]] bool GraphHasScrubbedHostedPluginState(const SignalGraph& graph);
[[nodiscard]] bool PresetHasScrubbedHostedPluginState(const Preset& preset);

/// Appends a one-line-per-node description of hosted plugins to `entries`,
/// for diagnostics logging.
void AppendHostedPluginGraphSummary(const SignalGraph& graph,
                                    const std::string& scopeLabel,
                                    std::vector<std::string>& entries);

[[nodiscard]] std::string SummarizeHostedPluginState(const Preset& preset);

} // namespace guitarfx::controller_detail
