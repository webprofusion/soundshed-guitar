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
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace guitarfx
{

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

    // ── NAM calibration ────────────────────────────────────────────
    struct NamCalibrationData
    {
        double inputLevelDb = 0.0;
        double outputLevelDb = 0.0;
    };

    struct NamCalibrationJob
    {
        std::string hash;
        std::filesystem::path path;
        std::string resourceType;
        std::string resourceId;
    };

    struct NamCalibrationResult
    {
        NamCalibrationJob job;
        bool success = false;
        std::string error;
        NamCalibrationData data;
    };

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
    void HandleRerunNamCalibrationRequest(const nlohmann::json& payload);
    void HandleAddSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleSplitSignalPathEdgeRequest(const nlohmann::json& payload);
    void HandleCollapseSignalPathSplitRequest(const nlohmann::json& payload);
    void HandleReplaceSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleReorderSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleDeleteSignalPathNodeRequest(const nlohmann::json& payload);
    void HandleImportRemoteResourceRequest(const nlohmann::json& payload);
    void HandlePreviewRemoteResourceRequest(const nlohmann::json& payload);
    void HandleCancelPreviewResourceRequest(const nlohmann::json& payload);
    void HandleSaveBlendDefinitionRequest(const nlohmann::json& payload);
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
    void ApplyBlendDefinitions(Preset& preset);
    bool ApplyNodeParameter(const GraphNode& node, const std::string& paramKey, double value);
    void ApplyDiagnosticsSettingsFromAppSettings();
    void ApplyInterfaceCalibrationSettingsFromAppSettings();
    void ApplyUiSettingsFromAppSettings();
    void SendMessageToUI(const std::string& jsonMessage);
    void ReportErrorToUI(const std::string& message, const std::string& detail = {});
    void SendGlobalChainStateToUI();
    void SendCompositeLibraryToUI();
    void SendEffectCatalogToUI();
    void SendPresetListToUI();
    void SendSignalDiagnosticsToUI();
    void SendPerformanceStatsToUI();
    void SendMetronomeStateToUI();

    // Composite edit helpers
    [[nodiscard]] bool IsCompositeEditMode() const;
    [[nodiscard]] SignalGraph* ResolveEditTarget();
    void BroadcastCompositeEditState();

    // Resource helpers
    bool UpdateResourceForNodeType(const std::string& nodeType,
                                   const std::string& resourceType,
                                   const std::filesystem::path& filePath,
                                   bool applyPreset = true);
    bool UpdateResourceForNodeId(const std::string& nodeId,
                                 const ResourceRef& ref,
                                 bool applyPreset = true);
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResourceRef(const ResourceRef& ref) const;
    void AppendUserLibraryResource(const LibraryResource& resource);
    void EnsureBasicGraph();
    bool ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& zipData,
                                     const std::string& resourceType,
                                     const std::filesystem::path& outputPath);

    // NAM calibration
    void QueueNamCalibrationForNode(const std::string& nodeId, const ResourceRef& ref, bool force = false);
    void ProcessNamCalibrationQueue();
    void ApplyNamCalibrationResult(const NamCalibrationResult& result);
    [[nodiscard]] std::optional<NamCalibrationData> GetNamCalibrationFromCache(const std::string& hash) const;
    void StoreNamCalibrationInCache(const std::string& hash, const NamCalibrationData& data);
    void RemoveNamCalibrationFromCache(const std::string& hash);
    void ApplyNamCalibrationToNode(const std::string& nodeId, const std::string& hash, const NamCalibrationData& data);
    void ClearNamCalibrationParams(GraphNode& node) const;
    void SendNamCalibrationStatus(const std::string& nodeId, const std::string& status);

    // Settings persistence
    void SaveAppSettings() const;
    void LoadAppSettings();
    void LoadLastSessionState();
    void LoadResourceLibraries();
    void LoadBlendLibrary();
    void SaveBlendLibrary() const;
    void LoadCompositeLibrary();
    void LoadLayoutLibrary();
    void SaveLayoutToFile(const std::string& effectType, const nlohmann::json& layoutJson);
    [[nodiscard]] std::filesystem::path ResolveUiStoragePath(const std::string& filename) const;
    [[nodiscard]] nlohmann::json LoadUiStorageJson(const std::string& filename, const nlohmann::json& fallback) const;
    void SaveUiStorageJson(const std::string& filename, const nlohmann::json& payload) const;

    bool WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const;

    void AppendSessionLog(const std::string& message);

    // ── State ──────────────────────────────────────────────────────
    IPluginHost& mHost;

    // DSP engine
    MultiPresetMixer mPresetMixer;
    mutable std::mutex mDSPMutex;

    // Resources and libraries
    ResourceLibrary mResourceLibrary;
    nlohmann::json mBlendLibrary = nlohmann::json::array();
    CompositeEffectLibrary mCompositeLibrary;
    FileSystem mFileSystem;
    ModelHasher mHasher;

    // Paths
    std::filesystem::path mResourceRoot;
    std::filesystem::path mUserPresetsPath;

    // Active preset state
    std::optional<Preset> mActivePreset;
    std::string mActivePresetJson;
    std::string mActivePresetId;

    // Composite edit mode
    std::optional<CompositeEffectDefinition> mEditingComposite;

    // Deferred broadcast
    bool mPendingStateBroadcast = true;

    // App settings
    nlohmann::json mAppSettings = nlohmann::json::object();
    nlohmann::json mUiSettings = nlohmann::json::object();

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

    // NAM calibration
    std::deque<NamCalibrationJob> mNamCalibrationQueue;
    std::optional<std::future<NamCalibrationResult>> mNamCalibrationFuture;
    std::optional<NamCalibrationJob> mNamCalibrationActiveJob;
    std::unordered_map<std::string, std::vector<std::string>> mNamCalibrationWaiters;
    std::unordered_set<std::string> mNamCalibrationInFlight;
    mutable std::mutex mNamCalibrationMutex;

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
    [[nodiscard]] double GetEffectiveTempoBpm() const;
    void RenderMetronome(float** outputs, int numSamples);
    void ApplyMetronomeSettingsFromAppSettings();
    void UpdateMetronomeClickConfigFromSettings();
    struct MetronomeClickTypeConfig;
    [[nodiscard]] const MetronomeClickTypeConfig* FindMetronomeClickType(const std::string& id) const;
    struct MetronomeClickSamples;
    std::shared_ptr<MetronomeClickSamples> BuildMetronomeClickSamples(const MetronomeClickTypeConfig& config, double targetSampleRate) const;
    void RefreshMetronomeClickSamples(double sampleRate);

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
    std::atomic<std::shared_ptr<MetronomeClickSamples>> mMetronomeClickSamples{nullptr};

    // Demo audio preview
    struct DemoAudioBuffer
    {
        std::string id;
        std::string title;
        double sampleRate = 0.0;
        int channels = 0;
        std::vector<std::vector<float>> channelSamples;
    };
    std::atomic<std::shared_ptr<DemoAudioBuffer>> mDemoAudioBuffer{nullptr};
    std::atomic<size_t> mDemoAudioCursor{0};
    std::atomic<bool> mDemoAudioActive{false};

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
    std::atomic<bool> mSignalDiagnosticsEnabled{false};
    int mDSPPerformanceUpdateCounter = 0;
    int mSignalDiagnosticsUpdateCounter = 0;

    // UI state
    bool mUIReady = false;

    // Layout library cache
    nlohmann::json mLayoutLibrary = nlohmann::json::object();
};

} // namespace guitarfx
