#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
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

    void ProcessBlock(iplug::sample **inputs, iplug::sample **outputs, int nFrames) override;
    void OnReset() override;
    void OnIdle() override;
    void OnUIOpen() override;
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
      kParamSimpleCabEnabled,
      kParamSimpleCabBass,
      kParamSimpleCabPresence,
      kParamSimpleCabBrightness,
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
      // Delay effect parameters
      kParamDelayEnabled,
      kParamDelayTime,
      kParamDelayFeedback,
      kParamDelayMix,
      // Reverb effect parameters
      kParamReverbEnabled,
      kParamReverbDecay,
      kParamReverbDamping,
      kParamReverbMix,
      kParamCount
    };

  private:
    void InitializeParameters();
    void SendMessageToUI(const std::string& jsonMessage);
    void HandleUIMessage(const std::string &message);
    void HandlePresetLoadRequest(const nlohmann::json &payload);
    void HandleStateRequest();
    void HandleSignalTestRequest(const nlohmann::json &payload);
    void HandlePreviewDemoRequest(const nlohmann::json &payload);
    void HandleSetParameterRequest(const nlohmann::json &payload);
    void HandleLoadModelRequest(const nlohmann::json &payload);
    void HandleLoadIRRequest(const nlohmann::json &payload);
    void HandleSavePresetRequest(const nlohmann::json &payload);
    void HandleBrowseModelRequest();
    void HandleBrowseIRRequest();
    void HandleTunerRequest(const nlohmann::json &payload);
    void HandleSetInputModeRequest(const nlohmann::json &payload);
    void HandleSetAmpCabStateRequest(const nlohmann::json &payload);
    void HandleSetAutoLevelRequest(const nlohmann::json &payload);
    void HandleUpdateSignalPathNodeParamRequest(const nlohmann::json &payload);
    void HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json &payload);
    void HandleUpdateNodeResourceRequest(const nlohmann::json &payload);
    void HandleBrowseNodeResourceRequest(const nlohmann::json &payload);
    void HandleAddSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleSplitSignalPathEdgeRequest(const nlohmann::json &payload);
    void HandleCollapseSignalPathSplitRequest(const nlohmann::json &payload);
    void HandleReplaceSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleReorderSignalPathNodeRequest(const nlohmann::json &payload);
    void HandleDeleteSignalPathNodeRequest(const nlohmann::json &payload);
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
    void ApplyNodeParameter(const guitarfx::GraphNode& node, const std::string& paramKey, double value);
    void ReportErrorToUI(std::string_view message, std::string_view detail = {});
    [[nodiscard]] std::optional<std::filesystem::path> ResolveResourceRef(const ResourceRef &ref) const;
    [[nodiscard]] static std::vector<std::uint8_t> DecodeBase64(const std::string &encoded);
    bool WriteFile(const std::filesystem::path &target, const std::vector<std::uint8_t> &data) const;
    
    // App settings persistence
    void SaveAppSettings() const;
    void LoadAppSettings();
    void LoadLastSessionState();
    void LoadResourceLibraries();

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
    
    // Flag to prevent loading HTML multiple times
    bool mUIContentLoaded = false;
  };
} // namespace guitarfx

using GuitarFXPlugin = guitarfx::GuitarFXPlugin;
