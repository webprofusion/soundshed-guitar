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

#include <functional>
#include <optional>
#include <vector>
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

    /// What the save dialog edits (the web UI's Details tab). A folder files the preset there
    /// ("" for none); nullopt leaves it where it is.
    struct PresetDetails
    {
        std::string name;
        std::string category;
        std::string description;
        std::vector<std::string> tags;
        std::optional<std::string> folderId;
        /// The input peak the preset was designed at (dBFS), when the dialog applied one.
        std::optional<double> designedPeakInputDbfs;
        /// The body to save instead of the preset playing: the inspector's cleaned copy.
        std::optional<nlohmann::json> preset;
    };

    /// Saves the preset playing with these details: over itself, or (`asNew`) as a new preset.
    void SavePresetWithDetails(const PresetDetails& details, bool asNew);
    void RenamePreset(const std::string& name, const std::string& category, const std::string& description);
    void NewPreset();
    void DeletePreset(const std::string& presetId);
    void SetPresetFavorite(const std::string& presetId, bool favorite);
    /// 1 to 5 stars, or 0 to clear.
    void SetPresetRating(const std::string& presetId, int rating);
    /// Told of each favourite and rating set above: ToneService passes those of presets installed
    /// from tone sharing on to the service, as the web UI's preset links do.
    std::function<void(const std::string& presetId, bool favorite)> onPresetFavoriteSet;
    std::function<void(const std::string& presetId, int rating)> onPresetRatingSet;

    // ── Preset folders, edited by the engine; each is answered by presetFolders ──
    /// A new folder, at the top or inside `parentId`; it becomes the folder shown.
    void CreatePresetFolder(const std::string& name, const std::string& parentId = {});
    void RenamePresetFolder(const std::string& folderId, const std::string& name);
    /// Into `parentId`, or to the top level with an empty one; never into itself.
    void MovePresetFolder(const std::string& folderId, const std::string& parentId);
    /// The folder and its subfolders go; their presets stay in the library.
    void DeletePresetFolder(const std::string& folderId);
    /// A preset into a folder (out of any other); an empty folder takes it out of all.
    void MovePresetToFolder(const std::string& presetId, const std::string& folderId);
    /// The folder the preset list shows (a real one, or __all__, __favorites__, __recents__).
    void SelectPresetFolder(const std::string& folderId);
    // ── Setlists, edited by the engine; each is answered by setlists and setlistCursorChanged ──
    /// A new setlist (with a bank number, or -1 for none); it becomes the active one.
    void CreateSetlist(const std::string& name, int bank = -1);
    /// A setlist's name and bank (-1 for none); an empty id is the active setlist.
    void UpdateSetlist(const std::string& setlistId, const std::string& name, int bank);
    void DeleteSetlist(const std::string& setlistId);
    /// The slot commands work on the active setlist when `setlistId` is empty.
    void AddSetlistSlot(const std::string& setlistId, const std::string& presetId);
    /// Puts a preset in a slot (a pad), padding with empty slots; an empty id clears it.
    void SetSetlistSlot(const std::string& setlistId, int index, const std::string& presetId);
    void MoveSetlistSlot(const std::string& setlistId, int fromIndex, int toIndex);
    void RemoveSetlistSlot(const std::string& setlistId, int index);

    // ── Multi-Rig (awareness only: rigs are built in Sepia) ──────
    void FocusMixerPreset(const std::string& presetId);
    void RemoveActivePreset(const std::string& presetId);

    // ── Multi-Rig: the mix, mirrored at once (the engine does not echo levels) ──
    void AddActivePreset(const std::string& presetId);
    void SetPresetMix(const std::string& presetId, double mix);
    void SetPresetPan(const std::string& presetId, double pan);
    void SetPresetMute(const std::string& presetId, bool mute);
    void SetPresetSolo(const std::string& presetId, bool solo);
    void SetMixGain(double gainDb);
    /// Saves the mix as a Multi-Rig; with `id`, updates that one in place.
    void SaveCompositePreset(const std::string& name, const std::string& description,
                             const std::vector<std::string>& tags, const std::string& id = {});
    void LoadCompositePreset(const std::string& id);
    void RemoveCompositePreset(const std::string& id);
    void GetCompositePresetList();

    // ── Setlists ────────────────────────────────────────────────────────────
    void SetSetlistCursor(int index);
    void SelectSetlist(const std::string& setlistId);

    // ── Signal chain ────────────────────────────────────────────────────────
    void SetNodeBypassed(const std::string& nodeId, bool bypassed);
    void SetNodeParam(const std::string& nodeId, const std::string& key, double value);
    /// A node's string config (updateSignalPathNodeConfig), mirrored at once as SetNodeParam is.
    void SetNodeConfig(const std::string& nodeId, const std::string& key, const std::string& value);
    /// Loads the active preset with `graph` as its chain, in its scene and its own: what the
    /// chain's undo does when the change it reverses rebuilt the chain (history.ts,
    /// applyWholeGraph). The engine compares the result with the preset as saved for dirty.
    void LoadActivePresetWithGraph(const SignalGraph& graph);
    void AddNode(const std::string& effectType, const std::string& afterNodeId);
    /// Adds an effect on one edge of the graph: the way into a parallel branch, which "after a
    /// node" cannot name.
    void AddNodeOnEdge(const std::string& effectType, const GraphEdge& edge);
    void RemoveNode(const std::string& nodeId);
    void MoveNode(const std::string& nodeId, const std::string& afterNodeId);
    /// Moves a node onto an edge of the graph: a cable, which may be inside a parallel branch.
    void MoveNodeToEdge(const std::string& nodeId, const GraphEdge& edge);
    void CollapseSplit(const std::string& splitterId);
    /// Gives a split 2 to 4 branches; the engine adds empty ones and removes only empty ones.
    void SetSplitBranchCount(const std::string& splitterId, int count);
    /// Puts another effect in the node's place, as the web UI's FX library replace does.
    void ReplaceNode(const std::string& nodeId, const std::string& effectType);
    /// The node's own name; empty hands it back its automatic one.
    void RenameNode(const std::string& nodeId, const std::string& title);
    /// Which channels a node takes: "" (follow what arrives), "mono" (summed), "monoLeft" or
    /// "monoRight".
    void SetNodeChannelMode(const std::string& nodeId, const std::string& channelMode);
    /// Loads a library resource (a NAM model, an IR) into one of the node's slots. An empty
    /// `resourceId` clears the slot. `resourceIndex` -1 is the node's only (first) slot.
    void SetNodeResource(const std::string& nodeId, const std::string& resourceType, const std::string& resourceId,
                         int resourceIndex = -1);
    /// Asks the engine to show a file dialog and load the chosen file into the node's slot,
    /// adding it to the library.
    void BrowseNodeResource(const std::string& nodeId, const std::string& resourceType, int resourceIndex = -1);
    /// Loads a file into a node's slot by path, as the resource browser's Folder tab previews
    /// one, without adding it to the library.
    void SetNodeResourceFile(const std::string& nodeId, const std::string& resourceType, const std::string& filePath,
                             int resourceIndex = -1);

    // ── The resource library ────────────────────────────────────────────────
    /// Stores downloaded bytes in the library (importRemoteResource: provider, resourceType,
    /// resourceId, name, category, subfolder, fileName, metadata, data as base64). Returns the
    /// request id the answer comes back under (ClientState::resourceImports).
    std::string ImportRemoteResource(nlohmann::json payload);
    /// Adds a file where it is to the library (saveLocalLibraryResource), so it is referenced in
    /// place rather than copied; with a node, loads it into that node's slot too.
    void SaveLocalLibraryResource(const std::string& resourceType, const std::string& filePath, const std::string& name,
                                  const nlohmann::json& metadata, const std::string& nodeId = {},
                                  int resourceIndex = -1);
    /// Stars or unstars a library resource (the shared resources.favorites setting).
    void SetResourceFavorite(const std::string& resourceId, bool favorite);
    /// Renames, recategorises or retags a library resource (updateLibraryResource).
    void UpdateLibraryResource(const std::string& resourceType, const std::string& resourceId, const std::string& name,
                               const std::string& category, const std::vector<std::string>& tags);
    /// Deletes one; the engine refuses one in use (resourceDeleteFailed), else resourceRemoved.
    void DeleteLibraryResource(const std::string& resourceType, const std::string& resourceId);
    /// Asks what uses a resource; answered into ClientState::resourceUsage.
    void QueryResourceUsage(const std::string& resourceType, const std::string& resourceId);
    /// Writes the library as one archive through a save dialog (exportLibraryArchive): the
    /// user's presets, every blend, and the models and IRs they use, or with `allResources`
    /// the whole library. The answer comes back as a notification.
    void ExportLibraryArchive(bool allResources);
    /// Plays downloaded bytes (base64) on a node's slot without importing them, until
    /// CancelPreviewResource or a selection takes the slot.
    void PreviewRemoteResource(const std::string& resourceType, const std::string& tempResourceId,
                               const std::string& nodeId, int resourceIndex, bool isZip, const std::string& base64);
    void CancelPreviewResource(const std::string& nodeId, int resourceIndex, bool restoreOriginal = true);
    /// Lists a folder's model and IR files (resourceFolderListing, then resourceFolderMetadata).
    void ListResourceFolder(const std::string& path);
    /// Asks the engine for a folder dialog; the folder picked comes back as resourceFolderPicked.
    void BrowseResourceFolder();
    /// A factory preset is sent as parameter changes; a saved one is applied by the engine,
    /// which restores its resources and config as well.
    void ApplyEffectPreset(const std::string& nodeId, const EffectPresetInfo& preset);
    /// Saves a node's current settings as a user preset for its effect type.
    void SaveEffectPreset(const std::string& nodeId, const std::string& name);
    /// Deletes one of the user's own presets of an effect type.
    void DeleteEffectPreset(const std::string& effectType, const std::string& presetId);

    // ── Global chain and output ─────────────────────────────────────────────
    void SetGlobalChainParam(const std::string& path, double value);
    void SetGlobalChainToggle(const std::string& path, bool enabled);
    void SetOutputMuted(bool muted);
    /// inputChannel: which input mono takes, 0 input 1, 1 input 2, 2 both summed. dualMono: with
    /// mono off, run a stereo input as two mono chains.
    void SetInputMode(bool monoMode, int inputChannel, bool dualMono = false);

    // ── Tuner, metronome, demo audio ────────────────────────────────────────
    void SetTunerActive(bool active);
    void SetTunerLiveMode(bool liveMode);
    void SetTunerReference(double frequency);
    /// The Scale Guide's LISTEN: tuner readings without turning the tuner on, with a chroma vector
    /// each when `chroma` is set.
    void SetTunerListening(bool listening, bool chroma);
    void SetMetronomeEnabled(bool enabled);
    void SetMetronomeBpm(double bpm);
    void SetMetronomeVolume(double volumeDb);
    void SetMetronomeClickType(const std::string& clickType);
    /// With `grouping` ("2+2+3") an odd meter's clusters; the engine re-seeds the accents.
    void SetMetronomeTimeSignature(int numerator, int denominator, const std::string& grouping = {});
    void SetMetronomePan(double pan);
    void SetMetronomeBeatPattern(const std::string& pattern);
    void SetMetronomeSubdivision(const std::string& subdivision);
    /// Sounds a Scale Guide view request's chosen position through the preset, or with `bar` that
    /// one chord of a progression's arpeggios. `volume`, here and below, is the guitar's level, 0 to 1.
    void PlayScaleGuide(const nlohmann::json& request, double volume = 1.0, int bar = -1);
    /// A short example of the request's scale, one octave up and back.
    void PlayScaleExample(const nlohmann::json& request, double volume = 1.0);
    /// A chord shape strummed: one fret per string, -1 for a string not played.
    void PlayScaleGuideChord(const nlohmann::json& request, const std::vector<int>& frets, double volume = 1.0);
    /// One note, as a click on the neck.
    void PlayScaleGuideNote(int midi, double volume = 1.0);
    void StopScaleGuide();
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
    /// Merges `patch` into the shared UI settings, at the top level, for what every UI reads
    /// (the web UI's own keys, such as the effect chooser's fxFavorites). An array replaces
    /// the stored one whole.
    void PatchUiSettings(const nlohmann::json& patch);
    void AudioDevice(const std::string& action, nlohmann::json payload = nlohmann::json::object());
    void OpenUrl(const std::string& url);

    // ── Tone sharing ────────────────────────────────────────────────────────────
    /// Installs downloaded tone sharing archives as one entry of the installedPacks setting.
    /// `entry` is {id, title, source, packId?}; each archive is {title, data: base64 zip}; a
    /// pack's presets go in a folder named `folder`. Answered by presetArchivesInstalled or
    /// presetArchivesInstallFailed.
    void InstallPresetArchives(const nlohmann::json& entry, const std::string& folder, nlohmann::json archives);
    /// Imports a preset archive file, or one the user picks when `path` is empty, as a zip import
    /// under `requestId` (importPresetArchiveFile). The answer is a ToneInstall under that id.
    void ImportPresetArchiveFile(const std::string& requestId, const std::string& path = {});
    /// Writes presets as a preset archive through a save dialog (exportPresetArchive): one id
    /// gives a .soundshed.preset named `name`; `collection` (a folder's export) always gives a
    /// .soundshed.presets, with the folders `folderId` holds them in.
    void ExportPresetArchive(const std::vector<std::string>& presetIds, const std::string& name,
                             const std::string& folderId = {}, bool collection = false);
    /// Takes an install out again: its presets, folder, and the models only it used.
    void DeleteInstalledPresetArchive(const std::string& entryId);

    // ── MIDI and automation (answered by "automation": Topic::Automation) ───────
    void GetAutomation();
    /// Creates or edits a slot. Only the fields given change: `label` and `address` (a custom
    /// slot's target; "" for none yet), and `presetId` for a new per-preset slot.
    void SetAutomationSlot(const std::string& slotId, const std::optional<std::string>& label,
                           const std::optional<std::string>& address, const std::string& presetId = {});
    /// Removes a slot's MIDI mapping, or its key mappings; the slot itself stays.
    void ClearAutomationMidiMap(const std::string& slotId);
    void ClearAutomationKeyMap(const std::string& slotId);
    void RemoveAutomationSlot(const std::string& slotId);
    /// Fires a slot from the UI (the panel's Test button), 0..1.
    void SetAutomationValue(const std::string& slotId, double value);
    /// Listens for the next MIDI event on a slot, replacing any learn already armed.
    void ArmMidiLearn(const std::string& slotId);
    void CancelMidiLearn();
    /// The engine forwards incoming MIDI as midiLog while enabled; a UI enables it only while
    /// its log is on screen.
    void SetMidiLogEnabled(bool enabled);
    /// The engine maps the next key the UI passes (AutomationKey) to `slotId`; "" stops waiting.
    void ArmKeyLearn(const std::string& slotId);
    /// A key the UI did not use itself, named as a DOM KeyboardEvent.key names it: the engine
    /// fires the slot it is mapped to, or completes a key learn. A UI passes no key typed into a
    /// text field.
    void AutomationKey(const std::string& key, bool repeat, bool ctrl = false, bool alt = false, bool meta = false);

    // ── Riffs (Topic::Riffs) ─────────────────────────────────────────────────────────
    void GetRiffLibrary();
    /// Counts in, then records from the first note (armRiffCapture), up to 64 bars as the web UI
    /// asks, so the live waveform's scale matches it.
    void ArmRiffCapture(double tempoBpm, int timeSigNum, int timeSigDen, int countInBars, bool click);
    /// Ends the take (keeping what was played), or with `cancel` drops it.
    void StopRiffCapture(bool cancel);
    /// Crops the pending take to [start, end] of its length (0..1).
    void TrimCapturedRiff(double startRatio, double endRatio);
    void PreviewCapturedRiff(double startRatio, double endRatio, bool repeat);
    /// Moves a playing preview's region; a UI calls it at most every 80 ms while dragging.
    void SetRiffPreviewRegion(double startRatio, double endRatio, bool repeat);

    struct RiffSave
    {
        std::string riffId; // set to add the take to that riff, else a new riff
        std::string title;
        std::vector<std::string> categories;
        std::vector<std::string> tags;
        std::string notes;
        bool favorite = false;
        double tempoBpm = 0.0;
        int timeSigNum = 4;
        int timeSigDen = 4;
        int bars = 0;
        std::string presetId;
    };

    /// Saves the pending take (riffSaved, then riffLibraryState).
    void SaveRiffTake(const RiffSave& save);
    void DeleteRiff(const std::string& riffId);
    void SetRiffFavorite(const std::string& riffId, bool favorite);
    void MarkRiffUsed(const std::string& riffId, bool used, const std::string& songTitle = {});
    /// Plays a library take, looping, with the guidance click (the riff list's Preview).
    void PreviewRiffTake(const std::string& takeId);
    /// Makes a library take the pending one again, under a new take id (riffCaptureStopped, editLoad).
    void LoadRiffTakeForEdit(const std::string& takeId);

    // ── Looper (Topic::Looper) ────────────────────────────────────────────────────────
    /// "recPlayDub", "stop", "undo", "redo" or "clear".
    void LooperCommand(const std::string& command);
    /// Any of setLooper's fields: enabled, maxSeconds, start, length, bars, countInBars,
    /// clickWhileRecording, afterRecord, alignOffsetMs.
    void SetLooper(const nlohmann::json& patch);
    /// Any of setLooperLayer's fields: level, balance, muted, solo, title.
    void SetLooperLayer(const std::string& layerId, const nlohmann::json& patch);
    void RemoveLooperLayer(const std::string& layerId);
    /// Re-amps the layers (empty: all of them) through the current rig, or puts their tone back.
    void RetoneLooperLayers(const std::vector<std::string>& layerIds, bool revert = false);
    /// "riff" (it comes back as a take to save) or "wetWav" (through a save dialog).
    void ExportLooperLayer(const std::string& layerId, const std::string& as);
    void GetLooperState();
    void GetLooperProjects();
    void SaveLooperProject(const std::string& title);
    void LoadLooperProject(const std::string& projectId);
    void DeleteLooperProject(const std::string& projectId);
    void RecoverLooperSession();
    void DismissLooperRecovery();
    void AddRiffAsLooperLayer(const std::string& takeId);

    // ── Practice Tool (Topic::PracticeTool) ───────────────────────────────────────────
    /// The engine's file dialog for a backing track; its answer is practiceToolFileLoaded.
    void BrowsePracticeToolFile();
    void LoadPracticeToolFile(const std::string& path);
    /// "play", "pause" or "stop".
    void SetPracticeToolTransport(const std::string& action);
    void SeekPracticeToolFile(double seconds);
    void SetPracticeToolSpeed(double ratio);
    void SetPracticeToolPitch(double semitones);
    void SetPracticeToolGain(double gain);
    void SetPracticeToolBalance(double balance);
    /// The active loop's bounds; `restart` also turns looping on and jumps to its start, as one
    /// change engine-side (what selecting a loop wants).
    void SetPracticeToolLoopRegion(double startSec, double endSec, bool restart = false);
    /// No loop region: the whole track.
    void ClearPracticeToolLoopRegion();
    void SetPracticeToolLooping(bool enabled);
    /// The backing-track EQ: any of ParametricEQEffect's parameters, and/or the switch.
    void SetPracticeToolEq(const std::optional<bool>& enabled, const nlohmann::json& params = nlohmann::json::object());

    // ── Input levelling (Topic::InputLevelling) ─────────────────────────────────────────
    /// Answered by "inputLevelling" (the profiles) and "inputLevellingTraining".
    void GetInputLevelling();
    /// "" for no levelling.
    void SelectInputLevellingProfile(const std::string& id);
    /// Bypasses the active profile and follows the raw input's peak until stopped or saved.
    void StartInputLevellingTraining();
    void StopInputLevellingTraining();
    /// Forgets the captured peak and clip, to capture again.
    void ResetInputLevellingTraining();
    /// Saves the peak captured since training started as a new, active profile; ends training.
    void SaveInputLevellingProfile(const std::string& name, const std::string& description);
    void RenameInputLevellingProfile(const std::string& id, const std::string& name, const std::string& description);
    void DeleteInputLevellingProfile(const std::string& id);

  private:
    UiClient& mClient;
};
} // namespace guitarfx::uiclient
