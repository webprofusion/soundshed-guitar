#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "IPlug_include_in_plug_hdr.h"
#include "models/ModelHasher.h"
#include "presets/PresetTypes.h"
#include "util/FileSystem.h"

namespace iplug
{
  namespace igraphics
  {
    class IGraphics;
  } // namespace igraphics
} // namespace iplug

namespace namguitar
{
  class NAMDSPManager;
  class WebUIBridge;
  class NAMGuitarPlugin final : public iplug::Plugin
  {
  public:
    explicit NAMGuitarPlugin(const iplug::InstanceInfo &info);

    void ProcessBlock(iplug::sample **inputs, iplug::sample **outputs, int nFrames) override;
    void OnReset() override;
    void OnIdle() override;
    bool SerializeState(iplug::IByteChunk &chunk) const override;
    int UnserializeState(const iplug::IByteChunk &chunk, int startPos) override;
    void OnParamChange(int paramIdx) override;

#ifdef VST3_API
    Steinberg::tresult PLUGIN_API initialize(FUnknown* context) override;
#endif

    struct SignalPathTestResult
    {
      double sampleRate = 0.0;
      double frequencyHz = 0.0;
      double durationSeconds = 0.0;
      double inputRMS = 0.0;
      std::array<double, 2> outputRMS{0.0, 0.0};
      bool passed = false;
    };

    [[nodiscard]] bool StartSignalPathTest(double frequencyHz = 440.0, double durationSeconds = 1.0);

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
      kParamCount
    };

  private:
    void InitializeParameters();
    void HandleWebViewMessages();
    void InitializeGraphics(iplug::igraphics::IGraphics &graphics);
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
    void BroadcastState();
    void ApplyPreset(namguitar::Preset &preset);
    void ReportErrorToUI(std::string_view message, std::string_view detail = {}) const;
    [[nodiscard]] std::optional<std::filesystem::path> MaterializeAttachment(const PresetAttachment &attachment) const;
    [[nodiscard]] std::filesystem::path ResolveAttachmentTarget(const PresetAttachment &attachment) const;
    [[nodiscard]] static namguitar::Preset ParsePresetFromJson(const nlohmann::json &jsonPreset);
    [[nodiscard]] static std::vector<std::uint8_t> DecodeBase64(const std::string &encoded);
    bool WriteFile(const std::filesystem::path &target, const std::vector<std::uint8_t> &data) const;

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
    };

    std::unique_ptr<NAMDSPManager> mDSP;
    std::unique_ptr<WebUIBridge> mWebUI;
    FileSystem mFileSystem;
    ModelHasher mHasher;
    std::filesystem::path mResourceRoot;
    std::optional<Preset> mActivePreset;
    std::string mActivePresetJson;
    std::string mActivePresetId;
    std::string mActiveModelPath;
    std::string mActiveIRPath;
    bool mPendingStateBroadcast = true;
    std::vector<iplug::sample> mSignalTestInputLeft;
    std::vector<iplug::sample> mSignalTestInputRight;
    std::vector<iplug::sample> mSignalTestOutputLeft;
    std::vector<iplug::sample> mSignalTestOutputRight;
    SignalTestRuntimeState mSignalTestState;
    SignalPathTestResult mSignalTestResult;
    std::atomic<bool> mSignalTestActive{false};
    std::atomic<bool> mSignalTestResultPending{false};

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
  };
} // namespace namguitar

using NAMGuitarPlugin = namguitar::NAMGuitarPlugin;
