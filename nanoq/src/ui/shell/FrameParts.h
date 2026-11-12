#pragma once

// Small pieces both frames (ShellFrame.cpp, the compact one; WideFrame.cpp, the full one) build
// their bars from.

#include "ui/widgets/Buttons.h"
#include "uiclient/ClientState.h"

#include <functional>
#include <memory>
#include <string>

namespace nanoq::ui::frame
{
// The web UI's settings for the demo clip, shared with Soundshed Guitar and Nano.
inline constexpr const char* kDemoClipSetting = "demoAudio.selectedId";
inline constexpr const char* kDemoRenderRateSetting = "demoAudio.renderSampleRate";
inline constexpr const char* kDemoRepeatSetting = "nativeUi.demoRepeat";
/// The web UI's density preference (compactMode.ts): auto, compact or full.
inline constexpr const char* kDensitySetting = "ui.density";
/// Touch sizes (Shell::TouchSizes): auto (a touch-first device), on or off.
inline constexpr const char* kTouchControlsSetting = "ui.touchControls";

inline bool BoolSetting(const nlohmann::json& settings, const char* key, bool fallback)
{
    return settings.contains(key) && settings[key].is_boolean() ? settings[key].get<bool>() : fallback;
}

inline double NumberSetting(const nlohmann::json& settings, const char* key, double fallback)
{
    return settings.contains(key) && settings[key].is_number() ? settings[key].get<double>() : fallback;
}

inline std::string StringSetting(const nlohmann::json& settings, const char* key, const std::string& fallback)
{
    return settings.contains(key) && settings[key].is_string() ? settings[key].get<std::string>() : fallback;
}

inline std::string PresetTitle(const guitarfx::uiclient::ClientState& state)
{
    if (const auto* summary = state.FindPresetSummary(state.activePresetId))
    {
        return summary->name;
    }

    if (state.activePreset && !state.activePreset->name.empty())
    {
        return state.activePreset->name;
    }

    return "No preset";
}

/// The preset pill's badge for where the preset comes from: it says why a factory preset's Edit
/// and Save are greyed.
inline std::string SourceLabel(const std::string& source)
{
    if (source == "user")
    {
        return "User";
    }

    if (source == "factory")
    {
        return "Factory";
    }

    if (source == "session")
    {
        return "Session";
    }

    return {};
}

inline std::shared_ptr<IconButton> ToolButton(const Theme& theme, Icons& icons, const char* icon,
                                              std::function<void()> onClick, float size = 28.0f, float iconSize = 14.0f)
{
    auto b = std::make_shared<IconButton>(theme, icons, icon);
    b->Size(size, size).IconSize(iconSize);
    b->onClick = std::move(onClick);
    return b;
}
} // namespace nanoq::ui::frame
