#pragma once

#include "storage/JsonStore.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
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
    // Lightweight (filePath, id) index for file-path matching. Avoids copying
    // full resource structs (with their metadata maps) on hot paths such as the
    // folder browser. Resources without a file path are skipped.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> GetResourcePathIndex() const;
    [[nodiscard]] bool HasResource(const std::string& type, const std::string& id) const;

    // Resolution
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResource(const ResourceRef& ref) const;

    // Persistence
    //
    // The library is backed by the document store: one row per resource, keyed
    // "<type>:<id>" — the same key used in memory. Individual mutations write a
    // single row, so a bulk import is N cheap upserts in one transaction rather
    // than N rewrites of one large file.
    //
    // `resourcesRoot` is the directory that file paths are stored relative to
    // (the profile's `resources` folder), so a profile stays portable.

    /// Replaces the in-memory contents with everything in the store.
    void LoadFromStore(storage::JsonStore& store, const std::filesystem::path& resourcesRoot);
    /// Writes the whole library, replacing whatever the store held. Atomic.
    bool SaveToStore(storage::JsonStore& store, const std::filesystem::path& resourcesRoot) const;
    /// Writes one resource. Prefer this over SaveToStore for single edits.
    static bool PutInStore(storage::JsonStore& store,
                           const LibraryResource& resource,
                           const std::filesystem::path& resourcesRoot);
    static bool RemoveFromStore(storage::JsonStore& store, const std::string& type, const std::string& id);

    /// Store key for a resource: "<type>:<id>", matching the in-memory key.
    [[nodiscard]] static std::string MakeStoreId(const std::string& type, const std::string& id);

    // Legacy file persistence. Retained because the preset-generator tool and
    // the pack/archive formats still speak the on-disk index shape; the running
    // app no longer reads or writes it.
    void LoadFromDirectory(const std::filesystem::path& directory);
    void SaveToFile(const std::filesystem::path& path) const;
    void LoadFromFile(const std::filesystem::path& path);

  private:
    // Key: "type:id"
    std::map<std::string, LibraryResource> mResources;

    [[nodiscard]] static std::string MakeKey(const std::string& type, const std::string& id);
  };

} // namespace guitarfx
