#include "resources/ResourceLibrary.h"
#include "presets/PresetTypesV2.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace namguitar
{
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
  }

  void ResourceLibrary::UpdateResource(const std::string& type, const std::string& id, const LibraryResource& updated)
  {
    const auto key = MakeKey(type, id);
    if (mResources.count(key))
    {
      mResources[key] = updated;
    }
  }

  void ResourceLibrary::RemoveResource(const std::string& type, const std::string& id)
  {
    const auto key = MakeKey(type, id);
    mResources.erase(key);
  }

  void ResourceLibrary::Clear()
  {
    mResources.clear();
  }

  std::optional<LibraryResource> ResourceLibrary::FindResource(const std::string& type, const std::string& id) const
  {
    const auto key = MakeKey(type, id);
    auto it = mResources.find(key);
    if (it != mResources.end())
    {
      return it->second;
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

  std::vector<LibraryResource> ResourceLibrary::GetResourcesByCategory(const std::string& type, const std::string& category) const
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

  bool ResourceLibrary::HasResource(const std::string& type, const std::string& id) const
  {
    return mResources.count(MakeKey(type, id)) > 0;
  }

  std::optional<std::filesystem::path> ResourceLibrary::ResolveResource(const ResourceRef& ref) const
  {
    // Priority: Library > FilePath
    if (ref.IsLibraryRef())
    {
      auto resource = FindResource(ref.resourceType, ref.resourceId);
      if (resource)
      {
        return resource->filePath;
      }
    }

    if (ref.IsFilePath())
    {
      if (std::filesystem::exists(ref.filePath))
      {
        return ref.filePath;
      }
    }

    // Embedded resources are handled separately by the preset loader
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

  void ResourceLibrary::SaveToFile(const std::filesystem::path& path) const
  {
    nlohmann::json json = nlohmann::json::array();

    for (const auto& [key, resource] : mResources)
    {
      nlohmann::json item;
      item["type"] = resource.type;
      item["id"] = resource.id;
      item["name"] = resource.name;
      item["category"] = resource.category;
      item["description"] = resource.description;
      item["filePath"] = resource.filePath.string();
      item["hash"] = resource.hash;
      item["tags"] = resource.tags;
      json.push_back(item);
    }

    std::ofstream file(path);
    if (file.is_open())
    {
      file << json.dump(2);
    }
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

      for (const auto& item : json)
      {
        LibraryResource resource;
        resource.type = item.value("type", "");
        resource.id = item.value("id", "");
        resource.name = item.value("name", "");
        resource.category = item.value("category", "");
        resource.description = item.value("description", "");
        resource.filePath = item.value("filePath", "");
        resource.hash = item.value("hash", "");

        if (item.contains("tags") && item["tags"].is_array())
        {
          for (const auto& tag : item["tags"])
          {
            resource.tags.push_back(tag.get<std::string>());
          }
        }

        if (!resource.type.empty() && !resource.id.empty())
        {
          AddResource(resource);
        }
      }
    }
    catch (const std::exception&)
    {
      // Invalid JSON, ignore
    }
  }

} // namespace namguitar
