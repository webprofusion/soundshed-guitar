#pragma once

/**
 * PresetBrowse.h - Which presets a preset browser shows, in what order.
 *
 * The rules of core/ui/ts/presets/filter.ts: a folder (the virtual All, Favourites and
 * Recents, or a real folder with its subfolders), then a text match on name and category,
 * then alphabetical by name (case-insensitive, numbers compared as numbers), except Recents,
 * which keep most-recent-first.
 */

#include "uiclient/ClientState.h"

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
inline constexpr const char* kPresetFolderAll = "__all__";
inline constexpr const char* kPresetFolderFavorites = "__favorites__";
inline constexpr const char* kPresetFolderRecents = "__recents__";

struct PresetQuery
{
    std::string folderId = kPresetFolderAll;
    std::string text;
};

[[nodiscard]] std::vector<PresetSummary> FilterPresets(const ClientState& state, const PresetQuery& query);

/// Natural, case-insensitive ordering of preset names ("Tone 2" before "Tone 10"); ties
/// broken by id, as comparePresetNames does.
[[nodiscard]] int ComparePresetNames(const PresetSummary& left, const PresetSummary& right);

[[nodiscard]] bool IsPresetFavorite(const ClientState& state, const std::string& presetId);

/// A real folder by id, searched through the whole tree; null for the virtual ones.
[[nodiscard]] const PresetFolder* FindPresetFolder(const std::vector<PresetFolder>& folders, const std::string& id);
} // namespace guitarfx::uiclient
