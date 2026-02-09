#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifdef UpdateResource
#undef UpdateResource
#endif

namespace guitarfx
{
  struct ResourceRef;

  /**
   * A resource entry in the library.
   */
  struct LibraryResource
  {
    std::string type;        // "nam", "ir", etc.
    std::string id;          // Unique ID within type
    std::string name;        // Display name
    std::string category;    // Grouping (e.g., "Marshall", "Fender")
    std::string description; // User-facing description
    std::filesystem::path filePath; // Actual file location
    std::string hash;        // SHA-256 for verification
    std::vector<std::string> tags; // Searchable tags
    std::map<std::string, std::string> metadata; // Arbitrary metadata (provider, gear, etc.)
  };

  /**
   * Library of pre-defined resources (NAM models, IRs, etc.)
   * When a resource is updated, all presets using it get the update.
   */
  class ResourceLibrary
  {
  public:
    ResourceLibrary();
    ~ResourceLibrary();

    // Resource management
    void AddResource(const LibraryResource& resource);
    void UpdateResource(const std::string& type, const std::string& id, const LibraryResource& updated);
    void RemoveResource(const std::string& type, const std::string& id);
    void Clear();

    // Queries
    [[nodiscard]] std::optional<LibraryResource> LookupResource(const std::string& type, const std::string& id) const;
    [[nodiscard]] std::vector<LibraryResource> GetResourcesByType(const std::string& type) const;
    [[nodiscard]] std::vector<LibraryResource> GetResourcesByCategory(const std::string& type, const std::string& category) const;
    [[nodiscard]] std::vector<LibraryResource> GetAllResources() const;
    [[nodiscard]] bool HasResource(const std::string& type, const std::string& id) const;

    // Resolution
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResource(const ResourceRef& ref) const;

    // Persistence
    void LoadFromDirectory(const std::filesystem::path& directory);
    void SaveToFile(const std::filesystem::path& path) const;
    void LoadFromFile(const std::filesystem::path& path);

  private:
    // Key: "type:id"
    std::map<std::string, LibraryResource> mResources;

    [[nodiscard]] static std::string MakeKey(const std::string& type, const std::string& id);
  };

} // namespace guitarfx
