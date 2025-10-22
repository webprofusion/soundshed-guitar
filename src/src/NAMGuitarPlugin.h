#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
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
  explicit NAMGuitarPlugin(const iplug::InstanceInfo& info);

  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnIdle() override;
  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
  void OnParamChange(int paramIdx) override;

  struct SignalPathTestResult
  {
    double sampleRate = 0.0;
    double frequencyHz = 0.0;
    double durationSeconds = 0.0;
    double inputRMS = 0.0;
    std::array<double, 2> outputRMS {0.0, 0.0};
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
    kParamCount
  };

private:
  void InitializeParameters();
  void HandleWebViewMessages();
  void InitializeGraphics(iplug::igraphics::IGraphics& graphics);
  void HandleUIMessage(const std::string& message);
  void HandlePresetLoadRequest(const nlohmann::json& payload);
  void HandleStateRequest();
  void HandleSignalTestRequest(const nlohmann::json& payload);
  void BroadcastState();
  void ApplyPreset(namguitar::Preset& preset);
  void ReportErrorToUI(std::string_view message, std::string_view detail = {}) const;
  [[nodiscard]] std::optional<std::filesystem::path> MaterializeAttachment(const PresetAttachment& attachment) const;
  [[nodiscard]] std::filesystem::path ResolveAttachmentTarget(const PresetAttachment& attachment) const;
  [[nodiscard]] static namguitar::Preset ParsePresetFromJson(const nlohmann::json& jsonPreset);
  [[nodiscard]] static std::vector<std::uint8_t> DecodeBase64(const std::string& encoded);
  bool WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const;

  struct SignalTestRuntimeState
  {
    double frequencyHz = 0.0;
    double phase = 0.0;
    double phaseIncrement = 0.0;
    int samplesRemaining = 0;
    int totalSamples = 0;
    double sampleRate = 0.0;
    double inputSumSquares = 0.0;
    std::array<double, 2> outputSumSquares {0.0, 0.0};
  };

  std::unique_ptr<NAMDSPManager> mDSP;
  std::unique_ptr<WebUIBridge> mWebUI;
  FileSystem mFileSystem;
  ModelHasher mHasher;
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
  std::atomic<bool> mSignalTestActive {false};
  std::atomic<bool> mSignalTestResultPending {false};
};
} // namespace namguitar

using NAMGuitarPlugin = namguitar::NAMGuitarPlugin;
