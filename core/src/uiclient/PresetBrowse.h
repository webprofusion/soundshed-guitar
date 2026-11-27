#pragma once

/**
 * PresetBrowse.h - Which presets a preset browser shows, in what order.
 *
 * The rules of core/ui/ts/presets/filter.ts: a folder (the virtual All, Favourites and
 * Recents, or a real folder with its subfolders), then the tags a preset must all carry, then
 * a text match on name, category and description, then alphabetical by name
 * (case-insensitive, numbers compared as numbers), except Recents, which keep
 * most-recent-first.
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
    /// Tags a preset must all carry (the browser's tag chips).
    std::vector<std::string> tags;
};

[[nodiscard]] std::vector<PresetSummary> FilterPresets(const ClientState& state, const PresetQuery& query);

/// Natural, case-insensitive ordering of preset names ("Tone 2" before "Tone 10"); ties
/// broken by id, as comparePresetNames does.
[[nodiscard]] int ComparePresetNames(const PresetSummary& left, const PresetSummary& right);

[[nodiscard]] bool IsPresetFavorite(const ClientState& state, const std::string& presetId);

/// A user preset's id, as presets/toolbar.ts isUserPreset reads one: a number (a timestamp), one
/// starting "user-", or a UUID. Factory and pack presets have other ids.
[[nodiscard]] bool IsUserPresetId(const std::string& presetId);

/// The folder tree in display order, each folder with its depth: by name at every level, ignoring
/// case, as the web UI's sortPresetFoldersAlphabetically.
[[nodiscard]] std::vector<std::pair<const PresetFolder*, int>> FlattenPresetFolders(
    const std::vector<PresetFolder>& folders);

/// The folders a preset is filed in, outermost first, as the web UI's preset card shows them
/// ("Preset Pack Vol. 1 > High Gain Rhythm"); empty when it is in none.
[[nodiscard]] std::string PresetFolderPath(const std::vector<PresetFolder>& folders, const std::string& presetId);

/// The setting that holds how many performance pads show (performancePads.ts).
inline constexpr const char* kPerformancePadCountSetting = "performancePads.padCount";

/// The setlist the pads and the setlist panel show: the active one, else the first, as the web
/// UI's getActiveSetlist; null when there are none.
[[nodiscard]] const Setlist* ActiveSetlist(const ClientState& state);

/// The bank a new setlist takes: one past the highest in use, or 1 (getNextSuggestedBank).
[[nodiscard]] int NextSetlistBank(const std::vector<Setlist>& setlists);

/// The pads' bank heading: "Bank 2", "Unassigned bank" for a setlist without a number, "No
/// bank" for none.
[[nodiscard]] std::string SetlistBankLabel(const Setlist* setlist);

/// How many performance pads show: the setting when it is 4, 6 or 8, else 8.
[[nodiscard]] int PerformancePadCount(const ClientState& state);

/// A preset's rating, 0 (not rated) to 5.
[[nodiscard]] int PresetRating(const ClientState& state, const std::string& presetId);

/// Whether the preset playing may be overwritten, renamed or deleted (canModifyPreset): a user
/// preset the library holds, or any preset while the Factory Preset Archives tools are on. The
/// toolbar's Edit and Save are off otherwise, and Save As makes the copy.
[[nodiscard]] bool CanModifyActivePreset(const ClientState& state);

/// A real folder by id, searched through the whole tree; null for the virtual ones.
[[nodiscard]] const PresetFolder* FindPresetFolder(const std::vector<PresetFolder>& folders, const std::string& id);

/// The names of a Multi-Rig's presets in its order (resolveCompositeSlotNames); an id the
/// library does not have is given as it is.
[[nodiscard]] std::vector<std::string> CompositeSlotNames(const ClientState& state, const CompositePreset& rig);
/// The Multi-Rigs the library search leaves (filterCompositePresets): the text in the name, the
/// description, a tag or one of its presets' names, ignoring case; all of them for no text.
[[nodiscard]] std::vector<const CompositePreset*> FilterCompositePresets(const ClientState& state,
                                                                         const std::string& text);
/// Every tag the Multi-Rigs use, trimmed and in lower case, once each, sorted
/// (collectCompositePresetTags).
[[nodiscard]] std::vector<std::string> CompositePresetTags(const std::vector<CompositePreset>& rigs);
} // namespace guitarfx::uiclient
