#include "controller/internal/ResourceSources.h"

#include "models/ContentHash.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"
#include "util/LongPath.h"
#include "util/PathEncoding.h"

#include <optional>
#include <set>
#include <utility>

namespace guitarfx::controller_detail
{
namespace
{
bool IsLibraryResourceType(const std::string& type)
{
    return type == "nam" || type == "ir";
}

template <typename PresetType, typename Visit> void ForEachResourceRef(PresetType& preset, Visit&& visit)
{
    const auto visitGraph = [&](auto& graph) {
        for (auto& node : graph.nodes)
        {
            for (auto& ref : node.resources)
            {
                if (IsLibraryResourceType(ref.resourceType))
                {
                    visit(ref);
                }
            }
        }
    };

    visitGraph(preset.graph);

    for (auto& scene : preset.scenes)
    {
        visitGraph(scene.graph);
    }
}

/// The library resource a reference names by id, when the library has it and its file.
std::optional<LibraryResource> PresentLibraryResource(const ResourceRef& ref, const ResourceLibrary& library)
{
    if (!ref.IsLibraryRef())
    {
        return std::nullopt;
    }

    auto resource = library.LookupResource(ref.resourceType, ref.resourceId);

    if (resource && util::PathExists(resource->filePath))
    {
        return resource;
    }

    return std::nullopt;
}
} // namespace

std::map<std::string, std::string> BuildResourceSource(const LibraryResource& resource)
{
    std::map<std::string, std::string> source;

    const auto put = [&](const char* key, const std::string& value) {
        if (!value.empty())
        {
            source[key] = value;
        }
    };

    const auto meta = [&](const char* key) {
        const auto it = resource.metadata.find(key);
        return it == resource.metadata.end() ? std::string{} : it->second;
    };

    put("name", resource.name);
    put("category", resource.category);
    put("provider", meta("provider"));
    put("providerKey", meta("providerKey"));
    put("toneId", meta("toneId"));
    put("modelId", meta("modelId"));
    put("creatorName", meta("creatorName").empty() ? meta("authorUsername") : meta("creatorName"));
    put("hash", resource.hash);

    if (const auto contentHash = meta(kContentHashMetadataKey); IsContentHash(contentHash))
    {
        source[kContentHashMetadataKey] = contentHash;
    }

    return source;
}

void StampResourceSources(Preset& preset, const ResourceLibrary& library)
{
    ForEachResourceRef(preset, [&](ResourceRef& ref) {
        if (!ref.IsLibraryRef())
        {
            return;
        }

        // The entry, even with its file gone: its name and provider are still right.
        if (const auto resource = library.LookupResource(ref.resourceType, ref.resourceId))
        {
            ref.source = BuildResourceSource(*resource);
        }
    });
}

std::vector<RepairedResourceRef> RepairResourceRefs(Preset& preset, const ResourceLibrary& library)
{
    std::vector<RepairedResourceRef> repaired;

    ForEachResourceRef(preset, [&](ResourceRef& ref) {
        if (ref.source.empty() || PresentLibraryResource(ref, library))
        {
            return;
        }

        // A reference by file path alone (a file picked from a folder) is left on its file
        // while that file is there.
        if (!ref.IsLibraryRef() && (ref.filePath.empty() || util::PathExists(ref.filePath)))
        {
            return;
        }

        const auto found = library.FindResourceBySource(ref.resourceType, ref.source);

        if (!found || found->id == ref.resourceId)
        {
            return;
        }

        repaired.push_back({ref.resourceType, ref.resourceId.empty() ? util::PathToUtf8(ref.filePath) : ref.resourceId,
                            found->id});
        ref.resourceId = found->id;
        ref.filePath.clear();
        ref.source = BuildResourceSource(*found);
    });

    return repaired;
}

std::vector<MissingResourceRef> FindMissingResourceRefs(const Preset& preset, const ResourceLibrary& library)
{
    std::vector<MissingResourceRef> missing;
    std::set<std::pair<std::string, std::string>> seen;

    ForEachResourceRef(preset, [&](const ResourceRef& ref) {
        if (!ref.IsLibraryRef() && !ref.IsFilePath())
        {
            return;
        }

        // By its id, its file path, or a copy its source found.
        if (const auto resolved = library.ResolveResource(ref); resolved && util::PathExists(*resolved))
        {
            return;
        }

        const auto key = ref.resourceId.empty() ? util::PathToUtf8(ref.filePath) : ref.resourceId;

        if (seen.insert({ref.resourceType, key}).second)
        {
            missing.push_back({ref.resourceType, key, ref.source});
        }
    });

    return missing;
}
} // namespace guitarfx::controller_detail
