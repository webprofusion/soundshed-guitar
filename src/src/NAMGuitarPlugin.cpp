#include "NAMGuitarPlugin.h"

#include <filesystem>
#include <algorithm>
#include <optional>
#include <nlohmann/json.hpp>

#include "config.h"
#include "IControls.h"
#include "IPlug_include_in_plug_src.h"
#include "IPlugPaths.h"
#include "wdlstring.h"

#include "dsp/NAMDSPManager.h"
#include "presets/PresetManager.h"
#include "presets/PresetStorage.h"
#include "ui/WebUIBridge.h"

namespace namguitar
{
namespace
{
constexpr int kNumPrograms = 0;

std::string ParamKey(NAMGuitarPlugin::ParameterId paramId);

nlohmann::json SerializeParametersToJson(const NAMGuitarPlugin& plugin)
{
  nlohmann::json parameters = nlohmann::json::array();
  for (int paramIdx = 0; paramIdx < NAMGuitarPlugin::kParamCount; ++paramIdx)
  {
    const auto* param = plugin.GetParam(paramIdx);
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
    default:
      return "";
  }
}

std::optional<NAMGuitarPlugin::ParameterId> ParamIdFromKey(const std::string& key)
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
  return std::nullopt;
}

nlohmann::json SerializePresetToJson(const Preset& preset)
{
  nlohmann::json jsonPreset;
  jsonPreset["id"] = preset.id;
  jsonPreset["name"] = preset.name;
  jsonPreset["category"] = preset.category;
  jsonPreset["description"] = preset.description;
  jsonPreset["namModelId"] = preset.namModelId;
  jsonPreset["irId"] = preset.irId;
  jsonPreset["fxChain"] = preset.fxChain;

  nlohmann::json attachments = nlohmann::json::array();
  for (const auto& attachment : preset.attachments)
  {
    attachments.push_back({
      {"type", attachment.type},
      {"filePath", attachment.filePath.generic_string()},
      {"hash", attachment.hash},
    });
  }
  jsonPreset["attachments"] = std::move(attachments);

  nlohmann::json parameters = nlohmann::json::array();
  for (const auto& parameter : preset.parameters)
  {
    parameters.push_back({
      {"id", parameter.id},
      {"value", parameter.value},
    });
  }
  jsonPreset["parameters"] = std::move(parameters);

  return jsonPreset;
}

std::filesystem::path ResolveAttachmentPath(const PresetAttachment& attachment)
{
  if (attachment.filePath.is_absolute())
  {
    return attachment.filePath;
  }

  const std::filesystem::path presetDirectory{"presets"};
  return presetDirectory / attachment.filePath;
}

} // namespace

NAMGuitarPlugin::NAMGuitarPlugin(const iplug::InstanceInfo& info)
  : iplug::Plugin(info, iplug::MakeConfig(kParamCount, kNumPrograms))
  , mDSP(std::make_unique<NAMDSPManager>())
  , mPresets(std::make_unique<PresetManager>())
  , mWebUI(std::make_unique<WebUIBridge>())
{
  if (mPresets)
  {
    mPresets->SetRemoteBaseUrl(mRemoteApiBaseUrl);
  }

  InitializeParameters();
  HandleWebViewMessages();

  for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
  {
    OnParamChange(paramIdx);
  }

#if PLUG_HAS_UI
  mMakeGraphicsFunc = [this]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [this](iplug::igraphics::IGraphics* graphics) {
    if (!graphics)
    {
      return;
    }
    graphics->AttachPanelBackground(iplug::igraphics::COLOR_BLACK);
    InitializeGraphics(*graphics);
  };
#endif
}

void NAMGuitarPlugin::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  if (!mDSP)
  {
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
}

void NAMGuitarPlugin::OnIdle()
{
  if (mWebUI)
  {
    mWebUI->PumpMessages();
  }

  if (mPendingStateBroadcast)
  {
    BroadcastState();
  }
}

bool NAMGuitarPlugin::SerializeState(iplug::IByteChunk& chunk) const
{
  if (!mPresets)
  {
    return false;
  }

  bool success = mPresets->Serialize(chunk);
  success &= chunk.PutStr(mActivePresetId.c_str());
  success &= chunk.PutStr(mActiveModelPath.c_str());
  success &= chunk.PutStr(mActiveIRPath.c_str());
  return success;
}

int NAMGuitarPlugin::UnserializeState(const iplug::IByteChunk& chunk, int startPos)
{
  if (!mPresets)
  {
    return startPos;
  }

  int position = mPresets->Unserialize(chunk, startPos);

  WDL_String activePreset;
  position = chunk.GetStr(activePreset, position);
  mActivePresetId = activePreset.Get();

  WDL_String modelPath;
  position = chunk.GetStr(modelPath, position);
  mActiveModelPath = modelPath.Get();

  WDL_String irPath;
  position = chunk.GetStr(irPath, position);
  mActiveIRPath = irPath.Get();

  mPendingStateBroadcast = true;
  return position;
}

void NAMGuitarPlugin::OnParamChange(int paramIdx)
{
  if (!mDSP)
  {
    return;
  }

  const auto* param = GetParam(paramIdx);
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
}

void NAMGuitarPlugin::HandleWebViewMessages()
{
  if (!mWebUI)
  {
    return;
  }

  mWebUI->RegisterMessageHandler([this](const std::string& message) {
    HandleUIMessage(message);
  });

  mWebUI->RegisterLogHandler([this](const std::string& logMessage) {
    (void) logMessage;
  });
}

void NAMGuitarPlugin::InitializeGraphics(iplug::igraphics::IGraphics& graphics)
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
  mWebUI->Initialize(graphics, resourceRoot);
  mPendingStateBroadcast = true;
}

void NAMGuitarPlugin::HandleUIMessage(const std::string& message)
{
  const nlohmann::json payload = nlohmann::json::parse(message, nullptr, false);
  if (payload.is_discarded())
  {
    return;
  }

  const std::string type = payload.value("type", "");
  if (type == "search")
  {
    HandlePresetSearch(payload);
  }
  else if (type == "downloadPreset")
  {
    HandlePresetDownload(payload);
  }
  else if (type == "loadPreset")
  {
    HandlePresetLoad(payload);
  }
}

void NAMGuitarPlugin::HandlePresetSearch(const nlohmann::json& payload)
{
  if (!mPresets)
  {
    return;
  }

  PresetSearchRequest request;
  request.query = payload.value("query", "");
  request.category = payload.value("category", "");

  mPresets->SearchRemotePresets(request, [this](std::vector<Preset> presets) {
    nlohmann::json message;
    message["type"] = "presetSearchResults";
    nlohmann::json results = nlohmann::json::array();
    for (const auto& preset : presets)
    {
      results.push_back(SerializePresetToJson(preset));
    }
    message["presets"] = std::move(results);

    if (mWebUI)
    {
      mWebUI->EnqueueMessage(message.dump());
    }
  });
}

void NAMGuitarPlugin::HandlePresetDownload(const nlohmann::json& payload)
{
  if (!mPresets)
  {
    return;
  }

  const std::string presetId = payload.value("presetId", "");
  if (presetId.empty())
  {
    return;
  }

  mPresets->DownloadRemotePreset(presetId, [this](std::vector<Preset> presets) {
    for (auto& preset : presets)
    {
      mPresets->SavePreset(preset);
      if (mWebUI)
      {
        nlohmann::json message;
        message["type"] = "presetLoaded";
        message["preset"] = SerializePresetToJson(preset);
        mWebUI->EnqueueMessage(message.dump());
      }
    }
  });
}

void NAMGuitarPlugin::HandlePresetLoad(const nlohmann::json& payload)
{
  if (!mPresets)
  {
    return;
  }

  const std::string presetId = payload.value("presetId", "");
  if (presetId.empty())
  {
    return;
  }

  const auto presets = mPresets->ListPresets();
  const auto it = std::find_if(presets.begin(), presets.end(), [&](const Preset& candidate) {
    return candidate.id == presetId;
  });

  if (it != presets.end())
  {
    ApplyPreset(*it);
    mActivePresetId = it->id;
    mPendingStateBroadcast = true;
  }
}

void NAMGuitarPlugin::BroadcastState()
{
  if (!mPresets || !mWebUI)
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

  nlohmann::json presetsJson = nlohmann::json::array();
  for (const auto& preset : mPresets->ListPresets())
  {
    presetsJson.push_back(SerializePresetToJson(preset));
  }
  message["presets"] = std::move(presetsJson);

  mWebUI->EnqueueMessage(message.dump());
  mPendingStateBroadcast = false;
}

void NAMGuitarPlugin::ApplyPreset(const Preset& preset)
{
  if (!mDSP)
  {
    return;
  }

  for (const auto& parameter : preset.parameters)
  {
    const auto paramId = ParamIdFromKey(parameter.id);
    if (paramId)
    {
      const auto index = static_cast<int>(*paramId);
      auto* param = GetParam(index);
      param->Set(parameter.value);
      OnParamChange(index);
    }
  }

  for (const auto& attachment : preset.attachments)
  {
    const auto path = ResolveAttachmentPath(attachment);
    if (attachment.type == "nam")
    {
      if (mDSP->LoadModel(path))
      {
        mActiveModelPath = path.generic_string();
      }
    }
    else if (attachment.type == "ir")
    {
      if (mDSP->LoadImpulseResponse(path))
      {
        mActiveIRPath = path.generic_string();
      }
    }
  }
}

void NAMGuitarPlugin::SaveCurrentStateToPreset(Preset& preset) const
{
  preset.parameters.clear();
  for (int paramIdx = 0; paramIdx < kParamCount; ++paramIdx)
  {
    PresetParameter entry;
    entry.id = ParamKey(static_cast<ParameterId>(paramIdx));
    entry.value = GetParam(paramIdx)->Value();
    preset.parameters.push_back(std::move(entry));
  }
}

} // namespace namguitar
