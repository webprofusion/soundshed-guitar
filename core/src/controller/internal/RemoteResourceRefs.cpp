#include "controller/internal/RemoteResourceRefs.h"

#include "models/ContentHash.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <unordered_set>

namespace guitarfx::controller_detail
{
namespace
{
std::string Text(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
} // namespace

nlohmann::json ReadArchiveRemoteResources(const nlohmann::json& root)
{
    nlohmann::json merged = nlohmann::json::array();
    std::unordered_set<std::string> seen;

    if (const auto it = root.find("remoteResources"); it != root.end() && it->is_array())
    {
        for (const auto& reference : *it)
        {
            const auto id = reference.is_object() ? Text(reference, "id") : std::string{};

            if (!id.empty() && !Text(reference, "provider").empty() && seen.insert(id).second)
            {
                merged.push_back(reference);
            }
        }
    }

    // Archives for older app versions list their Tone3000 models here too, or only here.
    if (const auto it = root.find("tone3000Resources"); it != root.end() && it->is_array())
    {
        for (const auto& legacy : *it)
        {
            const auto id = legacy.is_object() ? Text(legacy, "id") : std::string{};

            if (id.empty() || !seen.insert(id).second)
            {
                continue;
            }

            const auto modelId = Text(legacy, "modelId");
            nlohmann::json reference = {
                {"id", id},
                {"type", legacy.value("type", "nam")},
                {"name", Text(legacy, "name")},
                {"provider", "tone3000"},
                {"providerKey", modelId},
                {"providerData", {{"toneId", Text(legacy, "toneId")}, {"modelId", modelId}}},
            };

            if (const auto creator = Text(legacy, "creatorName"); !creator.empty())
            {
                reference["creatorName"] = creator;
            }

            merged.push_back(std::move(reference));
        }
    }

    return merged;
}

std::string FindRemoteResource(const ResourceLibrary& library, const nlohmann::json& reference)
{
    const auto id = Text(reference, "id");
    const auto type = reference.value("type", "nam");
    const auto provider = Text(reference, "provider");
    const auto providerKey = Text(reference, "providerKey");
    const auto hash = Text(reference, "hash");
    const auto contentHash = Text(reference, "contentHash");
    const auto data = reference.find("providerData");
    const bool hasData = data != reference.end() && data->is_object();
    const auto toneId = hasData ? Text(*data, "toneId") : std::string{};
    const auto modelId = hasData ? Text(*data, "modelId") : std::string{};

    const auto resources = library.GetAllResources();
    const auto find = [&](const auto& matches) {
        const auto it = std::find_if(resources.begin(), resources.end(), [&](const LibraryResource& resource) {
            return resource.type == type && matches(resource);
        });
        return it == resources.end() ? std::string{} : it->id;
    };
    const auto meta = [](const LibraryResource& resource, const char* key) {
        const auto it = resource.metadata.find(key);
        return it == resource.metadata.end() ? std::string{} : it->second;
    };

    if (!id.empty())
    {
        if (auto found = find([&](const LibraryResource& resource) { return resource.id == id; }); !found.empty())
        {
            return found;
        }
    }

    if (!provider.empty())
    {
        auto found = find([&](const LibraryResource& resource) {
            if (meta(resource, "provider") != provider)
            {
                return false;
            }

            if (!providerKey.empty() && meta(resource, "providerKey") == providerKey)
            {
                return true;
            }

            return provider == "tone3000" && !toneId.empty() && meta(resource, "toneId") == toneId &&
                   meta(resource, "modelId") == modelId;
        });

        if (!found.empty())
        {
            return found;
        }
    }

    if (!hash.empty())
    {
        if (auto found = find([&](const LibraryResource& resource) { return resource.hash == hash; }); !found.empty())
        {
            return found;
        }
    }

    // The same model under any name, from any source: a folder, another provider, a repost.
    if (IsContentHash(contentHash))
    {
        return find(
            [&](const LibraryResource& resource) { return meta(resource, kContentHashMetadataKey) == contentHash; });
    }

    return {};
}
} // namespace guitarfx::controller_detail
