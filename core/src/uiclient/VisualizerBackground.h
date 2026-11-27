#pragma once

/**
 * VisualizerBackground.h - the player's own picture behind the effect panel (Settings > User
 * Interface > Effect Panel Background), as the web UI's visualizerBackground.ts keeps it.
 *
 * The engine stores the picture (controller/UiBackgroundService.h) and answers
 * "uiBackgroundImageSelected" with its route, "ui-backgrounds/<16 hex>.<ext>", which the UI keeps
 * in an app setting so every instance shows it, as the theme is. Fade veils it toward the theme's
 * own background so the text over it stays readable.
 */

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace guitarfx::uiclient::visualizer_background
{
inline constexpr const char* kImageSetting = "ui.visualizerBackground.image";
inline constexpr const char* kFadeSetting = "ui.visualizerBackground.fade";

/// Percent the picture is veiled by until the player sets it, and the most it can be.
inline constexpr int kFadeDefault = 30;
inline constexpr int kFadeMax = 90;
inline constexpr int kFadeStep = 5;

/// Whether `url` is one the engine hands out: the setting is shared, so nothing else is drawn.
[[nodiscard]] bool IsStoredUrl(const std::string& url);

/// The picture's route, or none when there is none (or the stored value is not one the engine gave).
[[nodiscard]] std::optional<std::string> Url(const nlohmann::json& appSettings);

/// The fade, a whole percent within 0..kFadeMax.
[[nodiscard]] int Fade(const nlohmann::json& appSettings);
} // namespace guitarfx::uiclient::visualizer_background
