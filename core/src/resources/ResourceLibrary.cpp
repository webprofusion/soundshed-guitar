#include "resources/ResourceLibrary.h"
#include "models/ContentHash.h"
#include "util/FileIO.h"
#include "util/LongPath.h"
#include "util/PathEncoding.h"
#include "presets/PresetTypes.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace guitarfx
{
namespace
{
bool IsRelativeOutside(const std::filesystem::path& relativePath)
{
    if (relativePath.empty())
    {
        return false;
    }

    const auto it = relativePath.begin();
    return it != relativePath.end() && *it == "..";
}

/// Paths are stored relative to the profile's `resources` directory so a
/// profile can be moved or copied between machines. Anything that cannot be
/// expressed relative to it (a resource the user pointed at elsewhere on
/// disk) is stored absolute.
nlohmann::json ResourceToJson(const LibraryResource& resource, const std::filesystem::path& resourcesRoot)
{
    nlohmann::json item;
    item["type"] = resource.type;
    item["id"] = resource.id;
    item["name"] = resource.name;
    item["category"] = resource.category;
    item["description"] = resource.description;

    if (!resource.filePath.empty())
    {
        std::error_code relEc;
        const auto relativeToResources = std::filesystem::relative(resource.filePath, resourcesRoot, relEc);

        if (!relEc && !relativeToResources.empty() && relativeToResources != std::filesystem::path(".") &&
            !IsRelativeOutside(relativeToResources))
        {
            item["filePath"] = util::PathToUtf8(relativeToResources);
        }
        else
        {
            item["filePath"] = util::PathToUtf8(resource.filePath);
        }
    }
    else
    {
        item["filePath"] = "";
    }

    item["hash"] = resource.hash;
    item["tags"] = resource.tags;

    if (!resource.metadata.empty())
    {
        item["metadata"] = resource.metadata;
    }

    return item;
}

/// Inverse of ResourceToJson. `relativeBases` are tried in order when the
/// stored path is relative; the last one is used even if nothing exists, so
/// a resource whose file is temporarily missing still resolves to a sensible
/// path for the error message.
std::optional<LibraryResource> ResourceFromJson(const nlohmann::json& item,
                                                const std::vector<std::filesystem::path>& relativeBases)
{
    if (!item.is_object())
    {
        return std::nullopt;
    }

    LibraryResource resource;
    resource.type = item.value("type", "");
    resource.id = item.value("id", "");

    if (resource.type.empty() || resource.id.empty())
    {
        return std::nullopt;
    }

    resource.name = item.value("name", "");
    resource.category = item.value("category", "");
    resource.description = item.value("description", "");
    resource.hash = item.value("hash", "");

    const std::string rawPath = item.value("filePath", "");

    if (!rawPath.empty())
    {
        // Stored as UTF-8 (ResourceToJson). path(std::string) would read it in the ANSI code
        // page on Windows, and a model in a folder such as "Müzik" would go missing on reload.
        std::filesystem::path resolvedPath = util::PathFromUtf8(rawPath);

        if (resolvedPath.is_relative() && !relativeBases.empty())
        {
            std::filesystem::path candidate = relativeBases.back() / resolvedPath;

            for (const auto& base : relativeBases)
            {
                const auto attempt = base / resolvedPath;

                if (util::PathExists(attempt))
                {
                    candidate = attempt;
                    break;
                }
            }

            resolvedPath = candidate;
        }

        resource.filePath = resolvedPath;
    }

    if (item.contains("tags") && item["tags"].is_array())
    {
        for (const auto& tag : item["tags"])
        {
            if (tag.is_string())
            {
                resource.tags.push_back(tag.get<std::string>());
            }
        }
    }

    if (item.contains("metadata") && item["metadata"].is_object())
    {
        for (const auto& entry : item["metadata"].items())
        {
            const auto& value = entry.value();

            if (value.is_string())
            {
                resource.metadata[entry.key()] = value.get<std::string>();
            }
            else if (value.is_number())
            {
                resource.metadata[entry.key()] = value.dump();
            }
            else if (value.is_boolean())
            {
                resource.metadata[entry.key()] = value.get<bool>() ? "true" : "false";
            }
        }
    }

    return resource;
}
} // namespace

ResourceLibrary::ResourceLibrary() = default;
ResourceLibrary::~ResourceLibrary() = default;

std::string ResourceLibrary::MakeKey(const std::string& type, const std::string& id)
{
    return type + ":" + id;
}

void ResourceLibrary::AddResource(const LibraryResource& resource)
{
    const auto key = MakeKey(resource.type, resource.id);
    mResources[key] = resource;
    ++mRevision;
}

void ResourceLibrary::UpdateResource(const std::string& type, const std::string& id, const LibraryResource& updated)
{
    const auto key = MakeKey(type, id);

    if (mResources.count(key))
    {
        mResources[key] = updated;
        ++mRevision;
    }
}

void ResourceLibrary::RemoveResource(const std::string& type, const std::string& id)
{
    const auto key = MakeKey(type, id);

    if (mResources.erase(key) > 0)
    {
        ++mRevision;
    }
}

void ResourceLibrary::Clear()
{
    mResources.clear();
    ++mRevision;
}

std::optional<LibraryResource> ResourceLibrary::LookupResource(const std::string& type, const std::string& id) const
{
    const auto key = MakeKey(type, id);
    auto it = mResources.find(key);

    if (it != mResources.end())
    {
        return it->second;
    }

    if (id.find("__") != std::string::npos)
    {
        const auto lastDoubleUnderscore = id.rfind("__");

        if (lastDoubleUnderscore != std::string::npos && lastDoubleUnderscore + 2 < id.length())
        {
            const auto suffix = id.substr(lastDoubleUnderscore + 2);
            const auto fallbackKey = MakeKey(type, suffix);
            auto fallbackIt = mResources.find(fallbackKey);

            if (fallbackIt != mResources.end())
            {
                return fallbackIt->second;
            }

            // Search for any other resource whose ID ends with "__" + suffix or is suffix
            const std::string marker = "__" + suffix;

            for (const auto& [resKey, resource] : mResources)
            {
                if (resource.type == type)
                {
                    if (resource.id == suffix ||
                        (resource.id.length() >= marker.length() &&
                         resource.id.compare(resource.id.length() - marker.length(), marker.length(), marker) == 0))
                    {
                        return resource;
                    }
                }
            }
        }
    }
    else
    {
        // The search ID is a clean suffix, but maybe the library has it with a prefix
        const std::string marker = "__" + id;

        for (const auto& [resKey, resource] : mResources)
        {
            if (resource.type == type)
            {
                if (resource.id.length() >= marker.length() &&
                    resource.id.compare(resource.id.length() - marker.length(), marker.length(), marker) == 0)
                {
                    return resource;
                }
            }
        }
    }

    return std::nullopt;
}

std::vector<LibraryResource> ResourceLibrary::GetResourcesByType(const std::string& type) const
{
    std::vector<LibraryResource> result;

    for (const auto& [key, resource] : mResources)
    {
        if (resource.type == type)
        {
            result.push_back(resource);
        }
    }

    return result;
}

std::vector<LibraryResource> ResourceLibrary::GetResourcesByCategory(const std::string& type,
                                                                     const std::string& category) const
{
    std::vector<LibraryResource> result;

    for (const auto& [key, resource] : mResources)
    {
        if (resource.type == type && resource.category == category)
        {
            result.push_back(resource);
        }
    }

    return result;
}

std::vector<LibraryResource> ResourceLibrary::GetAllResources() const
{
    std::vector<LibraryResource> result;
    result.reserve(mResources.size());

    for (const auto& [key, resource] : mResources)
    {
        result.push_back(resource);
    }

    return result;
}

std::vector<std::pair<std::string, std::string>> ResourceLibrary::GetResourcePathIndex() const
{
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(mResources.size());

    for (const auto& [key, resource] : mResources)
    {
        if (resource.filePath.empty())
        {
            continue;
        }

        result.emplace_back(util::PathToUtf8(resource.filePath), resource.id);
    }

    return result;
}

bool ResourceLibrary::HasResource(const std::string& type, const std::string& id) const
{
    return LookupResource(type, id).has_value();
}

std::optional<std::filesystem::path> ResourceLibrary::ResolveResource(const ResourceRef& ref) const
{
    // Priority: Library > FilePath
    if (ref.IsLibraryRef())
    {
        auto resource = LookupResource(ref.resourceType, ref.resourceId);

        if (resource && util::PathExists(resource->filePath))
        {
            return resource->filePath;
        }

        // If the matched resource file doesn't exist, search the library for any fallback item whose file does exist
        const std::string& id = ref.resourceId;
        std::string suffix = id;

        if (id.find("__") != std::string::npos)
        {
            suffix = id.substr(id.rfind("__") + 2);
        }

        for (const auto& [resKey, res] : mResources)
        {
            if (res.type == ref.resourceType)
            {
                bool isMatch = (res.id == suffix) ||
                               (res.id.length() >= suffix.length() + 2 &&
                                res.id.compare(res.id.length() - suffix.length(), suffix.length(), suffix) == 0);

                if (!isMatch)
                {
                    auto origIdIt = res.metadata.find("originalId");

                    if (origIdIt != res.metadata.end() && origIdIt->second == suffix)
                    {
                        isMatch = true;
                    }
                }

                if (!isMatch && resource && !resource->hash.empty() && !res.hash.empty() && res.hash == resource->hash)
                {
                    isMatch = true;
                }

                if (isMatch && util::PathExists(res.filePath))
                {
                    return res.filePath;
                }
            }
        }

        // The library has lost this id, or its file: what the preset recorded about the resource
        // when it was saved can still find the same model under another id (imported again,
        // from another source, or renamed).
        if (auto described = FindResourceBySource(ref.resourceType, ref.source))
        {
            return described->filePath;
        }

        // Last resort: if we have a resolved entry but its file is missing, return its path anyway and let loader fail
        if (resource)
        {
            return resource->filePath;
        }
    }

    if (ref.IsFilePath())
    {
        if (util::PathExists(ref.filePath))
        {
            return ref.filePath;
        }

        if (auto described = FindResourceBySource(ref.resourceType, ref.source))
        {
            return described->filePath;
        }
    }

    // Embedded resources are handled separately by the preset loader
    return std::nullopt;
}

std::optional<LibraryResource> ResourceLibrary::FindResourceBySource(
    const std::string& type, const std::map<std::string, std::string>& source) const
{
    if (source.empty() || type.empty())
    {
        return std::nullopt;
    }

    const auto text = [](const std::map<std::string, std::string>& values, const char* key) {
        const auto it = values.find(key);
        return it == values.end() ? std::string{} : it->second;
    };

    const auto provider = text(source, "provider");
    const auto providerKey = text(source, "providerKey");
    const auto toneId = text(source, "toneId");
    const auto modelId = text(source, "modelId");
    const auto hash = text(source, "hash");
    const auto contentHash = text(source, kContentHashMetadataKey);

    const auto find = [&](const auto& matches) -> std::optional<LibraryResource> {
        for (const auto& [key, resource] : mResources)
        {
            if (resource.type == type && matches(resource) && util::PathExists(resource.filePath))
            {
                return resource;
            }
        }

        return std::nullopt;
    };

    if (!provider.empty() && !providerKey.empty())
    {
        if (auto found = find([&](const LibraryResource& resource) {
                return text(resource.metadata, "provider") == provider &&
                       text(resource.metadata, "providerKey") == providerKey;
            }))
        {
            return found;
        }
    }

    // Without a key, a Tone3000 model is its tone and model ids. Not with one: a zip's entries
    // share both ids, and the key is what tells them apart.
    if (provider == "tone3000" && providerKey.empty() && !toneId.empty() && !modelId.empty())
    {
        if (auto found = find([&](const LibraryResource& resource) {
                return text(resource.metadata, "toneId") == toneId && text(resource.metadata, "modelId") == modelId;
            }))
        {
            return found;
        }
    }

    if (!hash.empty())
    {
        if (auto found = find([&](const LibraryResource& resource) { return resource.hash == hash; }))
        {
            return found;
        }
    }

    // The same model under any name, from anywhere: a folder, another provider, a repost.
    if (IsContentHash(contentHash))
    {
        return find(
            [&](const LibraryResource& resource) { return text(resource.metadata, kContentHashMetadataKey) == contentHash; });
    }

    return std::nullopt;
}

void ResourceLibrary::LoadFromDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::exists(directory))
    {
        return;
    }

    // Look for library.json in the directory
    auto libraryFile = directory / "library.json";

    if (std::filesystem::exists(libraryFile))
    {
        LoadFromFile(libraryFile);
    }
}

std::string ResourceLibrary::MakeStoreId(const std::string& type, const std::string& id)
{
    return MakeKey(type, id);
}

void ResourceLibrary::LoadFromStore(storage::JsonStore& store, const std::filesystem::path& resourcesRoot)
{
    mResources.clear();
    ++mRevision;

    // Rows imported from a legacy index carry that index's own convention, and
    // older builds wrote paths relative to resources/indexes/ rather than to
    // resources/. Both bases are tried here for the same reason LoadFromFile
    // tries both — dropping the older one silently unresolves those resources
    // the first time an old profile is upgraded.
    const std::vector<std::filesystem::path> relativeBases{resourcesRoot / "indexes", resourcesRoot};

    for (const auto& item : store.List(storage::ItemType::kResource))
    {
        const auto parsed = item.Parse();

        if (!parsed)
        {
            continue;
        }

        if (auto resource = ResourceFromJson(*parsed, relativeBases))
        {
            AddResource(*resource);
        }
    }
}

bool ResourceLibrary::SaveToStore(storage::JsonStore& store, const std::filesystem::path& resourcesRoot) const
{
    std::vector<storage::StoreItem> items;
    items.reserve(mResources.size());

    for (const auto& [key, resource] : mResources)
    {
        storage::StoreItem item;
        item.type = storage::ItemType::kResource;
        item.id = MakeStoreId(resource.type, resource.id);
        item.json = ResourceToJson(resource, resourcesRoot).dump();
        items.push_back(std::move(item));
    }

    return store.ReplaceAll(storage::ItemType::kResource, items);
}

bool ResourceLibrary::PutInStore(storage::JsonStore& store, const LibraryResource& resource,
                                 const std::filesystem::path& resourcesRoot)
{
    if (resource.type.empty() || resource.id.empty())
    {
        return false;
    }

    return store.Put(storage::ItemType::kResource, MakeStoreId(resource.type, resource.id),
                     ResourceToJson(resource, resourcesRoot));
}

bool ResourceLibrary::RemoveFromStore(storage::JsonStore& store, const std::string& type, const std::string& id)
{
    return store.Remove(storage::ItemType::kResource, MakeStoreId(type, id));
}

void ResourceLibrary::SaveToFile(const std::filesystem::path& path) const
{
    nlohmann::json json = nlohmann::json::array();
    const auto indexDir = path.parent_path();
    const auto resourcesRoot = indexDir.parent_path();

    std::error_code dirEc;
    std::filesystem::create_directories(indexDir, dirEc);

    for (const auto& [key, resource] : mResources)
    {
        json.push_back(ResourceToJson(resource, resourcesRoot));
    }

    [[maybe_unused]] const bool saved = util::WriteTextFileAtomic(path, json.dump(2));
}

void ResourceLibrary::LoadFromFile(const std::filesystem::path& path)
{
    std::ifstream file(path);

    if (!file.is_open())
    {
        return;
    }

    try
    {
        nlohmann::json json;
        file >> json;

        if (!json.is_array())
        {
            return;
        }

        // Historically some indexes stored paths relative to the index directory
        // rather than the resources root, so both are tried.
        const std::vector<std::filesystem::path> relativeBases{path.parent_path(), path.parent_path().parent_path()};

        for (const auto& item : json)
        {
            if (auto resource = ResourceFromJson(item, relativeBases))
            {
                AddResource(*resource);
            }
        }
    }
    catch (const std::exception&)
    {
        // Invalid JSON, ignore
    }
}
} // namespace guitarfx
