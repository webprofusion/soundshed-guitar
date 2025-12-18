#include "PresetStorage.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>

#include "IPlugStructs.h"

namespace namguitar
{
namespace
{
/**
 * @brief Returns default presets for the application.
 *
 * Default preset definitions have been moved to JavaScript (main.js DEFAULT_PRESETS).
 * The UI layer is the source of truth for factory presets, which are sent to the
 * C++ backend when the user selects them. This function returns an empty vector
 * since the C++ backend no longer needs to define its own defaults.
 *
 * @return An empty vector - defaults are defined in JavaScript.
 */
std::vector<Preset> BuildDefaultPresets()
{
  // Default presets are now defined in JavaScript (resources/ui/main.js).
  // The UI sends preset data to the backend when the user loads a preset.
  // This keeps preset definitions in one place and simplifies maintenance.
  return {};
}

nlohmann::json SerializePreset(const Preset& preset)
{
  nlohmann::json jsonPreset;
  jsonPreset["id"] = preset.id;
  jsonPreset["name"] = preset.name;
  jsonPreset["category"] = preset.category;
  jsonPreset["description"] = preset.description;
  jsonPreset["namModelId"] = preset.namModelId;
  jsonPreset["irId"] = preset.irId;
  jsonPreset["fxChain"] = preset.fxChain;

  nlohmann::json attachments = nlohmann::json::array();
  for (const auto& attachment : preset.attachments)
  {
    attachments.push_back({
      {"type", attachment.type},
      {"id", attachment.id},
      {"filePath", attachment.filePath.generic_string()},
      {"hash", attachment.hash},
    });
  }
  jsonPreset["attachments"] = std::move(attachments);

  nlohmann::json parameters = nlohmann::json::array();
  for (const auto& parameter : preset.parameters)
  {
    parameters.push_back({
      {"id", parameter.id},
      {"value", parameter.value},
    });
  }
  jsonPreset["parameters"] = std::move(parameters);

  return jsonPreset;
}

nlohmann::json SerializeAllPresets(const std::vector<Preset>& presets)
{
  nlohmann::json jsonRoot;
  jsonRoot["presets"] = nlohmann::json::array();
  for (const auto& preset : presets)
  {
    jsonRoot["presets"].push_back(SerializePreset(preset));
  }

  return jsonRoot;
}

Preset DeserializePreset(const nlohmann::json& jsonPreset)
{
  Preset preset;
  preset.id = jsonPreset.value("id", "");
  preset.name = jsonPreset.value("name", "");
  preset.category = jsonPreset.value("category", "");
  preset.description = jsonPreset.value("description", "");
  preset.namModelId = jsonPreset.value("namModelId", "");
  preset.irId = jsonPreset.value("irId", "");

  if (jsonPreset.contains("fxChain") && jsonPreset["fxChain"].is_array())
  {
    for (const auto& fx : jsonPreset["fxChain"])
    {
      preset.fxChain.push_back(fx.get<std::string>());
    }
  }

  if (jsonPreset.contains("attachments") && jsonPreset["attachments"].is_array())
  {
    for (const auto& jsonAttachment : jsonPreset["attachments"])
    {
      PresetAttachment attachment;
      attachment.type = jsonAttachment.value("type", "");
      attachment.id = jsonAttachment.value("id", "");
      attachment.hash = jsonAttachment.value("hash", "");
      attachment.filePath = jsonAttachment.value("filePath", "");
      preset.attachments.push_back(std::move(attachment));
    }
  }

  if (jsonPreset.contains("parameters") && jsonPreset["parameters"].is_array())
  {
    for (const auto& jsonParameter : jsonPreset["parameters"])
    {
      PresetParameter parameter;
      parameter.id = jsonParameter.value("id", "");
      parameter.value = jsonParameter.value("value", 0.0);
      preset.parameters.push_back(parameter);
    }
  }

  return preset;
}

std::vector<Preset> DeserializeAllPresets(const std::string& serialized)
{
  std::vector<Preset> presets;
  if (serialized.empty())
  {
    return presets;
  }

  nlohmann::json jsonRoot = nlohmann::json::parse(serialized, nullptr, false);
  if (jsonRoot.is_discarded())
  {
    return presets;
  }

  if (!jsonRoot.contains("presets") || !jsonRoot["presets"].is_array())
  {
    return presets;
  }

  for (const auto& jsonPreset : jsonRoot["presets"])
  {
    presets.push_back(DeserializePreset(jsonPreset));
  }

  return presets;
}

} // namespace

PresetStorage::PresetStorage()
{
  const auto presetDir = mFileSystem.EnsureDirectory(mFileSystem.ResolvePresetDirectory());
  if (presetDir)
  {
    mPresetFile = *presetDir / "local_presets.json";
  }

  LoadFromDisk();
  EnsureDefaultPresets();
}

PresetStorage::~PresetStorage() = default;

bool PresetStorage::Serialize(iplug::IByteChunk& chunk) const
{
  const nlohmann::json serialized = SerializeAllPresets(mPresets);
  const std::string payload = serialized.dump();
  const uint32_t size = static_cast<uint32_t>(payload.size());
  chunk.PutBytes(&size, sizeof(size));
  chunk.PutBytes(payload.data(), static_cast<int>(payload.size()));
  return true;
}

int PresetStorage::Unserialize(const iplug::IByteChunk& chunk, int startPos)
{
  uint32_t size = 0;
  int position = chunk.GetBytes(&size, sizeof(size), startPos);
  if (position < 0 || size == 0)
  {
    return startPos;
  }

  std::string serialized(size, '\0');
  position = chunk.GetBytes(serialized.data(), static_cast<int>(size), position);
  if (position < 0)
  {
    return startPos;
  }

  mPresets = DeserializeAllPresets(serialized);
  PersistToDisk();

  return position;
}

void PresetStorage::SavePreset(const Preset& preset)
{
  Preset presetCopy = preset;
  for (auto& attachment : presetCopy.attachments)
  {
    if (attachment.hash.empty() && !attachment.filePath.empty())
    {
      attachment.hash = mHasher.HashFile(attachment.filePath);
    }
  }

  auto it = std::find_if(mPresets.begin(), mPresets.end(), [&](const Preset& candidate) {
    return candidate.id == presetCopy.id;
  });

  if (it != mPresets.end())
  {
    *it = std::move(presetCopy);
  }
  else
  {
    mPresets.push_back(std::move(presetCopy));
  }

  PersistToDisk();
}

std::vector<Preset> PresetStorage::ListPresets() const
{
  return mPresets;
}

std::optional<Preset> PresetStorage::FindPreset(const std::string& id) const
{
  auto it = std::find_if(mPresets.begin(), mPresets.end(), [&](const Preset& candidate) {
    return candidate.id == id;
  });

  if (it == mPresets.end())
  {
    return std::nullopt;
  }

  return *it;
}

void PresetStorage::PersistToDisk() const
{
  if (mPresetFile.empty())
  {
    return;
  }

  std::ofstream output(mPresetFile, std::ios::binary | std::ios::trunc);
  if (!output)
  {
    return;
  }

  output << SerializeAllPresets(mPresets).dump(2);
}

void PresetStorage::LoadFromDisk()
{
  if (mPresetFile.empty() || !std::filesystem::exists(mPresetFile))
  {
    return;
  }

  std::ifstream input(mPresetFile, std::ios::binary);
  if (!input)
  {
    return;
  }

  std::string serialized((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  mPresets = DeserializeAllPresets(serialized);
}

void PresetStorage::EnsureDefaultPresets()
{
  const auto defaults = BuildDefaultPresets();
  bool changed = false;

  for (const auto& preset : defaults)
  {
    const auto existing = std::find_if(mPresets.begin(), mPresets.end(), [&](const Preset& candidate) {
      return candidate.id == preset.id;
    });

    if (existing == mPresets.end())
    {
      mPresets.push_back(preset);
      changed = true;
    }
  }

  if (changed)
  {
    PersistToDisk();
  }
}

} // namespace namguitar
