/**
 * UiCommands.cpp - The native UI's requests to the engine.
 */

#include "uiclient/UiCommands.h"

#include <algorithm>

namespace guitarfx::uiclient
{
// ── Presets and scenes ─────────────────────────────────────────────────────

void UiCommands::LoadPreset(const std::string& presetId, const std::string& sceneId)
{
    if (presetId.empty())
    {
        return;
    }

    // By id: the engine reads the preset from its own store, so this UI never sends a
    // preset body it might have read differently.
    nlohmann::json payload = {{"presetId", presetId}};

    if (!sceneId.empty())
    {
        payload["sceneId"] = sceneId;
    }

    mClient.Send("loadPreset", std::move(payload));
}

void UiCommands::StepPreset(int delta)
{
    const auto& state = mClient.State();
    const auto& list = state.presetList;

    if (list.empty())
    {
        return;
    }

    const auto it = std::find_if(list.begin(), list.end(),
                                 [&state](const PresetSummary& p) { return p.id == state.activePresetId; });
    const auto size = static_cast<int>(list.size());
    int index = it == list.end() ? -1 : static_cast<int>(it - list.begin());

    if (delta < 0)
    {
        index = index <= 0 ? size - 1 : index - 1;
    }
    else
    {
        index = index < 0 || index >= size - 1 ? 0 : index + 1;
    }

    LoadPreset(list[static_cast<std::size_t>(index)].id);
}

void UiCommands::SelectScene(const std::string& sceneId)
{
    if (!sceneId.empty() && sceneId != mClient.State().activeSceneId)
    {
        mClient.Send("selectScene", {{"sceneId", sceneId}});
    }
}

void UiCommands::AddScene(const std::string& title)
{
    nlohmann::json payload = nlohmann::json::object();
    payload["fromSceneId"] = mClient.State().activeSceneId;

    if (!title.empty())
    {
        payload["title"] = title;
    }

    mClient.Send("addScene", std::move(payload));
}

void UiCommands::RenameScene(const std::string& sceneId, const std::string& title)
{
    mClient.Send("renameScene", {{"sceneId", sceneId}, {"title", title}});
}

void UiCommands::RemoveScene(const std::string& sceneId)
{
    mClient.Send("removeScene", {{"sceneId", sceneId}});
}

void UiCommands::SavePreset()
{
    const auto& state = mClient.State();

    if (!state.activePreset)
    {
        return;
    }

    // No preset body: the engine saves its own working copy.
    mClient.Send("savePreset", {{"saveMode", "overwrite"},
                                {"presetId", state.activePresetId},
                                {"name", state.activePreset->name},
                                {"category", state.activePreset->category},
                                {"description", state.activePreset->description},
                                {"includeGlobalSignalChain", false}});
}

void UiCommands::SavePresetAs(const std::string& name, const std::string& category, const std::string& description)
{
    mClient.Send("savePreset", {{"saveMode", "save-as"},
                                {"name", name},
                                {"category", category.empty() ? std::string("User") : category},
                                {"description", description},
                                {"includeGlobalSignalChain", false}});
}

void UiCommands::RenamePreset(const std::string& name, const std::string& category, const std::string& description)
{
    const auto& state = mClient.State();

    if (!state.activePreset)
    {
        return;
    }

    mClient.Send("savePreset", {{"saveMode", "overwrite"},
                                {"presetId", state.activePresetId},
                                {"name", name},
                                {"category", category},
                                {"description", description},
                                {"includeGlobalSignalChain", false}});
}

void UiCommands::NewPreset()
{
    mClient.Send("newPreset");
}

void UiCommands::DeletePreset(const std::string& presetId)
{
    mClient.Send("deletePreset", {{"presetId", presetId}});
}

void UiCommands::SetPresetFavorite(const std::string& presetId, bool favorite)
{
    mClient.Send("setPresetFavorite", {{"presetId", presetId}, {"favorite", favorite}});
}

// ── Multi-Rig ──────────────────────────────────────────────────────────

void UiCommands::FocusMixerPreset(const std::string& presetId)
{
    mClient.Send("focusMixerPreset", {{"presetId", presetId}});
}

void UiCommands::RemoveActivePreset(const std::string& presetId)
{
    mClient.Send("removeActivePreset", {{"presetId", presetId}});
}

// ── Setlists ───────────────────────────────────────────────────────────────

void UiCommands::SetSetlistCursor(int index)
{
    mClient.Send("setSetlistCursor", {{"cursorIndex", index}});
}

void UiCommands::SelectSetlist(const std::string& setlistId)
{
    mClient.Send("selectSetlist", {{"setlistId", setlistId}});
}

// ── Signal chain ───────────────────────────────────────────────────────────

void UiCommands::SetNodeBypassed(const std::string& nodeId, bool bypassed)
{
    mClient.Send("updateSignalPathNodeBypass", {{"nodeId", nodeId}, {"bypassed", bypassed}});
}

void UiCommands::SetNodeParam(const std::string& nodeId, const std::string& key, double value)
{
    // Mirror the change at once, as the web UI's optimistic knob does; the engine does not
    // echo a param change it was asked for.
    auto& state = mClient.MutableState();

    if (state.activePreset)
    {
        if (auto* node = state.activePreset->graph.FindNode(nodeId))
        {
            node->params[key] = value;
        }
    }

    mClient.Send("updateSignalPathNodeParam", {{"nodeId", nodeId}, {"paramKey", key}, {"value", value}});
}

void UiCommands::AddNode(const std::string& effectType, const std::string& afterNodeId)
{
    mClient.Send("addSignalPathNode", {{"effectType", effectType}, {"insertAfter", afterNodeId}});
}

void UiCommands::RemoveNode(const std::string& nodeId)
{
    mClient.Send("deleteSignalPathNode", {{"nodeId", nodeId}});
}

void UiCommands::MoveNode(const std::string& nodeId, const std::string& afterNodeId)
{
    if (nodeId != afterNodeId)
    {
        mClient.Send("reorderSignalPathNode", {{"nodeId", nodeId}, {"targetNodeId", afterNodeId}});
    }
}

void UiCommands::CollapseSplit(const std::string& splitterId)
{
    mClient.Send("collapseSignalPathSplit", {{"splitterId", splitterId}});
}

void UiCommands::ApplyEffectPreset(const std::string& nodeId, const EffectPresetInfo& preset)
{
    for (const auto& [key, value] : preset.parameters)
    {
        SetNodeParam(nodeId, key, value);
    }
}

void UiCommands::SaveEffectPreset(const std::string& nodeId, const std::string& name)
{
    const auto& state = mClient.State();
    const auto* node = state.activePreset ? state.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr || name.empty())
    {
        return;
    }

    nlohmann::json parameters = nlohmann::json::object();

    for (const auto& [key, value] : node->params)
    {
        parameters[key] = value;
    }

    mClient.Send("saveEffectPreset", {{"effectType", node->type}, {"name", name}, {"parameters", parameters}});
}

// ── Global chain and output ───────────────────────────────────────────────

void UiCommands::SetGlobalChainParam(const std::string& path, double value)
{
    mClient.Send("setGlobalChainParam", {{"path", path}, {"value", value}});
}

void UiCommands::SetGlobalChainToggle(const std::string& path, bool enabled)
{
    mClient.Send("setGlobalChainParam", {{"path", path}, {"value", enabled}});
}

void UiCommands::SetOutputMuted(bool muted)
{
    mClient.Send("setOutputMuted", {{"muted", muted}});
}

void UiCommands::SetInputMode(bool monoMode, int inputChannel)
{
    mClient.Send("setInputMode", {{"monoMode", monoMode}, {"inputChannel", inputChannel}});
}

// ── Tuner, metronome, demo audio ─────────────────────────────────────────────

void UiCommands::SetTunerActive(bool active)
{
    mClient.Send("tuner", {{"action", active ? "start" : "stop"}});
}

void UiCommands::SetTunerLiveMode(bool liveMode)
{
    mClient.Send("tuner", {{"action", "setLiveMode"}, {"liveMode", liveMode}});
}

void UiCommands::SetTunerReference(double frequency)
{
    mClient.Send("tuner", {{"action", "setReference"}, {"referenceFrequency", frequency}});
}

void UiCommands::SetMetronomeEnabled(bool enabled)
{
    mClient.Send("setMetronome", {{"enabled", enabled}});
}

void UiCommands::SetMetronomeBpm(double bpm)
{
    mClient.Send("setMetronome", {{"bpm", std::clamp(bpm, 20.0, 300.0)}});
}

void UiCommands::SetMetronomeVolume(double volumeDb)
{
    mClient.Send("setMetronome", {{"volumeDb", volumeDb}});
}

void UiCommands::SetMetronomeClickType(const std::string& clickType)
{
    mClient.Send("setMetronome", {{"clickType", clickType}});
}

void UiCommands::SetMetronomeTimeSignature(int numerator, int denominator)
{
    mClient.Send("setMetronome", {{"timeSigNum", numerator}, {"timeSigDen", denominator}});
}

void UiCommands::SetMetronomeBeatPattern(const std::string& pattern)
{
    mClient.Send("setMetronome", {{"beatPattern", pattern}});
}

void UiCommands::SetMetronomeSubdivision(const std::string& subdivision)
{
    mClient.Send("setMetronome", {{"subdivision", subdivision}});
}

void UiCommands::PreviewDemoClip(const std::string& clipId, bool repeat)
{
    mClient.Send("previewDemoAudio", {{"clipId", clipId}, {"repeat", repeat}});
}

void UiCommands::PreviewRiff(const std::string& takeId)
{
    mClient.Send("previewRiffTake", {{"takeId", takeId}, {"enableGuidance", false}});
}

void UiCommands::StopDemo()
{
    mClient.Send("stopDemoAudio");
}

void UiCommands::RenderDemoClip(const std::string& clipId, int sampleRate)
{
    mClient.Send("renderDemoAudio", {{"clipId", clipId}, {"renderSampleRate", sampleRate}});
}

void UiCommands::RenderRiffTake(const std::string& takeId, int sampleRate)
{
    mClient.Send("renderDemoAudio", {{"takeId", takeId}, {"renderSampleRate", sampleRate}});
}

// ── Settings and device ──────────────────────────────────────────────────────

void UiCommands::SetSetting(const std::string& key, const nlohmann::json& value)
{
    auto& state = mClient.MutableState();
    state.appSettings[key] = value;
    mClient.Send("setSetting", {{"key", key}, {"value", value}});

    // The engine does not echo a setting back to the UI that set it, so the views that read
    // it (the UI scale, the demo clip) hear of it here.
    mClient.Notify(Topic::Session);
}

void UiCommands::SetTheme(const std::string& theme)
{
    auto& state = mClient.MutableState();
    state.theme = theme;
    state.appSettings["theme"] = theme;
    mClient.Send("setTheme", {{"theme", theme}});
    mClient.Notify(Topic::Session);
}

void UiCommands::PatchNativeUiSettings(const nlohmann::json& patch)
{
    auto& state = mClient.MutableState();

    if (!state.uiSettings.is_object())
    {
        state.uiSettings = nlohmann::json::object();
    }

    auto& native = state.uiSettings["native"];

    if (!native.is_object())
    {
        native = nlohmann::json::object();
    }

    native.merge_patch(patch);
    mClient.Send("uiSettingsChanged", {{"patch", {{"native", patch}}}});
}

void UiCommands::AudioDevice(const std::string& action, nlohmann::json payload)
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["action"] = action;
    mClient.Send("audioDevice", std::move(payload));
}

void UiCommands::OpenUrl(const std::string& url)
{
    mClient.Send("openUrl", {{"url", url}});
}
} // namespace guitarfx::uiclient
