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
#include "IControls.h"
#include "IPlug_include_in_plug_src.h"
#include "IGraphics_include_in_plug_src.h"
#include "IPlugPaths.h"
#include "wdlstring.h"

#include "dsp/NAMDSPManager.h"
#include "ui/WebUIBridge.h"

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
      return std::nullopt;
    }

    nlohmann::json SerializePresetToJson(const Preset &preset)
    {
      nlohmann::json jsonPreset;
      jsonPreset["id"] = preset.id;
      jsonPreset["name"] = preset.name;
      jsonPreset["category"] = preset.category;
      jsonPreset["description"] = preset.description;
      jsonPreset["audioFxModelId"] = preset.audioFxModelId;
      jsonPreset["irId"] = preset.irId;
      jsonPreset["fxChain"] = preset.fxChain;

      nlohmann::json attachments = nlohmann::json::array();
      for (const auto &attachment : preset.attachments)
      {
        nlohmann::json attachmentJson;
        attachmentJson["type"] = attachment.type;
        attachmentJson["filePath"] = attachment.filePath.generic_string();
        attachmentJson["hash"] = attachment.hash;
        attachments.push_back(std::move(attachmentJson));
      }
      jsonPreset["attachments"] = std::move(attachments);

      nlohmann::json parameters = nlohmann::json::array();
      for (const auto &parameter : preset.parameters)
      {
        parameters.push_back({
            {"id", parameter.id},
            {"value", parameter.value},
        });
      }
      jsonPreset["parameters"] = std::move(parameters);

      return jsonPreset;
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
      : iplug::Plugin(info, iplug::MakeConfig(kParamCount, kNumPrograms)), mDSP(std::make_unique<NAMDSPManager>()), mWebUI(std::make_unique<WebUIBridge>())
  {
    // Write to a log file to verify execution
    FILE* logFile = fopen("c:\\temp\\plugin_log.txt", "a");
    if (logFile) {
      fprintf(logFile, "[Plugin] Constructor called at %llu\n", (unsigned long long)time(NULL));
      fclose(logFile);
    }

    std::cout << "[Plugin] Constructor called" << std::endl;

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

    InitializeParameters();
    HandleWebViewMessages();

    for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
    {
      OnParamChange(paramIdx);
    }

#if PLUG_HAS_UI
    mMakeGraphicsFunc = [this]()
    {
      return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
    };

    mLayoutFunc = [this](iplug::igraphics::IGraphics *graphics)
    {
      if (!graphics)
      {
        return;
      }
      graphics->AttachPanelBackground(iplug::igraphics::COLOR_BLACK);
      graphics->SetLayoutOnResize(true);
      InitializeGraphics(*graphics);
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
        const double totalFrames = std::max(1.0, static_cast<double>(state.totalSamples));
        mSignalTestResult.frequencyHz = state.frequencyHz;
        mSignalTestResult.sampleRate = state.sampleRate;
        mSignalTestResult.durationSeconds = static_cast<double>(state.totalSamples) / std::max(1.0, state.sampleRate);
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

    mDSP->Prepare(GetSampleRate(), GetBlockSize());

    mPreviewBuffer.store(nullptr, std::memory_order_release);
    mPreviewStartedBuffer.store(nullptr, std::memory_order_release);
    mPreviewCompletedBuffer.store(nullptr, std::memory_order_release);
    mPreviewCursor.store(0, std::memory_order_release);
  }

  void NAMGuitarPlugin::OnIdle()
  {
    std::cout << "[Plugin] OnIdle called" << std::endl;

    if (mWebUI)
    {
      mWebUI->PumpMessages();
    }

    if (mWebUI)
    {
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
        mWebUI->EnqueueMessage(message.dump());
      }

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
        mWebUI->EnqueueMessage(message.dump());
      }
    }

    if (mSignalTestResultPending.exchange(false, std::memory_order_acq_rel))
    {
      if (mWebUI)
      {
        nlohmann::json message;
        message["type"] = "signalPathTestResult";
        message["frequency"] = mSignalTestResult.frequencyHz;
        message["duration"] = mSignalTestResult.durationSeconds;
        message["sampleRate"] = mSignalTestResult.sampleRate;
        message["inputRMS"] = mSignalTestResult.inputRMS;
        message["outputRMS"] = {mSignalTestResult.outputRMS[0], mSignalTestResult.outputRMS[1]};
        message["passed"] = mSignalTestResult.passed;
        if (!mSignalTestResult.passed)
        {
          message["message"] = "Signal path test did not produce any output";
        }
        mWebUI->EnqueueMessage(message.dump());
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
        const auto jsonPreset = nlohmann::json::parse(mActivePresetJson);
        if (!jsonPreset.is_object())
        {
          ReportErrorToUI("Failed to restore preset state", "Preset JSON is not an object");
        }
        else
        {
          Preset preset = ParsePresetFromJson(jsonPreset);
          ApplyPreset(preset);
          mActivePreset = preset;
          if (mActivePresetId.empty())
          {
            mActivePresetId = preset.id;
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
  }

  void NAMGuitarPlugin::HandleWebViewMessages()
  {
    if (!mWebUI)
    {
      return;
    }

    mWebUI->RegisterMessageHandler([this](const std::string &message)
                                   { HandleUIMessage(message); });

    mWebUI->RegisterLogHandler([this](const std::string &logMessage)
                               { 
                                 // Log to console for debugging
                                 printf("[WebUI] %s\n", logMessage.c_str());
                               });
  }

  void NAMGuitarPlugin::InitializeGraphics(iplug::igraphics::IGraphics &graphics)
  {
    if (!mWebUI)
    {
      return;
    }

    WDL_String bundlePath;
    iplug::BundleResourcePath(bundlePath, ::gHINSTANCE);
    if (bundlePath.GetLength() == 0)
    {
      iplug::HostPath(bundlePath, nullptr);
      bundlePath.Append("resources\\");
    }
    const std::filesystem::path resourceRoot = std::filesystem::path{bundlePath.Get()};
    mResourceRoot = resourceRoot;
    mWebUI->Initialize(graphics, resourceRoot);
    mPendingStateBroadcast = true;
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
  }

  void NAMGuitarPlugin::BroadcastState()
  {
    if (!mWebUI)
    {
      return;
    }

    nlohmann::json message;
    message["type"] = "state";
    message["activePresetId"] = mActivePresetId;

    auto parameters = SerializeParametersToJson(*this);
    parameters["modelPath"] = mActiveModelPath;
    parameters["irPath"] = mActiveIRPath;
    message["parameters"] = std::move(parameters);

    if (mActivePreset)
    {
      message["preset"] = SerializePresetToJson(*mActivePreset);
    }

    mWebUI->EnqueueMessage(message.dump());
    mPendingStateBroadcast = false;
  }

  void NAMGuitarPlugin::ApplyPreset(Preset &preset)
  {
    if (!mDSP)
    {
      return;
    }

    for (const auto &parameter : preset.parameters)
    {
      const auto paramId = ParamIdFromKey(parameter.id);
      if (paramId)
      {
        const auto index = static_cast<int>(*paramId);
        auto *param = GetParam(index);
        if (param)
        {
          param->Set(parameter.value);
          OnParamChange(index);
        }
      }
    }

    for (auto &attachment : preset.attachments)
    {
      const auto resolvedPath = MaterializeAttachment(attachment);
      if (!resolvedPath)
      {
        continue;
      }

      attachment.filePath = *resolvedPath;
      attachment.data.clear();

      if (attachment.type == "nam" || attachment.type == "audiofx")
      {
        if (mDSP->LoadModel(*resolvedPath))
        {
          mActiveModelPath = resolvedPath->generic_string();
        }
      }
      else if (attachment.type == "ir")
      {
        if (mDSP->LoadImpulseResponse(*resolvedPath))
        {
          mActiveIRPath = resolvedPath->generic_string();
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
      Preset preset = ParsePresetFromJson(*presetJsonIter);
      ApplyPreset(preset);

      mActivePreset = preset;
      mActivePresetId = preset.id;
      mActivePresetJson = presetJsonIter->dump();
      mPendingStateBroadcast = true;

      if (mWebUI)
      {
        nlohmann::json message;
        message["type"] = "presetLoaded";
        message["preset"] = SerializePresetToJson(preset);
        mWebUI->EnqueueMessage(message.dump());
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

    // Map string parameter IDs to enum values
    int paramIdx = -1;
    if (paramId == "input_trim")
    {
      paramIdx = kParamInputTrim;
    }
    else if (paramId == "output_trim")
    {
      paramIdx = kParamOutputTrim;
    }
    else if (paramId == "drive")
    {
      paramIdx = kParamDrive;
    }
    else if (paramId == "tone")
    {
      paramIdx = kParamTone;
    }
    else if (paramId == "gate_enabled")
    {
      paramIdx = kParamGateEnabled;
    }
    else if (paramId == "gate_threshold")
    {
      paramIdx = kParamGateThreshold;
    }
    else if (paramId == "mix")
    {
      paramIdx = kParamMix;
    }
    else if (paramId == "doubler_enabled")
    {
      paramIdx = kParamDoublerEnabled;
    }
    else if (paramId == "doubler_delay")
    {
      paramIdx = kParamDoublerDelay;
    }

    if (paramIdx >= 0 && paramIdx < kParamCount)
    {
      GetParam(paramIdx)->Set(value);
      OnParamChange(paramIdx);
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

    if (mDSP->LoadModel(modelPath))
    {
      mActiveModelPath = modelPath.generic_string();
      // Clear active preset since state no longer matches
      mActivePreset.reset();
      mActivePresetId.clear();
      mActivePresetJson.clear();
      mPendingStateBroadcast = true;

      if (mWebUI)
      {
        nlohmann::json message;
        message["type"] = "modelLoaded";
        message["path"] = mActiveModelPath;
        mWebUI->EnqueueMessage(message.dump());
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

    if (mDSP->LoadImpulseResponse(irPath))
    {
      mActiveIRPath = irPath.generic_string();
      // Clear active preset since state no longer matches
      mActivePreset.reset();
      mActivePresetId.clear();
      mActivePresetJson.clear();
      mPendingStateBroadcast = true;

      if (mWebUI)
      {
        nlohmann::json message;
        message["type"] = "irLoaded";
        message["path"] = mActiveIRPath;
        mWebUI->EnqueueMessage(message.dump());
      }
    }
    else
    {
      ReportErrorToUI("Failed to load IR", "Could not parse IR file: " + filePath);
    }
  }

  void NAMGuitarPlugin::HandleSavePresetRequest(const nlohmann::json &payload)
  {
    const std::string presetName = payload.value("name", "");
    const std::string presetCategory = payload.value("category", "User");
    const std::string presetDescription = payload.value("description", "");

    if (presetName.empty())
    {
      ReportErrorToUI("Cannot save preset", "Preset name is required");
      return;
    }

    // Build the preset from current state
    Preset newPreset;
    newPreset.id = "user-" + std::to_string(std::time(nullptr));
    newPreset.name = presetName;
    newPreset.category = presetCategory;
    newPreset.description = presetDescription;

    // Capture current parameters
    for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
    {
      const auto *param = GetParam(paramIdx);
      if (param)
      {
        std::string key;
        switch (static_cast<ParameterId>(paramIdx))
        {
        case kParamInputTrim:
          key = "input_trim";
          break;
        case kParamOutputTrim:
          key = "output_trim";
          break;
        case kParamDrive:
          key = "drive";
          break;
        case kParamTone:
          key = "tone";
          break;
        case kParamGateEnabled:
          key = "gate_enabled";
          break;
        case kParamGateThreshold:
          key = "gate_threshold";
          break;
        case kParamMix:
          key = "mix";
          break;
        case kParamDoublerEnabled:
          key = "doubler_enabled";
          break;
        case kParamDoublerDelay:
          key = "doubler_delay";
          break;
        default:
          continue;
        }
        newPreset.parameters.push_back({key, param->Value()});
      }
    }

    // Add current model as attachment if loaded
    if (!mActiveModelPath.empty())
    {
      PresetAttachment modelAttachment;
      modelAttachment.type = "nam";
      modelAttachment.filePath = std::filesystem::path{mActiveModelPath};
      newPreset.attachments.push_back(modelAttachment);
    }

    // Add current IR as attachment if loaded
    if (!mActiveIRPath.empty())
    {
      PresetAttachment irAttachment;
      irAttachment.type = "ir";
      irAttachment.filePath = std::filesystem::path{mActiveIRPath};
      newPreset.attachments.push_back(irAttachment);
    }

    // Save to user presets directory
    const auto presetDir = mFileSystem.EnsureDirectory(mFileSystem.ResolvePresetDirectory());
    if (!presetDir)
    {
      ReportErrorToUI("Cannot save preset", "Could not create preset directory");
      return;
    }

    const std::filesystem::path presetFilePath = *presetDir / (newPreset.id + ".json");

    // Serialize preset to JSON
    nlohmann::json presetJson;
    presetJson["id"] = newPreset.id;
    presetJson["name"] = newPreset.name;
    presetJson["category"] = newPreset.category;
    presetJson["description"] = newPreset.description;

    nlohmann::json parametersArray = nlohmann::json::array();
    for (const auto &param : newPreset.parameters)
    {
      parametersArray.push_back({{"id", param.id}, {"value", param.value}});
    }
    presetJson["parameters"] = parametersArray;

    nlohmann::json attachmentsArray = nlohmann::json::array();
    for (const auto &attachment : newPreset.attachments)
    {
      nlohmann::json attachmentJson;
      attachmentJson["type"] = attachment.type;
      attachmentJson["filePath"] = attachment.filePath.generic_string();
      attachmentsArray.push_back(attachmentJson);
    }
    presetJson["attachments"] = attachmentsArray;

    // Write to file
    std::ofstream outputFile(presetFilePath);
    if (!outputFile)
    {
      ReportErrorToUI("Cannot save preset", "Could not create preset file");
      return;
    }

    outputFile << presetJson.dump(2);
    outputFile.close();

    // Update active preset
    mActivePreset = newPreset;
    mActivePresetId = newPreset.id;
    mActivePresetJson = presetJson.dump();
    mPendingStateBroadcast = true;

    if (mWebUI)
    {
      nlohmann::json message;
      message["type"] = "presetSaved";
      message["preset"] = presetJson;
      message["path"] = presetFilePath.generic_string();
      mWebUI->EnqueueMessage(message.dump());
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

  void NAMGuitarPlugin::ReportErrorToUI(std::string_view message, std::string_view detail) const
  {
    if (!mWebUI)
    {
      return;
    }

    nlohmann::json payload;
    payload["type"] = "error";
    payload["message"] = std::string{message};
    if (!detail.empty())
    {
      payload["detail"] = std::string{detail};
    }

    mWebUI->EnqueueMessage(payload.dump());
  }

  std::optional<std::filesystem::path> NAMGuitarPlugin::MaterializeAttachment(const PresetAttachment &attachment) const
  {
    std::filesystem::path target = ResolveAttachmentTarget(attachment);
    if (target.empty())
    {
      return std::nullopt;
    }

    if (!attachment.data.empty())
    {
      const auto data = DecodeBase64(attachment.data);
      if (data.empty())
      {
        return std::nullopt;
      }

      if (!WriteFile(target, data))
      {
        return std::nullopt;
      }
    }

    if (!std::filesystem::exists(target))
    {
      return std::nullopt;
    }

    // skip file hash check just now
    /*if (!attachment.hash.empty())
    {
      const auto hash = mHasher.HashFile(target);
      if (!hash.empty() && hash != attachment.hash)
      {
        return std::nullopt;
      }
    }*/

    return target;
  }

  std::filesystem::path NAMGuitarPlugin::ResolveAttachmentTarget(const PresetAttachment &attachment) const
  {
    if (!attachment.filePath.empty())
    {
      if (attachment.filePath.is_absolute())
      {
        return attachment.filePath;
      }

      // First check if the file exists in the bundled resources folder
      if (!mResourceRoot.empty())
      {
        const auto resourcePath = mResourceRoot / attachment.filePath;
        if (std::filesystem::exists(resourcePath))
        {
          return resourcePath;
        }
      }

      // Then check the user preset directory
      if (const auto presetDir = mFileSystem.EnsureDirectory(mFileSystem.ResolvePresetDirectory()))
      {
        const auto presetPath = *presetDir / attachment.filePath;
        if (std::filesystem::exists(presetPath))
        {
          return presetPath;
        }
        // Return preset directory path even if file doesn't exist (for writing)
        return presetPath;
      }

      return attachment.filePath;
    }

    if (!attachment.hash.empty())
    {
      if (const auto presetDir = mFileSystem.EnsureDirectory(mFileSystem.ResolvePresetDirectory()))
      {
        return *presetDir / (attachment.hash + (attachment.type.empty() ? std::string{} : std::string{"."} + attachment.type));
      }
    }

    return {};
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

  Preset NAMGuitarPlugin::ParsePresetFromJson(const nlohmann::json &jsonPreset)
  {
    Preset preset;
    preset.id = jsonPreset.value("id", "");
    preset.name = jsonPreset.value("name", "");
    preset.category = jsonPreset.value("category", "");
    preset.description = jsonPreset.value("description", "");
    preset.audioFxModelId = jsonPreset.value("audioFxModelId", "");
    preset.irId = jsonPreset.value("irId", "");

    if (jsonPreset.contains("fxChain") && jsonPreset["fxChain"].is_array())
    {
      for (const auto &fx : jsonPreset["fxChain"])
      {
        preset.fxChain.push_back(fx.get<std::string>());
      }
    }

    if (jsonPreset.contains("attachments") && jsonPreset["attachments"].is_array())
    {
      for (const auto &jsonAttachment : jsonPreset["attachments"])
      {
        PresetAttachment attachment;
        attachment.type = jsonAttachment.value("type", "");
        attachment.hash = jsonAttachment.value("hash", "");
        if (jsonAttachment.contains("filePath"))
        {
          attachment.filePath = jsonAttachment.value("filePath", "");
        }
        else if (jsonAttachment.contains("path"))
        {
          attachment.filePath = jsonAttachment.value("path", "");
        }
        attachment.data = jsonAttachment.value("data", "");
        preset.attachments.push_back(std::move(attachment));
      }
    }

    if (jsonPreset.contains("parameters") && jsonPreset["parameters"].is_array())
    {
      for (const auto &jsonParameter : jsonPreset["parameters"])
      {
        PresetParameter entry;
        entry.id = jsonParameter.value("id", "");
        entry.value = jsonParameter.value("value", 0.0);
        preset.parameters.push_back(entry);
      }
    }

    return preset;
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

} // namespace namguitar
