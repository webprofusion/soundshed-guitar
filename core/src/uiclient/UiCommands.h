#pragma once

/**
 * UiCommands.h - Everything the native UI asks the engine to do.
 *
 * One method per user intent, each sending the same message the WebView UI sends for it.
 * Anything that changes a persisted document goes through an engine-owned command
 * (selectScene, addScene, newPreset, setPresetFavorite, setOutputMuted, ...), never by
 * editing a copy here and sending it back whole (docs/plans/native-ui.md, rule C3).
 */

#include "uiclient/UiClient.h"

#include <optional>
#include <string>

namespace guitarfx::uiclient
{
class UiCommands
{
public:
    explicit UiCommands(UiClient& client) : mClient(client)
    {
    }

    // ── Presets and scenes ──────────────────────────────────────────────────
    void LoadPreset(const std::string& presetId, const std::string& sceneId = {});
    /// Steps through the library in the engine's order, wrapping, as the web UI's
    /// previous/next buttons do. `delta` is +1 or -1.
    void StepPreset(int delta);
    void SelectScene(const std::string& sceneId);
    void AddScene(const std::string& title = {});
    void RenameScene(const std::string& sceneId, const std::string& title);
    void RemoveScene(const std::string& sceneId);
    void SavePreset();
    void SavePresetAs(const std::string& name, const std::string& category, const std::string& description);
    void RenamePreset(const std::string& name, const std::string& category, const std::string& description);
    void NewPreset();
    void DeletePreset(const std::string& presetId);
    void SetPresetFavorite(const std::string& presetId, bool favorite);

    // ── Multi-Rig (awareness only: rigs are built in Soundshed Guitar) ──────
    void FocusMixerPreset(const std::string& presetId);
    void RemoveActivePreset(const std::string& presetId);

    // ── Setlists ────────────────────────────────────────────────────────────
    void SetSetlistCursor(int index);
    void SelectSetlist(const std::string& setlistId);

    // ── Signal chain ────────────────────────────────────────────────────────
    void SetNodeBypassed(const std::string& nodeId, bool bypassed);
    void SetNodeParam(const std::string& nodeId, const std::string& key, double value);
    void AddNode(const std::string& effectType, const std::string& afterNodeId);
    void RemoveNode(const std::string& nodeId);
    void MoveNode(const std::string& nodeId, const std::string& afterNodeId);
    void CollapseSplit(const std::string& splitterId);
    /// A factory preset is sent as parameter changes; a saved one is applied by the engine,
    /// which restores its resources and config as well.
    void ApplyEffectPreset(const std::string& nodeId, const EffectPresetInfo& preset);
    /// Saves a node's current settings as a user preset for its effect type.
    void SaveEffectPreset(const std::string& nodeId, const std::string& name);

    // ── Global chain and output ─────────────────────────────────────────────
    void SetGlobalChainParam(const std::string& path, double value);
    void SetGlobalChainToggle(const std::string& path, bool enabled);
    void SetOutputMuted(bool muted);
    void SetInputMode(bool monoMode, int inputChannel);

    // ── Tuner, metronome, demo audio ────────────────────────────────────────
    void SetTunerActive(bool active);
    void SetTunerLiveMode(bool liveMode);
    void SetTunerReference(double frequency);
    void SetMetronomeEnabled(bool enabled);
    void SetMetronomeBpm(double bpm);
    void SetMetronomeVolume(double volumeDb);
    void SetMetronomeClickType(const std::string& clickType);
    void SetMetronomeTimeSignature(int numerator, int denominator);
    void SetMetronomeBeatPattern(const std::string& pattern);
    void SetMetronomeSubdivision(const std::string& subdivision);
    void PreviewDemoClip(const std::string& clipId, bool repeat);
    void PreviewRiff(const std::string& takeId);
    void StopDemo();
    void RenderDemoClip(const std::string& clipId, int sampleRate);
    void RenderRiffTake(const std::string& takeId, int sampleRate);

    // ── Settings and device ─────────────────────────────────────────────────
    void SetSetting(const std::string& key, const nlohmann::json& value);
    void SetTheme(const std::string& theme);
    /// Merges into the "native" part of the UI settings, leaving the web UI's own alone.
    void PatchNativeUiSettings(const nlohmann::json& patch);
    void AudioDevice(const std::string& action, nlohmann::json payload = nlohmann::json::object());
    void OpenUrl(const std::string& url);

    // ── Tone sharing ────────────────────────────────────────────────────────────
    /// Installs downloaded tone sharing archives as one entry of the installedPacks setting.
    /// `entry` is {id, title, source, packId?}; each archive is {title, data: base64 zip}; a
    /// pack's presets go in a folder named `folder`. Answered by presetArchivesInstalled or
    /// presetArchivesInstallFailed.
    void InstallPresetArchives(const nlohmann::json& entry, const std::string& folder, nlohmann::json archives);
    /// Takes an install out again: its presets, folder, and the models only it used.
    void DeleteInstalledPresetArchive(const std::string& entryId);

private:
    UiClient& mClient;
};
} // namespace guitarfx::uiclient
