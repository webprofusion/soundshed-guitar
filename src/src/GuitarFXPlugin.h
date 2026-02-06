#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "IPlug_include_in_plug_hdr.h"
#include "models/ModelHasher.h"
#include "presets/PresetTypes.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "dsp/MultiPresetMixer.h"
#include "util/FileSystem.h"

namespace guitarfx
{
  class GuitarFXPlugin final : public iplug::Plugin
  {
  public:
    explicit GuitarFXPlugin(const iplug::InstanceInfo &info);

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

    void ProcessBlock(iplug::sample **inputs, iplug::sample **outputs, int nFrames) override;
    void OnReset() override;
    void OnIdle() override;
    void* OpenWindow(void* pParent) override;
    void CloseWindow() override;
    void OnUIOpen() override;
    void OnUIClose() override;
    void OnWebContentLoaded() override;
    void OnParentWindowResize(int width, int height) override;
    bool SerializeState(iplug::IByteChunk &chunk) const override;
    int UnserializeState(const iplug::IByteChunk &chunk, int startPos) override;
    void OnParamChange(int paramIdx) override;
    
    // WebViewEditorDelegate callback for receiving messages from the WebView
    bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;
    
    // Override to intercept custom JSON messages before base class processing
    void OnMessageFromWebView(const char* jsonStr) override;

#ifdef VST3_API
    Steinberg::tresult PLUGIN_API initialize(FUnknown* context) override;
#endif

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

    [[nodiscard]] bool StartSignalPathTest(double frequencyHz = 440.0, double durationSeconds = 1.0);

    // Multi-preset mixer controls (controller API)
    bool AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name);
    void RemoveActivePreset(const std::string& presetId);
    void SetActivePresetMix(const std::string& presetId, double value);
    void SetActivePresetPan(const std::string& presetId, double pan);
    void SetActivePresetMute(const std::string& presetId, bool mute);
    void SetActivePresetSolo(const std::string& presetId, bool solo);
    void SetMasterMixGain(double value);
    void SetMixLimiterEnabled(bool enabled);

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
      kParamIRQuality,  // IR quality mode (Economy/Standard/High/Full)
      // Parametric EQ parameters
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

  private:
    struct MetronomeClickSamples
    {
      std::vector<std::vector<iplug::sample>> low;
      std::vector<std::vector<iplug::sample>> high;
    };

    struct MetronomeClickTypeConfig
    {
      std::string id;
      std::string label;
      std::filesystem::path lowPath;
      std::filesystem::path highPath;
    };

    void InitializeParameters();
    void SendMessageToUI(const std::string& jsonMessage);
    void HandleUIMessage(const std::string &message);
    void HandlePresetLoadRequest(const nlohmann::json &payload);
    void HandleStateRequest();
    void HandleSignalTestRequest(const nlohmann::json &payload);
    void HandlePreviewDemoRequest(const nlohmann::json &payload);
    void HandleStopDemoRequest();
    void HandleSetParameterRequest(const nlohmann::json &payload);
    void HandleLoadModelRequest(const nlohmann::json &payload);
    void HandleLoadIRRequest(const nlohmann::json &payload);
    void HandleSavePresetRequest(const nlohmann::json &payload);
    void HandleBrowseModelRequest();
    void HandleBrowseIRRequest();
    void HandleOpenAudioPreferencesRequest();
    void HandleTunerRequest(const nlohmann::json &payload);
    void HandleSetInputModeRequest(const nlohmann::json &payload);
    void HandleSetAmpCabStateRequest(const nlohmann::json &payload);
    void HandleSetGlobalChainParamRequest(const nlohmann::json &payload);
    void SendGlobalChainStateToUI();
    void HandleSetAutoLevelRequest(const nlohmann::json &payload);
    void HandleSetMetronomeRequest(const nlohmann::json &payload);
    void HandleUpdateSignalPathNodeParamRequest(const nlohmann::json &payload);
    void HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json &payload);
    void HandleUpdateNodeResourceRequest(const nlohmann::json &payload);
    void HandleBrowseNodeResourceRequest(const nlohmann::json &payload);
    void HandleRerunNamCalibrationRequest(const nlohmann::json &payload);
    void HandleAddSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleSplitSignalPathEdgeRequest(const nlohmann::json &payload);
    void HandleCollapseSignalPathSplitRequest(const nlohmann::json &payload);
    void HandleReplaceSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleReorderSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleDeleteSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleImportRemoteResourceRequest(const nlohmann::json &payload);
    void HandlePreviewRemoteResourceRequest(const nlohmann::json &payload);
    void HandleCancelPreviewResourceRequest(const nlohmann::json &payload);
    bool ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& zipData, const std::string& resourceType, const std::filesystem::path& outputPath);
    void HandleSaveBlendDefinitionRequest(const nlohmann::json &payload);
    void HandleRequestResourceDataRequest(const nlohmann::json &payload);
    void HandleSaveBlendArchiveRequest(const nlohmann::json &payload);
    void HandleSavePresetArchiveRequest(const nlohmann::json &payload);
    void HandleSaveLibraryArchiveRequest(const nlohmann::json &payload);
    void HandleSaveEffectLayoutRequest(const nlohmann::json &payload);
    void HandleExportEffectLayoutRequest(const nlohmann::json &payload);
    void HandleBrowseLayoutImageRequest(const nlohmann::json &payload);
    void HandleSaveLayoutImageRequest(const nlohmann::json &payload);
    void LoadLayoutLibrary();
    void SaveLayoutToFile(const std::string& effectType, const nlohmann::json& layoutJson);
    void HandleCleanupResourceLibraryRequest(const nlohmann::json &payload);
    void RefreshMetronomeClickSamples();
    void UpdateMetronomeClickConfigFromSettings();
    const MetronomeClickTypeConfig* FindMetronomeClickType(const std::string& id) const;
    std::shared_ptr<MetronomeClickSamples> BuildMetronomeClickSamples(const MetronomeClickTypeConfig& config, double targetSampleRate) const;
    void AppendUserLibraryResource(const LibraryResource& resource);
    void EnsureBasicGraph();
    bool UpdateResourceForNodeType(const std::string& nodeType,
                     const std::string& resourceType,
                     const std::filesystem::path& filePath,
                     bool applyPreset = true);
    bool UpdateResourceForNodeId(const std::string& nodeId,
                   const ResourceRef& ref,
                   bool applyPreset = true);
    void ProcessThroughGlobalChain(iplug::sample **inputs, iplug::sample **outputs, int nFrames);
    void BroadcastState();
    void ApplyPreset(const guitarfx::Preset &preset);
    void ApplyBlendDefinitions(Preset& preset);
    bool ApplyNodeParameter(const guitarfx::GraphNode& node, const std::string& paramKey, double value);
    void QueueNamCalibrationForNode(const std::string& nodeId, const ResourceRef& ref, bool force = false);
    void ProcessNamCalibrationQueue();
    void ApplyNamCalibrationResult(const NamCalibrationResult& result);
    [[nodiscard]] std::optional<NamCalibrationData> GetNamCalibrationFromCache(const std::string& hash) const;
    void StoreNamCalibrationInCache(const std::string& hash, const NamCalibrationData& data);
    void RemoveNamCalibrationFromCache(const std::string& hash);
    void ApplyNamCalibrationToNode(const std::string& nodeId, const std::string& hash, const NamCalibrationData& data);
    void ClearNamCalibrationParams(GraphNode& node) const;
    void SendNamCalibrationStatus(const std::string& nodeId, const std::string& status);
    void AppendSessionLog(std::string_view message);
    void ReportErrorToUI(std::string_view message, std::string_view detail = {});
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResourceRef(const ResourceRef &ref) const;
    [[nodiscard]] static std::vector<std::uint8_t> DecodeBase64(const std::string &encoded);
    [[nodiscard]] static std::string EncodeBase64(const std::vector<std::uint8_t> &data);
    bool WriteFile(const std::filesystem::path &target, const std::vector<std::uint8_t> &data) const;
    
    // App settings persistence
    void SaveAppSettings() const;
    void LoadAppSettings();
    void LoadLastSessionState();
    void LoadResourceLibraries();
    void LoadBlendLibrary();
    void SaveBlendLibrary() const;
    void LoadWebViewContent(bool forceReload);
    void SendMetronomeStateToUI();
    void RenderMetronome(iplug::sample **outputs, int nFrames);
    double GetEffectiveTempoBpm() const;
  #ifdef _WIN32
    void ApplyWindowIcon();
    void ReleaseWindowIcon();
  #endif

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

    MultiPresetMixer mPresetMixer;
    ResourceLibrary mResourceLibrary;
    nlohmann::json mBlendLibrary = nlohmann::json::array();
    FileSystem mFileSystem;
    ModelHasher mHasher;
    std::filesystem::path mResourceRoot;
    std::filesystem::path mUserPresetsPath;
    std::optional<Preset> mActivePreset;
    std::string mActivePresetJson;
    std::string mActivePresetId;
    bool mPendingStateBroadcast = true;
    std::vector<iplug::sample> mSignalTestInputLeft;
    std::vector<iplug::sample> mSignalTestInputRight;
    std::vector<iplug::sample> mSignalTestOutputLeft;
    std::vector<iplug::sample> mSignalTestOutputRight;
    SignalTestRuntimeState mSignalTestState;
    SignalPathTestResult mSignalTestResult;
    std::atomic<bool> mSignalTestActive{false};
    std::atomic<bool> mSignalTestResultPending{false};
    std::deque<NamCalibrationJob> mNamCalibrationQueue;
    std::optional<std::future<NamCalibrationResult>> mNamCalibrationFuture;
    std::optional<NamCalibrationJob> mNamCalibrationActiveJob;
    std::unordered_map<std::string, std::vector<std::string>> mNamCalibrationWaiters;
    std::unordered_set<std::string> mNamCalibrationInFlight;
    mutable std::mutex mNamCalibrationMutex;

    // UI settings persistence
    double mUiZoom = 1.0;
    struct WindowBounds
    {
      int x = -1;
      int y = -1;
      int width = -1;
      int height = -1;
      bool HasBounds() const { return width > 0 && height > 0; }
    } mWindowBounds;
    nlohmann::json mAppSettings = nlohmann::json::object();
    std::atomic<double> mMetronomeBpm{120.0};
    std::atomic<bool> mMetronomeEnabled{false};
    std::atomic<double> mMetronomeVolume{0.25};
    std::atomic<double> mMetronomeVolumeDb{-12.0};
    std::atomic<double> mMetronomePan{0.0};
    std::atomic<bool> mMetronomeResetPending{false};
    std::atomic<std::shared_ptr<MetronomeClickSamples>> mMetronomeClickSamples{nullptr};
    std::vector<MetronomeClickTypeConfig> mMetronomeClickConfig;
    std::string mMetronomeClickType{"click"};
    double mMetronomeSamplesUntilClick = 0.0;
    int mMetronomeClickSamplesRemaining = 0;
    double mMetronomeClickPhase = 0.0;
    double mMetronomeClickPhaseIncrement = 0.0;
    int mMetronomeBeatIndex = 0;
    int mMetronomeClickSamplePosition = 0;
    bool mMetronomeClickUseHigh = false;
    double mLastBroadcastTempo = 120.0;
    int mMetronomeUpdateCounter = 0;
    struct PreviewPlaybackBuffer
    {
      std::string id;
      std::string title;
      double sampleRate = 0.0;
      int channels = 0;
      std::vector<std::vector<iplug::sample>> channelSamples;
    };

    std::atomic<std::shared_ptr<PreviewPlaybackBuffer>> mPreviewBuffer{nullptr};
    std::atomic<std::shared_ptr<PreviewPlaybackBuffer>> mPreviewStartedBuffer{nullptr};
    std::atomic<std::shared_ptr<PreviewPlaybackBuffer>> mPreviewCompletedBuffer{nullptr};
    std::atomic<std::size_t> mPreviewCursor{0};
    std::vector<iplug::sample> mPreviewInputLeft;
    std::vector<iplug::sample> mPreviewInputRight;
    std::vector<iplug::sample> mPreviewOutputLeft;
    std::vector<iplug::sample> mPreviewOutputRight;

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
      double debugRms = 0.0;
      double debugRawFreq = 0.0;
    };
    std::atomic<bool> mTunerDataPending{false};
    TunerData mPendingTunerData;
    mutable std::mutex mTunerMutex;

    // Preview resource state (for temp loading from Tone3000)
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

    // Mutex to protect DSP state during model/IR loading
    // ProcessBlock (audio thread) and ApplyPreset/LoadModel/LoadIR (UI thread) share mDSP
    mutable std::mutex mDSPMutex;
    
    // Float conversion buffers for MultiPresetMixer interface
    // (iplug::sample is double, but MultiPresetMixer uses float internally)
    std::vector<float> mFloatInputLeft;
    std::vector<float> mFloatInputRight;
    std::vector<float> mFloatOutputLeft;
    std::vector<float> mFloatOutputRight;
    
    // DSP performance reporting
    int mDSPPerformanceUpdateCounter = 0;
    int mSignalDiagnosticsUpdateCounter = 0;
    std::atomic<bool> mSignalDiagnosticsEnabled{false};
    
    // Flag to prevent loading HTML multiple times
    bool mUIContentLoaded = false;
    bool mUIVisible = false;
    bool mUIReady = false;
    bool mUIReloadInProgress = false;
    int mUIReloadAttempts = 0;
    std::chrono::steady_clock::time_point mUIReloadDeadline{};
    void* mParentWindow = nullptr;
#ifdef _WIN32
    void* mWindowIconLarge = nullptr;
    void* mWindowIconSmall = nullptr;
    bool mWindowIconApplied = false;
#endif
  };
} // namespace guitarfx

using GuitarFXPlugin = guitarfx::GuitarFXPlugin;
