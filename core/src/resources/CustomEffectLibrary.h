#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
  struct CustomEffectLibraryEntry
  {
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    std::string baseEffectType;
    std::string moduleResourceType;
    std::string moduleResourceId;
    std::string latestRevisionId;
    std::string thumbnailDataUrl;
    std::vector<std::string> tags;
    std::map<std::string, double> defaultParams;
    nlohmann::json descriptorSummary = nlohmann::json::object();
    std::string origin;
    std::string createdAt;
    std::string updatedAt;

    [[nodiscard]] bool IsValid() const;
  };

  [[nodiscard]] nlohmann::json SerializeCustomEffectLibraryEntry(const CustomEffectLibraryEntry& entry);
  [[nodiscard]] std::optional<CustomEffectLibraryEntry> DeserializeCustomEffectLibraryEntry(const nlohmann::json& json,
                                                                                            std::string* error = nullptr);

  class CustomEffectLibrary
  {
  public:
    void Clear();
    void UpsertEntry(const CustomEffectLibraryEntry& entry);
    [[nodiscard]] bool RemoveEntry(const std::string& id);
    [[nodiscard]] const CustomEffectLibraryEntry* GetEntry(const std::string& id) const;
    [[nodiscard]] const std::vector<CustomEffectLibraryEntry>& GetAllEntries() const { return mEntries; }

    void LoadFromFile(const std::filesystem::path& path);
    void SaveToFile(const std::filesystem::path& path) const;

  private:
    std::vector<CustomEffectLibraryEntry> mEntries;
  };

} // namespace guitarfx