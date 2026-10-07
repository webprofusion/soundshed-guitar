#pragma once

/**
 * InputModeSettings.h — the standalone app's input mode, as the app settings store it.
 *
 * Three keys, written by the UI (controls.ts) and read here only:
 *   inputChannel.monoMode  bool  Mono (true, the default) or two inputs (false)
 *   inputChannel.mono      int   which input Mono takes: 0 input 1, 1 input 2, 2 both summed
 *   inputChannel.dualMono  bool  with two inputs, run them as two mono chains
 *
 * In a DAW none of this applies: the track's bus decides (MultiPresetMixer::ApplyInputLayout).
 */

#include <nlohmann/json.hpp>

#include <algorithm>

namespace guitarfx::controller_detail
{
inline constexpr const char* kInputMonoModeSettingKey = "inputChannel.monoMode";
inline constexpr const char* kInputChannelSettingKey = "inputChannel.mono";
inline constexpr const char* kInputDualMonoSettingKey = "inputChannel.dualMono";

struct StoredInputMode
{
    bool monoMode = true; // so a guitar on input 1 comes through on first launch
    int inputChannel = 0;
    bool dualMono = false;
};

inline StoredInputMode ReadStoredInputMode(const nlohmann::json& appSettings)
{
    StoredInputMode stored;

    if (!appSettings.is_object())
    {
        return stored;
    }

    if (const auto it = appSettings.find(kInputMonoModeSettingKey); it != appSettings.end() && it->is_boolean())
    {
        stored.monoMode = it->get<bool>();
    }

    if (const auto it = appSettings.find(kInputChannelSettingKey); it != appSettings.end() && it->is_number_integer())
    {
        stored.inputChannel = std::clamp(it->get<int>(), 0, 2);
    }

    if (const auto it = appSettings.find(kInputDualMonoSettingKey); it != appSettings.end() && it->is_boolean())
    {
        stored.dualMono = it->get<bool>();
    }

    return stored;
}
} // namespace guitarfx::controller_detail
