#pragma once

/**
 * ResourceIndex.h - What a model or IR chooser lists from the resource library, in what order.
 *
 * The web UI's rules, so both UIs list, filter and step through the same entries:
 *  - duplicates (the same file under several ids) shown once (core/ui/ts/resourceDedup.ts);
 *  - the Library tab's filters (resourceBrowser.ts renderLibraryList): category, NAM
 *    architecture, creator, tags (all must be on), favourites only, and a search over name, id,
 *    category, description, creator and tags; missing files left out;
 *  - favourites first, then by name, then by path;
 *  - optionally grouped into packs, a Tone3000 tone or a factory archive
 *    (resourceBrowser/libraryPacks.ts), which is also the order previous/next steps through.
 */

#include "uiclient/ClientState.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class UiCommands;

struct ResourceDedup
{
    std::vector<const LibraryResource*> deduped;
    /// Each hidden duplicate's id, to the id of the entry shown in its place.
    std::map<std::string, std::string> aliasById;
};

/// One entry per file: grouped by hash (else by path, ignoring case and slashes); within a
/// group the entry kept is a preferred id if any, then one whose file exists, then (same name)
/// an A2 model over an A1, then the one with more tags and description, then the lowest id.
[[nodiscard]] ResourceDedup DeduplicateResources(const std::vector<LibraryResource>& resources,
                                                 const std::set<std::string>& preferredIds = {});

/// "A1", "A2", "Custom" or "" from a model's metadata.
[[nodiscard]] std::string ResourceArchitecture(const LibraryResource& resource);
/// Its tags and any in metadata.tags (a comma or semicolon separated string), each once.
[[nodiscard]] std::vector<std::string> ResourceTags(const LibraryResource& resource);
/// Who made it: creatorName, authorUsername, modeledBy, creator or provider.
[[nodiscard]] std::string ResourceCreator(const LibraryResource& resource);
/// The gear a model was captured from (formatGearDescription): make and model, the make left out
/// when the model already starts with it ("Diezel" and "Diezel Herbert" read "Diezel Herbert").
[[nodiscard]] std::string FormatGearDescription(const std::string& make, const std::string& model);
/// The one line under a chooser row's title (renderResourceMetaParts), its parts joined by " · ":
/// the category ("Uncategorized" for none) unless the list is filtered to one, "by" its creator,
/// a model's architecture, the gear and the tone type.
[[nodiscard]] std::string ResourceMetaLine(const LibraryResource& resource, const std::string& resourceType,
                                           bool withCategory);

struct ResourceFacets
{
    std::vector<std::string> tags;
    std::vector<std::string> creators;
};

[[nodiscard]] ResourceFacets LibraryFacets(const std::vector<const LibraryResource*>& resources);

/// Starred in any chooser: the engine-owned app setting resources.favorites.
[[nodiscard]] bool IsResourceFavorite(const ClientState& state, const std::string& resourceId);
/// Stars or unstars it (setResourceFavorite).
void SetResourceFavorite(UiCommands& commands, const std::string& resourceId, bool favorite);

struct ResourceFilter
{
    std::string query;
    std::string category = "all";     // "all", or a category ("Uncategorized" for none)
    std::string architecture = "all"; // "all", "A1", "A2" (models only)
    std::string creator = "all";
    std::vector<std::string> tags;
    bool favouritesOnly = false;
};

/// The Library tab's list for one resource type: deduped, filtered, favourites first, by name.
/// `selectedId` is kept over its duplicates. `aliases`, when given, receives the dedup's map.
[[nodiscard]] std::vector<const LibraryResource*> FilterLibrary(const ClientState& state,
                                                                const std::string& resourceType,
                                                                const ResourceFilter& filter,
                                                                const std::string& selectedId = {},
                                                                std::map<std::string, std::string>* aliases = nullptr);

/// The categories the library holds for a type, in name order ("Uncategorized" for none).
[[nodiscard]] std::vector<std::string> LibraryCategories(const ClientState& state, const std::string& resourceType);

/// The library category a slot's hint ("cab", "reverb", "amp", "full-rig", "pedal"; "ir" is a
/// cab) names among `available`: the same name, else one that holds the keyword; nothing when the
/// library has no such category (the web UI's resolveLibraryCategoryFromHint).
[[nodiscard]] std::optional<std::string> ResolveLibraryCategoryFromHint(const std::string& hint,
                                                                        const std::vector<std::string>& available);

struct LibraryPack
{
    std::string key;
    std::string title;
};

/// The pack a resource came from: its Tone3000 tone, or its factory archive; none for a file
/// the user brought in.
[[nodiscard]] std::optional<LibraryPack> LibraryPackOf(const LibraryResource& resource);
/// A model's name with its pack's title taken off the front, where the name repeats it.
[[nodiscard]] std::string NameWithinPack(const std::string& name, const LibraryPack& pack);

struct LibraryEntry
{
    std::optional<LibraryPack> pack; // none: a model on its own row (a pack of one is one)
    std::vector<const LibraryResource*> items;
};

/// Groups by pack, keeping each group's members in the order given; groups holding a
/// favourite first, then by title.
[[nodiscard]] std::vector<LibraryEntry> GroupLibraryByPack(const ClientState& state,
                                                           const std::vector<const LibraryResource*>& resources);

/// Whether the Library tab groups packs: resources.library.groupPacks, on unless turned off.
[[nodiscard]] bool IsPackGroupingEnabled(const ClientState& state);

/// The order the list draws its entries in, which previous/next follows.
[[nodiscard]] std::vector<const LibraryResource*> NavigationOrder(const ClientState& state,
                                                                  const std::vector<const LibraryResource*>& resources);
} // namespace guitarfx::uiclient
