#pragma once

/**
 * ExposedResources.h - The resource slots a node shows, as the web UI's getNodeEffectInfo merges
 * them (presetV2.ts): its type's exposedResources (a composite's inner amp or cab, the IR
 * Cabinet's A and B), then, for a Custom Effect, the guest files its module's descriptor declares
 * (resource.<id>.<field> entries in the node's "wasmGuestDescriptor" config).
 */

#include "presets/PresetTypes.h"
#include "uiclient/ClientState.h"

#include <vector>

namespace guitarfx::uiclient
{
/// The node's slots in the order they are drawn. Empty for a type that surfaces none.
[[nodiscard]] std::vector<ExposedResourceInfo> NodeExposedResources(const ClientState& state, const GraphNode& node);
} // namespace guitarfx::uiclient
