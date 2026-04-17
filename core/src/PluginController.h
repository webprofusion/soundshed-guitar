#pragma once

/**
 * PluginController — Framework-agnostic orchestration layer.
 *
 * Owns the DSP engine (MultiPresetMixer), resource library, preset storage,
 * composite library, and all shared plugin state. Communicates with the
 * framework-specific host through the IPluginHost interface.
 *
 * The framework adapter (iPlug2 GuitarFXPlugin / JUCE PluginProcessor) creates
 * one PluginController instance, implements IPluginHost, and delegates audio
 * processing and UI message handling to the controller.
 */

#include "IPluginHost.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/CompositeEffectProcessor.h"
#include "models/ModelHasher.h"
#include "presets/PresetTypes.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypesJson.h"
#include "resources/ResourceLibrary.h"
#include "util/FileSystem.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace guitarfx
{

class DemoPreviewService;

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
    [[nodiscard]] std::string SerializeState() const;
    void DeserializeState(const std::string& json);

    // ── UI message handling ────────────────────────────────────────
    /// Handle an incoming JSON message from the WebView UI.
    void HandleUIMessage(const std::string& jsonMessage);

    /// Called periodically from the host's idle/timer callback.
    void OnIdle();

    /// Called when the WebView content has finished loading.
    void OnWebContentLoaded();

    // ── Accessors ──────────────────────────────────────────────────
    [[nodiscard]] MultiPresetMixer& GetMixer() { return mPresetMixer; }
    [[nodiscard]] const MultiPresetMixer& GetMixer() const { return mPresetMixer; }
    [[nodiscard]] ResourceLibrary& GetResourceLibrary() { return mResourceLibrary; }
    [[nodiscard]] const std::optional<Preset>& GetActivePreset() const { return mActivePreset; }
    [[nodiscard]] const nlohmann::json& GetAppSettings() const { return mAppSettings; }
    [[nodiscard]] IPluginHost& GetHost() { return mHost; }

    // ── Parameter bridging ─────────────────────────────────────────
    /// Plugin parameter IDs (matches both iPlug2 and JUCE ParameterIndex enums).
    enum ParameterId
    {
        kParamInputTrim = 0,
        kParamOutputTrim,
        kParamDrive,
        kParamTone,
        kParamGateEnabled,
        kParamGateThreshold,
        kParamMix,
        kParamDoublerEnabled,
        kParamDoublerDelay,
        kParamTranspose,
        kParamIRQuality,
        kParamEQEnabled,
        kParamEQLowGain,
        kParamEQLowFreq,
        kParamEQLowMidGain,
        kParamEQLowMidFreq,
        kParamEQLowMidQ,
        kParamEQHighMidGain,
        kParamEQHighMidFreq,
        kParamEQHighMidQ,
        kParamEQHighGain,
        kParamEQHighFreq,
        kParamCount
    };

    /// Called when a DAW-automatable parameter changes (from host or UI).
    void OnParamChange(int paramIdx, double value);

    /// Get the current value of a parameter.
    [[nodiscard]] double GetParamValue(int paramIdx) const;

    // ── Multi-preset mixer controls ────────────────────────────────
    bool AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name);
    bool AddActivePresetById(const std::string& presetId);
    void RemoveActivePreset(const std::string& presetId);
    void SetActivePresetMix(const std::string& presetId, double value);
    void SetActivePresetPan(const std::string& presetId, double pan);
    void SetActivePresetMute(const std::string& presetId, bool mute);
    void SetActivePresetSolo(const std::string& presetId, bool solo);
    void SetMasterGain(double value);
    void SetLimiterEnabled(bool enabled);

    // ── Signal path test ───────────────────────────────────────────
    struct SignalPathTestResult
    {
        double sampleRate = 0.0;
        double frequencyHz = 0.0;
        double durationSeconds = 0.0;
        double elapsedSeconds = 0.0;
        double inputRMS = 0.0;
        std::array<double, 2> outputRMS{0.0, 0.0};
        bool passed = false;
    };

    bool StartSignalPathTest(double frequencyHz = 440.0, double durationSeconds = 1.0);
private:
    friend class MessageDispatcher;

    // ── Internal handler methods ───────────────────────────────────
    // These are called by MessageDispatcher and operate on controller state.
    void HandleStateRequest();
    void HandlePresetLoadRequest(const nlohmann::json& payload);
    void HandleSetParameterRequest(const nlohmann::json& payload);
    void HandleSetGlobalChainParamRequest(const nlohmann::json& payload);
    void HandleSignalTestRequest(const nlohmann::json& payload);
    void HandleLoadModelRequest(const nlohmann::json& payload);
    void HandleLoadIRRequest(const nlohmann::json& payload);
    void HandleSavePresetRequest(const nlohmann::json& payload);
    void HandleDeletePresetRequest(const nlohmann::json& payload);
    void HandleGetPresetByIdRequest(const nlohmann::json& payload);
    void HandleBrowseModelRequest();
    void HandleBrowseIRRequest();
    void HandleOpenAudioPreferencesRequest();
    void HandleTunerRequest(const nlohmann::json& payload);
    void HandleSetInputModeRequest(const nlohmann::json& payload);
    void HandleSetAmpCabStateRequest(const nlohmann::json& payload);
    void HandleSetAutoLevelRequest(const nlohmann::json& payload);
    void HandleDeleteLayoutRequest(const nlohmann::json& payload);
    void HandleSetMetronomeRequest(const nlohmann::json& payload);
    void HandleUpdateSignalPathNodeParamRequest(const nlohmann::json& payload);
    void HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json& payload);
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
    void HandleUpdateLibraryResourceRequest(const nlohmann::json& payload);
    void HandleBrowseLibraryResourcePathRequest(const nlohmann::json& payload);
    void HandleImportToneSharingPackRequest(const nlohmann::json& payload);
    void HandleDeleteImportedToneSharingPackRequest(const nlohmann::json& payload);
    void HandlePreviewRemoteResourceRequest(const nlohmann::json& payload);
    void HandleCancelPreviewResourceRequest(const nlohmann::json& payload);
    void HandleSaveBlendDefinitionRequest(const nlohmann::json& payload);
    void HandleDeleteBlendDefinitionRequest(const nlohmann::json& payload);
    void HandleRequestResourceDataRequest(const nlohmann::json& payload);
    void HandleSaveBlendArchiveRequest(const nlohmann::json& payload);
    void HandleSavePresetArchiveRequest(const nlohmann::json& payload);
    void HandleSaveLibraryArchiveRequest(const nlohmann::json& payload);
    void HandleSaveEffectLayoutRequest(const nlohmann::json& payload);
    void HandleExportEffectLayoutRequest(const nlohmann::json& payload);
    void HandleBrowseLayoutImageRequest(const nlohmann::json& payload);
    void HandleSaveLayoutImageRequest(const nlohmann::json& payload);
    void HandleCleanupResourceLibraryRequest(const nlohmann::json& payload);
    void HandleSaveCompositeDefinitionRequest(const nlohmann::json& payload);
    void HandleDeleteCompositeDefinitionRequest(const nlohmann::json& payload);
    void HandleEnterCompositeEditModeRequest(const nlohmann::json& payload);
    void HandleExitCompositeEditModeRequest(const nlohmann::json& payload);
    void HandlePreviewDemoRequest(const nlohmann::json& payload);
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

    // Composite presets (Multi-Rig)
    void HandleSaveCompositePresetRequest(const nlohmann::json& payload);
    void HandleLoadCompositePresetRequest(const nlohmann::json& payload);
    void HandleGetCompositePresetListRequest();
    void HandleRemoveCompositePresetRequest(const nlohmann::json& payload);

    // Signal diagnostics / performance
    void HandleGetSignalDiagnosticsRequest();
    void HandleGetPerformanceStatsRequest();
    void HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload);
    void HandleGetEffectCatalogRequest();
    void HandleGetPresetListRequest();
    void HandleGetPresetFoldersRequest();
    void HandleSetPresetFoldersRequest(const nlohmann::json& payload);
    void HandleGetPresetFavoritesRequest();
    void HandleSetPresetFavoritesRequest(const nlohmann::json& payload);
    void HandleGetPresetRatingsRequest();
    void HandleSetPresetRatingsRequest(const nlohmann::json& payload);
    void HandleGetSetlistsRequest();
    void HandleSetSetlistsRequest(const nlohmann::json& payload);
    void HandleGetThemeRequest();
    void HandleSetThemeRequest(const nlohmann::json& payload);

    [[nodiscard]] std::optional<LibraryResource> SaveLocalLibraryResource(const nlohmann::json& payload,
                                                                          std::string& error,
                                                                          bool allowCreate = true);
    void HandleGetAppInfoRequest();
    void HandleGetGlobalChainRequest();
    void HandleSetGlobalChainRequest(const nlohmann::json& payload);
    void HandleSetNodeEnabledRequest(const nlohmann::json& payload);
    void HandleSetNodeParamRequest(const nlohmann::json& payload);
    void HandleLoadNodeResourceRequest(const nlohmann::json& payload);
    void HandleSetTunerEnabledRequest(const nlohmann::json& payload);
    void HandleSetTunerReferenceRequest(const nlohmann::json& payload);

    // ── Internal helpers ───────────────────────────────────────────
    void BroadcastState();
    void ApplyPreset(const Preset& preset);
    void UpdateHostLatency();
    void ApplyBlendDefinitions(Preset& preset);
    bool ApplyNodeParameter(const GraphNode& node, const std::string& paramKey, double value);
    void ApplyDiagnosticsSettingsFromAppSettings();
    void ApplyDspLevelTargetSettingsFromAppSettings();
    void ApplyUserInputCalibrationSettingsFromAppSettings();
    void ApplyUiSettingsFromAppSettings();
    [[nodiscard]] bool IsFactoryPresetArchiveLoadingEnabled() const;
    void SendMessageToUI(const std::string& jsonMessage);
    void ReportErrorToUI(const std::string& message, const std::string& detail = {});
    void SendGlobalChainStateToUI();
    void SendCompositeLibraryToUI();
    void SendEffectCatalogToUI();
    void SendPresetListToUI();
    void SendCompositePresetListToUI();
    void SendSignalDiagnosticsToUI();
    void SendPerformanceStatsToUI();
    void SendMetronomeStateToUI();
    void SendRiffLibraryStateToUI();

    // Composite edit helpers
    [[nodiscard]] bool IsCompositeEditMode() const;
    [[nodiscard]] SignalGraph* ResolveEditTarget();
    [[nodiscard]] std::string GetResolvedActiveSceneId() const;
    void SyncActivePresetSceneGraph();
    void BroadcastCompositeEditState();

    // Resource helpers
    bool UpdateResourceForNodeType(const std::string& nodeType,
                                   const std::string& resourceType,
                                   const std::filesystem::path& filePath,
                                   bool applyPreset = true);
    bool UpdateResourceForNodeId(const std::string& nodeId,
                                 const ResourceRef& ref,
                                 bool applyPreset = true);
    void RefreshWasmNodeDescriptor(GraphNode& node);
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResourceRef(const ResourceRef& ref) const;
    void AppendUserLibraryResource(const LibraryResource& resource);
    void EnsureBasicGraph();
    bool ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& zipData,
                                     const std::string& resourceType,
                                     const std::filesystem::path& outputPath);

    // NAM level-state normalization
    void ResetNamNodeLevelState(const std::string& nodeId);
    void ClearNamCalibrationParams(GraphNode& node) const;

    // Settings persistence
    void SaveAppSettings() const;
    void LoadAppSettings();
    void LoadLastSessionState();
    [[nodiscard]] std::optional<Preset> LoadPresetById(const std::string& presetId) const;
    [[nodiscard]] std::optional<std::string> FindPresetIdByTitle(const std::string& presetTitle) const;
    bool TryLoadConfiguredDefaultPreset();
    void LoadResourceLibraries();
    void LoadFactoryPresetArchives();
    void LoadBlendLibrary();
    void SaveBlendLibrary() const;
    void LoadCompositeLibrary();
    void LoadLayoutLibrary();
    void SaveLayoutToFile(const std::string& effectType, const nlohmann::json& layoutJson);
    [[nodiscard]] std::filesystem::path ResolveUiStoragePath(const std::string& filename) const;
    [[nodiscard]] nlohmann::json LoadUiStorageJson(const std::string& filename, const nlohmann::json& fallback) const;
    void SaveUiStorageJson(const std::string& filename, const nlohmann::json& payload) const;
    [[nodiscard]] std::filesystem::path ResolveRiffLibraryPath() const;
    [[nodiscard]] std::filesystem::path ResolveRiffLibraryIndexPath() const;
    [[nodiscard]] nlohmann::json LoadRiffLibraryIndex() const;
    bool SaveRiffLibraryIndex(const nlohmann::json& payload) const;
    [[nodiscard]] std::string BuildRiffTakeId() const;
    [[nodiscard]] std::string BuildRiffId() const;
    [[nodiscard]] std::string BuildTimestampUtcIso() const;
    [[nodiscard]] std::optional<nlohmann::json> FindRiffTakeById(const std::string& takeId) const;
    void FinalizeRiffCaptureLocked(bool canceled);

    bool WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const;

    void AppendSessionLog(const std::string& message);
    void ApplyParamChangeLocked(int paramIdx, double value);
    void ProcessAudioLocked(float** inputs, float** outputs, int numSamples);

    // ── State ──────────────────────────────────────────────────────
    IPluginHost& mHost;

    // DSP engine
    MultiPresetMixer mPresetMixer;
    mutable std::mutex mDSPMutex;

    // Resources and libraries
    ResourceLibrary mResourceLibrary;
    nlohmann::json mBlendLibrary = nlohmann::json::array();
    std::unordered_set<std::string> mFactoryArchiveBlendIds;
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

    // Deferred broadcast
    bool mPendingStateBroadcast = true;

    // App settings
    nlohmann::json mAppSettings = nlohmann::json::object();
    nlohmann::json mUiSettings = nlohmann::json::object();
    nlohmann::json mUiViewState = nlohmann::json::object();
    bool mUserInputCalibrationTrainingActive = false;

    // Parameter values (shadow of host parameters)
    std::array<double, kParamCount> mParamValues{};

    // Signal path test
    struct SignalTestRuntimeState
    {
        double frequencyHz = 0.0;
        double phase = 0.0;
        double phaseIncrement = 0.0;
        int samplesRemaining = 0;
        int totalSamples = 0;
        double sampleRate = 0.0;
        double inputSumSquares = 0.0;
        std::array<double, 2> outputSumSquares{0.0, 0.0};
        std::chrono::steady_clock::time_point startTime;
    };
    std::atomic<bool> mSignalTestActive{false};
    std::atomic<bool> mSignalTestResultPending{false};
    SignalTestRuntimeState mSignalTestState;
    SignalPathTestResult mSignalTestResult;

    // Tuner state
    std::atomic<bool> mTunerActive{false};
    struct TunerData
    {
        std::string noteName;
        int octave = 0;
        double frequency = 0.0;
        double centOffset = 0.0;
        double confidence = 0.0;
        bool detected = false;
    };
    std::atomic<bool> mTunerDataPending{false};
    TunerData mPendingTunerData;
    mutable std::mutex mTunerMutex;

    // Metronome helpers
    struct RiffCaptureConfig;
    [[nodiscard]] double GetEffectiveTempoBpm() const;
    void RenderMetronome(float** outputs, int numSamples);
    void ApplyMetronomeSettingsFromAppSettings();
    void UpdateMetronomeClickConfigFromSettings();
    struct MetronomeClickTypeConfig;
    [[nodiscard]] const MetronomeClickTypeConfig* FindMetronomeClickType(const std::string& id) const;
    struct MetronomeClickSamples;
    std::shared_ptr<MetronomeClickSamples> BuildMetronomeClickSamples(const MetronomeClickTypeConfig& config, double targetSampleRate) const;
    void RefreshMetronomeClickSamples(double sampleRate);
    void ActivateRiffGuidance(const RiffCaptureConfig& config, bool forPreview);
    void DeactivateRiffGuidance(bool previewOnly = false);

    // Metronome state
    std::atomic<double> mMetronomeBpm{120.0};
    std::atomic<bool> mMetronomeEnabled{false};
    std::atomic<double> mMetronomeVolumeDb{-12.0};
    std::atomic<double> mMetronomePan{0.0};
    std::string mMetronomeClickType{"click"};
    std::atomic<bool> mMetronomeResetPending{false};
    double mMetronomeSamplesUntilClick = 0.0;
    int mMetronomeClickSamplesRemaining = 0;
    double mMetronomeClickPhase = 0.0;
    double mMetronomeClickPhaseIncrement = 0.0;
    int mMetronomeBeatIndex = 0;
    int mMetronomeClickSamplePosition = 0;
    bool mMetronomeClickUseHigh = false;

    struct MetronomeClickTypeConfig
    {
        std::string id;
        std::string label;
        std::filesystem::path lowPath;
        std::filesystem::path highPath;
    };

    struct MetronomeClickSamples
    {
        std::vector<std::vector<float>> low;
        std::vector<std::vector<float>> high;
    };

    std::vector<MetronomeClickTypeConfig> mMetronomeClickConfig;
    std::shared_ptr<MetronomeClickSamples> mMetronomeClickSamples;
    bool mRiffGuidanceActive = false;
    bool mRiffGuidanceForPreview = false;
    double mRiffGuidanceBpm = 120.0;
    int mRiffGuidanceBeatsPerBar = 4;
    double mRiffGuidanceBeatScale = 1.0;
    std::shared_ptr<MetronomeClickSamples> mRiffGuidanceClickSamples;
    std::string mRiffGuidanceBeatPattern;
    bool mRiffGuidancePreviewWasActive = false;
    std::string mMetronomeBeatPattern; // e.g. "HLLL"

    std::unique_ptr<DemoPreviewService> mDemoPreview;

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
    RiffCaptureRuntime mRiffCapture;

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

    // Signal diagnostics
    std::atomic<bool> mSignalDiagnosticsEnabled{true};
    int mDSPPerformanceUpdateCounter = 0;
    int mSignalDiagnosticsUpdateCounter = 0;

    // UI state
    bool mUIReady = false;

    // Layout library cache
    nlohmann::json mLayoutLibrary = nlohmann::json::object();
};

} // namespace guitarfx
