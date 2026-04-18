#include "resources/CustomEffectLibrary.h"

#include <algorithm>
#include <fstream>

namespace guitarfx
{
  bool CustomEffectLibraryEntry::IsValid() const
  {
    return !id.empty()
      && !name.empty()
      && !baseEffectType.empty()
      && !moduleResourceType.empty()
      && !moduleResourceId.empty();
  }

  nlohmann::json SerializeCustomEffectLibraryEntry(const CustomEffectLibraryEntry& entry)
  {
    nlohmann::json json;
    json["id"] = entry.id;
    json["name"] = entry.name;
    json["category"] = entry.category;
    if (!entry.description.empty())
      json["description"] = entry.description;
    json["baseEffectType"] = entry.baseEffectType;
    json["moduleResourceType"] = entry.moduleResourceType;
    json["moduleResourceId"] = entry.moduleResourceId;
    if (!entry.latestRevisionId.empty())
      json["latestRevisionId"] = entry.latestRevisionId;
    if (!entry.thumbnailDataUrl.empty())
      json["thumbnailDataUrl"] = entry.thumbnailDataUrl;
    if (!entry.tags.empty())
      json["tags"] = entry.tags;
    if (!entry.defaultParams.empty())
    {
      nlohmann::json params = nlohmann::json::object();
      for (const auto& [key, value] : entry.defaultParams)
        params[key] = value;
      json["defaultParams"] = std::move(params);
    }
    if (entry.descriptorSummary.is_object() && !entry.descriptorSummary.empty())
      json["descriptorSummary"] = entry.descriptorSummary;
    if (!entry.origin.empty())
      json["origin"] = entry.origin;
    if (!entry.createdAt.empty())
      json["createdAt"] = entry.createdAt;
    if (!entry.updatedAt.empty())
      json["updatedAt"] = entry.updatedAt;
    return json;
  }

  std::optional<CustomEffectLibraryEntry> DeserializeCustomEffectLibraryEntry(const nlohmann::json& json,
                                                                              std::string* error)
  {
    if (!json.is_object())
    {
      if (error) *error = "Entry must be a JSON object";
      return std::nullopt;
    }

    CustomEffectLibraryEntry entry;
    entry.id = json.value("id", "");
    entry.name = json.value("name", "");
    entry.category = json.value("category", "");
    entry.description = json.value("description", "");
    entry.baseEffectType = json.value("baseEffectType", "");
    entry.moduleResourceType = json.value("moduleResourceType", "");
    entry.moduleResourceId = json.value("moduleResourceId", "");
    entry.latestRevisionId = json.value("latestRevisionId", "");
    entry.thumbnailDataUrl = json.value("thumbnailDataUrl", "");
    entry.origin = json.value("origin", "");
    entry.createdAt = json.value("createdAt", "");
    entry.updatedAt = json.value("updatedAt", "");

    if (json.contains("tags") && json["tags"].is_array())
    {
      for (const auto& tag : json["tags"])
      {
        if (tag.is_string())
          entry.tags.push_back(tag.get<std::string>());
      }
    }

    if (json.contains("defaultParams") && json["defaultParams"].is_object())
    {
      for (const auto& [key, value] : json["defaultParams"].items())
      {
        if (value.is_number())
          entry.defaultParams[key] = value.get<double>();
      }
    }

    if (json.contains("descriptorSummary") && json["descriptorSummary"].is_object())
      entry.descriptorSummary = json["descriptorSummary"];

    if (!entry.IsValid())
    {
      if (error) *error = "Entry is missing required id/name/baseEffectType/moduleResourceType/moduleResourceId fields";
      return std::nullopt;
    }

    return entry;
  }

  void CustomEffectLibrary::Clear()
  {
    mEntries.clear();
  }

  void CustomEffectLibrary::UpsertEntry(const CustomEffectLibraryEntry& entry)
  {
    auto it = std::find_if(mEntries.begin(), mEntries.end(), [&](const CustomEffectLibraryEntry& existing) {
      return existing.id == entry.id;
    });
    if (it != mEntries.end())
      *it = entry;
    else
      mEntries.push_back(entry);
  }

  bool CustomEffectLibrary::RemoveEntry(const std::string& id)
  {
    const auto originalSize = mEntries.size();
    mEntries.erase(std::remove_if(mEntries.begin(), mEntries.end(), [&](const CustomEffectLibraryEntry& entry) {
      return entry.id == id;
    }), mEntries.end());
    return mEntries.size() != originalSize;
  }

  const CustomEffectLibraryEntry* CustomEffectLibrary::GetEntry(const std::string& id) const
  {
    const auto it = std::find_if(mEntries.begin(), mEntries.end(), [&](const CustomEffectLibraryEntry& entry) {
      return entry.id == id;
    });
    return it != mEntries.end() ? &(*it) : nullptr;
  }

  void CustomEffectLibrary::LoadFromFile(const std::filesystem::path& path)
  {
    Clear();

    std::ifstream file(path);
    if (!file.is_open())
      return;

    try
    {
      nlohmann::json json;
      file >> json;
      if (!json.is_array())
        return;

      for (const auto& item : json)
      {
        if (auto entry = DeserializeCustomEffectLibraryEntry(item))
          mEntries.push_back(*entry);
      }
    }
    catch (const std::exception&)
    {
      Clear();
    }
  }

  void CustomEffectLibrary::SaveToFile(const std::filesystem::path& path) const
  {
    std::error_code dirEc;
    std::filesystem::create_directories(path.parent_path(), dirEc);

    nlohmann::json json = nlohmann::json::array();
    for (const auto& entry : mEntries)
      json.push_back(SerializeCustomEffectLibraryEntry(entry));

    std::ofstream file(path);
    if (file.is_open())
      file << json.dump(2);
  }

} // namespace guitarfx