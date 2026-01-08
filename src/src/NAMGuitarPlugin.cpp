#include "NAMGuitarPlugin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>
#include <iostream> // For std::cout
#include <ctime> // For time()

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#endif

#include <nlohmann/json.hpp>

#include "config.h"
#include "dsp/IRTypes.h"
#include "resources/ResourceLibrary.h"
#include "IPlug_include_in_plug_src.h"
#include "IPlugPaths.h"
#include "wdlstring.h"

#include "dsp/NAMDSPManager.h"
#include "presets/PresetStorage.h"

namespace namguitar
{
  namespace
  {
    constexpr int kNumPrograms = 0;
    constexpr double kTwoPi = 6.28318530717958647692;

    std::string ParamKey(NAMGuitarPlugin::ParameterId paramId);

    nlohmann::json SerializeParametersToJson(const NAMGuitarPlugin &plugin)
    {
      nlohmann::json parameters = nlohmann::json::array();
      for (int paramIdx = 0; paramIdx < NAMGuitarPlugin::kParamCount; ++paramIdx)
      {
        const auto *param = plugin.GetParam(paramIdx);
        nlohmann::json paramJson;
        std::string key = ParamKey(static_cast<NAMGuitarPlugin::ParameterId>(paramIdx));
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
      payload["gateEnabled"] = plugin.GetParam(NAMGuitarPlugin::kParamGateEnabled)->Bool();
      payload["gateThreshold"] = plugin.GetParam(NAMGuitarPlugin::kParamGateThreshold)->Value();
      return payload;
    }

    std::string ParamKey(NAMGuitarPlugin::ParameterId paramId)
    {
      switch (paramId)
      {
      case NAMGuitarPlugin::kParamInputTrim:
        return "input_trim";
      case NAMGuitarPlugin::kParamOutputTrim:
        return "output_trim";
      case NAMGuitarPlugin::kParamDrive:
        return "drive";
      case NAMGuitarPlugin::kParamTone:
        return "tone";
      case NAMGuitarPlugin::kParamGateEnabled:
        return "gate_enabled";
      case NAMGuitarPlugin::kParamGateThreshold:
        return "gate_threshold";
      case NAMGuitarPlugin::kParamMix:
        return "mix";
      case NAMGuitarPlugin::kParamDoublerEnabled:
        return "doubler_enabled";
      case NAMGuitarPlugin::kParamDoublerDelay:
        return "doubler_delay";
      case NAMGuitarPlugin::kParamTranspose:
        return "transpose";
      case NAMGuitarPlugin::kParamSimpleCabEnabled:
        return "simplecab_enabled";
      case NAMGuitarPlugin::kParamSimpleCabBass:
        return "simplecab_bass";
      case NAMGuitarPlugin::kParamSimpleCabPresence:
        return "simplecab_presence";
      case NAMGuitarPlugin::kParamSimpleCabBrightness:
        return "simplecab_brightness";
      case NAMGuitarPlugin::kParamIRQuality:
        return "ir_quality";
      case NAMGuitarPlugin::kParamEQEnabled:
        return "eq_enabled";
      case NAMGuitarPlugin::kParamEQLowGain:
        return "eq_low_gain";
      case NAMGuitarPlugin::kParamEQLowFreq:
        return "eq_low_freq";
      case NAMGuitarPlugin::kParamEQLowMidGain:
        return "eq_lowmid_gain";
      case NAMGuitarPlugin::kParamEQLowMidFreq:
        return "eq_lowmid_freq";
      case NAMGuitarPlugin::kParamEQLowMidQ:
        return "eq_lowmid_q";
      case NAMGuitarPlugin::kParamEQHighMidGain:
        return "eq_highmid_gain";
      case NAMGuitarPlugin::kParamEQHighMidFreq:
        return "eq_highmid_freq";
      case NAMGuitarPlugin::kParamEQHighMidQ:
        return "eq_highmid_q";
      case NAMGuitarPlugin::kParamEQHighGain:
        return "eq_high_gain";
      case NAMGuitarPlugin::kParamEQHighFreq:
        return "eq_high_freq";
      // Delay effect
      case NAMGuitarPlugin::kParamDelayEnabled:
        return "delay_enabled";
      case NAMGuitarPlugin::kParamDelayTime:
        return "delay_time";
      case NAMGuitarPlugin::kParamDelayFeedback:
        return "delay_feedback";
      case NAMGuitarPlugin::kParamDelayMix:
        return "delay_mix";
      // Reverb effect
      case NAMGuitarPlugin::kParamReverbEnabled:
        return "reverb_enabled";
      case NAMGuitarPlugin::kParamReverbDecay:
        return "reverb_decay";
      case NAMGuitarPlugin::kParamReverbDamping:
        return "reverb_damping";
      case NAMGuitarPlugin::kParamReverbMix:
        return "reverb_mix";
      default:
        return "";
      }
    }

    std::optional<NAMGuitarPlugin::ParameterId> ParamIdFromKey(const std::string &key)
    {
      if (key == "input_trim")
      {
        return NAMGuitarPlugin::kParamInputTrim;
      }
      if (key == "output_trim")
      {
        return NAMGuitarPlugin::kParamOutputTrim;
      }
      if (key == "drive")
      {
        return NAMGuitarPlugin::kParamDrive;
      }
      if (key == "tone")
      {
        return NAMGuitarPlugin::kParamTone;
      }
      if (key == "gate_enabled")
      {
        return NAMGuitarPlugin::kParamGateEnabled;
      }
      if (key == "gate_threshold")
      {
        return NAMGuitarPlugin::kParamGateThreshold;
      }
      if (key == "mix")
      {
        return NAMGuitarPlugin::kParamMix;
      }
      if (key == "doubler_enabled")
      {
        return NAMGuitarPlugin::kParamDoublerEnabled;
      }
      if (key == "doubler_delay")
      {
        return NAMGuitarPlugin::kParamDoublerDelay;
      }
      if (key == "transpose")
      {
        return NAMGuitarPlugin::kParamTranspose;
      }
      if (key == "simplecab_enabled")
      {
        return NAMGuitarPlugin::kParamSimpleCabEnabled;
      }
      if (key == "simplecab_bass")
      {
        return NAMGuitarPlugin::kParamSimpleCabBass;
      }
      if (key == "simplecab_presence")
      {
        return NAMGuitarPlugin::kParamSimpleCabPresence;
      }
      if (key == "simplecab_brightness")
      {
        return NAMGuitarPlugin::kParamSimpleCabBrightness;
      }
      if (key == "ir_quality")
      {
        return NAMGuitarPlugin::kParamIRQuality;
      }
      if (key == "eq_enabled")
      {
        return NAMGuitarPlugin::kParamEQEnabled;
      }
      if (key == "eq_low_gain")
      {
        return NAMGuitarPlugin::kParamEQLowGain;
      }
      if (key == "eq_low_freq")
      {
        return NAMGuitarPlugin::kParamEQLowFreq;
      }
      if (key == "eq_lowmid_gain")
      {
        return NAMGuitarPlugin::kParamEQLowMidGain;
      }
      if (key == "eq_lowmid_freq")
      {
        return NAMGuitarPlugin::kParamEQLowMidFreq;
      }
      if (key == "eq_lowmid_q")
      {
        return NAMGuitarPlugin::kParamEQLowMidQ;
      }
      if (key == "eq_highmid_gain")
      {
        return NAMGuitarPlugin::kParamEQHighMidGain;
      }
      if (key == "eq_highmid_freq")
      {
        return NAMGuitarPlugin::kParamEQHighMidFreq;
      }
      if (key == "eq_highmid_q")
      {
        return NAMGuitarPlugin::kParamEQHighMidQ;
      }
      if (key == "eq_high_gain")
      {
        return NAMGuitarPlugin::kParamEQHighGain;
      }
      if (key == "eq_high_freq")
      {
        return NAMGuitarPlugin::kParamEQHighFreq;
      }
      // Delay effect
      if (key == "delay_enabled")
      {
        return NAMGuitarPlugin::kParamDelayEnabled;
      }
      if (key == "delay_time")
      {
        return NAMGuitarPlugin::kParamDelayTime;
      }
      if (key == "delay_feedback")
      {
        return NAMGuitarPlugin::kParamDelayFeedback;
      }
      if (key == "delay_mix")
      {
        return NAMGuitarPlugin::kParamDelayMix;
      }
      // Reverb effect
      if (key == "reverb_enabled")
      {
        return NAMGuitarPlugin::kParamReverbEnabled;
      }
      if (key == "reverb_decay")
      {
        return NAMGuitarPlugin::kParamReverbDecay;
      }
      if (key == "reverb_damping")
      {
        return NAMGuitarPlugin::kParamReverbDamping;
      }
      if (key == "reverb_mix")
      {
        return NAMGuitarPlugin::kParamReverbMix;
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

  NAMGuitarPlugin::NAMGuitarPlugin(const iplug::InstanceInfo &info)
      : iplug::Plugin(info, iplug::MakeConfig(kParamCount, kNumPrograms)), mDSP(std::make_unique<NAMDSPManager>())
  {
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

    InitializeParameters();

    for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
    {
      OnParamChange(paramIdx);
    }

    // Load last session state (preset, parameters, model, IR) from settings file
    LoadLastSessionState();

#if PLUG_HAS_UI
    SetEnableDevTools(true);
    
    // WebViewEditorDelegate pattern: use mEditorInitFunc to load the HTML UI
    mEditorInitFunc = [this]()
    {
      std::cout << "[Plugin] mEditorInitFunc called" << std::endl;
      
      // Build path to index.html in resources
      std::filesystem::path htmlPath = mResourceRoot / "ui" / "index.html";
      std::cout << "[Plugin] Loading HTML from: " << htmlPath.generic_string() << std::endl;
      
      if (std::filesystem::exists(htmlPath))
      {
        LoadFile(htmlPath.string().c_str(), nullptr);
        EnableScroll(false);
        mPendingStateBroadcast = true;
      }
      else
      {
        std::cerr << "[Plugin] index.html not found at: " << htmlPath.generic_string() << std::endl;
      }
    };
#endif
  }

#ifdef VST3_API
  Steinberg::tresult PLUGIN_API NAMGuitarPlugin::initialize(FUnknown* context)
  {
    std::cout << "[Plugin] initialize called" << std::endl;
    return iplug::Plugin::initialize(context);
  }
#endif

  void NAMGuitarPlugin::ProcessBlock(iplug::sample **inputs, iplug::sample **outputs, int nFrames)
  {
    if (!mDSP)
    {
      return;
    }

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
        const std::size_t rightChannel = channelCount > 1 ? 1 : 0;
        mPreviewInputLeft[static_cast<std::size_t>(frame)] = previewBuffer->channelSamples[leftChannel][sourceIndex];
        mPreviewInputRight[static_cast<std::size_t>(frame)] = previewBuffer->channelSamples[rightChannel][sourceIndex];
      }

      iplug::sample *previewInputs[2] = {mPreviewInputLeft.data(), mPreviewInputRight.data()};
      iplug::sample *previewOutputs[2] = {mPreviewOutputLeft.data(), mPreviewOutputRight.data()};
      mDSP->Process(previewInputs, previewOutputs, nFrames);

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
      mDSP->Process(testInputs, testOutputs, nFrames);

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

    mDSP->Process(inputs, outputs, nFrames);
  }

  void NAMGuitarPlugin::OnReset()
  {
    if (!mDSP)
    {
      return;
    }

    // Lock DSP mutex during prepare/reset
    std::lock_guard<std::mutex> lock(mDSPMutex);

    mDSP->Prepare(GetSampleRate(), GetBlockSize());

    mPreviewBuffer.store(nullptr, std::memory_order_release);
    mPreviewStartedBuffer.store(nullptr, std::memory_order_release);
    mPreviewCompletedBuffer.store(nullptr, std::memory_order_release);
    mPreviewCursor.store(0, std::memory_order_release);
  }

  void NAMGuitarPlugin::OnIdle()
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
      if (mTunerActive.load(std::memory_order_acquire))
      {
        TunerData data;
        {
          std::lock_guard<std::mutex> lock(mTunerMutex);
          data = mPendingTunerData;
        }

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
  }

  bool NAMGuitarPlugin::SerializeState(iplug::IByteChunk &chunk) const
  {
    bool success = chunk.PutStr(mActivePresetJson.c_str());
    success &= chunk.PutStr(mActivePresetId.c_str());
    success &= chunk.PutStr(mActiveModelPath.c_str());
    success &= chunk.PutStr(mActiveIRPath.c_str());
    return success;
  }

  int NAMGuitarPlugin::UnserializeState(const iplug::IByteChunk &chunk, int startPos)
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

    WDL_String modelPath;
    position = chunk.GetStr(modelPath, position);
    if (position < 0)
    {
      return startPos;
    }
    mActiveModelPath = modelPath.Get();

    WDL_String irPath;
    position = chunk.GetStr(irPath, position);
    if (position < 0)
    {
      return startPos;
    }
    mActiveIRPath = irPath.Get();

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
          ApplyPreset(*presetOpt);
          mActivePreset = *presetOpt;
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

  void NAMGuitarPlugin::OnParamChange(int paramIdx)
  {
    if (!mDSP)
    {
      return;
    }

    const auto *param = GetParam(paramIdx);
    if (!param)
    {
      return;
    }

    switch (static_cast<ParameterId>(paramIdx))
    {
    case kParamInputTrim:
      mDSP->SetInputTrim(param->Value());
      break;
    case kParamOutputTrim:
      mDSP->SetOutputTrim(param->Value());
      break;
    case kParamDrive:
      mDSP->SetDrive(param->Value());
      break;
    case kParamTone:
      mDSP->SetTone(param->Value() * 2.0 - 1.0);
      break;
    case kParamGateEnabled:
      mDSP->SetGateEnabled(param->Bool());
      break;
    case kParamGateThreshold:
      mDSP->SetGateThreshold(param->Value());
      break;
    case kParamMix:
      mDSP->SetMix(param->Value());
      break;
    case kParamDoublerEnabled:
      mDSP->SetDoublerEnabled(param->Bool());
      break;
    case kParamDoublerDelay:
      mDSP->SetDoublerDelay(param->Value());
      break;
    case kParamTranspose:
      mDSP->SetTranspose(static_cast<int>(std::round(param->Value())));
      break;
    case kParamSimpleCabEnabled:
      mDSP->SetSimpleCabEnabled(param->Bool());
      break;
    case kParamSimpleCabBass:
      mDSP->SetSimpleCabBass(param->Value());
      break;
    case kParamSimpleCabPresence:
      mDSP->SetSimpleCabPresence(param->Value());
      break;
    case kParamSimpleCabBrightness:
      mDSP->SetSimpleCabBrightness(param->Value());
      break;
    case kParamIRQuality:
      mDSP->SetIRQuality(namguitar::GetIRQualityFromInt(static_cast<int>(param->Value())));
      break;
    case kParamEQEnabled:
      mDSP->SetEQEnabled(param->Bool());
      break;
    case kParamEQLowGain:
      mDSP->SetEQBandGain(0, param->Value());
      break;
    case kParamEQLowFreq:
      mDSP->SetEQBandFrequency(0, param->Value());
      break;
    case kParamEQLowMidGain:
      mDSP->SetEQBandGain(1, param->Value());
      break;
    case kParamEQLowMidFreq:
      mDSP->SetEQBandFrequency(1, param->Value());
      break;
    case kParamEQLowMidQ:
      mDSP->SetEQBandQ(1, param->Value());
      break;
    case kParamEQHighMidGain:
      mDSP->SetEQBandGain(2, param->Value());
      break;
    case kParamEQHighMidFreq:
      mDSP->SetEQBandFrequency(2, param->Value());
      break;
    case kParamEQHighMidQ:
      mDSP->SetEQBandQ(2, param->Value());
      break;
    case kParamEQHighGain:
      mDSP->SetEQBandGain(3, param->Value());
      break;
    case kParamEQHighFreq:
      mDSP->SetEQBandFrequency(3, param->Value());
      break;
    // Delay effect
    case kParamDelayEnabled:
      mDSP->SetDelayEnabled(param->Bool());
      break;
    case kParamDelayTime:
      mDSP->SetDelayTime(param->Value());
      break;
    case kParamDelayFeedback:
      mDSP->SetDelayFeedback(param->Value() / 100.0);
      break;
    case kParamDelayMix:
      mDSP->SetDelayMix(param->Value() / 100.0);
      break;
    // Reverb effect
    case kParamReverbEnabled:
      mDSP->SetReverbEnabled(param->Bool());
      break;
    case kParamReverbDecay:
      mDSP->SetReverbDecay(param->Value());
      break;
    case kParamReverbDamping:
      mDSP->SetReverbDamping(param->Value());
      break;
    case kParamReverbMix:
      mDSP->SetReverbMix(param->Value() / 100.0);
      break;
    default:
      break;
    }

    mPendingStateBroadcast = true;
  }

  void NAMGuitarPlugin::InitializeParameters()
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
  void NAMGuitarPlugin::SendMessageToUI(const std::string& jsonMessage)
  {
    // Call the JavaScript handler function in the WebView
    // The UI registers window.IPlugReceiveData to receive messages as a JSON string
    // We need to escape the JSON string for JavaScript string literal
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
    std::string jsCode = "if (window.IPlugReceiveData) { window.IPlugReceiveData(\"" + escaped + "\"); }";
    EvaluateJavaScript(jsCode.c_str());
  }

  // Override to intercept custom JSON messages before base class processing
  // The base class expects messages with a "msg" field for iPlug2 internal protocol.
  // Our UI sends custom messages with a "type" field instead.
  void NAMGuitarPlugin::OnMessageFromWebView(const char* jsonStr)
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
    
    // Otherwise, let the base class handle iPlug2 internal messages (SPVFUI, BPCFUI, etc.)
    iplug::WebViewEditorDelegate::OnMessageFromWebView(jsonStr);
  }

  // Handle messages from the WebView via OnMessage callback
  // The WebViewEditorDelegate calls this for arbitrary messages (SAMFUI from JS)
  bool NAMGuitarPlugin::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
  {
    // msgTag == -1 means this is a JSON message from SendArbitraryMsgFromUI
    if (msgTag == -1 && dataSize > 0 && pData != nullptr)
    {
      std::string message(reinterpret_cast<const char*>(pData), dataSize);
      std::cout << "[Plugin] OnMessage received JSON: " << message.substr(0, 100) << "..." << std::endl;
      HandleUIMessage(message);
      return true;
    }
    return false;
  }

  void NAMGuitarPlugin::HandleUIMessage(const std::string &message)
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
    std::cerr << "[NAMGuitarPlugin] Received UI message of type: " << type << std::endl;
    
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
    else if (type == "updateNodeParam")
    {
      HandleUpdateNodeParamRequest(payload);
    }
    else if (type == "updateNodeBypass")
    {
      HandleUpdateNodeBypassRequest(payload);
    }
    else if (type == "updateNodeResource")
    {
      HandleUpdateNodeResourceRequest(payload);
    }
    else if (type == "browseNodeResource")
    {
      HandleBrowseNodeResourceRequest(payload);
    }
  }

  void NAMGuitarPlugin::BroadcastState()
  {
    nlohmann::json message;
    message["type"] = "state";
    message["activePresetId"] = mActivePresetId;

    auto parameters = SerializeParametersToJson(*this);
    parameters["modelPath"] = mActiveModelPath;
    parameters["irPath"] = mActiveIRPath;
    message["parameters"] = std::move(parameters);

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
  }

  void NAMGuitarPlugin::ApplyPreset(const Preset &preset)
  {
    if (!mDSP)
    {
      return;
    }

    // Lock DSP mutex to prevent ProcessBlock from accessing DSP during modification
    std::lock_guard<std::mutex> lock(mDSPMutex);

    // Reset DSP state before applying new preset to avoid artifacts
    mDSP->Reset();
    
    // Clear current model and IR - preset should fully replace current state
    mDSP->ClearModel();
    mDSP->ClearImpulseResponse();
    mActiveModelPath.clear();
    mActiveIRPath.clear();

    // Apply global settings from the preset
    auto *inputTrimParam = GetParam(kParamInputTrim);
    if (inputTrimParam)
    {
      inputTrimParam->Set(preset.global.inputTrim);
      OnParamChange(kParamInputTrim);
    }

    auto *outputTrimParam = GetParam(kParamOutputTrim);
    if (outputTrimParam)
    {
      outputTrimParam->Set(preset.global.outputTrim);
      OnParamChange(kParamOutputTrim);
    }

    auto *transposeParam = GetParam(kParamTranspose);
    if (transposeParam)
    {
      transposeParam->Set(static_cast<double>(preset.global.transpose));
      OnParamChange(kParamTranspose);
    }

    // Process each node in the signal graph
    for (const auto &node : preset.graph.nodes)
    {
      // Apply node parameters to plugin parameters based on node type
      if (node.type == "amp_nam" || node.type == "nam_amp" || node.type == "nam")
      {
        // Load NAM model if resource is specified
        if (node.resource && node.resource->IsValid())
        {
          auto resolvedPath = ResolveResourceRef(*node.resource);
          if (resolvedPath && mDSP->LoadModel(*resolvedPath))
          {
            mActiveModelPath = resolvedPath->generic_string();
          }
        }

        // Apply node parameters
        for (const auto &[key, value] : node.params)
        {
          if (key == "drive")
          {
            auto *driveParam = GetParam(kParamDrive);
            if (driveParam)
            {
              driveParam->Set(value);
              OnParamChange(kParamDrive);
            }
          }
          else if (key == "tone")
          {
            auto *toneParam = GetParam(kParamTone);
            if (toneParam)
            {
              toneParam->Set(value);
              OnParamChange(kParamTone);
            }
          }
        }
      }
      else if (node.type == "cab_ir" || node.type == "ir_cab" || node.type == "ir")
      {
        // Load IR if resource is specified
        if (node.resource && node.resource->IsValid())
        {
          auto resolvedPath = ResolveResourceRef(*node.resource);
          if (resolvedPath && mDSP->LoadImpulseResponse(*resolvedPath))
          {
            mActiveIRPath = resolvedPath->generic_string();
          }
        }
      }
      else if (node.type == "dynamics_gate" || node.type == "noise_gate" || node.type == "gate")
      {
        auto *gateEnabledParam = GetParam(kParamGateEnabled);
        if (gateEnabledParam)
        {
          gateEnabledParam->Set(node.enabled ? 1.0 : 0.0);
          OnParamChange(kParamGateEnabled);
        }

        for (const auto &[key, value] : node.params)
        {
          if (key == "threshold")
          {
            auto *thresholdParam = GetParam(kParamGateThreshold);
            if (thresholdParam)
            {
              thresholdParam->Set(value);
              OnParamChange(kParamGateThreshold);
            }
          }
        }
      }
      else if (node.type == "eq_parametric" || node.type == "eq")
      {
        auto *eqEnabledParam = GetParam(kParamEQEnabled);
        if (eqEnabledParam)
        {
          eqEnabledParam->Set(node.enabled ? 1.0 : 0.0);
          OnParamChange(kParamEQEnabled);
        }

        // Map EQ node parameters to plugin EQ parameters
        for (const auto &[key, value] : node.params)
        {
          std::optional<ParameterId> paramId;
          if (key == "lowGain") paramId = kParamEQLowGain;
          else if (key == "lowFreq") paramId = kParamEQLowFreq;
          else if (key == "lowMidGain") paramId = kParamEQLowMidGain;
          else if (key == "lowMidFreq") paramId = kParamEQLowMidFreq;
          else if (key == "lowMidQ") paramId = kParamEQLowMidQ;
          else if (key == "highMidGain") paramId = kParamEQHighMidGain;
          else if (key == "highMidFreq") paramId = kParamEQHighMidFreq;
          else if (key == "highMidQ") paramId = kParamEQHighMidQ;
          else if (key == "highGain") paramId = kParamEQHighGain;
          else if (key == "highFreq") paramId = kParamEQHighFreq;

          if (paramId)
          {
            auto *param = GetParam(static_cast<int>(*paramId));
            if (param)
            {
              param->Set(value);
              OnParamChange(static_cast<int>(*paramId));
            }
          }
        }
      }
      else if (node.type == "delay")
      {
        auto *delayEnabledParam = GetParam(kParamDelayEnabled);
        if (delayEnabledParam)
        {
          delayEnabledParam->Set(node.enabled ? 1.0 : 0.0);
          OnParamChange(kParamDelayEnabled);
        }

        for (const auto &[key, value] : node.params)
        {
          std::optional<ParameterId> paramId;
          if (key == "time") paramId = kParamDelayTime;
          else if (key == "feedback") paramId = kParamDelayFeedback;
          else if (key == "mix") paramId = kParamDelayMix;

          if (paramId)
          {
            auto *param = GetParam(static_cast<int>(*paramId));
            if (param)
            {
              param->Set(value);
              OnParamChange(static_cast<int>(*paramId));
            }
          }
        }
      }
      else if (node.type == "reverb")
      {
        auto *reverbEnabledParam = GetParam(kParamReverbEnabled);
        if (reverbEnabledParam)
        {
          reverbEnabledParam->Set(node.enabled ? 1.0 : 0.0);
          OnParamChange(kParamReverbEnabled);
        }

        for (const auto &[key, value] : node.params)
        {
          std::optional<ParameterId> paramId;
          if (key == "decay") paramId = kParamReverbDecay;
          else if (key == "damping") paramId = kParamReverbDamping;
          else if (key == "mix") paramId = kParamReverbMix;

          if (paramId)
          {
            auto *param = GetParam(static_cast<int>(*paramId));
            if (param)
            {
              param->Set(value);
              OnParamChange(static_cast<int>(*paramId));
            }
          }
        }
      }
      else if (node.type == "simple_cab")
      {
        auto *simpleCabEnabledParam = GetParam(kParamSimpleCabEnabled);
        if (simpleCabEnabledParam)
        {
          simpleCabEnabledParam->Set(node.enabled ? 1.0 : 0.0);
          OnParamChange(kParamSimpleCabEnabled);
        }

        for (const auto &[key, value] : node.params)
        {
          std::optional<ParameterId> paramId;
          if (key == "bass") paramId = kParamSimpleCabBass;
          else if (key == "presence") paramId = kParamSimpleCabPresence;
          else if (key == "brightness") paramId = kParamSimpleCabBrightness;

          if (paramId)
          {
            auto *param = GetParam(static_cast<int>(*paramId));
            if (param)
            {
              param->Set(value);
              OnParamChange(static_cast<int>(*paramId));
            }
          }
        }
      }
    }
  }

  void NAMGuitarPlugin::HandlePresetLoadRequest(const nlohmann::json &payload)
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

      ApplyPreset(*presetOpt);

      mActivePreset = *presetOpt;
      mActivePresetId = presetOpt->id;
      mActivePresetJson = presetJsonIter->dump();
      mPendingStateBroadcast = true;

      // Save settings so this preset is restored on next startup
      SaveAppSettings();

      {
        nlohmann::json message;
        message["type"] = "presetLoaded";
        message["preset"] = *presetJsonIter;
        
        // Include current model/IR paths so UI can update signal path display
        nlohmann::json parameters;
        parameters["modelPath"] = mActiveModelPath;
        parameters["irPath"] = mActiveIRPath;
        message["parameters"] = std::move(parameters);
        
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

  void NAMGuitarPlugin::HandleStateRequest()
  {
    mPendingStateBroadcast = true;
  }

  void NAMGuitarPlugin::HandleSetParameterRequest(const nlohmann::json &payload)
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

  void NAMGuitarPlugin::HandleSignalTestRequest(const nlohmann::json &payload)
  {
    const double frequency = payload.value("frequency", 440.0);
    const double duration = payload.value("duration", 1.0);

    if (StartSignalPathTest(frequency, duration))
    {
      return;
    }

    ReportErrorToUI("Unable to start signal path test", "Another test is already running or DSP is not ready");
  }

  void NAMGuitarPlugin::HandleLoadModelRequest(const nlohmann::json &payload)
  {
    if (!mDSP)
    {
      ReportErrorToUI("Cannot load model", "DSP not initialized");
      return;
    }

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

    // Lock DSP mutex during model loading
    std::lock_guard<std::mutex> lock(mDSPMutex);

    if (mDSP->LoadModel(modelPath))
    {
      mActiveModelPath = modelPath.generic_string();
      // Clear active preset since state no longer matches
      mActivePreset.reset();
      mActivePresetId.clear();
      mActivePresetJson.clear();
      mPendingStateBroadcast = true;

      // Save settings so this model is restored on next startup
      SaveAppSettings();

      {
        nlohmann::json message;
        message["type"] = "modelLoaded";
        message["path"] = mActiveModelPath;
        SendMessageToUI(message.dump());
      }
    }
    else
    {
      ReportErrorToUI("Failed to load model", "Could not parse model file: " + filePath);
    }
  }

  void NAMGuitarPlugin::HandleLoadIRRequest(const nlohmann::json &payload)
  {
    if (!mDSP)
    {
      ReportErrorToUI("Cannot load IR", "DSP not initialized");
      return;
    }

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

    // Lock DSP mutex during IR loading
    std::lock_guard<std::mutex> lock(mDSPMutex);

    if (mDSP->LoadImpulseResponse(irPath))
    {
      mActiveIRPath = irPath.generic_string();
      // Clear active preset since state no longer matches
      mActivePreset.reset();
      mActivePresetId.clear();
      mActivePresetJson.clear();
      mPendingStateBroadcast = true;

      // Save settings so this IR is restored on next startup
      SaveAppSettings();

      {
        nlohmann::json message;
        message["type"] = "irLoaded";
        message["path"] = mActiveIRPath;
        SendMessageToUI(message.dump());
      }
    }
    else
    {
      ReportErrorToUI("Failed to load IR", "Could not parse IR file: " + filePath);
    }
  }

  void NAMGuitarPlugin::HandleSavePresetRequest(const nlohmann::json &payload)
  {
    std::cerr << "[NAMGuitarPlugin] HandleSavePresetRequest called" << std::endl;
    
    const std::string presetName = payload.value("name", "");
    const std::string presetCategory = payload.value("category", "User");
    const std::string presetDescription = payload.value("description", "");

    std::cerr << "[NAMGuitarPlugin] Saving preset: name=" << presetName 
              << ", category=" << presetCategory << std::endl;

    if (presetName.empty())
    {
      std::cerr << "[NAMGuitarPlugin] Error: Preset name is empty" << std::endl;
      ReportErrorToUI("Cannot save preset", "Preset name is required");
      return;
    }

    // Build the preset using V2 format with signal graph
    Preset newPreset;
    newPreset.id = "user-" + std::to_string(std::time(nullptr));
    newPreset.name = presetName;
    newPreset.category = presetCategory;
    newPreset.description = presetDescription;
    newPreset.version = 2;

    // Capture global settings from current parameters
    if (auto *param = GetParam(kParamInputTrim)) newPreset.global.inputTrim = param->Value();
    if (auto *param = GetParam(kParamOutputTrim)) newPreset.global.outputTrim = param->Value();
    if (auto *param = GetParam(kParamTranspose)) newPreset.global.transpose = static_cast<int>(param->Value());

    // Build the signal graph nodes

    // Input node (always first)
    GraphNode inputNode;
    inputNode.id = "input";
    inputNode.type = kNodeTypeInput;
    inputNode.category = "routing";
    newPreset.graph.nodes.push_back(inputNode);

    // Add NAM amp node if model is loaded
    if (!mActiveModelPath.empty())
    {
      GraphNode ampNode;
      ampNode.id = "amp";
      ampNode.type = "nam_amp";
      ampNode.category = "amp";
      ampNode.enabled = true;

      ampNode.resource = ResourceRef{};
      ampNode.resource->filePath = std::filesystem::path{mActiveModelPath};

      // Capture amp parameters
      if (auto *param = GetParam(kParamDrive)) ampNode.params["drive"] = param->Value();
      if (auto *param = GetParam(kParamTone)) ampNode.params["tone"] = param->Value();
      if (auto *param = GetParam(kParamMix)) ampNode.params["mix"] = param->Value();

      newPreset.graph.nodes.push_back(ampNode);
    }

    // Add noise gate node
    if (auto *param = GetParam(kParamGateEnabled))
    {
      GraphNode gateNode;
      gateNode.id = "gate";
      gateNode.type = "noise_gate";
      gateNode.category = "dynamics";
      gateNode.enabled = param->Value() > 0.5;

      if (auto *threshParam = GetParam(kParamGateThreshold))
        gateNode.params["threshold"] = threshParam->Value();

      newPreset.graph.nodes.push_back(gateNode);
    }

    // Add IR cab node if IR is loaded
    if (!mActiveIRPath.empty())
    {
      GraphNode cabNode;
      cabNode.id = "cab";
      cabNode.type = "ir_cab";
      cabNode.category = "cab";
      cabNode.enabled = true;

      cabNode.resource = ResourceRef{};
      cabNode.resource->filePath = std::filesystem::path{mActiveIRPath};

      newPreset.graph.nodes.push_back(cabNode);
    }

    // Add simple cab node
    if (auto *param = GetParam(kParamSimpleCabEnabled))
    {
      GraphNode simpleCabNode;
      simpleCabNode.id = "simple_cab";
      simpleCabNode.type = "simple_cab";
      simpleCabNode.category = "cab";
      simpleCabNode.enabled = param->Value() > 0.5;

      if (auto *p = GetParam(kParamSimpleCabBass)) simpleCabNode.params["bass"] = p->Value();
      if (auto *p = GetParam(kParamSimpleCabPresence)) simpleCabNode.params["presence"] = p->Value();
      if (auto *p = GetParam(kParamSimpleCabBrightness)) simpleCabNode.params["brightness"] = p->Value();

      newPreset.graph.nodes.push_back(simpleCabNode);
    }

    // Add EQ node
    if (auto *param = GetParam(kParamEQEnabled))
    {
      GraphNode eqNode;
      eqNode.id = "eq";
      eqNode.type = "eq_parametric";
      eqNode.category = "eq";
      eqNode.enabled = param->Value() > 0.5;

      if (auto *p = GetParam(kParamEQLowGain)) eqNode.params["lowGain"] = p->Value();
      if (auto *p = GetParam(kParamEQLowFreq)) eqNode.params["lowFreq"] = p->Value();
      if (auto *p = GetParam(kParamEQLowMidGain)) eqNode.params["lowMidGain"] = p->Value();
      if (auto *p = GetParam(kParamEQLowMidFreq)) eqNode.params["lowMidFreq"] = p->Value();
      if (auto *p = GetParam(kParamEQLowMidQ)) eqNode.params["lowMidQ"] = p->Value();
      if (auto *p = GetParam(kParamEQHighMidGain)) eqNode.params["highMidGain"] = p->Value();
      if (auto *p = GetParam(kParamEQHighMidFreq)) eqNode.params["highMidFreq"] = p->Value();
      if (auto *p = GetParam(kParamEQHighMidQ)) eqNode.params["highMidQ"] = p->Value();
      if (auto *p = GetParam(kParamEQHighGain)) eqNode.params["highGain"] = p->Value();
      if (auto *p = GetParam(kParamEQHighFreq)) eqNode.params["highFreq"] = p->Value();

      newPreset.graph.nodes.push_back(eqNode);
    }

    // Add delay node
    if (auto *param = GetParam(kParamDelayEnabled))
    {
      GraphNode delayNode;
      delayNode.id = "delay";
      delayNode.type = "delay";
      delayNode.category = "fx";
      delayNode.enabled = param->Value() > 0.5;

      if (auto *p = GetParam(kParamDelayTime)) delayNode.params["time"] = p->Value();
      if (auto *p = GetParam(kParamDelayFeedback)) delayNode.params["feedback"] = p->Value();
      if (auto *p = GetParam(kParamDelayMix)) delayNode.params["mix"] = p->Value();

      newPreset.graph.nodes.push_back(delayNode);
    }

    // Add reverb node
    if (auto *param = GetParam(kParamReverbEnabled))
    {
      GraphNode reverbNode;
      reverbNode.id = "reverb";
      reverbNode.type = "reverb";
      reverbNode.category = "fx";
      reverbNode.enabled = param->Value() > 0.5;

      if (auto *p = GetParam(kParamReverbDecay)) reverbNode.params["decay"] = p->Value();
      if (auto *p = GetParam(kParamReverbDamping)) reverbNode.params["damping"] = p->Value();
      if (auto *p = GetParam(kParamReverbMix)) reverbNode.params["mix"] = p->Value();

      newPreset.graph.nodes.push_back(reverbNode);
    }

    // Output node (always last)
    GraphNode outputNode;
    outputNode.id = "output";
    outputNode.type = kNodeTypeOutput;
    outputNode.category = "routing";
    newPreset.graph.nodes.push_back(outputNode);

    // Build edges (linear chain for now)
    for (size_t i = 0; i < newPreset.graph.nodes.size() - 1; ++i)
    {
      GraphEdge edge;
      edge.from = newPreset.graph.nodes[i].id;
      edge.to = newPreset.graph.nodes[i + 1].id;
      newPreset.graph.edges.push_back(edge);
    }

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

  void NAMGuitarPlugin::HandleBrowseModelRequest()
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

  void NAMGuitarPlugin::HandleBrowseIRRequest()
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

  void NAMGuitarPlugin::HandleTunerRequest(const nlohmann::json &payload)
  {
    if (!mDSP)
    {
      return;
    }

    const std::string action = payload.value("action", "");
    
    if (action == "start")
    {
      // Set up the tuner callback
      mDSP->SetTunerCallback([this](const NAMDSPManager::TunerResult& result) {
        std::lock_guard<std::mutex> lock(mTunerMutex);
        mPendingTunerData.noteName = result.noteName;
        mPendingTunerData.octave = result.octave;
        mPendingTunerData.frequency = result.frequency;
        mPendingTunerData.centOffset = result.centOffset;
        mPendingTunerData.confidence = result.confidence;
        mPendingTunerData.detected = result.detected;
        mPendingTunerData.debugRms = result.debugRms;
        mPendingTunerData.debugRawFreq = result.debugRawFreq;
        mTunerDataPending.store(true, std::memory_order_release);
      });
      
      // Set reference frequency if provided
      if (payload.contains("referenceFrequency"))
      {
        const double refFreq = payload.value("referenceFrequency", 440.0);
        mDSP->SetTunerReferenceFrequency(refFreq);
      }
      
      mTunerActive.store(true, std::memory_order_release);
      mDSP->SetTunerEnabled(true);
      
      // Acknowledge tuner started
      {
        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = mDSP->GetTunerReferenceFrequency();
        SendMessageToUI(message.dump());
      }
    }
    else if (action == "stop")
    {
      mDSP->SetTunerEnabled(false);
      mDSP->SetTunerCallback(nullptr);
      mTunerActive.store(false, std::memory_order_release);
      
      // Acknowledge tuner stopped
      {
        nlohmann::json message;
        message["type"] = "tunerStopped";
        SendMessageToUI(message.dump());
      }
    }
    else if (action == "setReference")
    {
      const double refFreq = payload.value("referenceFrequency", 440.0);
      mDSP->SetTunerReferenceFrequency(refFreq);
      
      {
        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = mDSP->GetTunerReferenceFrequency();
        SendMessageToUI(message.dump());
      }
    }
    else if (action == "setLiveMode")
    {
      const bool liveMode = payload.value("liveMode", true);
      mDSP->SetLiveTunerMode(liveMode);
      
      {
        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = mDSP->IsLiveTunerMode();
        SendMessageToUI(message.dump());
      }
    }
  }

  void NAMGuitarPlugin::HandleSetInputModeRequest(const nlohmann::json &payload)
  {
    if (!mDSP)
    {
      return;
    }

    // Set mono/stereo mode
    if (payload.contains("monoMode"))
    {
      const bool mono = payload.value("monoMode", true);
      mDSP->SetMonoMode(mono);
    }

    // Set input channel (0 = input 1, 1 = input 2)
    if (payload.contains("inputChannel"))
    {
      const int channel = payload.value("inputChannel", 1);
      mDSP->SetInputChannel(channel);
    }

    // Acknowledge the change
    {
      nlohmann::json message;
      message["type"] = "inputModeChanged";
      message["monoMode"] = mDSP->IsMonoMode();
      message["inputChannel"] = mDSP->GetInputChannel();
      SendMessageToUI(message.dump());
    }
  }

  void NAMGuitarPlugin::HandleSetAmpCabStateRequest(const nlohmann::json &payload)
  {
    if (!mDSP)
    {
      return;
    }

    // Set amp (NAM model) enabled state
    if (payload.contains("ampEnabled"))
    {
      const bool enabled = payload.value("ampEnabled", true);
      mDSP->SetAmpEnabled(enabled);
    }

    // Set cab (IR) enabled state
    if (payload.contains("cabEnabled"))
    {
      const bool enabled = payload.value("cabEnabled", true);
      mDSP->SetCabEnabled(enabled);
    }

    // Acknowledge the change
    {
      nlohmann::json message;
      message["type"] = "ampCabStateChanged";
      message["ampEnabled"] = mDSP->IsAmpEnabled();
      message["cabEnabled"] = mDSP->IsCabEnabled();
      SendMessageToUI(message.dump());
    }
  }

  void NAMGuitarPlugin::HandleUpdateNodeParamRequest(const nlohmann::json &payload)
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
        ApplyNodeParameter(*node, paramKey, value);
        
        // Don't broadcast state on every param change during drag - too noisy
      }
    }
  }

  void NAMGuitarPlugin::ApplyNodeParameter(const GraphNode& node, const std::string& paramKey, double value)
  {
    // Map node type and param key to plugin parameter
    if (node.type == "amp_nam" || node.type == "nam_amp" || node.type == "nam")
    {
      if (paramKey == "drive")
      {
        auto *param = GetParam(kParamDrive);
        if (param) { param->Set(value); OnParamChange(kParamDrive); }
      }
      else if (paramKey == "tone")
      {
        auto *param = GetParam(kParamTone);
        if (param) { param->Set(value); OnParamChange(kParamTone); }
      }
      else if (paramKey == "inputGain")
      {
        auto *param = GetParam(kParamInputTrim);
        if (param) { param->Set(value); OnParamChange(kParamInputTrim); }
      }
      else if (paramKey == "outputGain")
      {
        auto *param = GetParam(kParamOutputTrim);
        if (param) { param->Set(value); OnParamChange(kParamOutputTrim); }
      }
    }
    else if (node.type == "dynamics_gate" || node.type == "noise_gate" || node.type == "gate")
    {
      if (paramKey == "threshold")
      {
        auto *param = GetParam(kParamGateThreshold);
        if (param) { param->Set(value); OnParamChange(kParamGateThreshold); }
      }
    }
    else if (node.type == "eq_parametric" || node.type == "eq")
    {
      std::optional<ParameterId> paramId;
      if (paramKey == "lowGain") paramId = kParamEQLowGain;
      else if (paramKey == "lowFreq") paramId = kParamEQLowFreq;
      else if (paramKey == "lowMidGain") paramId = kParamEQLowMidGain;
      else if (paramKey == "lowMidFreq") paramId = kParamEQLowMidFreq;
      else if (paramKey == "lowMidQ") paramId = kParamEQLowMidQ;
      else if (paramKey == "highMidGain") paramId = kParamEQHighMidGain;
      else if (paramKey == "highMidFreq") paramId = kParamEQHighMidFreq;
      else if (paramKey == "highMidQ") paramId = kParamEQHighMidQ;
      else if (paramKey == "highGain") paramId = kParamEQHighGain;
      else if (paramKey == "highFreq") paramId = kParamEQHighFreq;

      if (paramId)
      {
        auto *param = GetParam(static_cast<int>(*paramId));
        if (param) { param->Set(value); OnParamChange(static_cast<int>(*paramId)); }
      }
    }
    else if (node.type == "delay_digital" || node.type == "delay")
    {
      if (paramKey == "time")
      {
        auto *param = GetParam(kParamDelayTime);
        if (param) { param->Set(value); OnParamChange(kParamDelayTime); }
      }
      else if (paramKey == "feedback")
      {
        auto *param = GetParam(kParamDelayFeedback);
        if (param) { param->Set(value); OnParamChange(kParamDelayFeedback); }
      }
      else if (paramKey == "mix")
      {
        auto *param = GetParam(kParamDelayMix);
        if (param) { param->Set(value); OnParamChange(kParamDelayMix); }
      }
    }
    else if (node.type == "reverb_room" || node.type == "reverb")
    {
      if (paramKey == "decay")
      {
        auto *param = GetParam(kParamReverbDecay);
        if (param) { param->Set(value); OnParamChange(kParamReverbDecay); }
      }
      else if (paramKey == "damping")
      {
        auto *param = GetParam(kParamReverbDamping);
        if (param) { param->Set(value); OnParamChange(kParamReverbDamping); }
      }
      else if (paramKey == "mix")
      {
        auto *param = GetParam(kParamReverbMix);
        if (param) { param->Set(value); OnParamChange(kParamReverbMix); }
      }
    }
    else if (node.type == "cab_simple" || node.type == "simple_cab")
    {
      if (paramKey == "bass")
      {
        auto *param = GetParam(kParamSimpleCabBass);
        if (param) { param->Set(value); OnParamChange(kParamSimpleCabBass); }
      }
      else if (paramKey == "presence")
      {
        auto *param = GetParam(kParamSimpleCabPresence);
        if (param) { param->Set(value); OnParamChange(kParamSimpleCabPresence); }
      }
      else if (paramKey == "brightness")
      {
        auto *param = GetParam(kParamSimpleCabBrightness);
        if (param) { param->Set(value); OnParamChange(kParamSimpleCabBrightness); }
      }
    }
  }

  void NAMGuitarPlugin::HandleUpdateNodeBypassRequest(const nlohmann::json &payload)
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

  void NAMGuitarPlugin::HandleUpdateNodeResourceRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    const std::string filePath = payload.value("filePath", "");

    if (nodeId.empty())
    {
      return;
    }

    // Update the resource in the active preset's graph
    if (mActivePreset)
    {
      GraphNode* node = mActivePreset->graph.FindNode(nodeId);
      if (node)
      {
        if (!resourceId.empty())
        {
          // Library resource
          ResourceRef ref;
          ref.resourceType = resourceType;
          ref.resourceId = resourceId;
          node->resource = ref;
        }
        else if (!filePath.empty())
        {
          // Custom file path
          ResourceRef ref;
          ref.resourceType = resourceType;
          ref.filePath = filePath;
          node->resource = ref;
        }
        else
        {
          // Clear resource
          node->resource.reset();
        }
        
        // Re-serialize the preset JSON to reflect the change
        mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
        
        // Apply the updated preset to DSP
        ApplyPreset(*mActivePreset);
        
        // Broadcast state change to UI
        mPendingStateBroadcast = true;
      }
    }
  }

  void NAMGuitarPlugin::HandleBrowseNodeResourceRequest(const nlohmann::json &payload)
  {
    const std::string nodeId = payload.value("nodeId", "");
    const std::string resourceType = payload.value("resourceType", "");

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
      HandleUpdateNodeResourceRequest(updatePayload);
    }
#else
    ReportErrorToUI("Browse not supported", "File browser is only available on Windows");
#endif
  }

  void NAMGuitarPlugin::HandlePreviewDemoRequest(const nlohmann::json &payload)
  {
    if (!mDSP)
    {
      return;
    }

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

    auto previewBuffer = std::make_shared<PreviewPlaybackBuffer>();
    previewBuffer->id = audioIter->value("id", "");
    previewBuffer->title = audioIter->value("title", previewBuffer->id);
    previewBuffer->sampleRate = targetSampleRate;
    previewBuffer->channels = static_cast<int>(resampled.size());
    previewBuffer->channelSamples = std::move(resampled);

    mDSP->Reset();

    mPreviewCursor.store(0, std::memory_order_release);
    mPreviewBuffer.store(previewBuffer, std::memory_order_release);
    mPreviewStartedBuffer.store(previewBuffer, std::memory_order_release);
    mPreviewCompletedBuffer.store(nullptr, std::memory_order_release);
  }

  void NAMGuitarPlugin::ReportErrorToUI(std::string_view message, std::string_view detail)
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

  std::optional<std::filesystem::path> NAMGuitarPlugin::ResolveResourceRef(const ResourceRef &ref) const
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

  std::vector<std::uint8_t> NAMGuitarPlugin::DecodeBase64(const std::string &encoded)
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

  bool NAMGuitarPlugin::WriteFile(const std::filesystem::path &target, const std::vector<std::uint8_t> &data) const
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

  bool NAMGuitarPlugin::StartSignalPathTest(double frequencyHz, double durationSeconds)
  {
    if (!mDSP)
    {
      return false;
    }

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
    mDSP->Reset();

    mSignalTestResultPending.store(false, std::memory_order_release);
    mSignalTestActive.store(true, std::memory_order_release);
    return true;
  }

  void NAMGuitarPlugin::SaveAppSettings() const
  {
    try
    {
      const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
      (void)mFileSystem.EnsureDirectory(settingsDir);
      const auto settingsFile = mFileSystem.ResolveSettingsFile();

      nlohmann::json settings;
      
      // Save last preset info
      settings["lastPresetId"] = mActivePresetId;
      settings["lastPresetJson"] = mActivePresetJson;
      settings["lastModelPath"] = mActiveModelPath;
      settings["lastIRPath"] = mActiveIRPath;

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

  void NAMGuitarPlugin::LoadAppSettings()
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

      // Restore paths
      mActivePresetId = settings.value("lastPresetId", "");
      mActivePresetJson = settings.value("lastPresetJson", "");
      mActiveModelPath = settings.value("lastModelPath", "");
      mActiveIRPath = settings.value("lastIRPath", "");

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

  void NAMGuitarPlugin::LoadResourceLibraries()
  {
    if (mResourceRoot.empty())
    {
      std::cerr << "[Plugin] Cannot load resource libraries: resource root not set" << std::endl;
      return;
    }

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
                mResourceLibrary.AddResource(resource);
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
                mResourceLibrary.AddResource(resource);
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
  }

  void NAMGuitarPlugin::LoadLastSessionState()
  {
    if (!mDSP)
    {
      return;
    }

    LoadAppSettings();

    // Lock DSP mutex during state restoration
    std::lock_guard<std::mutex> lock(mDSPMutex);

    // Apply loaded parameters to DSP
    for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
    {
      OnParamChange(paramIdx);
    }

    // Load model if path is set
    if (!mActiveModelPath.empty())
    {
      const std::filesystem::path modelPath{mActiveModelPath};
      if (std::filesystem::exists(modelPath))
      {
        if (!mDSP->LoadModel(modelPath))
        {
          std::cerr << "[Plugin] Failed to load last model: " << mActiveModelPath << std::endl;
          mActiveModelPath.clear();
        }
      }
      else
      {
        mActiveModelPath.clear();
      }
    }

    // Load IR if path is set
    if (!mActiveIRPath.empty())
    {
      const std::filesystem::path irPath{mActiveIRPath};
      if (std::filesystem::exists(irPath))
      {
        if (!mDSP->LoadImpulseResponse(irPath))
        {
          std::cerr << "[Plugin] Failed to load last IR: " << mActiveIRPath << std::endl;
          mActiveIRPath.clear();
        }
      }
      else
      {
        mActiveIRPath.clear();
      }
    }

    // Restore preset from JSON if available
    if (!mActivePresetJson.empty() && nlohmann::json::accept(mActivePresetJson))
    {
      try
      {
        auto presetOpt = PresetStorage::DeserializeFromJson(mActivePresetJson);
        if (presetOpt)
        {
          mActivePreset = *presetOpt;
        }
      }
      catch (...)
      {
        mActivePresetJson.clear();
      }
    }

    mPendingStateBroadcast = true;
    std::cout << "[Plugin] Last session state restored" << std::endl;
  }

} // namespace namguitar
