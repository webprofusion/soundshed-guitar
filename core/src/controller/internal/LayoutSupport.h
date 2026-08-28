#pragma once

/**
 * LayoutSupport.h — On-disk locations for custom effect layouts.
 *
 * A layout is an index entry in one shared settings file plus a content
 * directory of its own. Both the layout handlers and the startup loader need
 * to agree on where those live, so the path rules are stated once here.
 */

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class FileSystem;
}

namespace guitarfx::controller_detail
{

/// Index file listing every known effect layout.
[[nodiscard]] std::filesystem::path ResolveEffectLayoutsSettingsPath(const FileSystem& fileSystem);

/// Reads the layout index, returning an empty object when it is missing or unreadable.
[[nodiscard]] nlohmann::json LoadEffectLayoutsSettings(const FileSystem& fileSystem);

void SaveEffectLayoutsSettings(const FileSystem& fileSystem, const nlohmann::json& root);

/// Content directory for one layout (images and layout.json).
[[nodiscard]] std::filesystem::path ResolveLayoutDir(const FileSystem& fileSystem, const std::string& layoutId);

[[nodiscard]] std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId);

} // namespace guitarfx::controller_detail
