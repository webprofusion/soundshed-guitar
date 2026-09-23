#pragma once

/**
 * UiClientParsing.h - Turning engine payloads into ClientState values.
 *
 * Lenient on purpose: a field of the wrong type or a missing one gives its default, the way
 * the TypeScript UI's normalisers do, so an older or newer engine never takes the UI down.
 */

#include "uiclient/ClientState.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient::parse
{
[[nodiscard]] std::vector<std::string> StringArray(const nlohmann::json& object, const char* key);

/// Through the core's own preset reader, so a preset parses here exactly as it does in the engine.
[[nodiscard]] std::optional<Preset> ParsePreset(const nlohmann::json& preset);

[[nodiscard]] std::map<std::string, std::vector<LibraryResource>> ResourceLibrary(const nlohmann::json& library);

[[nodiscard]] LibraryResource Resource(const nlohmann::json& entry, const std::string& type);

[[nodiscard]] std::vector<EffectTypeInfo> EffectCatalog(const nlohmann::json& catalog);

[[nodiscard]] EffectParamInfo EffectParam(const nlohmann::json& param);

[[nodiscard]] EffectPresetInfo EffectPreset(const nlohmann::json& preset, const std::string& defaultSource);
} // namespace guitarfx::uiclient::parse
