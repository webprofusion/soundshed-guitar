#include "GuitarFXPlugin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>
#include <iostream> // For std::cout

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#endif

#include <nlohmann/json.hpp>

#include "config.h"
#include "dsp/IRTypes.h"
#include "dsp/EffectRegistry.h"
#include "resources/ResourceLibrary.h"
#include "IPlug_include_in_plug_src.h"
#include "IPlugPaths.h"
#include "wdlstring.h"

#include "presets/PresetStorage.h"
#include "presets/PresetTypesJson.h"  // For GlobalSignalChainConfig JSON serialization
#include "dsp/effects/BuiltinEffects.h"

#ifdef _WIN32
#include "platform/WebViewDPIHelper_win.h"
#endif

namespace guitarfx
{
  namespace
  {
    constexpr const char* kSignalDiagnosticsSettingKey = "diagnostics.signalLevelsEnabled";
    constexpr const char* kMetronomeEnabledSettingKey = "metronome.enabled";
    constexpr const char* kMetronomeBpmSettingKey = "metronome.bpm";
    constexpr double kMetronomeDefaultBpm = 120.0;
    constexpr double kMetronomeMinBpm = 30.0;
    constexpr double kMetronomeMaxBpm = 300.0;
    constexpr double kMetronomeClickSeconds = 0.02;
    constexpr double kMetronomeClickFrequencyHz = 1800.0;
    constexpr double kMinDbFS = -120.0;
    constexpr double kMinLinear = 1e-6;

    double ToDbFS(double linear)
    {
      if (!std::isfinite(linear) || linear <= kMinLinear)
      {
        return kMinDbFS;
      }
      return 20.0 * std::log10(linear);
    }

    double HeadroomDbFromPeak(double peak)
    {
      const double peakDb = ToDbFS(peak);
      return std::max(0.0, -peakDb);
    }

    double ClampValue(double value, double minimum, double maximum)
    {
      return std::min(maximum, std::max(minimum, value));
    }
#ifdef APP_API
    constexpr bool kIsStandaloneBuild = true;
#else
    constexpr bool kIsStandaloneBuild = false;
#endif
    std::string SanitizeFilename(const std::string &raw)
    {
      std::string result;
      result.reserve(raw.size());
      for (unsigned char c : raw)
      {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
        {
          result.push_back(static_cast<char>(c));
        }
        else if (std::isspace(c))
        {
          result.push_back('_');
        }
      }

      if (result.empty())
      {
        return "resource";
      }

      return result;
    }

    bool IsEqNode(const GraphNode& node)
    {
      return node.type == "eq_parametric" || node.type == "eq";
    }

    GraphNode* FindEqNode(SignalGraph& graph)
    {
      for (auto& node : graph.nodes)
      {
        if (IsEqNode(node))
        {
          return &node;
        }
      }
      return nullptr;
    }

    const GraphNode* FindEqNode(const SignalGraph& graph)
    {
      for (const auto& node : graph.nodes)
      {
        if (IsEqNode(node))
        {
          return &node;
        }
      }
      return nullptr;
    }

    std::string MakeUniqueNodeId(const SignalGraph& graph, const std::string& baseId)
    {
      std::string candidate = baseId;
      int suffix = 1;
      while (graph.FindNode(candidate))
      {
        candidate = baseId + std::to_string(suffix++);
      }
      return candidate;
    }

    double GetParamValueOr(const GuitarFXPlugin& plugin, GuitarFXPlugin::ParameterId id, double fallback)
    {
      const auto* param = plugin.GetParam(static_cast<int>(id));
      return param ? param->Value() : fallback;
    }

    GraphNode BuildEqNodeFromParams(const GuitarFXPlugin& plugin, const std::string& nodeId)
    {
      GraphNode eqNode;
      eqNode.id = nodeId;
      eqNode.type = "eq_parametric";
      eqNode.category = "eq";
      eqNode.enabled = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQEnabled, 0.0) > 0.5;

      eqNode.params["lowGain"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQLowGain, 0.0);
      eqNode.params["lowFreq"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQLowFreq, 100.0);
      eqNode.params["lowMidGain"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQLowMidGain, 0.0);
      eqNode.params["lowMidFreq"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQLowMidFreq, 500.0);
      eqNode.params["lowMidQ"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQLowMidQ, 1.0);
      eqNode.params["highMidGain"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQHighMidGain, 0.0);
      eqNode.params["highMidFreq"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQHighMidFreq, 2000.0);
      eqNode.params["highMidQ"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQHighMidQ, 1.0);
      eqNode.params["highGain"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQHighGain, 0.0);
      eqNode.params["highFreq"] = GetParamValueOr(plugin, GuitarFXPlugin::kParamEQHighFreq, 8000.0);

      return eqNode;
    }

    void EnsureParametricEQNode(Preset& preset, const GuitarFXPlugin& plugin)
    {
      if (FindEqNode(preset.graph))
      {
        return;
      }

      std::string outputId;
      for (const auto& node : preset.graph.nodes)
      {
        if (node.type == kNodeTypeOutput || node.id == "output")
        {
          outputId = node.id;
          break;
        }
      }

      const std::string eqId = MakeUniqueNodeId(preset.graph, "eq");
      GraphNode eqNode = BuildEqNodeFromParams(plugin, eqId);

      preset.graph.nodes.push_back(eqNode);

      bool rewired = false;
      if (!outputId.empty())
      {
        for (auto& edge : preset.graph.edges)
        {
          if (edge.to == outputId)
          {
            edge.to = eqId;
            rewired = true;
          }
        }

        GraphEdge eqToOutput;
        eqToOutput.from = eqId;
        eqToOutput.to = outputId;
        preset.graph.edges.push_back(eqToOutput);
      }

      if (!rewired && !outputId.empty())
      {
        std::string fromId;
        for (auto it = preset.graph.nodes.rbegin(); it != preset.graph.nodes.rend(); ++it)
        {
          if (it->id != eqId && it->id != outputId)
          {
            fromId = it->id;
            break;
          }
        }

        if (!fromId.empty())
        {
          GraphEdge edge;
          edge.from = fromId;
          edge.to = eqId;
          preset.graph.edges.push_back(edge);
        }
      }
    }
  } // namespace

  namespace
  {
    constexpr int kNumPrograms = 0;
    constexpr double kTwoPi = 6.28318530717958647692;

    std::string ParamKey(GuitarFXPlugin::ParameterId paramId);

    nlohmann::json SerializeParametersToJson(const GuitarFXPlugin &plugin)
    {
      nlohmann::json parameters = nlohmann::json::array();
      for (int paramIdx = 0; paramIdx < GuitarFXPlugin::kParamCount; ++paramIdx)
      {
        const auto *param = plugin.GetParam(paramIdx);
        nlohmann::json paramJson;
        std::string key = ParamKey(static_cast<GuitarFXPlugin::ParameterId>(paramIdx));
        if (key.empty())
        {
          key = param->GetName();
        }
        paramJson["id"] = std::move(key);
        paramJson["value"] = param->Value();
        paramJson["label"] = param->GetName();
        parameters.push_back(std::move(paramJson));
      }

      nlohmann::json payload;
      payload["parameters"] = std::move(parameters);
      payload["gateEnabled"] = plugin.GetParam(GuitarFXPlugin::kParamGateEnabled)->Bool();
      payload["gateThreshold"] = plugin.GetParam(GuitarFXPlugin::kParamGateThreshold)->Value();
      return payload;
    }

    std::string ParamKey(GuitarFXPlugin::ParameterId paramId)
    {
      switch (paramId)
      {
      case GuitarFXPlugin::kParamInputTrim:
        return "input_trim";
      case GuitarFXPlugin::kParamOutputTrim:
        return "output_trim";
      case GuitarFXPlugin::kParamDrive:
        return "drive";
      case GuitarFXPlugin::kParamTone:
        return "tone";
      case GuitarFXPlugin::kParamGateEnabled:
        return "gate_enabled";
      case GuitarFXPlugin::kParamGateThreshold:
        return "gate_threshold";
      case GuitarFXPlugin::kParamMix:
        return "mix";
      case GuitarFXPlugin::kParamDoublerEnabled:
        return "doubler_enabled";
      case GuitarFXPlugin::kParamDoublerDelay:
        return "doubler_delay";
      case GuitarFXPlugin::kParamTranspose:
        return "transpose";
      case GuitarFXPlugin::kParamSimpleCabEnabled:
        return "simplecab_enabled";
      case GuitarFXPlugin::kParamSimpleCabBass:
        return "simplecab_bass";
      case GuitarFXPlugin::kParamSimpleCabPresence:
        return "simplecab_presence";
      case GuitarFXPlugin::kParamSimpleCabBrightness:
        return "simplecab_brightness";
      case GuitarFXPlugin::kParamIRQuality:
        return "ir_quality";
      case GuitarFXPlugin::kParamEQEnabled:
        return "eq_enabled";
      case GuitarFXPlugin::kParamEQLowGain:
        return "eq_low_gain";
      case GuitarFXPlugin::kParamEQLowFreq:
        return "eq_low_freq";
      case GuitarFXPlugin::kParamEQLowMidGain:
        return "eq_lowmid_gain";
      case GuitarFXPlugin::kParamEQLowMidFreq:
        return "eq_lowmid_freq";
      case GuitarFXPlugin::kParamEQLowMidQ:
        return "eq_lowmid_q";
      case GuitarFXPlugin::kParamEQHighMidGain:
        return "eq_highmid_gain";
      case GuitarFXPlugin::kParamEQHighMidFreq:
        return "eq_highmid_freq";
      case GuitarFXPlugin::kParamEQHighMidQ:
        return "eq_highmid_q";
      case GuitarFXPlugin::kParamEQHighGain:
        return "eq_high_gain";
      case GuitarFXPlugin::kParamEQHighFreq:
        return "eq_high_freq";
      // Delay effect
      case GuitarFXPlugin::kParamDelayEnabled:
        return "delay_enabled";
      case GuitarFXPlugin::kParamDelayTime:
        return "delay_time";
      case GuitarFXPlugin::kParamDelayFeedback:
        return "delay_feedback";
      case GuitarFXPlugin::kParamDelayMix:
        return "delay_mix";
      // Reverb effect
      case GuitarFXPlugin::kParamReverbEnabled:
        return "reverb_enabled";
      case GuitarFXPlugin::kParamReverbDecay:
        return "reverb_decay";
      case GuitarFXPlugin::kParamReverbDamping:
        return "reverb_damping";
      case GuitarFXPlugin::kParamReverbMix:
        return "reverb_mix";
      default:
        return "";
      }
    }

    std::optional<GuitarFXPlugin::ParameterId> ParamIdFromKey(const std::string &key)
    {
      if (key == "input_trim")
      {
        return GuitarFXPlugin::kParamInputTrim;
      }
      if (key == "output_trim")
      {
        return GuitarFXPlugin::kParamOutputTrim;
      }
      if (key == "drive")
      {
        return GuitarFXPlugin::kParamDrive;
      }
      if (key == "tone")
      {
        return GuitarFXPlugin::kParamTone;
      }
      if (key == "gate_enabled")
      {
        return GuitarFXPlugin::kParamGateEnabled;
      }
      if (key == "gate_threshold")
      {
        return GuitarFXPlugin::kParamGateThreshold;
      }
      if (key == "mix")
      {
        return GuitarFXPlugin::kParamMix;
      }
      if (key == "doubler_enabled")
      {
        return GuitarFXPlugin::kParamDoublerEnabled;
      }
      if (key == "doubler_delay")
      {
        return GuitarFXPlugin::kParamDoublerDelay;
      }
      if (key == "transpose")
      {
        return GuitarFXPlugin::kParamTranspose;
      }
      if (key == "simplecab_enabled")
      {
        return GuitarFXPlugin::kParamSimpleCabEnabled;
      }
      if (key == "simplecab_bass")
      {
        return GuitarFXPlugin::kParamSimpleCabBass;
      }
      if (key == "simplecab_presence")
      {
        return GuitarFXPlugin::kParamSimpleCabPresence;
      }
      if (key == "simplecab_brightness")
      {
        return GuitarFXPlugin::kParamSimpleCabBrightness;
      }
      if (key == "ir_quality")
      {
        return GuitarFXPlugin::kParamIRQuality;
      }
      if (key == "eq_enabled")
      {
        return GuitarFXPlugin::kParamEQEnabled;
      }
      if (key == "eq_low_gain")
      {
        return GuitarFXPlugin::kParamEQLowGain;
      }
      if (key == "eq_low_freq")
      {
        return GuitarFXPlugin::kParamEQLowFreq;
      }
      if (key == "eq_lowmid_gain")
      {
        return GuitarFXPlugin::kParamEQLowMidGain;
      }
      if (key == "eq_lowmid_freq")
      {
        return GuitarFXPlugin::kParamEQLowMidFreq;
      }
      if (key == "eq_lowmid_q")
      {
        return GuitarFXPlugin::kParamEQLowMidQ;
      }
      if (key == "eq_highmid_gain")
      {
        return GuitarFXPlugin::kParamEQHighMidGain;
      }
      if (key == "eq_highmid_freq")
      {
        return GuitarFXPlugin::kParamEQHighMidFreq;
      }
      if (key == "eq_highmid_q")
      {
        return GuitarFXPlugin::kParamEQHighMidQ;
      }
      if (key == "eq_high_gain")
      {
        return GuitarFXPlugin::kParamEQHighGain;
      }
      if (key == "eq_high_freq")
      {
        return GuitarFXPlugin::kParamEQHighFreq;
      }
      // Delay effect
      if (key == "delay_enabled")
      {
        return GuitarFXPlugin::kParamDelayEnabled;
      }
      if (key == "delay_time")
      {
        return GuitarFXPlugin::kParamDelayTime;
      }
      if (key == "delay_feedback")
      {
        return GuitarFXPlugin::kParamDelayFeedback;
      }
      if (key == "delay_mix")
      {
        return GuitarFXPlugin::kParamDelayMix;
      }
      // Reverb effect
      if (key == "reverb_enabled")
      {
        return GuitarFXPlugin::kParamReverbEnabled;
      }
      if (key == "reverb_decay")
      {
        return GuitarFXPlugin::kParamReverbDecay;
      }
      if (key == "reverb_damping")
      {
        return GuitarFXPlugin::kParamReverbDamping;
      }
      if (key == "reverb_mix")
      {
        return GuitarFXPlugin::kParamReverbMix;
      }
      return std::nullopt;
    }

    std::uint32_t ReadUint32LE(const std::uint8_t *data)
    {
      return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) | (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
    }

    std::uint16_t ReadUint16LE(const std::uint8_t *data)
    {
      return static_cast<std::uint16_t>(data[0]) | (static_cast<std::uint16_t>(data[1]) << 8U);
    }

    struct DecodedWav
    {
      double sampleRate = 0.0;
      int channels = 0;
      int bitsPerSample = 0;
      std::vector<std::vector<double>> channelSamples;
    };

    std::optional<DecodedWav> DecodePcmWav(const std::vector<std::uint8_t> &bytes)
    {
      if (bytes.size() < 44)
      {
        return std::nullopt;
      }

      if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
      {
        return std::nullopt;
      }

      std::size_t offset = 12;
      std::uint16_t audioFormat = 0;
      std::uint16_t channels = 0;
      std::uint32_t sampleRate = 0;
      std::uint16_t bitsPerSample = 0;
      std::uint16_t blockAlign = 0;
      std::size_t dataOffset = 0;
      std::uint32_t dataSize = 0;

      while (offset + 8 <= bytes.size())
      {
        const char *chunkHeader = reinterpret_cast<const char *>(bytes.data() + offset);
        const std::string chunkId(chunkHeader, chunkHeader + 4);
        const std::uint32_t chunkSize = ReadUint32LE(bytes.data() + offset + 4);
        const std::size_t chunkDataStart = offset + 8;

        if (chunkDataStart + chunkSize > bytes.size())
        {
          return std::nullopt;
        }

        if (chunkId == "fmt ")
        {
          audioFormat = ReadUint16LE(bytes.data() + chunkDataStart);
          channels = ReadUint16LE(bytes.data() + chunkDataStart + 2);
          sampleRate = ReadUint32LE(bytes.data() + chunkDataStart + 4);
          blockAlign = ReadUint16LE(bytes.data() + chunkDataStart + 12);
          bitsPerSample = ReadUint16LE(bytes.data() + chunkDataStart + 14);
        }
        else if (chunkId == "data")
        {
          dataOffset = chunkDataStart;
          dataSize = chunkSize;
          break;
        }

        offset = chunkDataStart + chunkSize + (chunkSize % 2);
      }

      if (audioFormat == 0 || channels == 0 || sampleRate == 0 || bitsPerSample == 0 || blockAlign == 0 || dataOffset == 0)
      {
        return std::nullopt;
      }

      const std::size_t bytesPerSample = static_cast<std::size_t>(bitsPerSample) / 8;
      if (bytesPerSample == 0)
      {
        return std::nullopt;
      }

      const std::size_t frameCount = dataSize / blockAlign;
      if (frameCount == 0)
      {
        return std::nullopt;
      }

      DecodedWav wav;
      wav.sampleRate = static_cast<double>(sampleRate);
      wav.channels = static_cast<int>(channels);
      wav.bitsPerSample = static_cast<int>(bitsPerSample);
      wav.channelSamples.assign(static_cast<std::size_t>(channels), std::vector<double>(frameCount, 0.0));

      const bool isFloat = (audioFormat == 3);
      for (std::size_t frame = 0; frame < frameCount; ++frame)
      {
        const std::size_t frameOffset = dataOffset + frame * blockAlign;
        for (std::size_t channel = 0; channel < static_cast<std::size_t>(channels); ++channel)
        {
          const std::size_t sampleOffset = frameOffset + channel * bytesPerSample;
          if (sampleOffset + bytesPerSample > dataOffset + dataSize)
          {
            return std::nullopt;
          }

          double sample = 0.0;
          if (isFloat)
          {
            if (bitsPerSample == 32)
            {
              float value = 0.0f;
              std::memcpy(&value, bytes.data() + sampleOffset, sizeof(float));
              sample = static_cast<double>(value);
            }
            else if (bitsPerSample == 64)
            {
              double value = 0.0;
              std::memcpy(&value, bytes.data() + sampleOffset, sizeof(double));
              sample = value;
            }
            else
            {
              return std::nullopt;
            }
          }
          else
          {
            switch (bitsPerSample)
            {
            case 8:
            {
              const int value = static_cast<int>(bytes[sampleOffset]);
              sample = (static_cast<double>(value) - 128.0) / 128.0;
              break;
            }
            case 16:
            {
              const auto value = static_cast<std::int16_t>(ReadUint16LE(bytes.data() + sampleOffset));
              sample = static_cast<double>(value) / 32768.0;
              break;
            }
            case 24:
            {
              std::int32_t value = static_cast<std::int32_t>(bytes[sampleOffset]) | (static_cast<std::int32_t>(bytes[sampleOffset + 1]) << 8) | (static_cast<std::int32_t>(bytes[sampleOffset + 2]) << 16);
              if (value & 0x800000)
              {
                value |= ~0xFFFFFF;
              }
              sample = static_cast<double>(value) / 8388608.0;
              break;
            }
            case 32:
            {
              const auto value = static_cast<std::int32_t>(ReadUint32LE(bytes.data() + sampleOffset));
              sample = static_cast<double>(value) / 2147483648.0;
              break;
            }
            default:
              return std::nullopt;
            }
          }

          wav.channelSamples[channel][frame] = std::clamp(sample, -1.0, 1.0);
        }
      }

      return wav;
    }

    std::vector<std::vector<iplug::sample>> ConvertToSampleRate(const DecodedWav &wav, double targetRate)
    {
      if (wav.channelSamples.empty() || wav.channelSamples.front().empty())
      {
        return {};
      }

      const double sourceRate = wav.sampleRate > 0.0 ? wav.sampleRate : targetRate;
      if (sourceRate <= 0.0)
      {
        return {};
      }

      const std::size_t channelCount = std::max<std::size_t>(1, wav.channelSamples.size());
      const std::size_t sourceFrames = wav.channelSamples.front().size();
      std::vector<std::vector<iplug::sample>> output(channelCount);

      if (targetRate <= 0.0 || std::fabs(sourceRate - targetRate) < 1e-6)
      {
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
          const std::size_t sourceChannel = std::min(channel, wav.channelSamples.size() - 1);
          output[channel].resize(sourceFrames);
          for (std::size_t frame = 0; frame < sourceFrames; ++frame)
          {
            output[channel][frame] = static_cast<iplug::sample>(std::clamp(wav.channelSamples[sourceChannel][frame], -1.0, 1.0));
          }
        }
        return output;
      }

      const double ratio = targetRate / sourceRate;
      const std::size_t destFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(sourceFrames) * ratio)));

      for (std::size_t channel = 0; channel < channelCount; ++channel)
      {
        const std::size_t sourceChannel = std::min(channel, wav.channelSamples.size() - 1);
        output[channel].resize(destFrames);
        for (std::size_t frame = 0; frame < destFrames; ++frame)
        {
          const double sourcePosition = (static_cast<double>(frame) * sourceRate) / targetRate;
          const std::size_t index = std::min<std::size_t>(static_cast<std::size_t>(sourcePosition), sourceFrames - 1);
          const std::size_t nextIndex = std::min<std::size_t>(index + 1, sourceFrames - 1);
          const double fraction = std::clamp(sourcePosition - static_cast<double>(index), 0.0, 1.0);
          const double sample0 = wav.channelSamples[sourceChannel][index];
          const double sample1 = wav.channelSamples[sourceChannel][nextIndex];
          const double value = sample0 + (sample1 - sample0) * fraction;
          output[channel][frame] = static_cast<iplug::sample>(std::clamp(value, -1.0, 1.0));
        }
      }

      return output;
    }

  } // namespace

    GuitarFXPlugin::GuitarFXPlugin(const iplug::InstanceInfo &info)
      : iplug::Plugin(info, iplug::MakeConfig(kParamCount, kNumPrograms))
  {
    // Register all built-in effects and force NAM factory registration
    // This must be called before loading any presets
    RegisterAllEffects();

    // Write to a log file to verify execution
    FILE* logFile = fopen("c:\\temp\\plugin_log.txt", "a");
    if (logFile) {
      fprintf(logFile, "[Plugin] Constructor called at %llu, gHINSTANCE=%p\n", (unsigned long long)time(NULL), (void*)::gHINSTANCE);
      fclose(logFile);
    }

    std::cout << "[Plugin] Constructor called, gHINSTANCE=" << (void*)::gHINSTANCE << std::endl;

    // Initialize the resource root early so preset loading can find bundled assets
    WDL_String bundlePath;
    iplug::BundleResourcePath(bundlePath, ::gHINSTANCE);
    if (bundlePath.GetLength() == 0)
    {
      iplug::HostPath(bundlePath, nullptr);
      bundlePath.Append("resources\\");
    }
    mResourceRoot = std::filesystem::path{bundlePath.Get()};
    std::cout << "[Plugin] Resource root set to: " << mResourceRoot.generic_string() << std::endl;

    // Load resource libraries (NAM models, IRs) from JSON files
    LoadResourceLibraries();

    // Share the plugin's resource library with the multi-preset mixer
    mPresetMixer.SetResourceLibrary(&mResourceLibrary);

    // Set up tuner callback to forward results to UI
    mPresetMixer.SetTunerCallback([this](const MultiPresetMixer::TunerResult &result) {
      // Store tuner data for sending in OnIdle (thread-safe handoff from audio thread)
      {
        std::lock_guard<std::mutex> lock(mTunerMutex);
        mPendingTunerData.detected = result.detected;
        mPendingTunerData.noteName = result.noteName;
        mPendingTunerData.octave = result.octave;
        mPendingTunerData.frequency = result.frequency;
        mPendingTunerData.centOffset = result.centOffset;
        mPendingTunerData.confidence = result.confidence;
        mPendingTunerData.debugRms = result.debugRms;
        mPendingTunerData.debugRawFreq = result.debugRawFreq;
      }
      mTunerDataPending.store(true, std::memory_order_release);
    });

    InitializeParameters();

    for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
    {
      OnParamChange(paramIdx);
    }

    // Load last session state (preset, parameters, model, IR) from settings file
    LoadLastSessionState();

#if PLUG_HAS_UI
    // Enable WebView developer tools for debugging
    SetEnableDevTools(true);
    
    // WebViewEditorDelegate pattern: mEditorInitFunc is called when the editor window is opened.
    // This is the correct lifecycle point to load HTML content into the WebView.
    // The base class (WebViewEditorDelegate) provides LoadFile, EnableScroll, etc.
    mEditorInitFunc = [this]()
    {
      std::cout << "[Plugin] mEditorInitFunc called - delegating to OnUIOpen" << std::endl;
      OnUIOpen();
    };
#endif
  }

#ifdef VST3_API
  Steinberg::tresult PLUGIN_API GuitarFXPlugin::initialize(FUnknown* context)
  {
    std::cout << "[Plugin] initialize called" << std::endl;
    return iplug::Plugin::initialize(context);
  }
#endif

  void GuitarFXPlugin::ProcessBlock(iplug::sample **inputs, iplug::sample **outputs, int nFrames)
  {
    // Try to acquire the DSP mutex. If we can't (e.g., model/IR is being loaded),
    // output silence to avoid crashes. This prevents blocking the audio thread.
    std::unique_lock<std::mutex> lock(mDSPMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
      // DSP is being modified, output silence
      if (outputs && outputs[0] && outputs[1])
      {
        std::fill(outputs[0], outputs[0] + nFrames, static_cast<iplug::sample>(0));
        std::fill(outputs[1], outputs[1] + nFrames, static_cast<iplug::sample>(0));
      }
      return;
    }

    if (auto previewBuffer = mPreviewBuffer.load(std::memory_order_acquire))
    {
      if (!outputs || !outputs[0] || !outputs[1])
      {
        mPreviewBuffer.store(nullptr, std::memory_order_release);
        mPreviewCompletedBuffer.store(previewBuffer, std::memory_order_release);
        return;
      }

      if (previewBuffer->channelSamples.empty() || previewBuffer->channelSamples.front().empty())
      {
        std::fill(outputs[0], outputs[0] + nFrames, static_cast<iplug::sample>(0.0f));
        std::fill(outputs[1], outputs[1] + nFrames, static_cast<iplug::sample>(0.0f));
        mPreviewBuffer.store(nullptr, std::memory_order_release);
        mPreviewCompletedBuffer.store(previewBuffer, std::memory_order_release);
        mPreviewCursor.store(0, std::memory_order_release);
        return;
      }

      const std::size_t channelCount = std::max<std::size_t>(1, previewBuffer->channelSamples.size());
      const std::size_t totalFrames = previewBuffer->channelSamples.front().size();
      std::size_t cursor = mPreviewCursor.load(std::memory_order_acquire);

      if (cursor >= totalFrames)
      {
        std::fill(outputs[0], outputs[0] + nFrames, static_cast<iplug::sample>(0.0f));
        std::fill(outputs[1], outputs[1] + nFrames, static_cast<iplug::sample>(0.0f));
        mPreviewBuffer.store(nullptr, std::memory_order_release);
        mPreviewCompletedBuffer.store(previewBuffer, std::memory_order_release);
        mPreviewCursor.store(0, std::memory_order_release);
        return;
      }

      const std::size_t framesRemaining = totalFrames - cursor;
      const std::size_t framesToProcessSize = std::min<std::size_t>(framesRemaining, static_cast<std::size_t>(nFrames));
      const int framesToProcess = static_cast<int>(framesToProcessSize);

      const std::size_t leftChannelFrames = previewBuffer->channelSamples.front().size();
      const std::size_t rightChannelIndex = channelCount > 1 ? 1 : 0;
      const std::size_t rightChannelFrames = previewBuffer->channelSamples[rightChannelIndex].size();

      mPreviewInputLeft.resize(static_cast<std::size_t>(nFrames));
      mPreviewInputRight.resize(static_cast<std::size_t>(nFrames));
      mPreviewOutputLeft.resize(static_cast<std::size_t>(nFrames));
      mPreviewOutputRight.resize(static_cast<std::size_t>(nFrames));

      std::fill(mPreviewInputLeft.begin(), mPreviewInputLeft.end(), static_cast<iplug::sample>(0.0f));
      std::fill(mPreviewInputRight.begin(), mPreviewInputRight.end(), static_cast<iplug::sample>(0.0f));

      for (int frame = 0; frame < framesToProcess; ++frame)
      {
        const std::size_t sourceIndex = cursor + static_cast<std::size_t>(frame);
        const std::size_t leftChannel = 0;
        const std::size_t rightChannel = rightChannelIndex;
        mPreviewInputLeft[static_cast<std::size_t>(frame)] = sourceIndex < leftChannelFrames
          ? previewBuffer->channelSamples[leftChannel][sourceIndex]
          : static_cast<iplug::sample>(0.0f);
        mPreviewInputRight[static_cast<std::size_t>(frame)] = sourceIndex < rightChannelFrames
          ? previewBuffer->channelSamples[rightChannel][sourceIndex]
          : static_cast<iplug::sample>(0.0f);
      }

      iplug::sample *previewInputs[2] = {mPreviewInputLeft.data(), mPreviewInputRight.data()};
      iplug::sample *previewOutputs[2] = {mPreviewOutputLeft.data(), mPreviewOutputRight.data()};
      ProcessThroughGlobalChain(previewInputs, previewOutputs, nFrames);

      for (int frame = 0; frame < nFrames; ++frame)
      {
        outputs[0][frame] = mPreviewOutputLeft[static_cast<std::size_t>(frame)];
        outputs[1][frame] = mPreviewOutputRight[static_cast<std::size_t>(frame)];
      }

      cursor += framesToProcessSize;
      if (cursor >= totalFrames)
      {
        mPreviewBuffer.store(nullptr, std::memory_order_release);
        mPreviewCompletedBuffer.store(previewBuffer, std::memory_order_release);
        mPreviewCursor.store(0, std::memory_order_release);
      }
      else
      {
        mPreviewCursor.store(cursor, std::memory_order_release);
      }

      return;
    }

    if (mSignalTestActive.load(std::memory_order_acquire))
    {
      const int channels = 2;
      if (!outputs || !outputs[0] || !outputs[1])
      {
        return;
      }

      auto &state = mSignalTestState;
      const int framesToProcess = std::min(nFrames, state.samplesRemaining);

      mSignalTestInputLeft.resize(static_cast<std::size_t>(nFrames));
      mSignalTestInputRight.resize(static_cast<std::size_t>(nFrames));
      mSignalTestOutputLeft.resize(static_cast<std::size_t>(nFrames));
      mSignalTestOutputRight.resize(static_cast<std::size_t>(nFrames));

      for (int frame = 0; frame < framesToProcess; ++frame)
      {
        const double sample = std::sin(state.phase) * 0.5;
        state.phase += state.phaseIncrement;
        if (state.phase >= kTwoPi)
        {
          state.phase -= kTwoPi;
        }

        mSignalTestInputLeft[static_cast<std::size_t>(frame)] = static_cast<iplug::sample>(sample);
        mSignalTestInputRight[static_cast<std::size_t>(frame)] = static_cast<iplug::sample>(sample);
        state.inputSumSquares += sample * sample;
      }

      for (int frame = framesToProcess; frame < nFrames; ++frame)
      {
        mSignalTestInputLeft[static_cast<std::size_t>(frame)] = 0.0f;
        mSignalTestInputRight[static_cast<std::size_t>(frame)] = 0.0f;
      }

      iplug::sample *testInputs[channels] = {mSignalTestInputLeft.data(), mSignalTestInputRight.data()};
      iplug::sample *testOutputs[channels] = {mSignalTestOutputLeft.data(), mSignalTestOutputRight.data()};
      ProcessThroughGlobalChain(testInputs, testOutputs, nFrames);

      for (int frame = 0; frame < framesToProcess; ++frame)
      {
        const double left = static_cast<double>(mSignalTestOutputLeft[static_cast<std::size_t>(frame)]);
        const double right = static_cast<double>(mSignalTestOutputRight[static_cast<std::size_t>(frame)]);
        state.outputSumSquares[0] += left * left;
        state.outputSumSquares[1] += right * right;
      }

      for (int frame = 0; frame < nFrames; ++frame)
      {
        outputs[0][frame] = mSignalTestOutputLeft[static_cast<std::size_t>(frame)];
        outputs[1][frame] = mSignalTestOutputRight[static_cast<std::size_t>(frame)];
      }

      state.samplesRemaining -= framesToProcess;
      if (state.samplesRemaining <= 0)
      {
        state.samplesRemaining = 0;
        const auto endTime = std::chrono::steady_clock::now();
        const double totalFrames = std::max(1.0, static_cast<double>(state.totalSamples));
        mSignalTestResult.frequencyHz = state.frequencyHz;
        mSignalTestResult.sampleRate = state.sampleRate;
        mSignalTestResult.durationSeconds = static_cast<double>(state.totalSamples) / std::max(1.0, state.sampleRate);
        mSignalTestResult.elapsedSeconds = std::chrono::duration<double>(endTime - state.startTime).count();
        mSignalTestResult.inputRMS = std::sqrt(state.inputSumSquares / totalFrames);
        mSignalTestResult.outputRMS[0] = std::sqrt(state.outputSumSquares[0] / totalFrames);
        mSignalTestResult.outputRMS[1] = std::sqrt(state.outputSumSquares[1] / totalFrames);
        mSignalTestResult.passed = (mSignalTestResult.outputRMS[0] > 1e-4) || (mSignalTestResult.outputRMS[1] > 1e-4);

        mSignalTestActive.store(false, std::memory_order_release);
        mSignalTestResultPending.store(true, std::memory_order_release);
      }

      return;
    }

    ProcessThroughGlobalChain(inputs, outputs, nFrames);
    RenderMetronome(outputs, nFrames);
  }

  void GuitarFXPlugin::ProcessThroughGlobalChain(iplug::sample **inputs, iplug::sample **outputs, int nFrames)
  {
    // All audio processing now goes through MultiPresetMixer exclusively
    // Per-preset FX (gate, transpose, doubler) are handled inside the mixer
    
    // Convert from iplug::sample (double) to float for MultiPresetMixer
    // This is necessary because iPlug2 uses double samples internally by default,
    // but the NAM core and our DSP chain use floats for performance.
    const std::size_t frames = static_cast<std::size_t>(nFrames);
    
    // Ensure buffers are large enough (should be set in OnReset, but safety check)
    if (mFloatInputLeft.size() < frames)
    {
      mFloatInputLeft.resize(frames);
      mFloatInputRight.resize(frames);
      mFloatOutputLeft.resize(frames);
      mFloatOutputRight.resize(frames);
    }
    
    // Convert input from double to float
    if (inputs && inputs[0])
    {
      for (std::size_t i = 0; i < frames; ++i)
      {
        mFloatInputLeft[i] = static_cast<float>(inputs[0][i]);
      }
    }
    if (inputs && inputs[1])
    {
      for (std::size_t i = 0; i < frames; ++i)
      {
        mFloatInputRight[i] = static_cast<float>(inputs[1][i]);
      }
    }
    
    // Set up float pointers for the mixer
    float* floatInputs[2] = { mFloatInputLeft.data(), mFloatInputRight.data() };
    float* floatOutputs[2] = { mFloatOutputLeft.data(), mFloatOutputRight.data() };
    
    // Process through the mixer
    mPresetMixer.Process(floatInputs, floatOutputs, nFrames);
    
    // Convert output from float back to double
    if (outputs && outputs[0])
    {
      for (std::size_t i = 0; i < frames; ++i)
      {
        outputs[0][i] = static_cast<iplug::sample>(mFloatOutputLeft[i]);
      }
    }
    if (outputs && outputs[1])
    {
      for (std::size_t i = 0; i < frames; ++i)
      {
        outputs[1][i] = static_cast<iplug::sample>(mFloatOutputRight[i]);
      }
    }
  }

  double GuitarFXPlugin::GetEffectiveTempoBpm() const
  {
    if (kIsStandaloneBuild)
    {
      return ClampValue(mMetronomeBpm.load(std::memory_order_relaxed), kMetronomeMinBpm, kMetronomeMaxBpm);
    }

    const double hostTempo = GetTempo();
    if (hostTempo > 0.0)
    {
      return ClampValue(hostTempo, kMetronomeMinBpm, kMetronomeMaxBpm);
    }

    return kMetronomeDefaultBpm;
  }

  void GuitarFXPlugin::RenderMetronome(iplug::sample **outputs, int nFrames)
  {
    if (!outputs || !outputs[0] || !outputs[1])
    {
      return;
    }

    if (!kIsStandaloneBuild)
    {
      return;
    }

    const bool enabled = mMetronomeEnabled.load(std::memory_order_relaxed);
    if (!enabled)
    {
      return;
    }

    if (mMetronomeResetPending.exchange(false, std::memory_order_acq_rel))
    {
      mMetronomeSamplesUntilClick = 0.0;
      mMetronomeClickSamplesRemaining = 0;
      mMetronomeClickPhase = 0.0;
    }

    const double bpm = GetEffectiveTempoBpm();
    const double sampleRate = GetSampleRate();
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, bpm));
    const int clickSamples = std::max(1, static_cast<int>(sampleRate * kMetronomeClickSeconds));

    if (sampleRate > 0.0)
    {
      mMetronomeClickPhaseIncrement = kTwoPi * kMetronomeClickFrequencyHz / sampleRate;
    }

    const double volume = ClampValue(mMetronomeVolume.load(std::memory_order_relaxed), 0.0, 1.0);

    for (int frame = 0; frame < nFrames; ++frame)
    {
      if (mMetronomeSamplesUntilClick <= 0.0)
      {
        mMetronomeClickSamplesRemaining = clickSamples;
        mMetronomeSamplesUntilClick += samplesPerBeat;
      }

      float clickSample = 0.0f;
      if (mMetronomeClickSamplesRemaining > 0)
      {
        const double envelope = static_cast<double>(mMetronomeClickSamplesRemaining) / static_cast<double>(clickSamples);
        clickSample = static_cast<float>(std::sin(mMetronomeClickPhase) * envelope * volume);
        mMetronomeClickPhase += mMetronomeClickPhaseIncrement;
        if (mMetronomeClickPhase >= kTwoPi)
        {
          mMetronomeClickPhase -= kTwoPi;
        }
        --mMetronomeClickSamplesRemaining;
      }

      outputs[0][frame] += clickSample;
      outputs[1][frame] += clickSample;
      mMetronomeSamplesUntilClick -= 1.0;
    }
  }

  void GuitarFXPlugin::OnReset()
  {
    // Lock DSP mutex during prepare/reset
    std::lock_guard<std::mutex> lock(mDSPMutex);

    mPresetMixer.Prepare(GetSampleRate(), GetBlockSize());

    // Resize float conversion buffers for MultiPresetMixer interface
    const int blockSize = GetBlockSize();
    mFloatInputLeft.resize(static_cast<std::size_t>(blockSize));
    mFloatInputRight.resize(static_cast<std::size_t>(blockSize));
    mFloatOutputLeft.resize(static_cast<std::size_t>(blockSize));
    mFloatOutputRight.resize(static_cast<std::size_t>(blockSize));

    mPreviewBuffer.store(nullptr, std::memory_order_release);
    mPreviewStartedBuffer.store(nullptr, std::memory_order_release);
    mPreviewCompletedBuffer.store(nullptr, std::memory_order_release);
    mPreviewCursor.store(0, std::memory_order_release);
    mMetronomeSamplesUntilClick = 0.0;
    mMetronomeClickSamplesRemaining = 0;
    mMetronomeClickPhase = 0.0;
  }
  // ============================
  // MultiPresetMixer controller
  // ============================
  bool GuitarFXPlugin::AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name)
  {
    std::lock_guard<std::mutex> lock(mDSPMutex);
    return mPresetMixer.AddActivePreset(preset, presetId, name);
  }

  void GuitarFXPlugin::RemoveActivePreset(const std::string& presetId)
  {
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.RemoveActivePreset(presetId);
  }

  void GuitarFXPlugin::SetActivePresetMix(const std::string& presetId, double value)
  {
    mPresetMixer.SetPresetMix(presetId, value);
  }

  void GuitarFXPlugin::SetActivePresetPan(const std::string& presetId, double pan)
  {
    mPresetMixer.SetPresetPan(presetId, pan);
  }

  void GuitarFXPlugin::SetActivePresetMute(const std::string& presetId, bool mute)
  {
    mPresetMixer.SetPresetMute(presetId, mute);
  }

  void GuitarFXPlugin::SetActivePresetSolo(const std::string& presetId, bool solo)
  {
    mPresetMixer.SetPresetSolo(presetId, solo);
  }

  void GuitarFXPlugin::SetMasterMixGain(double value)
  {
    mPresetMixer.SetMasterGain(value);
  }

  void GuitarFXPlugin::SetMixLimiterEnabled(bool enabled)
  {
    mPresetMixer.SetLimiterEnabled(enabled);
  }

  void GuitarFXPlugin::OnIdle()
  {
    static int idleCount = 0;
    idleCount++;
    if (idleCount % 100 == 0) {
      FILE* logFile = fopen("c:\\temp\\plugin_log.txt", "a");
      if (logFile) {
        fprintf(logFile, "[Plugin] OnIdle count=%d\n", idleCount);
        fclose(logFile);
      }
    }

    // Handle preview started notification
    if (auto started = mPreviewStartedBuffer.exchange(nullptr, std::memory_order_acq_rel))
    {
      nlohmann::json message;
      message["type"] = "previewStarted";
      if (!started->id.empty())
      {
        message["id"] = started->id;
      }
      if (!started->title.empty())
      {
        message["title"] = started->title;
      }
      const double duration = (started->sampleRate > 0.0 && !started->channelSamples.empty())
                                  ? static_cast<double>(started->channelSamples.front().size()) / started->sampleRate
                                  : 0.0;
      message["duration"] = duration;
      SendMessageToUI(message.dump());
    }

    // Handle preview completed notification
    if (auto completed = mPreviewCompletedBuffer.exchange(nullptr, std::memory_order_acq_rel))
    {
      nlohmann::json message;
      message["type"] = "previewComplete";
      if (!completed->id.empty())
      {
        message["id"] = completed->id;
      }
      if (!completed->title.empty())
      {
        message["title"] = completed->title;
      }
      const double duration = (completed->sampleRate > 0.0 && !completed->channelSamples.empty())
                                  ? static_cast<double>(completed->channelSamples.front().size()) / completed->sampleRate
                                  : 0.0;
      message["duration"] = duration;
      SendMessageToUI(message.dump());
    }

    // Handle signal test result
    if (mSignalTestResultPending.exchange(false, std::memory_order_acq_rel))
    {
      nlohmann::json message;
      message["type"] = "signalPathTestResult";
      message["frequency"] = mSignalTestResult.frequencyHz;
      message["duration"] = mSignalTestResult.durationSeconds;
      message["elapsed"] = mSignalTestResult.elapsedSeconds;
      message["sampleRate"] = mSignalTestResult.sampleRate;
      message["inputRMS"] = mSignalTestResult.inputRMS;
      message["outputRMS"] = {mSignalTestResult.outputRMS[0], mSignalTestResult.outputRMS[1]};
      message["passed"] = mSignalTestResult.passed;
      if (!mSignalTestResult.passed)
      {
        message["message"] = "Signal path test did not produce any output";
      }
      SendMessageToUI(message.dump());
    }

    // Send tuner data updates to UI
    if (mTunerDataPending.exchange(false, std::memory_order_acq_rel))
    {
      std::cout << "[Plugin] OnIdle: tuner data pending, mTunerActive=" << mTunerActive.load() << std::endl;
      if (mTunerActive.load(std::memory_order_acquire))
      {
        TunerData data;
        {
          std::lock_guard<std::mutex> lock(mTunerMutex);
          data = mPendingTunerData;
        }

        std::cout << "[Plugin] OnIdle: Sending tuner update, detected=" << data.detected 
                  << ", note=" << data.noteName << ", freq=" << data.frequency << std::endl;

        nlohmann::json message;
        message["type"] = "tunerUpdate";
        message["detected"] = data.detected;
        message["debugRms"] = data.debugRms;
        message["debugRawFreq"] = data.debugRawFreq;
        if (data.detected)
        {
          message["noteName"] = data.noteName;
          message["octave"] = data.octave;
          message["frequency"] = data.frequency;
          message["centOffset"] = data.centOffset;
          message["confidence"] = data.confidence;
        }
        SendMessageToUI(message.dump());
      }
    }

    if (mPendingStateBroadcast)
    {
      BroadcastState();
    }

    if (mUIVisible && !mUIReady && mUIReloadAttempts > 0)
    {
      if (std::chrono::steady_clock::now() > mUIReloadDeadline)
      {
        if (mUIReloadAttempts < 3)
        {
          std::cout << "[Plugin] UI not ready; reloading WebView (attempt " << (mUIReloadAttempts + 1) << ")" << std::endl;
          mUIReloadInProgress = false;
          mUIReloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          LoadWebViewContent(true);
        }
        else
        {
          std::cout << "[Plugin] UI failed to become ready after retries" << std::endl;
          mUIReloadAttempts = 0;
          mUIReloadInProgress = false;
        }
      }
    }

    if (mUIVisible)
    {
      mMetronomeUpdateCounter++;
      if (mMetronomeUpdateCounter >= 60)
      {
        mMetronomeUpdateCounter = 0;
        const double bpm = GetEffectiveTempoBpm();
        if (std::abs(bpm - mLastBroadcastTempo) > 0.01)
        {
          mLastBroadcastTempo = bpm;
          SendMetronomeStateToUI();
        }
      }
    }

    // Send DSP performance stats to UI periodically (every ~1 second)
    mDSPPerformanceUpdateCounter++;
    if (mDSPPerformanceUpdateCounter >= 60)
    {
      mDSPPerformanceUpdateCounter = 0;
      
      // Get aggregated stats from MultiPresetMixer
      SignalGraphExecutor::DSPPerformanceStats stats = mPresetMixer.GetPerformanceStats();
      
      nlohmann::json message = {
        {"type", "dspPerformance"},
        {"stats", {
          {"totalProcessingTimeUs", stats.totalProcessingTimeUs},
          {"realTimeUs", stats.realTimeUs},
          {"dspLoadPercent", stats.dspLoadPercent},
          {"nodeProcessingTimesUs", stats.nodeProcessingTimesUs}
        }}
      };
      SendMessageToUI(message.dump());
    }

    if (mSignalDiagnosticsEnabled.load(std::memory_order_acquire))
    {
      mSignalDiagnosticsUpdateCounter++;
      if (mSignalDiagnosticsUpdateCounter >= 15)
      {
        mSignalDiagnosticsUpdateCounter = 0;

        const auto snapshot = mPresetMixer.GetSignalDiagnosticsSnapshot();
        auto buildLevelJson = [](const MultiPresetMixer::SignalLevelStats &stats)
        {
          const double peakDb = ToDbFS(stats.peak);
          const double rmsDb = ToDbFS(stats.rms);
          const double headroomDb = HeadroomDbFromPeak(stats.peak);
          const bool clipped = stats.clipCount > 0 || stats.peak >= 1.0;
          return nlohmann::json{
            {"peak", stats.peak},
            {"rms", stats.rms},
            {"peakDbfs", peakDb},
            {"rmsDbfs", rmsDb},
            {"headroomDb", headroomDb},
            {"clipped", clipped},
            {"clipCount", stats.clipCount},
          };
        };

        nlohmann::json message;
        message["type"] = "signalLevelDiagnostics";
        message["input"] = buildLevelJson(snapshot.input);
        message["output"] = buildLevelJson(snapshot.output);

        nlohmann::json nodes = nlohmann::json::array();
        for (const auto &node : snapshot.nodes)
        {
          nlohmann::json nodeJson;
          nodeJson["scope"] = node.scope;
          if (!node.presetId.empty())
          {
            nodeJson["presetId"] = node.presetId;
          }
          nodeJson["nodeId"] = node.nodeId;
          nodeJson["nodeType"] = node.nodeType;
          nodeJson["levels"] = buildLevelJson(node.levels);
          nodes.push_back(std::move(nodeJson));
        }
        message["nodes"] = std::move(nodes);
        message["timestamp"] = static_cast<std::int64_t>(std::time(nullptr));
        SendMessageToUI(message.dump());
      }
    }
    else
    {
      mSignalDiagnosticsUpdateCounter = 0;
    }
  }

  void* GuitarFXPlugin::OpenWindow(void* pParent)
  {
    mParentWindow = pParent;
    mUIVisible = true;
    mUIReady = false;
    mUIContentLoaded = false;
    mUIReloadInProgress = false;
    mUIReloadAttempts = 0;
    return iplug::WebViewEditorDelegate::OpenWindow(pParent);
  }

  void GuitarFXPlugin::CloseWindow()
  {
    iplug::WebViewEditorDelegate::CloseWindow();
    OnUIClose();
  }

  void GuitarFXPlugin::OnUIOpen()
  {
    std::cout << "[Plugin] OnUIOpen called - loading WebView content" << std::endl;
    
    // Check and log DPI information (Windows only)
#ifdef _WIN32
    // TODO: Fix mView access - it's a private member
    // if (mView) // mView is the window handle from WebViewEditorDelegate
    // {
    //   SetWebViewDPIScale(mView);
    // }
#endif
    
    mUIVisible = true;
    LoadWebViewContent(false);
  }

  void GuitarFXPlugin::OnUIClose()
  {
    std::cout << "[Plugin] OnUIClose called - resetting WebView content flag" << std::endl;
    mUIContentLoaded = false;
    mUIVisible = false;
    mUIReady = false;
    mUIReloadInProgress = false;
    mUIReloadAttempts = 0;
    mPendingStateBroadcast = true;
  }

  void GuitarFXPlugin::OnWebContentLoaded()
  {
    iplug::WebViewEditorDelegate::OnWebContentLoaded();
    mUIContentLoaded = true;
    mUIReloadInProgress = false;
    mPendingStateBroadcast = true;
  }

  void GuitarFXPlugin::OnParentWindowResize(int width, int height)
  {
    iplug::WebViewEditorDelegate::OnParentWindowResize(width, height);

    const bool nowVisible = width > 1 && height > 1;
    if (nowVisible && !mUIVisible)
    {
      std::cout << "[Plugin] UI became visible - reloading WebView content" << std::endl;
      LoadWebViewContent(true);
      mPendingStateBroadcast = true;
    }

    mUIVisible = nowVisible;
  }

  void GuitarFXPlugin::LoadWebViewContent(bool forceReload)
  {
    if (mUIReloadInProgress)
    {
      return;
    }

    if (!forceReload && mUIContentLoaded)
    {
      return;
    }

    // Build path to index.html in resources
    std::filesystem::path htmlPath = mResourceRoot / "ui" / "index.html";
    std::cout << "[Plugin] Loading HTML from: " << htmlPath.generic_string() << std::endl;

    if (!std::filesystem::exists(htmlPath))
    {
      std::cerr << "[Plugin] index.html not found at: " << htmlPath.generic_string() << std::endl;
      mUIReloadInProgress = false;
      return;
    }

    mUIReloadInProgress = true;
    mUIReady = false;
    mUIReloadAttempts++;
    mUIReloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    // LoadFile is provided by WebViewEditorDelegate base class
    LoadFile(htmlPath.string().c_str(), nullptr);
    EnableScroll(false);
    mPendingStateBroadcast = true;
  }

  bool GuitarFXPlugin::SerializeState(iplug::IByteChunk &chunk) const
  {
    bool success = chunk.PutStr(mActivePresetJson.c_str());
    success &= chunk.PutStr(mActivePresetId.c_str());
    return success;
  }

  int GuitarFXPlugin::UnserializeState(const iplug::IByteChunk &chunk, int startPos)
  {
    int position = startPos;

    WDL_String presetJson;
    position = chunk.GetStr(presetJson, position);
    if (position < 0)
    {
      return startPos;
    }
    mActivePresetJson = presetJson.Get();

    WDL_String activePresetId;
    position = chunk.GetStr(activePresetId, position);
    if (position < 0)
    {
      return startPos;
    }
    mActivePresetId = activePresetId.Get();

    if (!mActivePresetJson.empty())
    {
      if (!nlohmann::json::accept(mActivePresetJson))
      {
        ReportErrorToUI("Failed to restore preset state", "Saved preset JSON is invalid");
      }
      else
      {
        auto presetOpt = PresetStorage::DeserializeFromJson(mActivePresetJson);
        if (!presetOpt)
        {
          ReportErrorToUI("Failed to restore preset state", "Preset JSON is not valid");
        }
        else
        {
          EnsureParametricEQNode(*presetOpt, *this);
          ApplyPreset(*presetOpt);
          mActivePreset = *presetOpt;
          mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
          if (mActivePresetId.empty())
          {
            mActivePresetId = presetOpt->id;
          }
        }
      }
    }

    mPendingStateBroadcast = true;
    return position;
  }

  void GuitarFXPlugin::OnParamChange(int paramIdx)
  {
    const auto *param = GetParam(paramIdx);
    if (!param)
    {
      return;
    }

    switch (static_cast<ParameterId>(paramIdx))
    {
    case kParamInputTrim:
      mPresetMixer.SetInputTrim(param->Value());
      break;
    case kParamOutputTrim:
      mPresetMixer.SetOutputTrim(param->Value());
      break;
    case kParamDrive:
      mPresetMixer.SetAmpDrive(param->Value());
      break;
    case kParamTone:
      mPresetMixer.SetAmpTone(param->Value() * 2.0 - 1.0);
      break;
    case kParamGateEnabled:
      mPresetMixer.SetGateEnabled(param->Bool());
      break;
    case kParamGateThreshold:
      mPresetMixer.SetGateThreshold(param->Value());
      break;
    case kParamMix:
      // Mix is now per-preset, handled by SetPresetMix
      break;
    case kParamDoublerEnabled:
      mPresetMixer.SetDoublerEnabled(param->Bool());
      break;
    case kParamDoublerDelay:
      mPresetMixer.SetDoublerDelay(param->Value());
      break;
    case kParamTranspose:
      mPresetMixer.SetTranspose(static_cast<int>(std::round(param->Value())));
      break;
    case kParamSimpleCabEnabled:
      mPresetMixer.SetSimpleCabEnabled(param->Bool());
      break;
    case kParamSimpleCabBass:
      mPresetMixer.SetSimpleCabBass(param->Value());
      break;
    case kParamSimpleCabPresence:
      mPresetMixer.SetSimpleCabPresence(param->Value());
      break;
    case kParamSimpleCabBrightness:
      mPresetMixer.SetSimpleCabBrightness(param->Value());
      break;
    case kParamIRQuality:
      mPresetMixer.SetIRQuality(param->Value());
      break;
    case kParamEQEnabled:
      mPresetMixer.SetEQEnabled(param->Bool());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->enabled = param->Bool();
          mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
        }
      }
      break;
    case kParamEQLowGain:
      mPresetMixer.SetEQBandGain(0, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["lowGain"] = param->Value();
        }
      }
      break;
    case kParamEQLowFreq:
      mPresetMixer.SetEQBandFrequency(0, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["lowFreq"] = param->Value();
        }
      }
      break;
    case kParamEQLowMidGain:
      mPresetMixer.SetEQBandGain(1, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["lowMidGain"] = param->Value();
        }
      }
      break;
    case kParamEQLowMidFreq:
      mPresetMixer.SetEQBandFrequency(1, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["lowMidFreq"] = param->Value();
        }
      }
      break;
    case kParamEQLowMidQ:
      mPresetMixer.SetEQBandQ(1, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["lowMidQ"] = param->Value();
        }
      }
      break;
    case kParamEQHighMidGain:
      mPresetMixer.SetEQBandGain(2, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["highMidGain"] = param->Value();
        }
      }
      break;
    case kParamEQHighMidFreq:
      mPresetMixer.SetEQBandFrequency(2, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["highMidFreq"] = param->Value();
        }
      }
      break;
    case kParamEQHighMidQ:
      mPresetMixer.SetEQBandQ(2, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["highMidQ"] = param->Value();
        }
      }
      break;
    case kParamEQHighGain:
      mPresetMixer.SetEQBandGain(3, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["highGain"] = param->Value();
        }
      }
      break;
    case kParamEQHighFreq:
      mPresetMixer.SetEQBandFrequency(3, param->Value());
      if (mActivePreset)
      {
        if (auto* eqNode = FindEqNode(mActivePreset->graph))
        {
          eqNode->params["highFreq"] = param->Value();
        }
      }
      break;
    // Delay effect
    case kParamDelayEnabled:
      mPresetMixer.SetDelayEnabled(param->Bool());
      break;
    case kParamDelayTime:
      mPresetMixer.SetDelayTime(param->Value());
      break;
    case kParamDelayFeedback:
      mPresetMixer.SetDelayFeedback(param->Value() / 100.0);
      break;
    case kParamDelayMix:
      mPresetMixer.SetDelayMix(param->Value() / 100.0);
      break;
    // Reverb effect
    case kParamReverbEnabled:
      mPresetMixer.SetReverbEnabled(param->Bool());
      break;
    case kParamReverbDecay:
      mPresetMixer.SetReverbDecay(param->Value());
      break;
    case kParamReverbDamping:
      mPresetMixer.SetReverbDamping(param->Value());
      break;
    case kParamReverbMix:
      mPresetMixer.SetReverbMix(param->Value() / 100.0);
      mPresetMixer.SetReverbMix(param->Value() / 100.0);
      break;
    default:
      break;
    }

    mPendingStateBroadcast = true;
  }

  void GuitarFXPlugin::InitializeParameters()
  {
    GetParam(kParamInputTrim)->InitDouble("Input Trim", 0.0, -24.0, 12.0, 0.1, "dB");
    GetParam(kParamOutputTrim)->InitDouble("Output Trim", 0.0, -24.0, 12.0, 0.1, "dB");
    GetParam(kParamDrive)->InitDouble("Drive", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamTone)->InitDouble("Tone Tilt", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamGateEnabled)->InitBool("Noise Gate", false);
    GetParam(kParamGateThreshold)->InitDouble("Gate Threshold", -60.0, -80.0, -20.0, 0.1, "dB");
    GetParam(kParamMix)->InitDouble("Mix", 1.0, 0.0, 1.0, 0.01);
    GetParam(kParamDoublerEnabled)->InitBool("Doubler", false);
    GetParam(kParamDoublerDelay)->InitDouble("Doubler Delay", 6.0, 0.5, 50.0, 0.1, "ms");
    GetParam(kParamTranspose)->InitInt("Transpose", 0, -12, 12, "st");
    GetParam(kParamSimpleCabEnabled)->InitBool("Simple Cab", false);
    GetParam(kParamSimpleCabBass)->InitDouble("Simple Cab Bass", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamSimpleCabPresence)->InitDouble("Simple Cab Presence", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamSimpleCabBrightness)->InitDouble("Simple Cab Brightness", 0.5, 0.0, 1.0, 0.01);
    // IR Quality: 0=Economy, 1=Standard, 2=High, 3=Full
    GetParam(kParamIRQuality)->InitEnum("IR Quality", 1, 4, "", iplug::IParam::kFlagsNone,
      "", "Economy", "Standard", "High", "Full");
    // Parametric EQ parameters
    GetParam(kParamEQEnabled)->InitBool("EQ", false);
    GetParam(kParamEQLowGain)->InitDouble("EQ Low Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQLowFreq)->InitDouble("EQ Low Freq", 100.0, 20.0, 500.0, 1.0, "Hz");
    GetParam(kParamEQLowMidGain)->InitDouble("EQ Low-Mid Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQLowMidFreq)->InitDouble("EQ Low-Mid Freq", 500.0, 100.0, 2000.0, 1.0, "Hz");
    GetParam(kParamEQLowMidQ)->InitDouble("EQ Low-Mid Q", 1.0, 0.1, 10.0, 0.1);
    GetParam(kParamEQHighMidGain)->InitDouble("EQ High-Mid Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQHighMidFreq)->InitDouble("EQ High-Mid Freq", 2000.0, 500.0, 8000.0, 1.0, "Hz");
    GetParam(kParamEQHighMidQ)->InitDouble("EQ High-Mid Q", 1.0, 0.1, 10.0, 0.1);
    GetParam(kParamEQHighGain)->InitDouble("EQ High Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQHighFreq)->InitDouble("EQ High Freq", 8000.0, 2000.0, 16000.0, 1.0, "Hz");

    // Delay effect parameters
    GetParam(kParamDelayEnabled)->InitBool("Delay", false);
    GetParam(kParamDelayTime)->InitDouble("Delay Time", 300.0, 1.0, 2000.0, 1.0, "ms");
    GetParam(kParamDelayFeedback)->InitDouble("Delay Feedback", 30.0, 0.0, 95.0, 1.0, "%");
    GetParam(kParamDelayMix)->InitDouble("Delay Mix", 30.0, 0.0, 100.0, 1.0, "%");

    // Reverb effect parameters
    GetParam(kParamReverbEnabled)->InitBool("Reverb", false);
    GetParam(kParamReverbDecay)->InitDouble("Reverb Decay", 0.5, 0.1, 0.99, 0.01);
    GetParam(kParamReverbDamping)->InitDouble("Reverb Damping", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamReverbMix)->InitDouble("Reverb Mix", 30.0, 0.0, 100.0, 1.0, "%");
  }

  // Send a JSON message to the WebView UI
  // This method uses EvaluateJavaScript (provided by WebViewEditorDelegate) to send
  // custom JSON messages to the UI. The UI should register window.IPlugReceiveData
  // to receive these messages.
  void GuitarFXPlugin::SendMessageToUI(const std::string& jsonMessage)
  {
    // Log tuner-related messages for debugging
    if (jsonMessage.find("tuner") != std::string::npos || jsonMessage.find("Tuner") != std::string::npos)
    {
      std::cout << "[Plugin] SendMessageToUI (tuner): " << jsonMessage.substr(0, 200) << std::endl;
    }
    
    // Escape the JSON string for embedding in JavaScript code
    std::string escaped;
    escaped.reserve(jsonMessage.size() + 10);
    for (char c : jsonMessage) {
      switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
      }
    }
    // Execute JavaScript to deliver the message to the UI
    std::string jsCode = "if (window.IPlugReceiveData) { window.IPlugReceiveData(\"" + escaped + "\"); }";
    EvaluateJavaScript(jsCode.c_str());
  }

  // Override to intercept custom JSON messages before base class processing
  // The base class expects messages with a "msg" field for iPlug2 internal protocol.
  // Our UI sends custom messages with a "type" field instead.
  void GuitarFXPlugin::OnMessageFromWebView(const char* jsonStr)
  {
    if (!jsonStr)
    {
      return;
    }

    // First, try to parse and check if it's one of our custom messages
    auto json = nlohmann::json::parse(jsonStr, nullptr, false);
    
    // Handle double-encoded JSON strings (e.g., "{\"type\":...}" arrives as a JSON string)
    // If the result is a string, try parsing its contents as JSON
    if (!json.is_discarded() && json.is_string())
    {
      std::string innerStr = json.get<std::string>();
      json = nlohmann::json::parse(innerStr, nullptr, false);
    }
    
    // Check if it's a valid JSON object with a "type" field (our custom messages)
    if (!json.is_discarded() && json.is_object() && json.contains("type"))
    {
      // This is our custom message format - handle it directly
      std::cout << "[Plugin] OnMessageFromWebView handling custom message: " << json.value("type", "") << std::endl;
      HandleUIMessage(json.dump());
      return;
    }
    
    // Otherwise, delegate to base class to handle standard iPlug2 protocol messages
    // (e.g., SPVFUI for parameter changes, BPCFUI for begin/end param changes)
    // Note: iplug::Plugin is a typedef for the API-specific base class (IPlugAPP, IPlugVST3, etc.)
    // which inherits from WebViewEditorDelegate when WEBVIEW_EDITOR_DELEGATE is defined
    iplug::Plugin::OnMessageFromWebView(jsonStr);
  }

  // Handle arbitrary messages from the WebView via OnMessage callback.
  // 
  // This is called by WebViewEditorDelegate when JavaScript sends messages via
  // the SAMFUI (Send Arbitrary Msg From UI) protocol. This is an alternative to
  // OnMessageFromWebView for sending non-parameter data.
  // 
  // By convention, msgTag == -1 indicates a JSON message sent via SendArbitraryMsgFromUI.
  bool GuitarFXPlugin::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
  {
    // Check if this is a JSON message from the UI
    if (msgTag == -1 && dataSize > 0 && pData != nullptr)
    {
      std::string message(reinterpret_cast<const char*>(pData), dataSize);
      std::cout << "[Plugin] OnMessage received JSON: " << message.substr(0, 100) << "..." << std::endl;
      HandleUIMessage(message);
      return true;
    }
    return false;
  }

  void GuitarFXPlugin::HandleUIMessage(const std::string &message)
  {
    if (!nlohmann::json::accept(message))
    {
      ReportErrorToUI("Received invalid message", "UI sent malformed JSON payload");
      return;
    }

    auto payload = nlohmann::json::parse(message);
    if (payload.is_string())
    {
      const std::string nested = payload.get<std::string>();
      if (!nlohmann::json::accept(nested))
      {
        ReportErrorToUI("Received invalid message", "UI sent malformed JSON payload");
        return;
      }
      payload = nlohmann::json::parse(nested);
    }

    if (!payload.is_object())
    {
      ReportErrorToUI("Received invalid message", "UI sent malformed JSON payload");
      return;
    }

    const std::string type = payload.value("type", "");
    
    std::cerr << "[GuitarFXPlugin] Received UI message of type: " << type << std::endl;
    
    if (type == "loadPreset")
    {
      HandlePresetLoadRequest(payload);
    }
    else if (type == "requestState")
    {
      HandleStateRequest();
    }
    else if (type == "runSignalPathTest")
    {
      HandleSignalTestRequest(payload);
    }
    else if (type == "previewDemoAudio")
    {
      HandlePreviewDemoRequest(payload);
    }
    else if (type == "setParameter")
    {
      HandleSetParameterRequest(payload);
    }
    else if (type == "loadModel")
    {
      HandleLoadModelRequest(payload);
    }
    else if (type == "loadIR")
    {
      HandleLoadIRRequest(payload);
    }
    else if (type == "savePreset")
    {
      HandleSavePresetRequest(payload);
    }
    else if (type == "browseModel")
    {
      HandleBrowseModelRequest();
    }
    else if (type == "browseIR")
    {
      HandleBrowseIRRequest();
    }
    else if (type == "tuner")
    {
      HandleTunerRequest(payload);
    }
    else if (type == "setInputMode")
    {
      HandleSetInputModeRequest(payload);
    }
    else if (type == "setAmpCabState")
    {
      HandleSetAmpCabStateRequest(payload);
    }
    else if (type == "uiSettingsChanged")
    {
      const auto settingsIt = payload.find("settings");
      if (settingsIt != payload.end() && settingsIt->is_object())
      {
        if (settingsIt->contains("zoom"))
        {
          mUiZoom = settingsIt->value("zoom", mUiZoom);
        }
        if (settingsIt->contains("bounds") && (*settingsIt)["bounds"].is_object())
        {
          const auto &b = (*settingsIt)["bounds"];
          mWindowBounds.x = b.value("x", mWindowBounds.x);
          mWindowBounds.y = b.value("y", mWindowBounds.y);
          mWindowBounds.width = b.value("width", mWindowBounds.width);
          mWindowBounds.height = b.value("height", mWindowBounds.height);
        }
        SaveAppSettings();
      }
    }
    else if (type == "uiReady")
    {
      mUIReady = true;
      mUIReloadInProgress = false;
      mUIReloadAttempts = 0;
      mPendingStateBroadcast = true;
    }
    else if (type == "uiVisibility")
    {
      const bool visible = payload.value("visible", true);
      mUIVisible = visible;
      HideWebView(!visible);
      if (visible && !mUIReady)
      {
        LoadWebViewContent(true);
      }
    }
    else if (type == "setSetting")
    {
      const auto keyIt = payload.find("key");
      if (keyIt != payload.end() && keyIt->is_string())
      {
        const std::string key = keyIt->get<std::string>();
        if (key.empty())
        {
          return;
        }

        const auto valueIt = payload.find("value");
        if (valueIt == payload.end() || valueIt->is_null())
        {
          mAppSettings.erase(key);
        }
        else if (valueIt->is_string() || valueIt->is_number() || valueIt->is_boolean())
        {
          mAppSettings[key] = *valueIt;
        }
        else
        {
          std::cerr << "[GuitarFXPlugin] Unsupported setting value type for key: " << key << std::endl;
          return;
        }

        if (key == kSignalDiagnosticsSettingKey)
        {
          bool enabled = false;
          if (valueIt != payload.end() && valueIt->is_boolean())
          {
            enabled = valueIt->get<bool>();
          }
          else if (valueIt != payload.end() && valueIt->is_number())
          {
            enabled = valueIt->get<double>() != 0.0;
          }
          mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
          mPresetMixer.SetSignalDiagnosticsEnabled(enabled);
        }

        SaveAppSettings();
        mPendingStateBroadcast = true;
      }
    }
    else if (type == "setAutoLevel")
    {
      HandleSetAutoLevelRequest(payload);
    }
    else if (type == "setMetronome")
    {
      HandleSetMetronomeRequest(payload);
    }
    else if (type == "updateSignalPathNodeParam")
    {
      HandleUpdateSignalPathNodeParamRequest(payload);
    }
    else if (type == "updateSignalPathNodeBypass")
    {
      HandleUpdateSignalPathNodeBypassRequest(payload);
    }
    else if (type == "updateNodeResource")
    {
      HandleUpdateNodeResourceRequest(payload);
    }
    else if (type == "browseNodeResource")
    {
      HandleBrowseNodeResourceRequest(payload);
    }
    else if (type == "addSignalPathNode")
    {
      HandleAddSignalPathNodeRequest(payload);
    }
    else if (type == "importRemoteResource")
    {
      HandleImportRemoteResourceRequest(payload);
    }
    else if (type == "splitSignalPathEdge")
    {
      HandleSplitSignalPathEdgeRequest(payload);
    }
    else if (type == "collapseSignalPathSplit")
    {
      HandleCollapseSignalPathSplitRequest(payload);
    }
    else if (type == "replaceSignalPathNode")
    {
      HandleReplaceSignalPathNodeRequest(payload);
    }
    else if (type == "reorderSignalPathNode")
    {
      HandleReorderSignalPathNodeRequest(payload);
    }
    else if (type == "deleteSignalPathNode")
    {
      HandleDeleteSignalPathNodeRequest(payload);
    }
    else if (type == "addActivePreset")
    {
      const auto pIt = payload.find("preset");
      const std::string presetId = payload.value("presetId", "");
      const std::string name = payload.value("name", "");
      if (pIt != payload.end() && pIt->is_object())
      {
        if (auto p = PresetStorage::DeserializeFromJson(pIt->dump()))
        {
          const std::string id = !presetId.empty() ? presetId : (!p->id.empty() ? p->id : "preset");
          AddActivePreset(*p, id, !name.empty() ? name : p->name);
        }
      }
    }
    else if (type == "removeActivePreset")
    {
      const std::string presetId = payload.value("presetId", "");
      if (!presetId.empty())
        RemoveActivePreset(presetId);
    }
    else if (type == "setPresetMix")
    {
      const std::string presetId = payload.value("presetId", "");
      const double value = payload.value("value", 1.0);
      if (!presetId.empty())
        SetActivePresetMix(presetId, value);
    }
    else if (type == "setPresetPan")
    {
      const std::string presetId = payload.value("presetId", "");
      const double pan = payload.value("pan", 0.0);
      if (!presetId.empty())
        SetActivePresetPan(presetId, pan);
    }
    else if (type == "setPresetMute")
    {
      const std::string presetId = payload.value("presetId", "");
      const bool mute = payload.value("mute", false);
      if (!presetId.empty())
        SetActivePresetMute(presetId, mute);
    }
    else if (type == "setPresetSolo")
    {
      const std::string presetId = payload.value("presetId", "");
      const bool solo = payload.value("solo", false);
      if (!presetId.empty())
        SetActivePresetSolo(presetId, solo);
    }
    else if (type == "setMasterGain")
    {
      const double gain = payload.value("value", 1.0);
      SetMasterMixGain(gain);
    }
  }

  void GuitarFXPlugin::BroadcastState()
  {
    nlohmann::json message;
    message["type"] = "state";
    message["activePresetId"] = mActivePresetId;

    auto parameters = SerializeParametersToJson(*this);
    message["parameters"] = std::move(parameters);

    // Include UI settings so the WebView can restore zoom and window bounds
    nlohmann::json uiSettings;
    uiSettings["zoom"] = mUiZoom;
    if (mWindowBounds.HasBounds())
    {
      uiSettings["bounds"] = {
        {"x", mWindowBounds.x},
        {"y", mWindowBounds.y},
        {"width", mWindowBounds.width},
        {"height", mWindowBounds.height},
      };
    }
    message["uiSettings"] = std::move(uiSettings);
    message["appSettings"] = mAppSettings;
    message["environment"] = {
      {"standalone", kIsStandaloneBuild}
    };
    message["metronome"] = {
      {"bpm", GetEffectiveTempoBpm()},
      {"enabled", mMetronomeEnabled.load(std::memory_order_relaxed)},
      {"editable", kIsStandaloneBuild},
      {"source", kIsStandaloneBuild ? "app" : "host"}
    };

    if (mActivePreset)
    {
      message["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(*mActivePreset));
    }

    // Include user presets from user presets directory
    nlohmann::json userPresetsJson = nlohmann::json::array();
    if (!mUserPresetsPath.empty() && std::filesystem::exists(mUserPresetsPath))
    {
      const auto userPresets = PresetStorage::LoadAllFromDirectory(mUserPresetsPath);
      for (const auto &preset : userPresets)
      {
        userPresetsJson.push_back(nlohmann::json::parse(PresetStorage::SerializeToJson(preset)));
      }
    }
    message["userPresets"] = std::move(userPresetsJson);

    // Include resource library for UI to present resource choices
    nlohmann::json resourceLibraryJson = nlohmann::json::object();
    const auto allResources = mResourceLibrary.GetAllResources();
    for (const auto &res : allResources)
    {
      if (!resourceLibraryJson.contains(res.type))
      {
        resourceLibraryJson[res.type] = nlohmann::json::array();
      }
      nlohmann::json resJson;
      resJson["id"] = res.id;
      resJson["name"] = res.name;
      resJson["category"] = res.category;
      resJson["description"] = res.description;
      resJson["filePath"] = res.filePath.generic_string();
      resourceLibraryJson[res.type].push_back(std::move(resJson));
    }
    message["resourceLibrary"] = std::move(resourceLibraryJson);

    SendMessageToUI(message.dump());
    mPendingStateBroadcast = false;

    // Send a test DSP performance message to verify UI communication
    {
      auto stats = mPresetMixer.GetPerformanceStats();
      nlohmann::json perfMessage = {
        {"type", "dspPerformance"},
        {"stats", {
          {"totalProcessingTimeUs", stats.totalProcessingTimeUs},
          {"realTimeUs", stats.realTimeUs},
          {"dspLoadPercent", stats.dspLoadPercent},
          {"nodeProcessingTimesUs", stats.nodeProcessingTimesUs}
        }}
      };
      
      SendMessageToUI(perfMessage.dump());
    }
  }

  void GuitarFXPlugin::SendMetronomeStateToUI()
  {
    nlohmann::json message;
    message["type"] = "metronomeState";
    message["bpm"] = GetEffectiveTempoBpm();
    message["enabled"] = mMetronomeEnabled.load(std::memory_order_relaxed);
    message["editable"] = kIsStandaloneBuild;
    message["source"] = kIsStandaloneBuild ? "app" : "host";
    SendMessageToUI(message.dump());
  }

  void GuitarFXPlugin::EnsureBasicGraph()
  {
    if (mActivePreset)
    {
      return;
    }

    Preset preset;
    preset.id = "default";
    preset.name = "Default";
    preset.version = 2;

    GraphNode input;
    input.id = "__input__";
    input.type = kNodeTypeInput;
    input.category = "routing";

    GraphNode amp;
    amp.id = "amp";
    amp.type = "amp_nam";
    amp.category = "amp";

    GraphNode cab;
    cab.id = "cab";
    cab.type = "cab_ir";
    cab.category = "cab";

    GraphNode output;
    output.id = "__output__";
    output.type = kNodeTypeOutput;
    output.category = "routing";

    preset.graph.nodes = {input, amp, cab, output};
    preset.graph.edges = {
      {input.id, amp.id, 0, 0, 1.0},
      {amp.id, cab.id, 0, 0, 1.0},
      {cab.id, output.id, 0, 0, 1.0}
    };

    EnsureParametricEQNode(preset, *this);

    mActivePreset = preset;
    mActivePresetId = preset.id;
    mActivePresetJson = PresetStorage::SerializeToJson(preset);
  }

  bool GuitarFXPlugin::UpdateResourceForNodeType(const std::string &nodeType,
                                                 const std::string &resourceType,
                                                 const std::filesystem::path &filePath,
                                                 bool applyPreset)
  {
    EnsureBasicGraph();

    if (!mActivePreset)
    {
      return false;
    }

    GraphNode *target = nullptr;
    for (auto &node : mActivePreset->graph.nodes)
    {
      if (node.type == nodeType)
      {
        target = &node;
        break;
      }
    }

    if (!target)
    {
      return false;
    }

    ResourceRef ref;
    ref.resourceType = resourceType;
    ref.filePath = filePath;
    target->resource = ref;

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);

    if (applyPreset && mActivePreset)
    {
      ApplyPreset(*mActivePreset);
      mPendingStateBroadcast = true;
    }

    return true;
  }

  bool GuitarFXPlugin::UpdateResourceForNodeId(const std::string &nodeId,
                                               const ResourceRef &ref,
                                               bool applyPreset)
  {
    EnsureBasicGraph();

    if (!mActivePreset)
    {
      return false;
    }

    GraphNode *target = mActivePreset->graph.FindNode(nodeId);
    if (!target)
    {
      return false;
    }

    target->resource = ref;

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);

    if (applyPreset && mActivePreset)
    {
      ApplyPreset(*mActivePreset);
      mPendingStateBroadcast = true;
    }

    return true;
  }

  void GuitarFXPlugin::ApplyPreset(const Preset &preset)
  {
    // Lock DSP mutex to prevent ProcessBlock from accessing DSP during modification
    std::lock_guard<std::mutex> lock(mDSPMutex);

    Preset presetWithEQ = preset;
    EnsureParametricEQNode(presetWithEQ, *this);

    // Preserve global signal chain settings across mixer resets
    const auto globalChainConfig = mPresetMixer.GetGlobalChainConfig();

    // Initialize multi-preset mixer with a single active preset by default
    mPresetMixer = MultiPresetMixer();
    mPresetMixer.SetResourceLibrary(&mResourceLibrary);
    mPresetMixer.SetGlobalChainConfig(globalChainConfig);
    mPresetMixer.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    
    // Re-register tuner callback after resetting the mixer
    // (The constructor sets this up, but ApplyPreset creates a new mixer instance)
    mPresetMixer.SetTunerCallback([this](const MultiPresetMixer::TunerResult &result) {
      // Store tuner data for sending in OnIdle (thread-safe handoff from audio thread)
      {
        std::lock_guard<std::mutex> lock(mTunerMutex);
        mPendingTunerData.detected = result.detected;
        mPendingTunerData.noteName = result.noteName;
        mPendingTunerData.octave = result.octave;
        mPendingTunerData.frequency = result.frequency;
        mPendingTunerData.centOffset = result.centOffset;
        mPendingTunerData.confidence = result.confidence;
        mPendingTunerData.debugRms = result.debugRms;
        mPendingTunerData.debugRawFreq = result.debugRawFreq;
      }
      mTunerDataPending.store(true, std::memory_order_release);
    });
    
    mPresetMixer.Prepare(GetSampleRate(), GetBlockSize());
    const std::string presetId = !preset.id.empty() ? preset.id : "preset";
    mPresetMixer.AddActivePreset(presetWithEQ, presetId, preset.name);

    std::cout << "[Plugin] Loaded preset into MultiPresetMixer: " << preset.name << std::endl;

    // Sync plugin parameters with preset globals
    auto *inputTrimParam = GetParam(kParamInputTrim);
    if (inputTrimParam)
    {
      inputTrimParam->Set(preset.global.inputTrim);
    }

    auto *outputTrimParam = GetParam(kParamOutputTrim);
    if (outputTrimParam)
    {
      outputTrimParam->Set(preset.global.outputTrim);
    }

    // Sync auto-level globals into the preset mixer
    mPresetMixer.SetAutoLevelInput(preset.global.autoLevelInput);
    mPresetMixer.SetAutoLevelOutput(preset.global.autoLevelOutput);

    auto *transposeParam = GetParam(kParamTranspose);
    if (transposeParam)
    {
      transposeParam->Set(static_cast<double>(preset.global.transpose));
    }

    const GraphNode* eqNode = FindEqNode(presetWithEQ.graph);
    if (eqNode)
    {
      auto applyEqParam = [&](ParameterId id, const char* key)
      {
        auto *param = GetParam(static_cast<int>(id));
        if (!param)
        {
          return;
        }

        double value = param->Value();
        const auto it = eqNode->params.find(key);
        if (it != eqNode->params.end())
        {
          value = it->second;
        }

        param->Set(value);
        OnParamChange(static_cast<int>(id));
      };

      if (auto *enabledParam = GetParam(kParamEQEnabled))
      {
        enabledParam->Set(eqNode->enabled ? 1.0 : 0.0);
        OnParamChange(kParamEQEnabled);
      }

      applyEqParam(kParamEQLowGain, "lowGain");
      applyEqParam(kParamEQLowFreq, "lowFreq");
      applyEqParam(kParamEQLowMidGain, "lowMidGain");
      applyEqParam(kParamEQLowMidFreq, "lowMidFreq");
      applyEqParam(kParamEQLowMidQ, "lowMidQ");
      applyEqParam(kParamEQHighMidGain, "highMidGain");
      applyEqParam(kParamEQHighMidFreq, "highMidFreq");
      applyEqParam(kParamEQHighMidQ, "highMidQ");
      applyEqParam(kParamEQHighGain, "highGain");
      applyEqParam(kParamEQHighFreq, "highFreq");
    }
  }

  void GuitarFXPlugin::HandlePresetLoadRequest(const nlohmann::json &payload)
  {
    const auto presetJsonIter = payload.find("preset");
    if (presetJsonIter == payload.end() || !presetJsonIter->is_object())
    {
      return;
    }

    try
    {
      auto presetOpt = PresetStorage::DeserializeFromJson(presetJsonIter->dump());
      if (!presetOpt)
      {
        ReportErrorToUI("Failed to load preset", "Could not parse preset JSON");
        return;
      }

      EnsureParametricEQNode(*presetOpt, *this);
      ApplyPreset(*presetOpt);

      mActivePreset = *presetOpt;
      mActivePresetId = presetOpt->id;
      mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
      mPendingStateBroadcast = true;

      // Save settings so this preset is restored on next startup
      SaveAppSettings();

      {
        nlohmann::json message;
        message["type"] = "presetLoaded";
        message["preset"] = nlohmann::json::parse(mActivePresetJson);
        
        // Include active mixer preset ids
        message["activePresetIds"] = mPresetMixer.GetActivePresetIds();
        
        SendMessageToUI(message.dump());
      }
    }
    catch (const std::exception &exception)
    {
      mPendingStateBroadcast = true;
      ReportErrorToUI("Failed to load preset", exception.what());
    }
    catch (...)
    {
      mPendingStateBroadcast = true;
      ReportErrorToUI("Failed to load preset", "An unknown error occurred");
    }
  }

  void GuitarFXPlugin::HandleStateRequest()
  {
    mPendingStateBroadcast = true;
  }

  void GuitarFXPlugin::HandleSetParameterRequest(const nlohmann::json &payload)
  {
    const std::string paramId = payload.value("id", "");
    const double value = payload.value("value", 0.0);

    // Use the existing ParamIdFromKey helper function
    const auto paramIdx = ParamIdFromKey(paramId);
    if (paramIdx && *paramIdx >= 0 && *paramIdx < kParamCount)
    {
      GetParam(*paramIdx)->Set(value);
      OnParamChange(*paramIdx);
      
      // Save settings to persist parameter changes across sessions
      SaveAppSettings();
    }
  }

  void GuitarFXPlugin::HandleSignalTestRequest(const nlohmann::json &payload)
  {
    const double frequency = payload.value("frequency", 440.0);
    const double duration = payload.value("duration", 1.0);

    if (StartSignalPathTest(frequency, duration))
    {
      return;
    }

    ReportErrorToUI("Unable to start signal path test", "Another test is already running or DSP is not ready");
  }

  void GuitarFXPlugin::HandleLoadModelRequest(const nlohmann::json &payload)
  {
    const std::string filePath = payload.value("filePath", "");
    if (filePath.empty())
    {
      ReportErrorToUI("Cannot load model", "No file path provided");
      return;
    }

    const std::filesystem::path modelPath{filePath};
    if (!std::filesystem::exists(modelPath))
    {
      ReportErrorToUI("Cannot load model", "File does not exist: " + filePath);
      return;
    }

    if (!UpdateResourceForNodeType("amp_nam", "nam", modelPath))
    {
      ReportErrorToUI("Failed to load model", "No NAM amp node available in current graph");
      return;
    }

    SaveAppSettings();

    nlohmann::json message;
    message["type"] = "modelLoaded";
    message["path"] = filePath;
    SendMessageToUI(message.dump());
  }

  void GuitarFXPlugin::HandleLoadIRRequest(const nlohmann::json &payload)
  {
    const std::string filePath = payload.value("filePath", "");
    if (filePath.empty())
    {
      ReportErrorToUI("Cannot load IR", "No file path provided");
      return;
    }

    const std::filesystem::path irPath{filePath};
    if (!std::filesystem::exists(irPath))
    {
      ReportErrorToUI("Cannot load IR", "File does not exist: " + filePath);
      return;
    }

    if (!UpdateResourceForNodeType("cab_ir", "ir", irPath) &&
        !UpdateResourceForNodeType("ir_cab", "ir", irPath))
    {
      ReportErrorToUI("Failed to load IR", "No IR cab node available in current graph");
      return;
    }

    SaveAppSettings();

    nlohmann::json message;
    message["type"] = "irLoaded";
    message["path"] = filePath;
    SendMessageToUI(message.dump());
  }

  void GuitarFXPlugin::HandleSavePresetRequest(const nlohmann::json &payload)
  {
    std::cerr << "[GuitarFXPlugin] HandleSavePresetRequest called" << std::endl;
    
    const std::string presetName = payload.value("name", "");
    const std::string presetCategory = payload.value("category", "User");
    const std::string presetDescription = payload.value("description", "");

    std::cerr << "[GuitarFXPlugin] Saving preset: name=" << presetName 
              << ", category=" << presetCategory << std::endl;

    if (presetName.empty())
    {
      std::cerr << "[GuitarFXPlugin] Error: Preset name is empty" << std::endl;
      ReportErrorToUI("Cannot save preset", "Preset name is required");
      return;
    }

    EnsureBasicGraph();
    if (!mActivePreset)
    {
      ReportErrorToUI("Cannot save preset", "No active preset to save");
      return;
    }

    // Build the preset from the active signal graph (V2 format)
    Preset newPreset = *mActivePreset;
    newPreset.id = "user-" + std::to_string(std::time(nullptr));
    newPreset.name = presetName;
    newPreset.category = presetCategory;
    newPreset.description = presetDescription;
    newPreset.version = 2;

    // Capture global preset settings (but not global FX chain settings)
    if (auto *param = GetParam(kParamInputTrim)) newPreset.global.inputTrim = param->Value();
    if (auto *param = GetParam(kParamOutputTrim)) newPreset.global.outputTrim = param->Value();
    if (auto *param = GetParam(kParamTranspose)) newPreset.global.transpose = static_cast<int>(param->Value());
    newPreset.global.autoLevelInput = mPresetMixer.GetAutoLevelInput();
    newPreset.global.autoLevelOutput = mPresetMixer.GetAutoLevelOutput();

    // Save preset to file in user presets directory
    if (mUserPresetsPath.empty())
    {
      mUserPresetsPath = mResourceRoot / "presets" / "user";
    }
    std::filesystem::create_directories(mUserPresetsPath);
    
    std::filesystem::path presetPath = mUserPresetsPath / (newPreset.id + ".json");
    if (!PresetStorage::SaveToFile(newPreset, presetPath))
    {
      ReportErrorToUI("Failed to save preset", "Could not write preset file");
      return;
    }

    // Update active preset
    mActivePreset = newPreset;
    mActivePresetId = newPreset.id;
    mActivePresetJson = PresetStorage::SerializeToJson(newPreset);
    mPendingStateBroadcast = true;

    // Save settings so this preset is restored on next startup
    SaveAppSettings();

    {
      nlohmann::json message;
      message["type"] = "presetSaved";
      message["preset"] = nlohmann::json::parse(mActivePresetJson);
      SendMessageToUI(message.dump());
    }
  }

  void GuitarFXPlugin::HandleBrowseModelRequest()
  {
#ifdef _WIN32
    wchar_t filePath[MAX_PATH] = {0};
    
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"NAM Model Files (*.nam)\0*.nam\0JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select NAM Model";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    
    if (GetOpenFileNameW(&ofn))
    {
      std::filesystem::path modelPath{filePath};
      nlohmann::json payload;
      payload["filePath"] = modelPath.generic_string();
      HandleLoadModelRequest(payload);
    }
#else
    ReportErrorToUI("Browse not supported", "File browser is only available on Windows");
#endif
  }

  void GuitarFXPlugin::HandleBrowseIRRequest()
  {
#ifdef _WIN32
    wchar_t filePath[MAX_PATH] = {0};
    
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"WAV Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Impulse Response";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    
    if (GetOpenFileNameW(&ofn))
    {
      std::filesystem::path irPath{filePath};
      nlohmann::json payload;
      payload["filePath"] = irPath.generic_string();
      HandleLoadIRRequest(payload);
    }
#else
    ReportErrorToUI("Browse not supported", "File browser is only available on Windows");
#endif
  }

  void GuitarFXPlugin::HandleTunerRequest(const nlohmann::json &payload)
  {
    const std::string action = payload.value("action", "");
    std::cerr << "[Plugin] HandleTunerRequest called with action: " << action << std::endl;

    if (action == "start")
    {
      std::cout << "[Plugin] Tuner starting" << std::endl;

      // Set live tuner mode if specified (UI sends as "liveMode")
      if (payload.contains("liveMode"))
      {
        mPresetMixer.SetLiveTunerMode(payload.value("liveMode", true));
      }

      // Set reference frequency if specified
      if (payload.contains("referenceFrequency"))
      {
        mPresetMixer.SetTunerReferenceFrequency(payload.value("referenceFrequency", 440.0));
      }

      // Enable tuner
      mTunerActive.store(true, std::memory_order_release);
      mPresetMixer.SetTunerEnabled(true);

      // Acknowledge tuner start
      nlohmann::json message;
      message["type"] = "tunerStarted";
      message["referenceFrequency"] = mPresetMixer.GetTunerReferenceFrequency();
      message["liveMode"] = mPresetMixer.IsLiveTunerMode();
      SendMessageToUI(message.dump());
    }
    else if (action == "stop")
    {
      std::cout << "[Plugin] Tuner stopping" << std::endl;

      // Disable tuner
      mTunerActive.store(false, std::memory_order_release);
      mPresetMixer.SetTunerEnabled(false);

      // Acknowledge tuner stop
      nlohmann::json message;
      message["type"] = "tunerStopped";
      SendMessageToUI(message.dump());
    }
    else if (action == "setLiveMode")
    {
      bool liveMode = payload.value("liveMode", true);
      mPresetMixer.SetLiveTunerMode(liveMode);

      // Acknowledge the change
      nlohmann::json message;
      message["type"] = "tunerLiveModeChanged";
      message["liveMode"] = liveMode;
      SendMessageToUI(message.dump());
    }
    else if (action == "setReference")
    {
      double freq = payload.value("referenceFrequency", 440.0);
      mPresetMixer.SetTunerReferenceFrequency(freq);

      // Acknowledge the change
      nlohmann::json message;
      message["type"] = "tunerReferenceChanged";
      message["referenceFrequency"] = mPresetMixer.GetTunerReferenceFrequency();
      SendMessageToUI(message.dump());
    }
  }

  void GuitarFXPlugin::HandleSetInputModeRequest(const nlohmann::json &payload)
  {
    // Set mono/stereo mode
    if (payload.contains("monoMode"))
    {
      const bool mono = payload.value("monoMode", true);
      mPresetMixer.SetMonoMode(mono);
    }

    // Set input channel (0 = input 1, 1 = input 2)
    if (payload.contains("inputChannel"))
    {
      const int channel = payload.value("inputChannel", 1);
      mPresetMixer.SetInputChannel(channel);
    }

    // Acknowledge the change
    {
      nlohmann::json message;
      message["type"] = "inputModeChanged";
      message["monoMode"] = mPresetMixer.IsMonoMode();
      message["inputChannel"] = mPresetMixer.GetInputChannel();
      SendMessageToUI(message.dump());
    }
  }

  void GuitarFXPlugin::HandleSetAmpCabStateRequest(const nlohmann::json &payload)
  {
    // Set amp (NAM model) enabled state
    if (payload.contains("ampEnabled"))
    {
      const bool enabled = payload.value("ampEnabled", true);
      // TODO: Add SetAmpEnabled to MultiPresetMixer or route to preset node
      // mPresetMixer.SetAmpEnabled(enabled);
    }

    // Set cab (IR) enabled state
    if (payload.contains("cabEnabled"))
    {
      const bool enabled = payload.value("cabEnabled", true);
      // TODO: Add SetCabEnabled to MultiPresetMixer or route to preset node
      // mPresetMixer.SetCabEnabled(enabled);
    }

    // Acknowledge the change - using placeholder values until methods are added
    {
      nlohmann::json message;
      message["type"] = "ampCabStateChanged";
      message["ampEnabled"] = true; // TODO: mPresetMixer.IsAmpEnabled()
      message["cabEnabled"] = true; // TODO: mPresetMixer.IsCabEnabled()
      SendMessageToUI(message.dump());
    }
  }

  void GuitarFXPlugin::HandleSetAutoLevelRequest(const nlohmann::json &payload)
  {
    const bool autoInput = payload.value("autoInput", mPresetMixer.GetAutoLevelInput());
    const bool autoOutput = payload.value("autoOutput", mPresetMixer.GetAutoLevelOutput());

    mPresetMixer.SetAutoLevelInput(autoInput);
    mPresetMixer.SetAutoLevelOutput(autoOutput);

    // Persist the change for next launch
    SaveAppSettings();

    nlohmann::json message;
    message["type"] = "autoLevelChanged";
    message["autoInput"] = mPresetMixer.GetAutoLevelInput();
    message["autoOutput"] = mPresetMixer.GetAutoLevelOutput();
    SendMessageToUI(message.dump());
  }

  void GuitarFXPlugin::HandleSetMetronomeRequest(const nlohmann::json &payload)
  {
    if (!kIsStandaloneBuild)
    {
      return;
    }

    bool changed = false;
    if (payload.contains("bpm") && payload["bpm"].is_number())
    {
      const double bpm = ClampValue(payload.value("bpm", kMetronomeDefaultBpm), kMetronomeMinBpm, kMetronomeMaxBpm);
      mMetronomeBpm.store(bpm, std::memory_order_release);
      mAppSettings[kMetronomeBpmSettingKey] = bpm;
      changed = true;
    }

    if (payload.contains("enabled") && payload["enabled"].is_boolean())
    {
      const bool enabled = payload.value("enabled", false);
      mMetronomeEnabled.store(enabled, std::memory_order_release);
      mAppSettings[kMetronomeEnabledSettingKey] = enabled;
      changed = true;
    }

    if (changed)
    {
      mMetronomeResetPending.store(true, std::memory_order_release);
      SaveAppSettings();
      mPendingStateBroadcast = true;
    }
  }

  void GuitarFXPlugin::HandleUpdateSignalPathNodeParamRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string paramKey = payload.value("paramKey", "");
    const double value = payload.value("value", 0.0);

    if (nodeId.empty() || paramKey.empty())
    {
      return;
    }

    // Update the parameter in the active preset's graph
    if (mActivePreset)
    {
      GraphNode* node = mActivePreset->graph.FindNode(nodeId);
      if (node)
      {
        node->params[paramKey] = value;
        
        // Re-serialize the preset JSON to reflect the change
        mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
        
        // Apply just the changed parameter based on node type (without full preset reload)
        const bool handled = ApplyNodeParameter(*node, paramKey, value);
        if (!handled)
        {
          // Fallback: re-apply the preset so new node param values take effect immediately.
          ApplyPreset(*mActivePreset);
        }
        
        // Don't broadcast state on every param change during drag - too noisy
      }
    }
  }

  bool GuitarFXPlugin::ApplyNodeParameter(const GraphNode& node, const std::string& paramKey, double value)
  {
    bool handled = false;
    auto applyParam = [&](ParameterId id)
    {
      auto *param = GetParam(static_cast<int>(id));
      if (!param)
      {
        return false;
      }
      param->Set(value);
      OnParamChange(static_cast<int>(id));
      return true;
    };

    // Map node type and param key to plugin parameter
    if (node.type == "amp_nam" || node.type == "nam_amp" || node.type == "nam")
    {
      if (paramKey == "drive")
      {
        auto *param = GetParam(kParamDrive);
        if (param) { param->Set(value); OnParamChange(kParamDrive); handled = true; }
      }
      else if (paramKey == "tone")
      {
        auto *param = GetParam(kParamTone);
        if (param) { param->Set(value); OnParamChange(kParamTone); handled = true; }
      }
      else if (paramKey == "inputGain")
      {
        auto *param = GetParam(kParamInputTrim);
        if (param) { param->Set(value); OnParamChange(kParamInputTrim); handled = true; }
      }
      else if (paramKey == "outputGain")
      {
        auto *param = GetParam(kParamOutputTrim);
        if (param) { param->Set(value); OnParamChange(kParamOutputTrim); handled = true; }
      }
    }
    else if (node.type == "dynamics_gate" || node.type == "noise_gate" || node.type == "gate")
    {
      if (paramKey == "threshold")
      {
        auto *param = GetParam(kParamGateThreshold);
        if (param) { param->Set(value); OnParamChange(kParamGateThreshold); handled = true; }
      }
    }
    else if (node.type == "eq_parametric" || node.type == "eq")
    {
      if (paramKey == "lowGain" || paramKey == "band0_gain") handled = applyParam(kParamEQLowGain) || handled;
      else if (paramKey == "lowFreq" || paramKey == "band0_freq") handled = applyParam(kParamEQLowFreq) || handled;
      else if (paramKey == "lowMidGain" || paramKey == "band1_gain") handled = applyParam(kParamEQLowMidGain) || handled;
      else if (paramKey == "lowMidFreq" || paramKey == "band1_freq") handled = applyParam(kParamEQLowMidFreq) || handled;
      else if (paramKey == "lowMidQ" || paramKey == "band1_q") handled = applyParam(kParamEQLowMidQ) || handled;
      else if (paramKey == "highMidGain" || paramKey == "band2_gain") handled = applyParam(kParamEQHighMidGain) || handled;
      else if (paramKey == "highMidFreq" || paramKey == "band2_freq") handled = applyParam(kParamEQHighMidFreq) || handled;
      else if (paramKey == "highMidQ" || paramKey == "band2_q") handled = applyParam(kParamEQHighMidQ) || handled;
      else if (paramKey == "highGain" || paramKey == "band3_gain") handled = applyParam(kParamEQHighGain) || handled;
      else if (paramKey == "highFreq" || paramKey == "band3_freq") handled = applyParam(kParamEQHighFreq) || handled;
    }
    else if (node.type == "delay_digital" || node.type == "delay")
    {
      if (paramKey == "time")
      {
        auto *param = GetParam(kParamDelayTime);
        if (param) { param->Set(value); OnParamChange(kParamDelayTime); handled = true; }
      }
      else if (paramKey == "feedback")
      {
        auto *param = GetParam(kParamDelayFeedback);
        if (param) { param->Set(value); OnParamChange(kParamDelayFeedback); handled = true; }
      }
      else if (paramKey == "mix")
      {
        auto *param = GetParam(kParamDelayMix);
        if (param) { param->Set(value); OnParamChange(kParamDelayMix); handled = true; }
      }
    }
    else if (node.type == "reverb_room" || node.type == "reverb")
    {
      if (paramKey == "decay")
      {
        auto *param = GetParam(kParamReverbDecay);
        if (param) { param->Set(value); OnParamChange(kParamReverbDecay); handled = true; }
      }
      else if (paramKey == "damping")
      {
        auto *param = GetParam(kParamReverbDamping);
        if (param) { param->Set(value); OnParamChange(kParamReverbDamping); handled = true; }
      }
      else if (paramKey == "mix")
      {
        auto *param = GetParam(kParamReverbMix);
        if (param) { param->Set(value); OnParamChange(kParamReverbMix); handled = true; }
      }
    }
    else if (node.type == "cab_simple" || node.type == "simple_cab")
    {
      if (paramKey == "bass")
      {
        auto *param = GetParam(kParamSimpleCabBass);
        if (param) { param->Set(value); OnParamChange(kParamSimpleCabBass); handled = true; }
      }
      else if (paramKey == "presence")
      {
        auto *param = GetParam(kParamSimpleCabPresence);
        if (param) { param->Set(value); OnParamChange(kParamSimpleCabPresence); handled = true; }
      }
      else if (paramKey == "brightness")
      {
        auto *param = GetParam(kParamSimpleCabBrightness);
        if (param) { param->Set(value); OnParamChange(kParamSimpleCabBrightness); handled = true; }
      }
    }

    return handled;
  }

  void GuitarFXPlugin::HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const bool bypassed = payload.value("bypassed", false);

    if (nodeId.empty())
    {
      return;
    }

    // Update the bypass state in the active preset's graph
    if (mActivePreset)
    {
      GraphNode* node = mActivePreset->graph.FindNode(nodeId);
      if (node)
      {
        node->enabled = !bypassed;
        
        // Re-serialize the preset JSON to reflect the change
        mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
        
        // Apply the updated preset to DSP
        ApplyPreset(*mActivePreset);
        
        // Broadcast state change to UI
        mPendingStateBroadcast = true;
      }
    }
  }

  void GuitarFXPlugin::HandleUpdateNodeResourceRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    const std::string filePath = payload.value("filePath", "");
    const int resourceIndex = payload.value("resourceIndex", -1);
    const std::string parameterId = payload.value("parameterId", "");
    const bool hasParameterValue = payload.contains("parameterValue") && payload["parameterValue"].is_number();
    const double parameterValue = hasParameterValue ? payload["parameterValue"].get<double>() : 0.0;

    if (nodeId.empty())
    {
      return;
    }

    ResourceRef ref;
    ref.resourceType = resourceType;
    if (!resourceId.empty())
    {
      ref.resourceId = resourceId;
    }
    if (!filePath.empty())
    {
      ref.filePath = filePath;
    }
    if (!parameterId.empty())
    {
      ref.parameterId = parameterId;
    }
    if (hasParameterValue)
    {
      ref.parameterValue = parameterValue;
    }

    if (resourceIndex >= 0)
    {
      EnsureBasicGraph();
      if (!mActivePreset)
      {
        return;
      }

      GraphNode* target = mActivePreset->graph.FindNode(nodeId);
      if (!target)
      {
        return;
      }

      if (static_cast<size_t>(resourceIndex) >= target->resources.size())
      {
        target->resources.resize(static_cast<size_t>(resourceIndex) + 1);
      }

      ResourceRef& slot = target->resources[static_cast<size_t>(resourceIndex)];
      if (!ref.resourceType.empty())
        slot.resourceType = ref.resourceType;
      if (!ref.resourceId.empty())
        slot.resourceId = ref.resourceId;
      if (!ref.filePath.empty())
        slot.filePath = ref.filePath;
      if (!ref.embeddedId.empty())
        slot.embeddedId = ref.embeddedId;
      if (!ref.parameterId.empty())
        slot.parameterId = ref.parameterId;
      if (ref.parameterValue.has_value())
        slot.parameterValue = ref.parameterValue;

      if (target->type == "amp_nam_blend")
      {
        target->resource.reset();
      }

      mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
      ApplyPreset(*mActivePreset);
      mPendingStateBroadcast = true;
      return;
    }

    // Update only the targeted node (single-resource)
    if (UpdateResourceForNodeId(nodeId, ref, true))
    {
      return;
    }
  }

  void GuitarFXPlugin::HandleBrowseNodeResourceRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string resourceType = payload.value("resourceType", "");
    const int resourceIndex = payload.value("resourceIndex", -1);

    if (nodeId.empty() || resourceType.empty())
    {
      return;
    }

#ifdef _WIN32
    wchar_t filePath[MAX_PATH] = {0};
    
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    
    // Set filter based on resource type
    if (resourceType == "nam")
    {
      ofn.lpstrFilter = L"NAM Model Files (*.nam)\0*.nam\0JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
      ofn.lpstrTitle = L"Select NAM Model";
    }
    else if (resourceType == "ir")
    {
      ofn.lpstrFilter = L"IR Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
      ofn.lpstrTitle = L"Select IR File";
    }
    else
    {
      ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
      ofn.lpstrTitle = L"Select Resource File";
    }
    
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    
    if (GetOpenFileNameW(&ofn))
    {
      std::filesystem::path selectedPath{filePath};
      
      // Update the node resource with the selected file
      nlohmann::json updatePayload;
      updatePayload["nodeId"] = nodeId;
      updatePayload["resourceType"] = resourceType;
      updatePayload["filePath"] = selectedPath.generic_string();
      if (resourceIndex >= 0)
      {
        updatePayload["resourceIndex"] = resourceIndex;
      }
      HandleUpdateNodeResourceRequest(updatePayload);
    }
#else
    ReportErrorToUI("Browse not supported", "File browser is only available on Windows");
#endif
  }

  void GuitarFXPlugin::HandlePreviewDemoRequest(const nlohmann::json &payload)
  {
    if (mSignalTestActive.load(std::memory_order_acquire))
    {
      ReportErrorToUI("Demo preview unavailable", "Signal path test is currently running");
      return;
    }

    const auto audioIter = payload.find("audio");
    if (audioIter == payload.end() || !audioIter->is_object())
    {
      ReportErrorToUI("Demo preview unavailable", "Audio payload is missing");
      return;
    }

    const std::string dataEncoded = audioIter->value("data", "");
    if (dataEncoded.empty())
    {
      ReportErrorToUI("Demo preview unavailable", "Audio payload did not include data");
      return;
    }

    const auto decodedBytes = DecodeBase64(dataEncoded);
    if (decodedBytes.empty())
    {
      ReportErrorToUI("Demo preview unavailable", "Unable to decode audio data");
      return;
    }

    const auto wavData = DecodePcmWav(decodedBytes);
    if (!wavData)
    {
      ReportErrorToUI("Demo preview unavailable", "Unsupported WAV format");
      return;
    }

    const double hostSampleRate = GetSampleRate();
    const double targetSampleRate = hostSampleRate > 0.0 ? hostSampleRate : wavData->sampleRate;

    if (targetSampleRate <= 0.0)
    {
      ReportErrorToUI("Demo preview unavailable", "Target sample rate is invalid");
      return;
    }

    auto resampled = ConvertToSampleRate(*wavData, targetSampleRate);
    if (resampled.empty() || resampled.front().empty())
    {
      ReportErrorToUI("Demo preview unavailable", "Audio buffer is empty");
      return;
    }

    std::size_t minFrames = resampled.front().size();
    for (const auto &channel : resampled)
    {
      if (channel.empty())
      {
        ReportErrorToUI("Demo preview unavailable", "Audio buffer is empty");
        return;
      }
      minFrames = std::min(minFrames, channel.size());
    }
    if (minFrames == 0)
    {
      ReportErrorToUI("Demo preview unavailable", "Audio buffer is empty");
      return;
    }
    for (auto &channel : resampled)
    {
      if (channel.size() > minFrames)
      {
        channel.resize(minFrames);
      }
    }

    auto previewBuffer = std::make_shared<PreviewPlaybackBuffer>();
    previewBuffer->id = audioIter->value("id", "");
    previewBuffer->title = audioIter->value("title", previewBuffer->id);
    previewBuffer->sampleRate = targetSampleRate;
    previewBuffer->channels = static_cast<int>(resampled.size());
    previewBuffer->channelSamples = std::move(resampled);

    {
      std::lock_guard<std::mutex> lock(mDSPMutex);
      mPresetMixer.Reset();

      mPreviewCursor.store(0, std::memory_order_release);
      mPreviewBuffer.store(previewBuffer, std::memory_order_release);
      mPreviewStartedBuffer.store(previewBuffer, std::memory_order_release);
      mPreviewCompletedBuffer.store(nullptr, std::memory_order_release);
    }
  }

  void GuitarFXPlugin::ReportErrorToUI(std::string_view message, std::string_view detail)
  {
    nlohmann::json payload;
    payload["type"] = "error";
    payload["message"] = std::string{message};
    if (!detail.empty())
    {
      payload["detail"] = std::string{detail};
    }

    SendMessageToUI(payload.dump());
  }

  std::optional<std::filesystem::path> GuitarFXPlugin::ResolveResourceRef(const ResourceRef &ref) const
  {
    // Check for direct file path first
    if (ref.IsFilePath())
    {
      if (ref.filePath.is_absolute() && std::filesystem::exists(ref.filePath))
      {
        return ref.filePath;
      }

      // Try relative to resource root
      if (!mResourceRoot.empty())
      {
        auto resourcePath = mResourceRoot / ref.filePath;
        if (std::filesystem::exists(resourcePath))
        {
          return resourcePath;
        }
      }

      // Try relative to preset directory
      if (auto presetDir = mFileSystem.EnsureDirectory(mFileSystem.ResolvePresetDirectory()))
      {
        auto presetPath = *presetDir / ref.filePath;
        if (std::filesystem::exists(presetPath))
        {
          return presetPath;
        }
      }

      // If the path looks like a filename (not a full path), check common resource subdirectories
      if (ref.filePath.filename() == ref.filePath)
      {
        std::vector<std::filesystem::path> searchPaths;
        
        if (!mResourceRoot.empty())
        {
          searchPaths.push_back(mResourceRoot / "amps" / ref.filePath);
          searchPaths.push_back(mResourceRoot / "ir" / ref.filePath);
          searchPaths.push_back(mResourceRoot / "models" / ref.filePath);
        }

        for (const auto& searchPath : searchPaths)
        {
          if (std::filesystem::exists(searchPath))
          {
            return searchPath;
          }
        }
      }
    }

    // Check for library reference - use ResourceLibrary to look up actual file path
    if (ref.IsLibraryRef())
    {
      // Look up resource in the library by type and ID
      auto resource = mResourceLibrary.LookupResource(ref.resourceType, ref.resourceId);
      if (resource && std::filesystem::exists(resource->filePath))
      {
        return resource->filePath;
      }
      
      // Fallback: try common resource locations with direct ID-to-filename mapping
      if (!mResourceRoot.empty())
      {
        std::filesystem::path resourcePath;
        
        if (ref.resourceType == "nam")
        {
          resourcePath = mResourceRoot / "amps" / (ref.resourceId + ".nam");
        }
        else if (ref.resourceType == "ir")
        {
          resourcePath = mResourceRoot / "ir" / (ref.resourceId + ".wav");
        }

        if (!resourcePath.empty() && std::filesystem::exists(resourcePath))
        {
          return resourcePath;
        }
      }
    }

    // Embedded resources would need to be materialized from embeddedResources
    // This is handled elsewhere for preset sharing

    return std::nullopt;
  }

  std::vector<std::uint8_t> GuitarFXPlugin::DecodeBase64(const std::string &encoded)
  {
    static const std::array<int, 256> decodeTable = []()
    {
      std::array<int, 256> table{};
      table.fill(-1);
      const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      for (std::size_t idx = 0; idx < alphabet.size(); ++idx)
      {
        table[static_cast<unsigned char>(alphabet[idx])] = static_cast<int>(idx);
      }
      table[static_cast<unsigned char>('-')] = 62;
      table[static_cast<unsigned char>('_')] = 63;
      return table;
    }();

    std::vector<std::uint8_t> output;
    int accumulator = 0;
    int bits = -8;

    for (unsigned char c : encoded)
    {
      if (std::isspace(c))
      {
        continue;
      }

      if (c == '=')
      {
        break;
      }

      const int value = decodeTable[c];
      if (value < 0)
      {
        return {};
      }

      accumulator = (accumulator << 6) + value;
      bits += 6;
      if (bits >= 0)
      {
        output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        bits -= 8;
      }
    }

    return output;
  }

  bool GuitarFXPlugin::WriteFile(const std::filesystem::path &target, const std::vector<std::uint8_t> &data) const
  {
    if (target.empty())
    {
      return false;
    }

    const auto parent = target.parent_path();
    if (!parent.empty())
    {
      if (!mFileSystem.EnsureDirectory(parent))
      {
        return false;
      }
    }

    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output)
    {
      return false;
    }

    output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    return output.good();
  }

  void GuitarFXPlugin::AppendUserLibraryResource(const LibraryResource& resource)
  {
    try
    {
      const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
      const auto libraryDir = settingsDir / "resources";
      const auto libraryFile = libraryDir / "library.json";
      (void)mFileSystem.EnsureDirectory(libraryDir);

      nlohmann::json entries = nlohmann::json::array();
      if (std::filesystem::exists(libraryFile))
      {
        std::ifstream input(libraryFile);
        if (input)
        {
          nlohmann::json parsed;
          input >> parsed;
          if (parsed.is_array())
          {
            entries = std::move(parsed);
          }
        }
      }

      bool exists = false;
      for (const auto& item : entries)
      {
        if (item.value("type", "") == resource.type && item.value("id", "") == resource.id)
        {
          exists = true;
          break;
        }
      }

      if (!exists)
      {
        nlohmann::json item;
        item["type"] = resource.type;
        item["id"] = resource.id;
        item["name"] = resource.name;
        item["category"] = resource.category;
        item["description"] = resource.description;
        item["filePath"] = resource.filePath.string();
        item["hash"] = resource.hash;
        item["tags"] = resource.tags;
        entries.push_back(std::move(item));
      }

      std::ofstream output(libraryFile);
      if (output)
      {
        output << entries.dump(2);
      }
    }
    catch (const std::exception& e)
    {
      std::cerr << "[Plugin] Failed to update user resource library: " << e.what() << std::endl;
    }
  }

  void GuitarFXPlugin::HandleImportRemoteResourceRequest(const nlohmann::json &payload)
  {
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    const std::string name = payload.value("name", resourceId);
    const std::string description = payload.value("description", "");
    const std::string category = payload.value("category", "");
    const std::string provider = payload.value("provider", "remote");
    const std::string subfolder = payload.value("subfolder", "");
    const std::string data = payload.value("data", "");
    const std::string fileName = payload.value("fileName", "");

    if (resourceType.empty() || resourceId.empty() || data.empty())
    {
      ReportErrorToUI("Import failed", "Missing resource metadata");
      SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Missing resource metadata"}}.dump());
      return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    auto targetDir = settingsDir / "resources" / provider;
    if (!subfolder.empty())
    {
      targetDir /= subfolder;
    }
    (void)mFileSystem.EnsureDirectory(targetDir);

    std::string resolvedName = fileName.empty() ? resourceId : fileName;
    resolvedName = SanitizeFilename(resolvedName);
    if (resolvedName.find('.') == std::string::npos)
    {
      resolvedName += resourceType == "ir" ? ".wav" : ".nam";
    }

    const auto targetPath = targetDir / resolvedName;
    const std::vector<std::uint8_t> bytes = DecodeBase64(data);
    if (bytes.empty())
    {
      ReportErrorToUI("Import failed", "Invalid base64 payload");
      SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Invalid base64 payload"}}.dump());
      return;
    }
    if (!WriteFile(targetPath, bytes))
    {
      ReportErrorToUI("Import failed", "Failed to write file");
      SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Failed to write file"}}.dump());
      return;
    }

    LibraryResource resource;
    resource.type = resourceType;
    resource.id = resourceId;
    resource.name = name;
    resource.category = category;
    resource.description = description;
    resource.filePath = targetPath;

    mResourceLibrary.AddResource(resource);
    AppendUserLibraryResource(resource);
    BroadcastState();

    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["resourceType"] = resourceType;
    msg["id"] = resourceId;
    msg["name"] = name;
    msg["filePath"] = targetPath.string();
    SendMessageToUI(msg.dump());
  }

  bool GuitarFXPlugin::StartSignalPathTest(double frequencyHz, double durationSeconds)
  {
    const double sampleRate = GetSampleRate();
    if (sampleRate <= 0.0)
    {
      return false;
    }

    if (mSignalTestActive.load(std::memory_order_acquire))
    {
      return false;
    }

    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0)
    {
      frequencyHz = 440.0;
    }

    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
    {
      durationSeconds = 1.0;
    }

    const double clampedDuration = std::max(0.05, std::min(durationSeconds, 10.0));
    const int totalSamples = std::max(1, static_cast<int>(clampedDuration * sampleRate));

    SignalTestRuntimeState state;
    state.frequencyHz = frequencyHz;
    state.phase = 0.0;
    state.phaseIncrement = (kTwoPi * frequencyHz) / sampleRate;
    state.samplesRemaining = totalSamples;
    state.totalSamples = totalSamples;
    state.sampleRate = sampleRate;
    state.inputSumSquares = 0.0;
    state.outputSumSquares = {0.0, 0.0};
    state.startTime = std::chrono::steady_clock::now();

    mSignalTestState = state;

    mSignalTestResult = {};
    mSignalTestResult.frequencyHz = frequencyHz;
    mSignalTestResult.sampleRate = sampleRate;
    mSignalTestResult.durationSeconds = static_cast<double>(totalSamples) / sampleRate;

    // Reset DSP state for clean test signal processing (same as demo audio preview)
    mPresetMixer.Reset();

    mSignalTestResultPending.store(false, std::memory_order_release);
    mSignalTestActive.store(true, std::memory_order_release);
    return true;
  }

  void GuitarFXPlugin::SaveAppSettings() const
  {
    try
    {
      const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
      (void)mFileSystem.EnsureDirectory(settingsDir);
      const auto settingsFile = mFileSystem.ResolveSettingsFile();

      nlohmann::json settings;
      
      // Save last preset info (standalone only)
      if (kIsStandaloneBuild)
      {
        settings["lastPresetId"] = mActivePresetId;
        settings["lastPresetJson"] = mActivePresetJson;
      }

      // UI settings
      nlohmann::json uiSettings;
      uiSettings["zoom"] = mUiZoom;
      if (mWindowBounds.HasBounds())
      {
        uiSettings["bounds"] = {
          {"x", mWindowBounds.x},
          {"y", mWindowBounds.y},
          {"width", mWindowBounds.width},
          {"height", mWindowBounds.height},
        };
      }
      settings["uiSettings"] = std::move(uiSettings);

      // App settings
      settings["appSettings"] = mAppSettings;

      // Audio settings
      nlohmann::json audioSettings;
      audioSettings["autoLevelInput"] = mPresetMixer.GetAutoLevelInput();
      audioSettings["autoLevelOutput"] = mPresetMixer.GetAutoLevelOutput();
      settings["audioSettings"] = std::move(audioSettings);

      // Global signal chain configuration
      settings["globalSignalChain"] = mPresetMixer.GetGlobalChainConfig();

      // Save all current parameter values
      nlohmann::json parameters = nlohmann::json::array();
      for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
      {
        const auto *param = GetParam(paramIdx);
        if (param)
        {
          const std::string key = ParamKey(static_cast<ParameterId>(paramIdx));
          if (!key.empty())
          {
            parameters.push_back({{"id", key}, {"value", param->Value()}});
          }
        }
      }
      settings["parameters"] = std::move(parameters);

      std::ofstream outputFile(settingsFile);
      if (outputFile)
      {
        outputFile << settings.dump(2);
        std::cout << "[Plugin] Saved app settings to: " << settingsFile.generic_string() << std::endl;
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << "[Plugin] Failed to save app settings: " << e.what() << std::endl;
    }
  }

  void GuitarFXPlugin::LoadAppSettings()
  {
    try
    {
      const auto settingsFile = mFileSystem.ResolveSettingsFile();
      if (!std::filesystem::exists(settingsFile))
      {
        std::cout << "[Plugin] No settings file found at: " << settingsFile.generic_string() << std::endl;
        return;
      }

      std::ifstream inputFile(settingsFile);
      if (!inputFile)
      {
        std::cerr << "[Plugin] Failed to open settings file" << std::endl;
        return;
      }

      // Read file contents first, then validate JSON
      std::stringstream buffer;
      buffer << inputFile.rdbuf();
      const std::string contents = buffer.str();
      
      if (!nlohmann::json::accept(contents))
      {
        std::cerr << "[Plugin] Settings file contains invalid JSON" << std::endl;
        return;
      }

      nlohmann::json settings = nlohmann::json::parse(contents, nullptr, false);
      if (settings.is_discarded() || !settings.is_object())
      {
        std::cerr << "[Plugin] Settings file is not a valid JSON object" << std::endl;
        return;
      }

      // Restore last preset (standalone only)
      if (kIsStandaloneBuild)
      {
        mActivePresetId = settings.value("lastPresetId", "");
        mActivePresetJson = settings.value("lastPresetJson", "");
      }

      // Restore UI settings
      if (settings.contains("uiSettings") && settings["uiSettings"].is_object())
      {
        const auto &uiSettings = settings["uiSettings"];
        mUiZoom = uiSettings.value("zoom", mUiZoom);
        if (uiSettings.contains("bounds") && uiSettings["bounds"].is_object())
        {
          const auto &b = uiSettings["bounds"];
          mWindowBounds.x = b.value("x", mWindowBounds.x);
          mWindowBounds.y = b.value("y", mWindowBounds.y);
          mWindowBounds.width = b.value("width", mWindowBounds.width);
          mWindowBounds.height = b.value("height", mWindowBounds.height);
        }
      }

      if (settings.contains("appSettings") && settings["appSettings"].is_object())
      {
        mAppSettings = settings["appSettings"];
        const auto it = mAppSettings.find(kSignalDiagnosticsSettingKey);
        if (it != mAppSettings.end())
        {
          bool enabled = false;
          if (it->is_boolean())
          {
            enabled = it->get<bool>();
          }
          else if (it->is_number())
          {
            enabled = it->get<double>() != 0.0;
          }
          mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
          mPresetMixer.SetSignalDiagnosticsEnabled(enabled);
        }

        if (kIsStandaloneBuild)
        {
          const auto bpmIt = mAppSettings.find(kMetronomeBpmSettingKey);
          if (bpmIt != mAppSettings.end() && bpmIt->is_number())
          {
            const double bpm = ClampValue(bpmIt->get<double>(), kMetronomeMinBpm, kMetronomeMaxBpm);
            mMetronomeBpm.store(bpm, std::memory_order_release);
          }

          const auto enabledIt = mAppSettings.find(kMetronomeEnabledSettingKey);
          if (enabledIt != mAppSettings.end())
          {
            bool enabled = false;
            if (enabledIt->is_boolean())
            {
              enabled = enabledIt->get<bool>();
            }
            else if (enabledIt->is_number())
            {
              enabled = enabledIt->get<double>() != 0.0;
            }
            mMetronomeEnabled.store(enabled, std::memory_order_release);
          }
        }
      }

      // Restore audio settings
      if (settings.contains("audioSettings") && settings["audioSettings"].is_object())
      {
        const auto &audioSettings = settings["audioSettings"];
        mPresetMixer.SetAutoLevelInput(audioSettings.value("autoLevelInput", mPresetMixer.GetAutoLevelInput()));
        mPresetMixer.SetAutoLevelOutput(audioSettings.value("autoLevelOutput", mPresetMixer.GetAutoLevelOutput()));
      }

      // Restore global signal chain configuration
      if (settings.contains("globalSignalChain") && settings["globalSignalChain"].is_object())
      {
        auto globalChain = settings["globalSignalChain"].get<GlobalSignalChainConfig>();
        mPresetMixer.SetGlobalChainConfig(globalChain);
        std::cout << "[Plugin] Restored global signal chain configuration" << std::endl;
      }

      // Restore parameters
      if (settings.contains("parameters") && settings["parameters"].is_array())
      {
        for (const auto &paramJson : settings["parameters"])
        {
          const std::string id = paramJson.value("id", "");
          const double value = paramJson.value("value", 0.0);
          
          const auto paramId = ParamIdFromKey(id);
          if (paramId)
          {
            auto *param = GetParam(static_cast<int>(*paramId));
            if (param)
            {
              param->Set(value);
            }
          }
        }
      }

      std::cout << "[Plugin] Loaded app settings from: " << settingsFile.generic_string() << std::endl;
    }
    catch (const std::exception &e)
    {
      std::cerr << "[Plugin] Failed to load app settings: " << e.what() << std::endl;
    }
  }

  void GuitarFXPlugin::LoadResourceLibraries()
  {
    if (mResourceRoot.empty())
    {
      std::cerr << "[Plugin] Cannot load resource libraries: resource root not set" << std::endl;
      return;
    }

    // Clear the library before reloading
    mResourceLibrary.Clear();

    auto addResource = [this](const LibraryResource& resource)
    {
      mResourceLibrary.AddResource(resource);
    };

    const std::filesystem::path dataDir = mResourceRoot / "ui" / "data";
    
    // Load AudioFX models library
    const std::filesystem::path modelsPath = dataDir / "audiofx-models.json";
    if (std::filesystem::exists(modelsPath))
    {
      try
      {
        std::ifstream file(modelsPath);
        if (file.is_open())
        {
          nlohmann::json json;
          file >> json;
          
          if (json.is_array())
          {
            for (const auto& item : json)
            {
              LibraryResource resource;
              resource.type = "nam";
              resource.id = item.value("id", "");
              resource.name = item.value("title", "");
              resource.category = item.value("category", "");
              resource.description = item.value("description", "");
              resource.hash = item.value("hash", "");
              
              // filePath is relative to resource root
              const std::string relPath = item.value("filePath", "");
              if (!relPath.empty())
              {
                resource.filePath = mResourceRoot / relPath;
              }
              
              if (item.contains("tags") && item["tags"].is_array())
              {
                for (const auto& tag : item["tags"])
                {
                  resource.tags.push_back(tag.get<std::string>());
                }
              }
              
              if (!resource.id.empty() && !resource.filePath.empty())
              {
                addResource(resource);
              }
            }
          }
          std::cout << "[Plugin] Loaded AudioFX models library: " 
                    << mResourceLibrary.GetResourcesByType("nam").size() << " models" << std::endl;
        }
      }
      catch (const std::exception& ex)
      {
        std::cerr << "[Plugin] Error loading AudioFX models: " << ex.what() << std::endl;
      }
    }
    else
    {
      std::cerr << "[Plugin] AudioFX models file not found: " << modelsPath.generic_string() << std::endl;
    }
    
    // Load IR library
    const std::filesystem::path irPath = dataDir / "ir-library.json";
    if (std::filesystem::exists(irPath))
    {
      try
      {
        std::ifstream file(irPath);
        if (file.is_open())
        {
          nlohmann::json json;
          file >> json;
          
          if (json.is_array())
          {
            for (const auto& item : json)
            {
              LibraryResource resource;
              resource.type = "ir";
              resource.id = item.value("id", "");
              resource.name = item.value("title", "");
              resource.category = item.value("category", "");
              resource.description = item.value("description", "");
              resource.hash = item.value("hash", "");
              
              // filePath is relative to resource root
              const std::string relPath = item.value("filePath", "");
              if (!relPath.empty())
              {
                resource.filePath = mResourceRoot / relPath;
              }
              
              if (item.contains("tags") && item["tags"].is_array())
              {
                for (const auto& tag : item["tags"])
                {
                  resource.tags.push_back(tag.get<std::string>());
                }
              }
              
              if (!resource.id.empty() && !resource.filePath.empty())
              {
                addResource(resource);
              }
            }
          }
          std::cout << "[Plugin] Loaded IR library: " 
                    << mResourceLibrary.GetResourcesByType("ir").size() << " IRs" << std::endl;
        }
      }
      catch (const std::exception& ex)
      {
        std::cerr << "[Plugin] Error loading IR library: " << ex.what() << std::endl;
      }
    }
    else
    {
      std::cerr << "[Plugin] IR library file not found: " << irPath.generic_string() << std::endl;
    }

    // Load user resource library entries from settings directory
    const auto userResourcesDir = mFileSystem.ResolveSettingsDirectory() / "resources";
    mResourceLibrary.LoadFromDirectory(userResourcesDir);
  }

  void GuitarFXPlugin::LoadLastSessionState()
  {
    LoadAppSettings();

    // Apply loaded parameters to DSP
    for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
    {
      OnParamChange(paramIdx);
    }

    // Restore preset from JSON if available
    if (!mActivePresetJson.empty() && nlohmann::json::accept(mActivePresetJson))
    {
      try
      {
        auto presetOpt = PresetStorage::DeserializeFromJson(mActivePresetJson);
        if (presetOpt)
        {
          EnsureParametricEQNode(*presetOpt, *this);
          mActivePreset = *presetOpt;
          mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
        }
      }
      catch (...)
      {
        mActivePresetJson.clear();
      }
    }

    bool shouldApplyPreset = static_cast<bool>(mActivePreset);

    if (shouldApplyPreset && mActivePreset)
    {
      ApplyPreset(*mActivePreset);
      mPendingStateBroadcast = true;
    }

    mPendingStateBroadcast = true;
    std::cout << "[Plugin] Last session state restored" << std::endl;
  }

  void GuitarFXPlugin::HandleAddSignalPathNodeRequest(const nlohmann::json &payload)
  {
    const std::string effectType = payload.value("effectType", "");
    const std::string insertAfter = payload.value("insertAfter", "");

    std::string edgeFrom;
    std::string edgeTo;
    int edgeFromPort = 0;
    int edgeToPort = 0;
    double edgeGain = 1.0;

    const auto edgeIt = payload.find("edge");
    if (edgeIt != payload.end() && edgeIt->is_object())
    {
      edgeFrom = edgeIt->value("from", "");
      edgeTo = edgeIt->value("to", "");
      edgeFromPort = edgeIt->value("fromPort", 0);
      edgeToPort = edgeIt->value("toPort", 0);
      edgeGain = edgeIt->value("gain", 1.0);
    }

    if (effectType.empty() || (insertAfter.empty() && edgeFrom.empty()))
    {
      ReportErrorToUI("Add node failed", "Missing effectType or insertion target (insertAfter/edge)");
      return;
    }

    if (!mActivePreset)
    {
      ReportErrorToUI("Add node failed", "No active preset");
      return;
    }

    auto& edges = mActivePreset->graph.edges;
    auto chosenEdgeIt = edges.end();

    if (!edgeFrom.empty() && !edgeTo.empty())
    {
      chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e)
        {
          return e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort;
        });
    }
    else
    {
      chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e)
        {
          if (e.from != insertAfter)
            return false;
          // Prefer the primary port for back-compat
          return e.fromPort == 0;
        });

      if (chosenEdgeIt == edges.end())
      {
        chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
          [&](const GraphEdge& e)
          {
            return e.from == insertAfter;
          });
      }
    }

    if (chosenEdgeIt == edges.end())
    {
      ReportErrorToUI("Add node failed", "Could not find target edge for insertion");
      return;
    }

    if (effectType == "splitter")
    {
      auto& graph = mActivePreset->graph;

      const std::string splitterId = MakeUniqueNodeId(graph, "split");
      const std::string mixerId = MakeUniqueNodeId(graph, "mix");

      GraphNode splitter;
      splitter.id = splitterId;
      splitter.type = "splitter";
      splitter.category = "utility";
      splitter.label = "Splitter";
      splitter.enabled = true;

      GraphNode mixer;
      mixer.id = mixerId;
      mixer.type = "mixer";
      mixer.category = "utility";
      mixer.label = "Mixer";
      mixer.enabled = true;

      // Preserve edge attributes for the final connection
      const std::string nextNodeId = chosenEdgeIt->to;
      const int preservedToPort = chosenEdgeIt->toPort;
      const double preservedGain = chosenEdgeIt->gain;

      // Rewire: prev -> splitter (replace the original edge)
      chosenEdgeIt->to = splitterId;
      chosenEdgeIt->toPort = 0;
      chosenEdgeIt->gain = 1.0;

      // Two initial branches: splitter -> mixer (port 0/1)
      GraphEdge branch0;
      branch0.from = splitterId;
      branch0.to = mixerId;
      branch0.fromPort = 0;
      branch0.toPort = 0;
      branch0.gain = 1.0;

      GraphEdge branch1;
      branch1.from = splitterId;
      branch1.to = mixerId;
      branch1.fromPort = 1;
      branch1.toPort = 1;
      branch1.gain = 1.0;

      // mixer -> next
      GraphEdge mixToNext;
      mixToNext.from = mixerId;
      mixToNext.to = nextNodeId;
      mixToNext.fromPort = 0;
      mixToNext.toPort = preservedToPort;
      mixToNext.gain = preservedGain;

      edges.push_back(branch0);
      edges.push_back(branch1);
      edges.push_back(mixToNext);
      graph.nodes.push_back(splitter);
      graph.nodes.push_back(mixer);

      mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
      ApplyPreset(*mActivePreset);
      BroadcastState();

      std::cout << "[Plugin] Inserted Splitter on edge " << chosenEdgeIt->from << " -> " << nextNodeId
                << " via " << splitterId << "/" << mixerId << std::endl;
      return;
    }

    // Create new node with default parameters
    GraphNode newNode;
    newNode.id = effectType + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    newNode.type = effectType;
    newNode.enabled = true;

    // Get effect info from registry to set category and display name
    const auto effectInfoOpt = EffectRegistry::Instance().GetTypeInfo(effectType);
    if (effectInfoOpt)
    {
      const auto& effectInfo = *effectInfoOpt;
      newNode.category = effectInfo.category;
      newNode.label = effectInfo.displayName;
      
      // Set default parameter values
      for (const auto& paramDef : effectInfo.parameters)
      {
        newNode.params[paramDef.id] = paramDef.defaultValue;
      }
    }
    else
    {
      newNode.category = "utility";
      newNode.label = effectType;
    }

    // Preserve edge attributes (important for mixer input gains)
    const std::string nextNodeId = chosenEdgeIt->to;
    const int preservedToPort = chosenEdgeIt->toPort;
    const double preservedGain = chosenEdgeIt->gain;

    (void)edgeGain; // UI-provided gain is informational; we honor the graph's current gain.

    // Update existing edge to point to new node
    chosenEdgeIt->to = newNode.id;
    chosenEdgeIt->toPort = 0;
    chosenEdgeIt->gain = 1.0;

    // Add new edge from new node to next node
    GraphEdge newEdge;
    newEdge.from = newNode.id;
    newEdge.to = nextNodeId;
    newEdge.fromPort = 0;
    newEdge.toPort = preservedToPort;
    newEdge.gain = preservedGain;
    edges.push_back(newEdge);

    // Add the node
    mActivePreset->graph.nodes.push_back(newNode);

    // Re-serialize and broadcast
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    ApplyPreset(*mActivePreset);
    BroadcastState();

    std::cout << "[Plugin] Added node: " << newNode.id << " (" << effectType << ") after " << insertAfter << std::endl;
  }

  void GuitarFXPlugin::HandleSplitSignalPathEdgeRequest(const nlohmann::json &payload)
  {
    if (!mActivePreset)
    {
      ReportErrorToUI("Split failed", "No active preset");
      return;
    }

    const auto edgeIt = payload.find("edge");
    if (edgeIt == payload.end() || !edgeIt->is_object())
    {
      ReportErrorToUI("Split failed", "Missing edge payload");
      return;
    }

    const std::string from = edgeIt->value("from", "");
    const std::string to = edgeIt->value("to", "");
    const int fromPort = edgeIt->value("fromPort", 0);
    const int toPort = edgeIt->value("toPort", 0);

    if (from.empty() || to.empty())
    {
      ReportErrorToUI("Split failed", "Edge is missing from/to");
      return;
    }

    auto& graph = mActivePreset->graph;
    auto& edges = graph.edges;

    auto targetEdgeIt = std::find_if(edges.begin(), edges.end(),
      [&](const GraphEdge& e)
      {
        return e.from == from && e.to == to && e.fromPort == fromPort && e.toPort == toPort;
      });

    if (targetEdgeIt == edges.end())
    {
      ReportErrorToUI("Split failed", "Target edge not found");
      return;
    }

    const std::string splitterId = MakeUniqueNodeId(graph, "split");
    const std::string mixerId = MakeUniqueNodeId(graph, "mix");

    GraphNode splitter;
    splitter.id = splitterId;
    splitter.type = "splitter";
    splitter.category = "utility";
    splitter.label = "Splitter";
    splitter.enabled = true;

    GraphNode mixer;
    mixer.id = mixerId;
    mixer.type = "mixer";
    mixer.category = "utility";
    mixer.label = "Mixer";
    mixer.enabled = true;

    // Preserve edge attributes for the final connection
    const std::string nextNodeId = targetEdgeIt->to;
    const int preservedToPort = targetEdgeIt->toPort;
    const double preservedGain = targetEdgeIt->gain;

    // Rewire: from -> splitter (replace the original edge)
    targetEdgeIt->to = splitterId;
    targetEdgeIt->toPort = 0;
    targetEdgeIt->gain = 1.0;

    // Two initial branches: splitter -> mixer (port 0/1)
    GraphEdge branch0;
    branch0.from = splitterId;
    branch0.to = mixerId;
    branch0.fromPort = 0;
    branch0.toPort = 0;
    branch0.gain = 1.0;

    GraphEdge branch1;
    branch1.from = splitterId;
    branch1.to = mixerId;
    branch1.fromPort = 1;
    branch1.toPort = 1;
    branch1.gain = 1.0;

    // mixer -> next
    GraphEdge mixToNext;
    mixToNext.from = mixerId;
    mixToNext.to = nextNodeId;
    mixToNext.fromPort = 0;
    mixToNext.toPort = preservedToPort;
    mixToNext.gain = preservedGain;

    edges.push_back(branch0);
    edges.push_back(branch1);
    edges.push_back(mixToNext);
    graph.nodes.push_back(splitter);
    graph.nodes.push_back(mixer);

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    ApplyPreset(*mActivePreset);
    BroadcastState();

    std::cout << "[Plugin] Split edge " << from << " -> " << to << " into parallel via " << splitterId << "/" << mixerId << std::endl;
  }

  void GuitarFXPlugin::HandleCollapseSignalPathSplitRequest(const nlohmann::json &payload)
  {
    if (!mActivePreset)
    {
      ReportErrorToUI("Collapse split failed", "No active preset");
      return;
    }

    const std::string splitterId = payload.value("splitterId", "");
    const std::string mixerId = payload.value("mixerId", "");
    if (splitterId.empty() || mixerId.empty())
    {
      ReportErrorToUI("Collapse split failed", "Missing splitterId/mixerId");
      return;
    }

    auto& graph = mActivePreset->graph;
    auto& edges = graph.edges;

    // Only support collapsing when branches are empty (splitter connects directly to mixer)
    std::vector<GraphEdge*> splitterOut;
    std::vector<GraphEdge*> mixerIn;
    GraphEdge* mixerOut = nullptr;
    GraphEdge* splitterIn = nullptr;

    for (auto& e : edges)
    {
      if (e.from == splitterId)
        splitterOut.push_back(&e);
      if (e.to == mixerId)
        mixerIn.push_back(&e);
      if (e.from == mixerId)
        mixerOut = &e;
      if (e.to == splitterId)
        splitterIn = &e;
    }

    if (!splitterIn || !mixerOut)
    {
      ReportErrorToUI("Collapse split failed", "Split is not connected correctly");
      return;
    }

    const bool branchesEmpty = !splitterOut.empty() && std::all_of(splitterOut.begin(), splitterOut.end(),
      [&](const GraphEdge* e) { return e && e->to == mixerId; });

    if (!branchesEmpty)
    {
      ReportErrorToUI("Collapse split failed", "Can only collapse an empty split (remove branch effects first)");
      return;
    }

    // Rewire splitterIn (prev -> splitter) to point to mixerOut target (next)
    splitterIn->to = mixerOut->to;
    splitterIn->toPort = mixerOut->toPort;
    splitterIn->gain = mixerOut->gain;

    // Remove edges related to the split
    edges.erase(std::remove_if(edges.begin(), edges.end(),
      [&](const GraphEdge& e)
      {
        if (e.from == splitterId) return true;
        if (e.from == mixerId) return true;
        if (e.to == mixerId) return true;
        return false;
      }), edges.end());

    // Remove nodes
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
      [&](const GraphNode& n)
      {
        return n.id == splitterId || n.id == mixerId;
      }), graph.nodes.end());

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    ApplyPreset(*mActivePreset);
    BroadcastState();

    std::cout << "[Plugin] Collapsed empty split " << splitterId << "/" << mixerId << std::endl;
  }

  void GuitarFXPlugin::HandleReplaceSignalPathNodeRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string newEffectType = payload.value("newEffectType", "");

    if (nodeId.empty() || newEffectType.empty())
    {
      ReportErrorToUI("Replace node failed", "Missing nodeId or newEffectType parameter");
      return;
    }

    if (!mActivePreset)
    {
      ReportErrorToUI("Replace node failed", "No active preset");
      return;
    }

    // Find the node
    GraphNode* node = mActivePreset->graph.FindNode(nodeId);
    if (!node)
    {
      ReportErrorToUI("Replace node failed", "Node not found: " + nodeId);
      return;
    }

    // Get old and new effect info
    const auto oldEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(node->type);
    const auto newEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(newEffectType);

    if (!newEffectInfoOpt)
    {
      ReportErrorToUI("Replace node failed", "Unknown effect type: " + newEffectType);
      return;
    }

    const auto& newEffectInfo = *newEffectInfoOpt;

    // Verify same category (safety check)
    if (oldEffectInfoOpt && oldEffectInfoOpt->category != newEffectInfo.category)
    {
      ReportErrorToUI("Replace node failed", "Cannot replace effect with different category");
      return;
    }

    // Replace the node's type and reset parameters
    node->type = newEffectType;
    node->label = newEffectInfo.displayName;
    node->category = newEffectInfo.category;
    node->params.clear();
    node->resource.reset();

    // Set default parameter values for new effect type
    for (const auto& paramDef : newEffectInfo.parameters)
    {
      node->params[paramDef.id] = paramDef.defaultValue;
    }

    // Re-serialize and broadcast
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    ApplyPreset(*mActivePreset);
    BroadcastState();

    std::cout << "[Plugin] Replaced node: " << nodeId << " with " << newEffectType << std::endl;
  }

  void GuitarFXPlugin::HandleReorderSignalPathNodeRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string targetNodeId = payload.value("targetNodeId", "");

    std::string edgeFrom;
    std::string edgeTo;
    int edgeFromPort = 0;
    int edgeToPort = 0;

    const auto edgeIt = payload.find("edge");
    if (edgeIt != payload.end() && edgeIt->is_object())
    {
      edgeFrom = edgeIt->value("from", "");
      edgeTo = edgeIt->value("to", "");
      edgeFromPort = edgeIt->value("fromPort", 0);
      edgeToPort = edgeIt->value("toPort", 0);
    }

    if (nodeId.empty() || (targetNodeId.empty() && edgeFrom.empty()))
    {
      return;
    }

    if (!mActivePreset)
    {
      ReportErrorToUI("Reorder node failed", "No active preset");
      return;
    }

    // Find moving node
    const GraphNode* node = mActivePreset->graph.FindNode(nodeId);
    if (!node)
    {
      ReportErrorToUI("Reorder node failed", "Node not found");
      return;
    }

    if (node->type == "splitter" || node->type == "mixer")
    {
      ReportErrorToUI("Reorder node failed", "Cannot move splitter/mixer nodes");
      return;
    }

    auto& edges = mActivePreset->graph.edges;

    // Find edges connected to the moving node (expect single in/out)
    auto incomingEdgeIt = std::find_if(edges.begin(), edges.end(),
      [&nodeId](const GraphEdge& e) { return e.to == nodeId; });
    auto outgoingEdgeIt = std::find_if(edges.begin(), edges.end(),
      [&nodeId](const GraphEdge& e) { return e.from == nodeId; });

    if (incomingEdgeIt == edges.end() || outgoingEdgeIt == edges.end())
    {
      ReportErrorToUI("Reorder node failed", "Missing edges");
      return;
    }

    // Preserve outgoing edge attributes before removal
    const std::string nextNodeId = outgoingEdgeIt->to;
    const int preservedToPort = outgoingEdgeIt->toPort;
    const double preservedGain = outgoingEdgeIt->gain;

    // Reconnect around the moving node
    incomingEdgeIt->to = nextNodeId;
    incomingEdgeIt->toPort = preservedToPort;
    incomingEdgeIt->gain = preservedGain;
    edges.erase(outgoingEdgeIt);

    if (!edgeFrom.empty() && !edgeTo.empty())
    {
      if (edgeFrom == nodeId || edgeTo == nodeId)
      {
        ReportErrorToUI("Reorder node failed", "Cannot move node onto itself");
        return;
      }

      auto targetEdgeIt = std::find_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e)
        {
          return e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort;
        });

      if (targetEdgeIt == edges.end())
      {
        ReportErrorToUI("Reorder node failed", "Cannot find target edge");
        return;
      }

      const std::string targetNextId = targetEdgeIt->to;
      const int targetPreservedToPort = targetEdgeIt->toPort;
      const double targetPreservedGain = targetEdgeIt->gain;

      targetEdgeIt->to = nodeId;
      targetEdgeIt->toPort = 0;
      targetEdgeIt->gain = 1.0;

      GraphEdge newEdge;
      newEdge.from = nodeId;
      newEdge.to = targetNextId;
      newEdge.fromPort = 0;
      newEdge.toPort = targetPreservedToPort;
      newEdge.gain = targetPreservedGain;
      edges.push_back(newEdge);
    }
    else
    {
      // Find edge after target node
      const GraphNode* targetNode = mActivePreset->graph.FindNode(targetNodeId);
      if (!targetNode)
      {
        ReportErrorToUI("Reorder node failed", "Target node not found");
        return;
      }

      auto targetOutgoingIt = std::find_if(edges.begin(), edges.end(),
        [&targetNodeId](const GraphEdge& e) { return e.from == targetNodeId; });

      if (targetOutgoingIt == edges.end())
      {
        ReportErrorToUI("Reorder node failed", "Cannot find target position");
        return;
      }

      const std::string afterTargetNodeId = targetOutgoingIt->to;
      const int targetPreservedToPort = targetOutgoingIt->toPort;
      const double targetPreservedGain = targetOutgoingIt->gain;

      // Insert node after target
      targetOutgoingIt->to = nodeId;
      targetOutgoingIt->toPort = 0;
      targetOutgoingIt->gain = 1.0;

      GraphEdge newEdge;
      newEdge.from = nodeId;
      newEdge.to = afterTargetNodeId;
      newEdge.fromPort = 0;
      newEdge.toPort = targetPreservedToPort;
      newEdge.gain = targetPreservedGain;
      edges.push_back(newEdge);
    }

    // Re-serialize and broadcast
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    ApplyPreset(*mActivePreset);
    BroadcastState();

    std::cout << "[Plugin] Reordered node: " << nodeId << " after " << targetNodeId << std::endl;
  }

  void GuitarFXPlugin::HandleDeleteSignalPathNodeRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");

    if (nodeId.empty())
    {
      return;
    }

    if (!mActivePreset)
    {
      ReportErrorToUI("Delete node failed", "No active preset");
      return;
    }

    // Find the node
    const GraphNode* node = mActivePreset->graph.FindNode(nodeId);
    if (!node)
    {
      ReportErrorToUI("Delete node failed", "Node not found: " + nodeId);
      return;
    }

    auto& edges = mActivePreset->graph.edges;
    auto& nodes = mActivePreset->graph.nodes;

    // Find incoming and outgoing edges
    auto incomingEdgeIt = std::find_if(edges.begin(), edges.end(),
      [&nodeId](const GraphEdge& e) { return e.to == nodeId; });
    auto outgoingEdgeIt = std::find_if(edges.begin(), edges.end(),
      [&nodeId](const GraphEdge& e) { return e.from == nodeId; });

    if (incomingEdgeIt == edges.end() || outgoingEdgeIt == edges.end())
    {
      ReportErrorToUI("Delete node failed", "Missing edges");
      return;
    }

    // Reconnect around deleted node
    std::string nextNodeId = outgoingEdgeIt->to;
    incomingEdgeIt->to = nextNodeId;

    // Remove outgoing edge and node
    edges.erase(outgoingEdgeIt);
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
      [&nodeId](const GraphNode& n) { return n.id == nodeId; }), nodes.end());

    // Re-serialize and broadcast
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    ApplyPreset(*mActivePreset);
    BroadcastState();

    std::cout << "[Plugin] Deleted node: " << nodeId << std::endl;
  }

} // namespace guitarfx
