#pragma once

/**
 * NodeLabels.h - What a node is called on screen, as core/ui/ts/signalPath/nodeLabels.ts
 * names it: Input and Output by role; an effect that plays a resource by the resource
 * (a NAM model, an IR); otherwise its label, or its effect type's name.
 */

#include "uiclient/ClientState.h"

#include <string>

namespace guitarfx::uiclient
{
[[nodiscard]] std::string NodeDisplayName(const ClientState& state, const GraphNode& node);

/// The loaded resource's name: the library entry's, or the file's name without extension.
[[nodiscard]] std::string NodeResourceName(const ClientState& state, const GraphNode& node, std::size_t index = 0);

/// "A2", "A1", "Custom", or empty: the NAM architecture of the node's model, from its
/// library metadata.
[[nodiscard]] std::string NodeArchitectureBadge(const ClientState& state, const GraphNode& node);

/// The effect type's category ("amp", "cab", "drive", ...), falling back to the node's own.
[[nodiscard]] std::string NodeCategory(const ClientState& state, const GraphNode& node);
} // namespace guitarfx::uiclient
