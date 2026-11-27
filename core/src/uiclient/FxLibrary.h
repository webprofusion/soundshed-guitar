#pragma once

/**
 * FxLibrary.h - What the FX Library offers beyond the effect catalog, as the web UI's
 * core/ui/ts/fxLibrary/catalog.ts: each blend in the blend library, and each saved Custom
 * Effect. Each is an FxLibraryItem that knows the node it adds: its effect type and the options
 * the engine starts that node with.
 */

#include "uiclient/ClientState.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/UiCommands.h"

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
struct FxLibraryItem
{
    std::string type; // the effect type of the node it adds
    std::string name;
    std::string category; // the FX Library category it is listed under
    std::string description;
    std::string thumbnailDataUrl;
    std::string blendId;        // a blend's
    std::string customEffectId; // a saved Custom Effect's
    bool requiresResource = false;
    std::string resourceType;
    NodeOptions options; // what the node it adds starts with

    /// Its favourites and recents key ("type|blendId|customId").
    [[nodiscard]] std::string Key() const;
};

/// A catalog effect, as it is listed.
[[nodiscard]] FxLibraryItem FxItemForEffect(const EffectTypeInfo& info);

/// The blend library's blends: blend nodes named after them, listed under their gear's category.
[[nodiscard]] std::vector<FxLibraryItem> BlendFxItems(const ClientState& state, const EffectPresentation& look);

/// The saved Custom Effects: Custom Effect nodes that load their module, with their settings.
[[nodiscard]] std::vector<FxLibraryItem> CustomEffectFxItems(const ClientState& state);
} // namespace guitarfx::uiclient
