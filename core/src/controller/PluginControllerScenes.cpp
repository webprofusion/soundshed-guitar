/**
 * PluginControllerScenes.cpp - The active preset's scenes: select, add, rename, move, remove.
 *
 * Engine-owned edits like those of PluginControllerPresetEdits.cpp, so both UIs follow the
 * same rules (the web UI's core/ui/ts/presetScenes.ts). Each answers with "presetLoaded".
 */

#include "PluginController.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace guitarfx
{
namespace
{
std::string BuildDefaultSceneId(std::size_t index)
{
    return "scene-" + std::to_string(index + 1);
}

std::string BuildUniqueSceneId(const Preset& preset)
{
    std::size_t index = preset.scenes.size();

    for (;;)
    {
        auto candidate = BuildDefaultSceneId(index);
        const bool taken = std::any_of(preset.scenes.begin(), preset.scenes.end(),
                                       [&candidate](const PresetScene& scene) { return scene.id == candidate; });

        if (!taken)
        {
            return candidate;
        }

        ++index;
    }
}
} // namespace

void PluginController::HandleSelectSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    NormalizePresetScenes(*mActivePreset);
    const auto sceneId = payload.value("sceneId", std::string{});
    const auto& scenes = mActivePreset->scenes;
    const auto it = std::find_if(scenes.begin(), scenes.end(),
                                 [&sceneId](const PresetScene& scene) { return scene.id == sceneId; });

    if (it == scenes.end())
    {
        ReportErrorToUI("Scene not found", sceneId);
        return;
    }

    // The same switch a footswitch makes: banks the outgoing scene's plugin state, applies
    // the new scene and reports it with "presetLoaded".
    SelectSceneByIndexDirect(static_cast<int>(it - scenes.begin()));
}

void PluginController::HandleAddSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    CaptureLiveHostedPluginStateIntoActivePreset();
    NormalizePresetScenes(*mActivePreset);

    const auto fromSceneId = payload.value("fromSceneId", GetResolvedActiveSceneId());
    const auto* source = FindPresetScene(*mActivePreset, fromSceneId);

    PresetScene scene;
    scene.id = BuildUniqueSceneId(*mActivePreset);
    scene.title = payload.value("title", std::string{});

    if (scene.title.empty())
    {
        scene.title = "Scene " + std::to_string(mActivePreset->scenes.size() + 1);
    }

    scene.graph = source ? source->graph : mActivePreset->graph;
    const auto newSceneId = scene.id;
    mActivePreset->scenes.push_back(std::move(scene));

    // The new scene is a copy of the one playing, so switching to it changes nothing audible.
    (void)SetPresetActiveScene(*mActivePreset, newSceneId, &mActiveSceneId);
    MirrorActivePresetJson();
    SendActivePresetLoaded();
}

void PluginController::HandleRenameSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    NormalizePresetScenes(*mActivePreset);
    auto* scene = FindPresetScene(*mActivePreset, payload.value("sceneId", std::string{}));

    if (!scene)
    {
        return;
    }

    auto title = payload.value("title", std::string{});
    title.erase(0, title.find_first_not_of(" \t\r\n"));
    title.erase(title.find_last_not_of(" \t\r\n") + 1);
    scene->title = title.empty() ? std::string("Scene") : title;

    MirrorActivePresetJson();
    SendActivePresetLoaded();
}

void PluginController::HandleMoveSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    NormalizePresetScenes(*mActivePreset);
    auto& scenes = mActivePreset->scenes;
    const auto sceneId = payload.value("sceneId", std::string{});
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&sceneId](const PresetScene& s) { return s.id == sceneId; });
    const auto toIt = payload.find("toIndex");

    if (it == scenes.end() || toIt == payload.end() || !toIt->is_number_integer())
    {
        return;
    }

    // Where the scene ends up, counted in the list as it will be: past the end is the end.
    const auto from = static_cast<std::size_t>(it - scenes.begin());
    const auto to = static_cast<std::size_t>(
        std::clamp<long long>(toIt->get<long long>(), 0, static_cast<long long>(scenes.size()) - 1));

    if (from != to)
    {
        // Only the order changes: the scene playing keeps playing, so nothing is rebuilt. A
        // footswitch or a mapped scene number now reaches it at its new position.
        PresetScene moved = std::move(*it);
        scenes.erase(it);
        scenes.insert(scenes.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        MirrorActivePresetJson();
    }

    SendActivePresetLoaded();
}

void PluginController::HandleRemoveSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    CaptureLiveHostedPluginStateIntoActivePreset();
    NormalizePresetScenes(*mActivePreset);
    auto& scenes = mActivePreset->scenes;

    if (scenes.size() <= 1)
    {
        ReportErrorToUI("Cannot remove scene", "A preset must keep at least one scene");
        return;
    }

    const auto sceneId = payload.value("sceneId", std::string{});
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&sceneId](const PresetScene& s) { return s.id == sceneId; });

    if (it == scenes.end())
    {
        return;
    }

    const bool wasActive = sceneId == GetResolvedActiveSceneId();
    const auto removeIndex = static_cast<std::size_t>(it - scenes.begin());
    scenes.erase(it);

    // As the web UI does: the scene now in the removed one's place, or the new last one.
    const auto& fallback = scenes[std::min(removeIndex, scenes.size() - 1)];
    const auto nextSceneId = wasActive ? fallback.id : GetResolvedActiveSceneId();

    (void)SetPresetActiveScene(*mActivePreset, nextSceneId, &mActiveSceneId);

    if (wasActive)
    {
        // The removed scene was playing: the one that replaces it has to be heard. In a
        // multi-preset mix only this preset's slot is rebuilt. Either way the rebuild leaves
        // the working copy's JSON and its slot's entry current.
        ApplyActivePresetInItsSlot();
    }
    else
    {
        MirrorActivePresetJson();
    }

    SendActivePresetLoaded();
}
} // namespace guitarfx
