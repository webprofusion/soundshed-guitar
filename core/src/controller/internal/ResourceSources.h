#pragma once

/**
 * ResourceSources.h - What a preset remembers about the models and IRs it uses.
 *
 * A preset (and a DAW project, which carries its presets) names a library resource by id. The
 * file lives in the library, not the project, so a project opened after its model has gone (a
 * library entry deleted, a profile moved to a new machine, a file cleaned away) has only that id
 * left. Every saved reference therefore also carries a `source` (ResourceRef::source): the
 * resource's name and what its provider needs to find it again. With it a missing model can be
 * named, matched to another copy already in the library, or downloaded again.
 */

#include <map>
#include <string>
#include <vector>

namespace guitarfx
{
struct LibraryResource;
struct Preset;
struct ResourceRef;
class ResourceLibrary;
} // namespace guitarfx

namespace guitarfx::controller_detail
{
/// The source a reference to this library resource records: its name and category, provider
/// fields (provider, providerKey, toneId, modelId, creatorName) and hashes, whichever it has.
[[nodiscard]] std::map<std::string, std::string> BuildResourceSource(const LibraryResource& resource);

/// Records the library's current source on every model and IR reference in the preset (its
/// graph and every scene's) that the library knows. A reference the library does not know keeps
/// the source it was read with, so saving a project again never loses what it knew.
void StampResourceSources(Preset& preset, const ResourceLibrary& library);

/// One reference whose library id is gone (or whose file is) and which its source matched to
/// another resource in the library. The preset now uses `toId`.
struct RepairedResourceRef
{
    std::string type;
    std::string fromId;
    std::string toId;
};

/// Points every reference the library cannot resolve at the library resource its source
/// describes (ResourceLibrary::FindResourceBySource), when there is one.
std::vector<RepairedResourceRef> RepairResourceRefs(Preset& preset, const ResourceLibrary& library);

/// A model or IR reference the library cannot resolve, and the source it was saved with.
struct MissingResourceRef
{
    std::string type;
    std::string id;
    std::map<std::string, std::string> source;
};

/// Every model and IR reference in the preset whose file cannot be found, once per type and id
/// (or file path, for a reference without an id).
[[nodiscard]] std::vector<MissingResourceRef> FindMissingResourceRefs(const Preset& preset,
                                                                      const ResourceLibrary& library);
} // namespace guitarfx::controller_detail
