#pragma once

/**
 * PluginController — Framework-agnostic orchestration layer.
 *
 * Owns the DSP engine (MultiPresetMixer), resource library, preset storage,
 * composite library, and all shared plugin state. Communicates with the
 * framework-specific host through the IPluginHost interface.
 *
 * The framework adapter creates one PluginController instance, implements
 * IPluginHost, and delegates audio processing and UI message handling to
 * the controller.
 */

#include "IPluginHost.h"
#include "MessageHandlerRegistry.h"
#include "automation/AutomationSlotTable.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/CompositeEffectProcessor.h"
#include "dsp/effects/NAMOversampling.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#include "models/ModelHasher.h"
#include "presets/PresetTypes.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypesJson.h"
#include "resources/CustomEffectLibrary.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "storage/StorageMigration.h"
#include "util/FileSystem.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace guitarfx
{
class ControlSurfaceQueue;
class ControllerDisplayFeed;
class DemoPreviewService;
class MetronomeService;
class SignalTestService;
class ResourceFolderScanner;
class TelemetryPublisher;
class TunerService;
class PracticeToolService;
class HostStateRelay;

/// How ApplySettingsToRuntime() treats settings a plugin instance owns rather than shares.
enum class SettingsApplyMode
{
    /// Derive everything from mAppSettings. Startup, and restoring host state.
    kApplyAll,
    /// Re-assert this instance's own NAM quality and editor layout over what was just
    /// reloaded from the shared store. Degrades to kApplyAll in the standalone app,
    /// which owns those keys itself.
    kPreserveInstanceOwned
};

/**
 * Shared plugin controller — all business logic lives here.
 *
 * Thread safety model:
 *   - Audio thread calls ProcessAudio() under a try-lock on mDSPMutex.
 *   - UI thread calls message handlers through MessageDispatcher.
 *   - Host's RunOnMainThread() is used for deferred operations.
 */
class PluginController
{
  public:
    explicit PluginController(IPluginHost& host);
    ~PluginController();

    PluginController(const PluginController&) = delete;
    PluginController& operator=(const PluginController&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────
    /// Called once at startup after the host is fully initialized.
    void Initialize();

    /// Prepare DSP for playback (called from host's prepareToPlay / OnReset).
    void Prepare(double sampleRate, int blockSize);

    /// Reset DSP state.
    void Reset();

    // ── Audio processing ───────────────────────────────────────────
    /**
     * Process a block of audio. Called from the audio thread.
     * Buffers are float, stereo (inputs[0]=L, inputs[1]=R).
     * Returns false if the lock could not be acquired (caller should output silence).
     */
    bool ProcessAudio(float** inputs, float** outputs, int numSamples);

    // ── State serialization (for DAW save/restore) ─────────────────
    /// Safe on any thread. The blob is only ever built on the message thread; a call from
    /// anywhere else is handed to it, and answered with the last blob built there if it
    /// cannot start in time (see HostStateRelay). On the message thread, a restore or program
    /// change still queued from another thread is applied first.
    [[nodiscard]] std::string SerializeState() const;
    /// Safe on any thread. The restore only ever runs on the message thread; a call from
    /// anywhere else is handed to it and waited for. Returns false when the message thread did
    /// not start on it in time: the restore then stays queued, saves are answered with `json`
    /// until it lands, and the host hears IPluginHost::NotifyDeferredStateRestored() once it has.
    bool DeserializeState(const std::string& json);

    // ── UI message handling ────────────────────────────────────────
    /// Handle an incoming JSON message from the WebView UI.
    void HandleUIMessage(const std::string& jsonMessage);

    /// Called periodically from the host's idle/timer callback.
    void OnIdle();

    /// Message thread: applies the setlist steps, bank changes and scene switches that MIDI and
    /// DAW automation parked (see ControlSurfaceQueue), after anything the host queued before
    /// them. OnIdle does this too, but only an open editor drives OnIdle, so the plugin also
    /// calls this off a timer of its own. Returns at once when nothing is parked.
    void DrainControlSurfaceRequests();

    /// Called when the WebView content has finished loading.
    void OnWebContentLoaded();

    // ── Accessors ──────────────────────────────────────────────────
    [[nodiscard]] MultiPresetMixer& GetMixer()
    {
        return mPresetMixer;
    }

    [[nodiscard]] const MultiPresetMixer& GetMixer() const
    {
        return mPresetMixer;
    }

    [[nodiscard]] ResourceLibrary& GetResourceLibrary()
    {
        return mResourceLibrary;
    }

    [[nodiscard]] const std::optional<Preset>& GetActivePreset() const
    {
        return mActivePreset;
    }

    [[nodiscard]] const nlohmann::json& GetAppSettings() const
    {
        return mAppSettings;
    }

    // ── Editor window size ─────────────────────────────────────────
    /// The size, in the wrapper's own logical units, that this instance's editor window
    /// was last left at. Zero in either field means "never sized".
    struct EditorWindowSize
    {
        int width = 0;
        int height = 0;

        [[nodiscard]] bool IsValid() const
        {
            return width > 0 && height > 0;
        }
    };

    /**
     * Editor size for *this* instance, persisted in host state so a DAW hands it back
     * when the project — or just the editor window — is reopened.
     *
     * Deliberately not the same thing as uiSettings["bounds"], which the web UI captures
     * from window.outerWidth/screenX. Those are the WebView's own CSS pixels, which stop
     * agreeing with the wrapper's logical units as soon as DPI scaling is in play (see
     * PluginEditor::applyHostScaleWorkaround), and a WebView cannot resize the native
     * window it sits in anyway. The editor reports its real bounds here instead.
     *
     * Position is not stored: the DAW owns where a plugin window sits on screen, and no
     * plugin wrapper API lets us ask for a placement.
     */
    [[nodiscard]] EditorWindowSize GetEditorWindowSize() const
    {
        return mEditorWindowSize;
    }

    /// Record the editor's current size. Non-positive or absurd values are ignored, so a
    /// transient zero-sized layout pass cannot wipe the remembered size.
    void SetEditorWindowSize(int width, int height);

    /**
     * Tell the controller whether the host is rendering offline (bounce/freeze/export)
     * rather than in real time. Called from the host adapter's setNonRealtime().
     *
     * Offline there is no CPU budget to protect, so NAM runs at full quality regardless
     * of the user's real-time tier: slimmable size goes to maximum and oversampling is
     * lifted to at least 2x. The user's stored settings are not modified — only what the
     * DSP is currently running at — so switching back to real time restores their tier.
     */
    void SetOfflineRendering(bool offline);

    [[nodiscard]] bool IsOfflineRendering() const
    {
        return mOfflineRendering;
    }

    /// NAM quality tier owned by this plugin instance.
    struct NamQualityConfig
    {
        double slimmableSize = kNamSlimmableSizeDefault;
        int oversamplingIndex = kNamOversamplingIndexDefault;
        int antiAliasPhaseIndex = kNamAntiAliasPhaseIndexDefault;
    };

    /// Minimum oversampling index used while rendering offline (index 1 == 2x).
    static constexpr int kOfflineMinimumOversamplingIndex = 1;

    /**
     * The offline-render quality policy: full slimmable size and at least 2x
     * oversampling. Both are floors, so a user already above them keeps their choice,
     * and the anti-alias filter is left alone — the host compensates its latency either
     * way, and changing it would alter the rendered phase response.
     */
    [[nodiscard]] static NamQualityConfig ApplyOfflineRenderBoost(NamQualityConfig quality);

    /// The tier the DSP is currently running at: the user's settings in real time, or
    /// the offline-render boost while bouncing.
    [[nodiscard]] NamQualityConfig EffectiveNamQuality() const;

    /// True for the NAM quality keys (slimmable size, oversampling, anti-alias phase).
    [[nodiscard]] static bool IsNamQualitySettingKey(const std::string& key);

    /// True for the UI layout keys (zoom, window bounds, signal-path split) and their
    /// legacy flattened aliases.
    [[nodiscard]] static bool IsUiLayoutSettingKey(const std::string& key);

    /// True when `key` belongs to this instance rather than the shared app.json — that is,
    /// a NAM quality key while hosted as a plugin. Instance-owned keys are persisted in
    /// host state, are never written to disk, and survive a shared-settings reload.
    [[nodiscard]] bool IsInstanceOwnedSettingKey(const std::string& key) const;

    [[nodiscard]] IPluginHost& GetHost()
    {
        return mHost;
    }

    // ── Automation (public API for host adapters) ───────────────────
    /// Queue a MIDI event from the audio thread. Non-blocking and allocation-free;
    /// the event is applied later via ProcessQueuedMidi().
    void EnqueueMidi(const MidiEvent& ev);
    /// Drain and apply queued MIDI events under the DSP lock. Audio thread only;
    /// uses try_lock so it never stalls the audio thread (events retried next block).
    void ProcessQueuedMidi();
    /// Enable/disable forwarding of raw MIDI events to the UI diagnostics log.
    void SetMidiLogEnabled(bool enabled);
    /// Audio thread: copies the preset-name SysEx waiting for the MIDI controller's display
    /// into `out` and returns its length, or 0 when there is none. Never blocks or allocates;
    /// the adapter adds it to the block's MIDI output. See ControllerDisplayFeed.
    [[nodiscard]] std::size_t TakeControllerDisplaySysEx(std::span<std::uint8_t> out);
    /// Any thread, never with mDSPMutex held: the host's program change and the UI's. From a
    /// host's own thread (an AU factory preset, an LV2 program preset) it is handed to the
    /// message thread and waited for, as a restore is (see DeserializeState), and stays queued
    /// if that does not start in time. Automation's setlist steps are parked for the message
    /// thread instead (see DrainControlSurfaceRequests).
    void ApplySetlistPresetByIndex(int index);
    void SetlistBankUp(int steps);
    void SetlistBankDown(int steps);
    void SelectSetlistBank(int bankNumber);
    /// Select a scene of the active preset by 0-based index. Safe to call from the
    /// audio thread: when the DSP lock is already held it is parked for the message
    /// thread, which applies it whether or not the editor is open (see
    /// DrainControlSurfaceRequests).
    void SelectSceneByIndex(int index);
    /// 0-based index of the active preset's current scene, or -1 when unavailable.
    [[nodiscard]] int GetActiveSceneIndex() const;
    [[nodiscard]] int GetSetlistLength() const;
    [[nodiscard]] int GetSetlistBankBase() const;
    [[nodiscard]] int GetSetlistBankNumber() const;

    /// Index of the currently selected setlist slot (DAW "program"). Any thread.
    [[nodiscard]] int GetSetlistCursorIndex() const
    {
        return mSetlistCursorIndex.load(std::memory_order_relaxed);
    }

    /// Preset ID at the given active-setlist slot, or empty if out of range.
    [[nodiscard]] std::string GetSetlistSlotPresetId(int index) const;

    [[nodiscard]] std::vector<std::string> GetAutomationSlotIds() const
    {
        return mAutomationSlots.GetSlotIds();
    }

    [[nodiscard]] AutomationSlotTable& GetAutomationSlots()
    {
        return mAutomationSlots;
    }

    [[nodiscard]] const AutomationSlotTable& GetAutomationSlots() const
    {
        return mAutomationSlots;
    }

    /// Apply a normalized 0..1 value from the DAW host to a slot. Takes DSP lock.
    void ApplyAutomationFromDAW(const std::string& slotId, float normalized);

    /// Message thread, once, as the host adapter registers its parameters: the slot behind each
    /// DAW parameter, in parameter order (see AutomationSlotTable::BindDawParameters).
    void BindDawParameters(const std::vector<std::string>& slotIds);

    /// Any thread, without a lock: the normalized 0..1 value of DAW parameter `parameterIndex`,
    /// for the host to read back. Hosts read parameters on the audio thread as well, and while
    /// the message thread is replacing the slots.
    [[nodiscard]] float GetDawParameterValue(int parameterIndex) const
    {
        return mAutomationSlots.GetDawParameterValue(parameterIndex);
    }

    // ── Parameter bridging ─────────────────────────────────────────
    // Global FX have no flat parameter mirror: GlobalSignalChainConfig on the mixer is
    // the single source of truth, read back via GetMixer().GetGlobalChainConfig().
    // DAW-visible parameters are the automation slots above, registered by the host
    // adapter — they were never bound to the retired ParameterId enum.

    // ── Multi-preset mixer controls ────────────────────────────────
    bool AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name);
    bool AddActivePresetById(const std::string& presetId);
    /// Loads presetId from storage and makes it *the* active preset, swapping the whole mixer
    /// down to that single instance via ApplyPreset()'s gapless crossfade. This is the
    /// "change preset" verb — as opposed to AddActivePresetById(), which is the Multi-Rig
    /// "add another rig to the mix" verb. Also notifies the UI with "presetLoaded".
    bool ApplyActivePresetById(const std::string& presetId);
    void RemoveActivePreset(const std::string& presetId);
    void SetActivePresetMix(const std::string& presetId, double value);
    void SetActivePresetPan(const std::string& presetId, double pan);
    void SetActivePresetMute(const std::string& presetId, bool mute);
    void SetActivePresetSolo(const std::string& presetId, bool solo);
    void SetMasterGain(double value);
    void SetMixGainDb(double dB);
    void SetLimiterEnabled(bool enabled);
    /// Switches the editing focus (mActivePreset) to an already-active mixer slot without
    /// touching the running DSP instances, so signal-chain edits target the correct preset.
    void FocusMixerPreset(const std::string& presetId);
    /// Rebuilds one already-active mixer slot in place (e.g. a scene switch on a preset
    /// that is one of several active mixer presets) without disturbing any other slot.
    /// Returns false if presetId is not currently an active mixer slot.
    bool ReplaceActiveMixerPresetInPlace(const Preset& preset, const std::string& presetId, const std::string& name);

    // ── Signal path test ───────────────────────────────────────────
    /// Injects a test tone and reports what reached the output. Returns false
    /// if the host has no sample rate yet. See controller/SignalTestService.h.
    bool StartSignalPathTest(double frequencyHz = 440.0, double durationSeconds = 1.0);

  private:
    friend class MessageDispatcher;

    // ── Internal handler methods ───────────────────────────────────
    // These are called by MessageDispatcher and operate on controller state.
    void HandleStateRequest();
    void HandleCaptureDebugSnapshotRequest(const nlohmann::json& payload);
    void HandleDebugReportUiStateRequest(const nlohmann::json& payload);
    void HandlePresetLoadRequest(const nlohmann::json& payload);
    void HandleSetParameterRequest(const nlohmann::json& payload);
    void HandleSetGlobalChainParamRequest(const nlohmann::json& payload);
    void HandleSignalTestRequest(const nlohmann::json& payload);
    void HandleLoadModelRequest(const nlohmann::json& payload);
    void HandleLoadIRRequest(const nlohmann::json& payload);
    void HandleSavePresetRequest(const nlohmann::json& payload);
    void HandleDeletePresetRequest(const nlohmann::json& payload);
    void HandleStartPresetArchiveSessionRequest(const nlohmann::json& payload);
    void HandleEndPresetArchiveSessionRequest();
    void HandleGetPresetByIdRequest(const nlohmann::json& payload);
    void HandleBrowseModelRequest();
    void HandleBrowseIRRequest();
    void HandleAudioDeviceRequest(const nlohmann::json& payload);
    void HandleSetInputModeRequest(const nlohmann::json& payload);
    void HandleSetAmpCabStateRequest(const nlohmann::json& payload);
    void HandleDeleteLayoutRequest(const nlohmann::json& payload);
    void HandleSetMetronomeRequest(const nlohmann::json& payload);
    void HandleUpdateSignalPathNodeParamRequest(const nlohmann::json& payload);
    void HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json& payload);
    void HandleUpdateSignalPathNodeConfigRequest(const nlohmann::json& payload);
    void HandleUpdateNodeResourceRequest(const nlohmann::json& payload);
    void HandleBrowseNodeResourceRequest(const nlohmann::json& payload);
    void HandleAddSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleSplitSignalPathEdgeRequest(const nlohmann::json& payload);
    void HandleCollapseSignalPathSplitRequest(const nlohmann::json& payload);
    void HandleReplaceSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleReorderSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleDeleteSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleImportRemoteResourceRequest(const nlohmann::json& payload);
    void HandleSaveLocalLibraryResourceRequest(const nlohmann::json& payload);
    void HandleDeleteLibraryResourceRequest(const nlohmann::json& payload);
    void HandleQueryResourceUsageRequest(const nlohmann::json& payload);
    void HandleUpdateLibraryResourceRequest(const nlohmann::json& payload);
    void HandleBrowseLibraryResourcePathRequest(const nlohmann::json& payload);
    void HandleBrowseResourceFolderRequest();
    void HandleDeleteImportedToneSharingPackRequest(const nlohmann::json& payload);
    void HandlePreviewRemoteResourceRequest(const nlohmann::json& payload);
    void HandleCancelPreviewResourceRequest(const nlohmann::json& payload);
    void HandleSaveBlendDefinitionRequest(const nlohmann::json& payload);
    void HandleDeleteBlendDefinitionRequest(const nlohmann::json& payload);
    void HandleSaveCustomEffectEntryRequest(const nlohmann::json& payload);
    void HandleSaveCurrentCustomEffectRequest(const nlohmann::json& payload);
    void HandleImportGeneratedCustomEffectRequest(const nlohmann::json& payload);
    void HandleExportGeneratedCustomEffectBundleRequest(const nlohmann::json& payload);
    void HandleDeleteCustomEffectEntryRequest(const nlohmann::json& payload);
    void HandleRequestResourceDataRequest(const nlohmann::json& payload);
    void HandleSaveBlendArchiveRequest(const nlohmann::json& payload);
    void HandleSavePresetArchiveRequest(const nlohmann::json& payload);
    void HandleSaveLibraryArchiveRequest(const nlohmann::json& payload);
    void HandleSaveEffectLayoutRequest(const nlohmann::json& payload);
    void HandleExportEffectLayoutRequest(const nlohmann::json& payload);
    void HandleBrowseLayoutImageRequest(const nlohmann::json& payload);
    void HandleSaveLayoutImageRequest(const nlohmann::json& payload);
    void HandleRequestLayoutImagesRequest();
    void HandleCleanupResourceLibraryRequest(const nlohmann::json& payload);
    void HandleSaveCompositeDefinitionRequest(const nlohmann::json& payload);
    void HandleDeleteCompositeDefinitionRequest(const nlohmann::json& payload);
    void HandleEnterCompositeEditModeRequest(const nlohmann::json& payload);
    void HandleExitCompositeEditModeRequest(const nlohmann::json& payload);
    void HandlePreviewDemoRequest(const nlohmann::json& payload);
    void HandleRenderDemoAudioRequest(const nlohmann::json& payload);
    void HandleStopDemoRequest();
    void HandleGetRiffLibraryRequest();
    void HandleSetRiffLibraryPathRequest(const nlohmann::json& payload);
    void HandleArmRiffCaptureRequest(const nlohmann::json& payload);
    void HandleStartRiffCaptureRequest(const nlohmann::json& payload);
    void HandleStopRiffCaptureRequest(const nlohmann::json& payload);
    void HandleImportRiffWavRequest(const nlohmann::json& payload);
    void HandleTrimCapturedRiffRequest(const nlohmann::json& payload);
    void HandleLoadRiffTakeForEditRequest(const nlohmann::json& payload);
    void HandleSaveRiffTakeRequest(const nlohmann::json& payload);
    void HandleDeleteRiffRequest(const nlohmann::json& payload);
    void HandleSetRiffFavoriteRequest(const nlohmann::json& payload);
    void HandleMarkRiffUsedRequest(const nlohmann::json& payload);
    void HandlePreviewRiffTakeRequest(const nlohmann::json& payload);
    void HandlePreviewCapturedRiffRequest(const nlohmann::json& payload);
    void HandleSetRiffPreviewRegionRequest(const nlohmann::json& payload);

    // Composite presets (Multi-Rig)
    void HandleSaveCompositePresetRequest(const nlohmann::json& payload);
    void HandleLoadCompositePresetRequest(const nlohmann::json& payload);
    void HandleGetCompositePresetListRequest();
    void HandleRemoveCompositePresetRequest(const nlohmann::json& payload);

    // Signal diagnostics / performance
    void HandleGetSignalDiagnosticsRequest();
    void HandleGetPerformanceStatsRequest();
    void HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload);
    void HandleSetSpectrumWatchRequest(const nlohmann::json& payload);
    void HandleGetEffectCatalogRequest();
    void HandleGetPresetListRequest();
    void HandleGetPresetFoldersRequest();
    void HandleSetPresetFoldersRequest(const nlohmann::json& payload);
    void HandleGetPresetFavoritesRequest();
    void HandleSetPresetFavoritesRequest(const nlohmann::json& payload);
    void HandleGetPresetRatingsRequest();
    void HandleSetPresetRatingsRequest(const nlohmann::json& payload);
    void HandleGetSetlistsRequest();
    void HandleGetEffectPresetsRequest();
    void HandleSaveEffectPresetRequest(const nlohmann::json& payload);
    void HandleDeleteEffectPresetRequest(const nlohmann::json& payload);
    void BroadcastEffectPresets();
    void HandleSetSetlistsRequest(const nlohmann::json& payload);

    // Effect analysis (controller/PluginControllerEffectAnalysis.cpp): an effect's response
    // curve, exporting it as an IR, and matching the Simple Cabinet to a library IR.
    void HandleGetEffectResponseRequest(const nlohmann::json& payload);
    void HandleExportEffectAsIrRequest(const nlohmann::json& payload);
    void HandleMatchSimpleCabToIrRequest(const nlohmann::json& payload);

    // Automation & MIDI mapping (handler methods — called by MessageDispatcher)
    void HandleGetAutomationRequest();
    void HandleSetAutomationSlotRequest(const nlohmann::json& payload);
    void HandleRemoveAutomationSlotRequest(const nlohmann::json& payload);
    void HandleSetAutomationValueRequest(const nlohmann::json& payload);
    void HandleArmMidiLearnRequest(const nlohmann::json& payload);
    void HandleCancelMidiLearnRequest();
    /// Drops a deleted preset's per-preset MIDI mappings.
    void ForgetPresetAutomation(const std::string& presetId);
    /// Message thread, never with mDSPMutex held: replaces the automation slots with those
    /// automation.json `automation` describes, their values taken from `values` if given (the
    /// state a DAW saved) and 0 otherwise. Built off the DSP lock and swapped in under it.
    void ReplaceAutomationSlots(const nlohmann::json& automation, const nlohmann::json* values);
    /// Hands the automation table the active preset id when it has changed (idle tick).
    void SyncAutomationActivePreset();
    /// Re-resolves the slots' node.* addresses when effect types have come or gone since (idle
    /// tick). Built off the DSP lock and swapped in under it.
    void RefreshAutomationBindings();
    /// Hands the controller-display feed the active preset's name (idle tick).
    void SyncControllerDisplay();
    void HandleGetThemeRequest();
    void HandleSetThemeRequest(const nlohmann::json& payload);
    void HandleGetSharedSyncStateRequest();

    [[nodiscard]] std::optional<LibraryResource> SaveLocalLibraryResource(const nlohmann::json& payload,
                                                                          std::string& error, bool allowCreate = true);
    /// Tells the UI the outcome of a SaveLocalLibraryResource call: the refreshed state and
    /// `resourceImported`, or an error and `resourceImportFailed`. `requestId` is echoed when set.
    void AnnounceSavedLocalResource(const LibraryResource& saved, const std::string& requestId = {});
    void AnnounceLocalResourceSaveFailure(const std::string& error, const std::string& requestId = {});
    void HandleGetAppInfoRequest();
    void HandleGetGlobalChainRequest();

    // ── Internal helpers ───────────────────────────────────────────
    /// How much of the app state a broadcast carries.
    ///
    /// The full payload is ~510 KB on a real library — 90% of it the resource library,
    /// which also costs one filesystem stat per entry to build. None of that changes when
    /// the user switches preset, so preset/scene switches ask for `PresetOnly` (~8 KB) and
    /// skip the effect-catalog and composite-library resends too. Every section the UI
    /// reads is already guarded by a presence check, so omitting them is a no-op there.
    enum class StateScope
    {
        PresetOnly,
        Full,
    };
    void BroadcastState(StateScope scope = StateScope::Full);
    void ApplyPreset(const Preset& preset);
    /// Call with mDSPMutex held: the lookups walk instances the audio thread erases. Safe
    /// for a hosted plugin too, whose SetRuntimeConfigChangedCallback only stores the callback.
    void AttachRuntimeConfigCallbacks(const std::string& presetId, const Preset& preset);
    void HandleRuntimeNodeConfigChanged(const std::string& presetId, const std::string& nodeId, const std::string& key,
                                        const std::string& value);
    void TryRemapHostedPluginResources(Preset& preset) const;
    void TryRemapHostedPluginResourcesInGraph(SignalGraph& graph) const;
    void PersistHostedPluginResourceMetadata(const GraphNode& node, const std::string& key, const std::string& value);
    bool ReportHostedPluginResourceLoadFailure(const std::string& nodeId, const ResourceRef& ref,
                                               int resourceIndex = -1);
    void NotifyHostedPluginResourceLoadCompleted(const std::string& nodeId, const ResourceRef& ref,
                                                 int resourceIndex = -1);
    void DiscardFailedHostedPluginResourceSelection(const std::string& nodeId, const ResourceRef& ref,
                                                    int resourceIndex = -1);
    /// Reads the chain's latency under mDSPMutex and tells the host after releasing it, so
    /// never call this with the lock held. A host can answer a latency change by asking for
    /// state straight away (SerializeState), which takes the lock again. Held back while host
    /// calls are held (see RunHostChangeOnMessageThread).
    void UpdateHostLatency();
    int mLastReportedLatency = -1; ///< Guards against redundant host latency notifications. mDSPMutex.
    /**
     * Runs `change` on the message thread for a host call made on another thread, through
     * HostStateRelay::ApplyOnMessageThread, and returns whether it ran before this returned.
     *
     * The host thread waits while it runs and may hold a lock the host takes to hear from the
     * plugin, so the message thread must not call into the host meanwhile: calls made during
     * `change` (latency, state changed) are held and made once that thread has been let go.
     * A restore (`isRestore`) that ran after its caller gave up also reports itself to the host
     * then, through IPluginHost::NotifyDeferredStateRestored().
     */
    bool RunHostChangeOnMessageThread(std::function<void()> change, bool isRestore,
                                      std::optional<std::string> pendingState);
    /// Message thread: makes the host calls held back during a relayed change.
    void ReleaseHeldHostCalls();
    /// Message thread only. See RunHostChangeOnMessageThread.
    bool mHostCallsHeld = false;
    bool mHeldLatencyReport = false;
    bool mHeldStateChange = false;
    /// The restore itself, on the message thread. See DeserializeState.
    void RestoreHostState(const std::string& json);
    /**
     * Tell the host that the state it would save has changed (marks the DAW project dirty).
     *
     * Silent while DeserializeState() is restoring: the host already holds that state, and
     * hearing it changed marks a project modified the moment it opens, or lands as a new step
     * in the middle of the host's own undo. Call this rather than mHost.NotifyStateChanged().
     */
    void NotifyHostStateChanged();
    /// Where BuildHostState() takes hosted plugin state from.
    enum class HostedPluginStateSource
    {
        Live,        ///< asked of each running plugin, as a save must
        WorkingCopy, ///< as last captured into the working copy; no plugin is asked anything
    };
    /// The blob SerializeState() hands the host. Message thread only.
    [[nodiscard]] std::string BuildHostState(HostedPluginStateSource source) const;
    /// Rebuilds, from the working copy, the blob the host is answered with when the message
    /// thread cannot build one (see HostStateRelay). Message thread only. A live capture would
    /// pass each hosted plugin's audio through while it runs, and the working copy already
    /// follows their changes. A build that throws leaves the previous blob in place.
    void RememberHostStateFromWorkingCopy() const;
    /// Fills each blend node's models in from the blend library. A saved preset carries only
    /// the node's blendId, so every chain build has to run this first.
    void ApplyBlendDefinitions(Preset& preset);
    /// Rebuilds every running mixer slot with a node playing `blendId`, so a saved edit to
    /// the blend is heard straight away.
    void RebuildSlotsUsingBlend(const std::string& blendId);
    /// RebuildSlotsUsingBlend() for each blend that differs between `previousLibrary` and the
    /// library now, including one that has gone.
    void RebuildSlotsForChangedBlends(const nlohmann::json& previousLibrary);
    /// The first preset (working copy, mixer slot, user or factory archive) with a node
    /// playing `blendId`, in any scene.
    [[nodiscard]] std::optional<std::string> FindFirstPresetUsingBlend(const std::string& blendId) const;
    /**
     * A node's config as its running processor in mixer slot `presetId` reports it, or the
     * slot graph's stored value when the node has no processor. Empty if there is no such slot.
     *
     * The slot lookup walks instances the audio thread erases as their fades finish, so it is
     * made under mDSPMutex. A hosted plugin is then asked after the lock is released: it
     * serialises its state under its own lock and can take a while, and holding the DSP lock
     * across that would silence every slot. Anything else answers under the lock. mDSPMutex
     * is not recursive, so never call this with it held.
     *
     * Message thread only: that is the one thread on which the hosted plugin cannot be
     * retired and destroyed between the lookup and the read.
     */
    [[nodiscard]] std::string ReadLiveNodeConfig(const std::string& presetId, const std::string& nodeId,
                                                 const std::string& key) const;
    /// The live mixer slots' ids, or their whole configs, copied under mDSPMutex so the
    /// caller can work through them without it. Never call these with the lock held.
    [[nodiscard]] std::vector<std::string> SnapshotActivePresetIds() const;
    [[nodiscard]] std::vector<MultiPresetMixer::InstanceConfig> SnapshotActivePresetConfigs() const;
    /**
     * Fill in hosted plugin state across `preset`'s graphs: live state for the graph the DSP
     * is running, stored state from the working copy for the other scenes.
     *
     * Pass `runningGraph` when `preset` did not come from the working copy, e.g. a UI payload
     * that may hold a different graph. Live state is then only taken for a node that still
     * points at the plugin running under its id.
     */
    void CaptureRuntimePluginStates(Preset& preset, const std::string& presetId,
                                    const SignalGraph* runningGraph = nullptr) const;
    /**
     * Fold live hosted plugin state back into the focused preset's working copy.
     *
     * Every graph edit tears down and rebuilds all processors, and a rebuilt hosted plugin
     * is restored from node.config — so anything the live plugin knows that node.config
     * does not is lost. Auto-capture usually keeps the two in step, but it depends on the
     * plugin telling us it changed, and plenty of plugins mutate non-parameter state
     * (internal preset browsers, sample/IR loaders) without ever notifying the host.
     *
     * Call this before any operation that rebuilds or replaces the working copy so the
     * rebuild starts from what is actually loaded rather than from the last notification.
     * Cheap: it only touches hosted plugin nodes, and no-ops when nothing changed.
     */
    void CaptureLiveHostedPluginStateIntoActivePreset();
    /**
     * Fold live hosted plugin state for a mixer slot that is not the editing focus into a
     * copy of its preset.
     *
     * Non-focused slots have no live Preset object — their working copy is the JSON in
     * mMixerPresetJsonCache — so the focused-preset paths cannot reach them.
     */
    void CaptureMixerSlotHostedPluginState(Preset& preset, const std::string& presetId) const;
    /// Apply one runtime config change to a non-focused mixer slot's cached working copy.
    void ApplyRuntimeNodeConfigToMixerCache(const std::string& presetId, const std::string& nodeId,
                                            const std::string& key, const std::string& value);
    /// Drop hosted plugin state that belongs to a plugin the node no longer hosts.
    /// Returns true when a stale chunk was removed.
    static bool ClearStaleHostedPluginState(GraphNode& node, const std::string& previousIdentity);
    /// Restores the hosted plugin state carried in standalone host state onto the preset
    /// that startup already loaded from the store. See the definition for why standalone
    /// takes this narrow path instead of the full DeserializeState restore.
    void RestoreStandaloneHostedPluginState(const std::string& json);
    /// The mixer slot id the focused preset runs under, when `presetId` names that preset
    /// (directly or through a factory-archive alias); nullopt for any other preset.
    [[nodiscard]] std::optional<std::string> ResolveActivePresetSlotId(const std::string& presetId) const;
    std::optional<Preset> TryLoadStoredPresetById(const std::string& presetId);
    /**
     * Push mAppSettings into the running DSP and UI state.
     *
     * The one definition of "apply the settings", shared by startup, host-state restore
     * and shared-settings reload — keeping three hand-maintained lists in step is what
     * previously let restored values be merged but never applied.
     *
     * The individual helpers below sanitise mAppSettings but never persist; the `bool`
     * returned here is true when sanitising changed something, so the caller can save
     * once for the whole batch (or decline to, as a host-state restore does).
     */
    [[nodiscard]] bool ApplySettingsToRuntime(SettingsApplyMode mode);

    void ApplyInputModeSettingsFromAppSettings();
    void ApplyGlobalFxSettingsFromAppSettings();
    void PersistGlobalFxSettingsToAppSettings();
    /// Return true when sanitising changed mAppSettings, and never persist — see above.
    /// Advisory: a caller that saves unconditionally anyway can ignore it.
    bool ApplyDspLevelTargetSettingsFromAppSettings();
    /// Sanitises the tail-spill length and writes back a missing or malformed entry, so the
    /// settings UI reads the same default the engine does. Nothing is pushed to the mixer
    /// here: the length is tempo-relative, so it is resolved per swap by
    /// UpdatePresetSwapTailBudget() against the tempo the player is on at that moment.
    bool ApplyPresetSwitchSettingsFromAppSettings();
    /// Tells the mixer how long the outgoing preset may ring after the swap it is about to
    /// be asked for: the stored bar count at the current tempo and meter.
    void UpdatePresetSwapTailBudget();
    bool ApplyNamQualitySettings();
    bool ApplyUserInputCalibrationSettingsFromAppSettings();
    void PushNamQualityToDsp();
    void RestoreInstanceOwnedSettings();
    void ApplyNamInterfaceCalibrationFromAppSettings();
    void ApplyUiSettingsFromAppSettings();
    [[nodiscard]] bool IsFactoryPresetArchiveLoadingEnabled() const;
    [[nodiscard]] bool IsPresetArchiveSessionActive() const;
    [[nodiscard]] std::filesystem::path GetEffectiveUserPresetDirectory() const;
    [[nodiscard]] std::filesystem::path GetEffectiveSettingsDirectory() const;
    void RefreshPresetLibraryViews();
    void ClearActivePresetMixerState();
    void StartPresetArchiveSession(const std::string& archiveFileName, const std::vector<std::uint8_t>& archiveBytes);
    void EndPresetArchiveSession(bool notifyUi = true);
    void SendPresetArchiveSessionStateToUI(const char* messageType = nullptr, const std::string& detail = {});
    void SendMessageToUI(const std::string& jsonMessage);
    void ReportErrorToUI(const std::string& message, const std::string& detail = {});
    void SendGlobalChainStateToUI();
    void SendCompositeLibraryToUI();
    void SendCustomEffectLibraryToUI();
    void SendEffectCatalogToUI();
    void SendPresetListToUI();
    void SendCompositePresetListToUI();
    void SendRiffLibraryStateToUI();

    // Composite edit helpers
    [[nodiscard]] bool IsCompositeEditMode() const;
    [[nodiscard]] SignalGraph* ResolveEditTarget();
    [[nodiscard]] std::string GetResolvedActiveSceneId() const;
    void SyncActivePresetSceneGraph();
    void BroadcastCompositeEditState();

    // Resource helpers
    bool UpdateResourceForNodeType(const std::string& nodeType, const std::string& resourceType,
                                   const std::filesystem::path& filePath, bool applyPreset = true);
    bool UpdateResourceForNodeId(const std::string& nodeId, const ResourceRef& ref, bool applyPreset = true);
    void RefreshWasmNodeDescriptor(GraphNode& node);
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResourceRef(const ResourceRef& ref) const;
    [[nodiscard]] std::optional<std::string> FindFirstPresetUsingResource(const std::string& resourceType,
                                                                          const std::string& resourceId) const;
    /// The name of the first blend that plays the model, for a resource no preset uses directly.
    [[nodiscard]] std::optional<std::string> FindFirstBlendUsingResource(const std::string& resourceType,
                                                                         const std::string& resourceId) const;
    void EnsureResourceUsageDiskIndex() const;
    void InvalidateResourceUsageIndex();
    void AppendUserLibraryResource(const LibraryResource& resource);
    void RemoveUserLibraryResource(const std::string& type, const std::string& id);
    void EnsureBasicGraph();
    bool ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& zipData, const std::string& resourceType,
                                     const std::filesystem::path& outputPath);

    // NAM level-state normalization
    void ResetNamNodeLevelState(const std::string& nodeId);
    /// Pushes mNamInterfaceCalibrationLevelDbu, or its absence, to one NAM node. Call with
    /// mDSPMutex held.
    void InjectNamInterfaceCalibration(const std::string& presetId, const std::string& nodeId);
    /// InjectNamInterfaceCalibration() for every calibratable NAM node of a slot just built
    /// from `preset`. Call with mDSPMutex held.
    void InjectNamInterfaceCalibrationIntoSlot(const std::string& presetId, const Preset& preset);
    void ClearNamCalibrationParams(GraphNode& node) const;

    // Settings persistence
    void SaveAppSettings() const;
    /// Record the current mAppSettings as already-published, so nothing in it is pending
    /// a write to the shared store. Instance-owned keys keep their prior baseline value.
    void AdoptAppSettingsAsBaseline() const;
    bool CleanupLegacyAppSettingsOnLoad();
    void LoadAppSettings();
    void LoadLastSessionState();
    [[nodiscard]] std::optional<Preset> LoadPresetById(const std::string& presetId) const;
    [[nodiscard]] std::optional<std::string> FindPresetIdByTitle(const std::string& presetTitle) const;
    bool TryLoadConfiguredDefaultPreset();
    void LoadResourceLibraries();
    void CleanupResourceLibraryCategoriesOnStartup();
    void LoadFactoryPresetArchives();
    /// The stored blends, then the factory-archive and preset-archive session blends.
    void LoadBlendLibrary();
    /// Re-adds the factory-archive and session blends to mBlendLibrary, replacing any earlier
    /// registration. A stored edit of a factory blend is kept instead of the archive's.
    void MergeTransientBlends();
    void SaveBlendLibrary() const;
    void LoadCustomEffectLibrary();
    void SaveCustomEffectLibrary() const;
    void LoadCompositeLibrary();
    void LoadLayoutLibrary();
    // Builds the (potentially heavy) base64-encoded layout image list on demand.
    // Kept out of the startup LoadLayoutLibrary payload so app load stays lightweight.
    [[nodiscard]] nlohmann::json BuildLayoutImages();
    void SaveLayoutToFile(const std::string& effectType, const nlohmann::json& layoutJson);
    [[nodiscard]] nlohmann::json LoadUiStorageJson(const std::string& filename, const nlohmann::json& fallback) const;
    void SaveUiStorageJson(const std::string& filename, const nlohmann::json& payload) const;
    /// Legacy UI-storage filename → document id ("setlists.json" → "setlists").
    [[nodiscard]] static std::string UiStorageDocumentId(const std::string& filename);

    // ── Document store ─────────────────────────────────────────────
    /**
     * The document store, opened (and migrated) on first use.
     *
     * Initialize() opens it eagerly so the migration log lands at startup, but
     * everything goes through here: a code path that touches storage before
     * Initialize() must not silently drop the write.
     */
    [[nodiscard]] storage::JsonStore& Store() const;
    [[nodiscard]] std::filesystem::path ResolveDocumentStorePath() const;
    /// Directory that resource file paths are stored relative to.
    [[nodiscard]] std::filesystem::path ResolveResourcesRoot() const;

    // User-preset access. These route to the store normally, and to the
    // throwaway sandbox directory while a preset-archive session is active —
    // session presets shadow the real library and must never enter it.
    [[nodiscard]] std::optional<Preset> LoadUserPreset(const std::string& presetId) const;
    [[nodiscard]] std::vector<Preset> LoadAllUserPresets() const;
    [[nodiscard]] bool UserPresetExists(const std::string& presetId) const;
    bool SaveUserPreset(const Preset& preset);
    bool DeleteUserPreset(const std::string& presetId);
    /// Opens the store and runs the one-time legacy import. A failure here is
    /// logged, not fatal: the store stays closed and every read returns empty.
    void OpenDocumentStore() const;
    void TouchSharedSyncState(const std::vector<std::string>& domains) const;
    void ReloadSharedSyncSourcesFromDisk();
    void PollSharedSyncState();
    [[nodiscard]] std::filesystem::path ResolveRiffLibraryPath() const;
    [[nodiscard]] nlohmann::json LoadRiffLibraryIndex() const;
    bool SaveRiffLibraryIndex(const nlohmann::json& payload) const;
    [[nodiscard]] std::string BuildRiffTakeId() const;
    [[nodiscard]] std::string BuildRiffId() const;
    [[nodiscard]] std::string BuildTimestampUtcIso() const;
    [[nodiscard]] std::optional<nlohmann::json> FindRiffTakeById(const std::string& takeId) const;
    void FinalizeRiffCapture(bool canceled);
    /// Audio thread, under the DSP lock. Only reached while a capture is armed or
    /// running — see ProcessAudioLocked. Defined in PluginControllerRiffs.cpp.
    void ProcessRiffCaptureBlock(float** inputs, int numSamples);

    /// Atomic: readers see the old file or all of the new one (util::WriteFileAtomic).
    /// Defined in PluginControllerStorage.cpp.
    bool WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const;

    void AppendSessionLog(const std::string& message) const;
    void ProcessAudioLocked(float** inputs, float** outputs, int numSamples);
    void ApplySetlistPresetByIndexDirect(int index);
    void SetlistBankChangeDirect(int delta);
    void SelectSetlistBankDirect(int bankNumber);
    void SelectSceneByIndexDirect(int index);

    // ── State ──────────────────────────────────────────────────────
    IPluginHost& mHost;

    // DSP engine
    MultiPresetMixer mPresetMixer;
    mutable std::mutex mDSPMutex;

    /**
     * Every app-owned document — settings, presets, resource metadata, blends,
     * layouts — lives here. Content files (.nam/.wav/.wasm) stay on disk and the
     * store holds only their paths and metadata.
     *
     * Mutable so const accessors can read: sqlite needs a non-const handle even
     * for a SELECT, and the store serializes itself internally.
     */
    mutable storage::JsonStore mStore;
    /// Guards the lazy open. Store() is reached from the message thread and from
    /// background workers (folder scans), so this has to be a real
    /// once-primitive rather than a bool — and a failed open must not be retried
    /// on every subsequent access.
    mutable std::once_flag mStoreOpenOnce;

    // Resources and libraries
    ResourceLibrary mResourceLibrary;
    nlohmann::json mBlendLibrary = nlohmann::json::array();
    CustomEffectLibrary mCustomEffectLibrary;
    /// The ids of the blends in mBlendLibrary that are served from mFactoryArchiveBlends and
    /// mPresetArchiveSessionBlends. Neither kind is stored; see MergeTransientBlends().
    std::unordered_set<std::string> mFactoryArchiveBlendIds;
    std::unordered_set<std::string> mPresetArchiveSessionBlendIds;
    nlohmann::json mFactoryArchiveBlends = nlohmann::json::array();
    nlohmann::json mPresetArchiveSessionBlends = nlohmann::json::array();
    std::unordered_set<std::string> mFactoryArchivePresetIds;
    std::unordered_set<std::string> mTrackedFactoryArchivePresetIds;
    std::unordered_map<std::string, std::string> mFactoryArchivePresetAliases;
    CompositeEffectLibrary mCompositeLibrary;
    FileSystem mFileSystem;
    ModelHasher mHasher;

    // Paths
    std::filesystem::path mResourceRoot;
    std::filesystem::path mUserPresetsPath;
    std::map<std::string, Preset> mFactoryArchivePresets;

    struct PresetArchiveSessionState
    {
        std::string archiveKey;
        std::string archiveName;
        std::filesystem::path rootPath;
        std::filesystem::path presetDir;
        std::size_t presetCount = 0;
    };

    std::optional<PresetArchiveSessionState> mPresetArchiveSession;

    // Cached index of which disk/archive presets reference each library resource.
    // Maps "resourceType:resourceId" -> first preset display name (user > factory > archive).
    // Active preset is checked live and is not part of this cache.
    mutable std::unordered_map<std::string, std::string> mResourceUsageDiskIndex;
    mutable bool mResourceUsageDiskIndexValid{false};

    // Active preset state
    std::optional<Preset> mActivePreset;
    std::string mActivePresetJson;
    std::string mActivePresetId;
    std::string mActiveSceneId;

    // Per-slot preset JSON cache, kept in sync with MultiPresetMixer instances.
    // Used by BroadcastState to send full graph data to the UI for each mixer slot.
    std::map<std::string, std::string> mMixerPresetJsonCache;

    // Composite edit mode
    std::optional<CompositeEffectDefinition> mEditingComposite;

    // Deferred broadcast. mPendingStateBroadcast means "a full broadcast is pending" — a
    // full request always wins over a preset-only one queued in the same idle window.
    bool mPendingStateBroadcast = true;
    bool mPendingPresetStateBroadcast = false;

    // Whether the editor UI is on screen. Set from the UI's "uiVisibility" message; gates
    // the periodic telemetry feeds, which exist only to drive visible meters.

    // App settings
    nlohmann::json mAppSettings = nlohmann::json::object();
    /**
     * The settings this instance is *not* responsible for publishing — normally what
     * the store held the last time this instance loaded or saved them.
     *
     * SaveAppSettings() diffs against this instead of rewriting every key, so a
     * setting another instance changed since our last load is left alone rather
     * than overwritten with our stale copy. Mutable because SaveAppSettings() is
     * const.
     *
     * Values restored from host state are folded in here too (see
     * AdoptAppSettingsAsBaseline): they are the DAW project's, so this instance must
     * not push them to the shared store, and the cleanest way to say that is to
     * record them as already-published.
     */
    mutable nlohmann::json mAppSettingsBaseline = nlohmann::json::object();

    /// True while DeserializeState() is restoring host state, during which
    /// SaveAppSettings() and NotifyHostStateChanged() are no-ops. Set only by its scope guard.
    bool mRestoringHostState = false;

    /**
     * The user's NAM quality tier, owned by *this* plugin instance.
     *
     * Persisted in host state (SerializeState) rather than app.json when running as a
     * plugin, so two instances in one DAW project can sit at different tiers. app.json
     * only seeds a brand-new instance. In standalone it stays app.json-backed.
     *
     * This always holds what the user chose. The offline-render boost is applied on top
     * by EffectiveNamQuality() and never written back here.
     */
    NamQualityConfig mNamQuality;

    /// True while the host renders offline (bounce/freeze/export). Boosts the tier the
    /// DSP actually runs at without touching mNamQuality — see SetOfflineRendering().
    bool mOfflineRendering = false;

    nlohmann::json mUiSettings = nlohmann::json::object();
    nlohmann::json mUiViewState = nlohmann::json::object();

    /// Size of this instance's editor window — see GetEditorWindowSize(). Written by the
    /// editor as the user drags it, read back when the editor is next created.
    EditorWindowSize mEditorWindowSize;
    /// The size the editor has most recently reported, not yet accepted as the remembered
    /// one. OnIdle promotes it once it has survived a tick without changing; a size that
    /// only existed while the window was being torn down never gets that far, because the
    /// editor stops driving OnIdle when it is destroyed. See SetEditorWindowSize().
    std::optional<EditorWindowSize> mPendingEditorWindowSize;
    bool mEditorWindowSizeChangedSinceIdle = false;
    bool mUserInputCalibrationTrainingActive = false;
    /// The interface calibration level injected into NAM nodes; empty while NAM auto input
    /// calibration is switched off. See InjectNamInterfaceCalibration().
    std::optional<double> mNamInterfaceCalibrationLevelDbu;

    // Metronome facade. The click engine itself is MetronomeService; these
    // stay on the controller because their callers are spread across the
    // riff, demo and broadcast paths, and because ActivateRiffGuidance is
    // where a RiffCaptureConfig is translated into the service's own terms.
    struct RiffCaptureConfig;
    [[nodiscard]] double GetEffectiveTempoBpm() const;
    void ActivateRiffGuidance(const RiffCaptureConfig& config, bool forPreview);
    void DeactivateRiffGuidance(bool previewOnly = false);

    // UI messages a service answers itself (the Practice Tool's, for one), registered by the
    // service as it is constructed. MessageDispatcher tries these before its own routes, so a
    // feature owned outright by a service needs no Handle* forwarding method here.
    MessageHandlerRegistry mMessageHandlers;

    std::unique_ptr<ControlSurfaceQueue> mControlSurface;
    std::unique_ptr<ControllerDisplayFeed> mControllerDisplay;
    std::unique_ptr<MetronomeService> mMetronome;
    std::unique_ptr<SignalTestService> mSignalTest;
    /// Answers "listResourceFolder" itself. Shut down explicitly in the destructor, ahead of
    /// the members its detached workers publish through.
    std::unique_ptr<ResourceFolderScanner> mResourceFolderScanner;
    std::unique_ptr<TelemetryPublisher> mTelemetry;
    std::unique_ptr<TunerService> mTuner;
    std::unique_ptr<DemoPreviewService> mDemoPreview;
    std::unique_ptr<PracticeToolService> mPracticeTool;
    std::unique_ptr<HostStateRelay> mHostStateRelay;
    /// Idle-tick divider for the practice tool's transport updates.
    int mPracticeToolUpdateCounter = 0;

    struct RiffCaptureConfig
    {
        double tempoBpm = 120.0;
        int timeSigNum = 4;
        int timeSigDen = 4;
        int bars = 1;
        int countInBars = 1;
        bool metronomeClickEnabled = true;
        std::string patternType = "click";
        std::string patternId;
        std::string beatPattern; // e.g. "HLLL" (H=High L=Low S=Silent)
        std::string presetId;
        std::string presetName;
    };

    struct RiffCaptureRuntime
    {
        bool active = false;
        bool complete = false;
        // ARM: click playing, waiting for input signal to trigger recording
        bool armed = false;
        bool armCountInComplete = false;
        std::size_t armCountInIndex = 0; // samples counted during arm count-in
        float armThreshold = 0.001f;     // ~-60 dBFS input level to trigger
        std::string takeId;
        RiffCaptureConfig config;
        std::vector<float> left;
        std::vector<float> right;
        std::size_t writeIndex = 0;
        std::size_t targetSamples = 0;
        std::size_t countInSamples = 0;
        double sampleRate = 0.0;
        int bitsPerSample = 16;
        // Live waveform during recording (256 peak buckets for up to 16 bars)
        std::vector<float> livePeaks;
        std::size_t livePeakBucketSize = 1;
        std::size_t lastProgressSample = 0;
        std::size_t armPostCountInSamples = 0; // samples elapsed in detection phase after count-in
        std::chrono::steady_clock::time_point startedAt;
        std::chrono::steady_clock::time_point endedAt;
    };

    mutable std::mutex mRiffLibraryMutex;
    nlohmann::json mRiffLibraryIndex = nlohmann::json::object();
    /// Published under mDSPMutex. Active captures are mutated only while that lock
    /// is held; completed captures are treated as immutable and replaced wholesale.
    std::shared_ptr<RiffCaptureRuntime> mRiffCapture;

    // Resource preview state (for temp loading from Tone3000)
    struct PreviewState
    {
        bool active = false;
        std::string nodeId;
        int resourceIndex = 0;
        std::string resourceType;
        std::filesystem::path tempFilePath;
        std::optional<ResourceRef> originalResourceRef;
    };

    PreviewState mPreviewState;

    // UI state
    bool mUIReady = false;
    mutable std::uint64_t mSharedSyncVersionSeen = 0;
    mutable bool mSharedSyncVersionSeenInitialized = false;
    std::uint64_t mSharedSyncVersionHandled = 0;
    std::chrono::steady_clock::time_point mNextSharedSyncPollAt{};

    // Automation
    AutomationSlotTable mAutomationSlots;
    /// Written on the message thread; a host reads it as its current program on any thread.
    std::atomic<int> mSetlistCursorIndex{0};
    int mSetlistBankSize = 8;
};
} // namespace guitarfx
