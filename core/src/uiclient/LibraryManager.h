#pragma once

/**
 * LibraryManager.h - the Resource Library manager in Settings > Library, as the web UI's
 * settings/libraryView.ts, libraryData.ts and libraryCleanup.ts: every model and IR in the
 * library, filtered by a search, type, creator, source and category, listed one per row or
 * grouped by the Tone3000 tone (or pack) they came from, and the cleanup of imported ones no
 * preset or blend uses.
 *
 * Unlike a chooser (ResourceIndex.h) nothing is deduplicated or left out: a missing file is listed,
 * flagged, so it can be pointed at again.
 */

#include "uiclient/ClientState.h"

#include <string>
#include <utility>
#include <vector>

namespace guitarfx::uiclient
{
class UiCommands;
}

namespace guitarfx::uiclient::library_manager
{
/// Where a resource came from (inferResourceOrigin): "Tone3000", "Imported", "Local", "Built-in",
/// or "Unknown" with no file. Its metadata's provider first, else its file's place.
[[nodiscard]] std::string Origin(const LibraryResource& resource);
/// "Amp Model", "Cab IR", else the type in capitals.
[[nodiscard]] std::string TypeLabel(const std::string& type);

/// The manager's filters, as the web's selects: "all" passes everything.
struct Filter
{
    std::string query;
    std::string type = "all"; // "all", "nam", "ir"
    std::string creator = "all";
    std::string source = "all"; // "all", "built-in", "local", "tone3000", "imported"
    std::string category = "all";
    bool grouped = true; // Group by Tone (the web's default) or List View
};

/// One library entry and its type.
struct Item
{
    const LibraryResource* resource = nullptr;
    std::string type;
};

/// Every model and IR in the library, in the order the engine lists them.
[[nodiscard]] std::vector<Item> AllItems(const ClientState& state);
/// Those the type filter keeps, which the category and creator choices are made from.
[[nodiscard]] std::vector<Item> OfType(const std::vector<Item>& items, const std::string& type);
/// The categories and creators among `items`, each once, sorted.
[[nodiscard]] std::vector<std::string> Categories(const std::vector<Item>& items);
[[nodiscard]] std::vector<std::string> Creators(const std::vector<Item>& items);
/// What the filters keep: type, category, source, creator, then the search over name, id,
/// category, description, path, creator and tags.
[[nodiscard]] std::vector<Item> Filtered(const ClientState& state, const Filter& filter);

/// A Tone3000 tone or a pack its entries came from (groupLibraryItemsByTone): entries with no
/// tone are not grouped.
struct ToneGroup
{
    std::string groupId;
    std::string title;
    std::string typeLabel;     // the one type, or "Mixed"
    std::string categoryLabel; // the one category, or "Multiple"
    std::string originLabel;   // the one origin, or "Mixed"
    std::string author;        // the first entry's authorUsername
    std::vector<Item> items;
};

/// The groups among `items`, by title.
[[nodiscard]] std::vector<ToneGroup> GroupByTone(const std::vector<Item>& items);

/// The line under a row's name: type, category ("Uncategorized" for none), origin, "by" its
/// creator, and its tags as #tags.
[[nodiscard]] std::string RowMeta(const Item& item);
/// The line under a group's title: type, category, origin, its count and its author.
[[nodiscard]] std::string GroupMeta(const ToneGroup& group);
/// "Showing N of M resources", or "... (grouped)" counting groups.
[[nodiscard]] std::string Summary(std::size_t shown, std::size_t total, bool grouped);

/// A row's file action: a local file's path is changed in place ("Change Path", or "Set Path"
/// when it is missing); none for another, whose file is the library's own copy.
[[nodiscard]] std::string PathActionLabel(const Item& item);
/// Whether a row has Edit: everything but a built-in entry.
[[nodiscard]] bool CanEdit(const Item& item);

/// What "Remove unused" offers to remove for a scope ("all", "nam", "ir"): the imported entries.
/// The engine keeps any a preset or blend uses, so this is what it may remove, not what it will.
[[nodiscard]] std::vector<Item> CleanupCandidates(const ClientState& state, const std::string& scope);
/// Removes `items` and their files (cleanupResourceLibrary); the engine skips ones in use.
void RemoveUnused(UiCommands& commands, const std::string& scope, const std::vector<Item>& items);
} // namespace guitarfx::uiclient::library_manager
