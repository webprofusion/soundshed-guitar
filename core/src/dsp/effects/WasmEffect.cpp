#include "dsp/effects/WasmEffect.h"

#include "util/FileIO.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx {

namespace {

constexpr std::size_t kModuleSlot = 0;
constexpr std::size_t kTempoParamIndex = WasmEffect::kGuestMacroCount;

float ClampFinite(float value, float fallback)
{
  return std::isfinite(value) ? value : fallback;
}

double DbToLinear(double value)
{
  return std::pow(10.0, value / 20.0);
}

std::optional<std::size_t> ParseResourceSlotIndex(const ResourceRef& ref)
{
  const auto it = ref.metadata.find("resourceSlotIndex");
  if (it == ref.metadata.end())
    return std::nullopt;

  try
  {
    return static_cast<std::size_t>(std::stoul(it->second));
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::optional<std::size_t> ParseGuestMacroIndex(const std::string& key)
{
  constexpr char kPrefix[] = "param";
  if (key.rfind(kPrefix, 0) != 0)
    return std::nullopt;

  try
  {
    const std::size_t oneBasedIndex = static_cast<std::size_t>(std::stoul(key.substr(sizeof(kPrefix) - 1)));
    if (oneBasedIndex == 0 || oneBasedIndex > WasmEffect::kGuestMacroCount)
      return std::nullopt;
    return oneBasedIndex - 1;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::string TrimDescriptorToken(std::string value)
{
  const auto notSpace = [](unsigned char ch) {
    return !std::isspace(ch);
  };

  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

bool ParseBoolLike(const std::string& value)
{
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

std::optional<double> ParseDoubleLike(const std::string& value)
{
  try
  {
    return std::stod(value);
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::optional<std::size_t> ParseSizeTLike(const std::string& value)
{
  try
  {
    return static_cast<std::size_t>(std::stoul(value));
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::vector<std::string> SplitLabels(const std::string& value)
{
  std::vector<std::string> labels;
  std::string current;
  std::istringstream stream(value);
  while (std::getline(stream, current, '|'))
  {
    labels.push_back(TrimDescriptorToken(current));
  }
  return labels;
}

std::optional<std::pair<std::size_t, std::string>> ParseIndexedDescriptorKey(const std::string& key,
                                                                              const std::string& prefix)
{
  const std::string stem = prefix + ".";
  if (key.rfind(stem, 0) != 0)
    return std::nullopt;

  const std::size_t fieldSeparator = key.find('.', stem.size());
  if (fieldSeparator == std::string::npos)
    return std::nullopt;

  const auto index = ParseSizeTLike(key.substr(stem.size(), fieldSeparator - stem.size()));
  if (!index)
    return std::nullopt;

  return std::make_pair(*index, key.substr(fieldSeparator + 1));
}

using FunctionTypePtr = std::unique_ptr<wasm_functype_t, decltype(&wasm_functype_delete)>;

std::string CopyWasmMessage(const wasm_name_t& message)
{
  if (!message.data || message.size == 0)
    return {};

  std::size_t size = message.size;
  if (size > 0 && message.data[size - 1] == '\0')
    --size;

  return std::string(message.data, size);
}

std::string TakeErrorMessage(wasmtime_error_t* error)
{
  if (!error)
    return {};

  wasm_name_t message;
  wasmtime_error_message(error, &message);
  std::string text = CopyWasmMessage(message);
  wasm_byte_vec_delete(&message);
  wasmtime_error_delete(error);
  return text;
}

std::string TakeTrapMessage(wasm_trap_t* trap)
{
  if (!trap)
    return {};

  wasm_name_t message;
  wasm_trap_message(trap, &message);
  std::string text = CopyWasmMessage(message);
  wasm_byte_vec_delete(&message);
  wasm_trap_delete(trap);
  return text;
}

std::optional<WasmModuleDescriptor> BuildDescriptorFromEntries(const std::vector<WasmMetadataEntry>& entries,
                                                               std::string* error)
{
  WasmModuleDescriptor descriptor;
  descriptor.entries = entries;

  std::map<std::size_t, std::map<std::string, std::string>> parameterFields;
  std::map<std::size_t, std::map<std::string, std::string>> resourceFields;
  std::string thumbnailBase64;
  std::string thumbnailMimeType = "image/png";

  for (const auto& entry : entries)
  {
    if (entry.key == "effect.name" || entry.key == "effect.title")
    {
      descriptor.displayName = entry.value;
      continue;
    }

    if (entry.key == "effect.description")
    {
      descriptor.description = entry.value;
      continue;
    }

    if (entry.key == "effect.version")
    {
      descriptor.version = entry.value;
      continue;
    }

    if (entry.key == "effect.category")
    {
      descriptor.category = entry.value.empty() ? "utility" : entry.value;
      continue;
    }

    if (entry.key == "effect.thumbnailDataUrl")
    {
      descriptor.thumbnailDataUrl = entry.value;
      continue;
    }

    if (entry.key == "effect.thumbnailBase64")
    {
      thumbnailBase64 = entry.value;
      continue;
    }

    if (entry.key == "effect.thumbnailMimeType")
    {
      thumbnailMimeType = entry.value.empty() ? "image/png" : entry.value;
      continue;
    }

    if (const auto parsed = ParseIndexedDescriptorKey(entry.key, "param"))
    {
      parameterFields[parsed->first][parsed->second] = entry.value;
      continue;
    }

    if (const auto parsed = ParseIndexedDescriptorKey(entry.key, "resource"))
    {
      resourceFields[parsed->first][parsed->second] = entry.value;
      continue;
    }
  }

  std::unordered_map<std::string, std::size_t> usedParamIds;
  std::unordered_map<std::size_t, std::string> usedSlots;

  for (const auto& [declIndex, fields] : parameterFields)
  {
    const auto idIt = fields.find("id");
    if (idIt == fields.end() || idIt->second.empty())
    {
      if (error)
        *error = "WASM descriptor param." + std::to_string(declIndex) + " is missing required 'id'.";
      return std::nullopt;
    }

    const auto slotIt = fields.find("slot");
    const std::size_t slot = slotIt != fields.end()
        ? ParseSizeTLike(slotIt->second).value_or(WasmEffect::kGuestMacroCount)
        : declIndex;
    if (slot >= WasmEffect::kGuestMacroCount)
    {
      if (error)
        *error = "WASM descriptor param." + std::to_string(declIndex) + " declares slot outside the supported 0..7 range.";
      return std::nullopt;
    }

    if (usedParamIds.count(idIt->second) > 0)
    {
      if (error)
        *error = "WASM descriptor declares duplicate parameter id '" + idIt->second + "'.";
      return std::nullopt;
    }
    if (usedSlots.count(slot) > 0)
    {
      if (error)
        *error = "WASM descriptor declares multiple parameters for guest slot " + std::to_string(slot) + ".";
      return std::nullopt;
    }

    WasmGuestParameterDescriptor paramDescriptor;
    paramDescriptor.slot = slot;
    paramDescriptor.definition.id = idIt->second;

    if (const auto titleIt = fields.find("title"); titleIt != fields.end() && !titleIt->second.empty())
      paramDescriptor.definition.displayName = titleIt->second;
    else if (const auto nameIt = fields.find("name"); nameIt != fields.end() && !nameIt->second.empty())
      paramDescriptor.definition.displayName = nameIt->second;
    else
      paramDescriptor.definition.displayName = idIt->second;

    if (const auto defaultIt = fields.find("default"); defaultIt != fields.end())
      paramDescriptor.definition.defaultValue = ParseDoubleLike(defaultIt->second).value_or(0.0);
    if (const auto minIt = fields.find("min"); minIt != fields.end())
      paramDescriptor.definition.minValue = ParseDoubleLike(minIt->second).value_or(0.0);
    if (const auto maxIt = fields.find("max"); maxIt != fields.end())
      paramDescriptor.definition.maxValue = ParseDoubleLike(maxIt->second).value_or(1.0);
    if (paramDescriptor.definition.maxValue < paramDescriptor.definition.minValue)
    {
      if (error)
        *error = "WASM descriptor param '" + idIt->second + "' has max < min.";
      return std::nullopt;
    }

    if (const auto unitIt = fields.find("unit"); unitIt != fields.end())
      paramDescriptor.definition.unit = unitIt->second;
    if (const auto groupIt = fields.find("group"); groupIt != fields.end())
      paramDescriptor.definition.group = groupIt->second;
    if (const auto advancedIt = fields.find("advanced"); advancedIt != fields.end())
      paramDescriptor.definition.advanced = ParseBoolLike(advancedIt->second);
    if (const auto stepIt = fields.find("step"); stepIt != fields.end())
      paramDescriptor.definition.step = ParseDoubleLike(stepIt->second).value_or(0.0);
    if (const auto labelsIt = fields.find("labels"); labelsIt != fields.end())
      paramDescriptor.definition.labels = SplitLabels(labelsIt->second);
    if (!paramDescriptor.definition.labels.empty() && paramDescriptor.definition.unit.empty())
      paramDescriptor.definition.unit = "enum";
    if (!paramDescriptor.definition.labels.empty() && paramDescriptor.definition.step == 0.0)
      paramDescriptor.definition.step = 1.0;

    usedParamIds[paramDescriptor.definition.id] = slot;
    usedSlots[slot] = paramDescriptor.definition.id;
    descriptor.parameters.push_back(std::move(paramDescriptor));
  }

  for (const auto& [declIndex, fields] : resourceFields)
  {
    const auto idIt = fields.find("id");
    if (idIt == fields.end() || idIt->second.empty())
    {
      if (error)
        *error = "WASM descriptor resource." + std::to_string(declIndex) + " is missing required 'id'.";
      return std::nullopt;
    }

    ExposedResource resource;
    resource.resourceId = idIt->second;
    if (const auto titleIt = fields.find("title"); titleIt != fields.end() && !titleIt->second.empty())
      resource.displayName = titleIt->second;
    else if (const auto nameIt = fields.find("name"); nameIt != fields.end() && !nameIt->second.empty())
      resource.displayName = nameIt->second;
    else
      resource.displayName = idIt->second;

    resource.resourceType = fields.contains("type") ? fields.at("type") : "blob";
    resource.resourceIndex = static_cast<int>(fields.contains("slot")
        ? ParseSizeTLike(fields.at("slot")).value_or(declIndex + 1)
        : declIndex + 1);
    resource.allowBrowseFile = !fields.contains("allowBrowseFile") || ParseBoolLike(fields.at("allowBrowseFile"));
    if (resource.resourceIndex <= 0)
    {
      if (error)
        *error = "WASM descriptor resource '" + resource.resourceId + "' must use a positive resource slot; slot 0 is reserved for the module itself.";
      return std::nullopt;
    }

    if (const auto parameterIdIt = fields.find("parameterId"); parameterIdIt != fields.end())
      resource.parameterId = parameterIdIt->second;
    if (const auto parameterValueIt = fields.find("parameterValue"); parameterValueIt != fields.end())
      resource.parameterValue = ParseDoubleLike(parameterValueIt->second).value_or(0.0);

    descriptor.exposedResources.push_back(std::move(resource));
  }

  if (descriptor.thumbnailDataUrl.empty() && !thumbnailBase64.empty())
    descriptor.thumbnailDataUrl = "data:" + thumbnailMimeType + ";base64," + thumbnailBase64;

  return descriptor;
}

std::optional<WasmModuleDescriptor> ParseDescriptorBlob(const std::string& text, std::string* error)
{
  std::vector<WasmMetadataEntry> entries;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    line = TrimDescriptorToken(line);
    if (line.empty() || line[0] == '#')
      continue;

    const std::size_t separator = line.find('=');
    if (separator == std::string::npos)
    {
      if (error)
        *error = "WASM descriptor line is missing '=': " + line;
      return std::nullopt;
    }

    WasmMetadataEntry entry;
    entry.key = TrimDescriptorToken(line.substr(0, separator));
    entry.value = TrimDescriptorToken(line.substr(separator + 1));
    if (entry.key.empty())
    {
      if (error)
        *error = "WASM descriptor contains an empty key.";
      return std::nullopt;
    }
    entries.push_back(std::move(entry));
  }

  return BuildDescriptorFromEntries(entries, error);
}

FunctionTypePtr MakeFunctionType(wasm_functype_t* type)
{
  return FunctionTypePtr(type, wasm_functype_delete);
}

wasmtime_val_t MakeI32Value(int32_t value)
{
  wasmtime_val_t result{};
  result.kind = WASMTIME_I32;
  result.of.i32 = value;
  return result;
}

wasmtime_val_t MakeF32Value(float value)
{
  wasmtime_val_t result{};
  result.kind = WASMTIME_F32;
  result.of.f32 = value;
  return result;
}

extern "C" wasm_trap_t* HostReadParamRaw(void* env,
                                          wasmtime_caller_t* caller,
                                          const wasmtime_val_t* args,
                                          size_t nargs,
                                          wasmtime_val_t* results,
                                          size_t nresults)
{
  (void)caller;

  if (nresults > 0)
    results[0] = MakeF32Value(0.0f);

  if (!env || nargs < 1 || nresults < 1)
    return nullptr;

  auto* host = static_cast<WasmEffect*>(env);
  results[0] = MakeF32Value(host->ReadGuestParam(args[0].of.i32));
  return nullptr;
}

extern "C" wasm_trap_t* HostReadResourceSizeRaw(void* env,
                                                 wasmtime_caller_t* caller,
                                                 const wasmtime_val_t* args,
                                                 size_t nargs,
                                                 wasmtime_val_t* results,
                                                 size_t nresults)
{
  (void)caller;

  if (nresults > 0)
    results[0] = MakeI32Value(0);

  if (!env || nargs < 1 || nresults < 1)
    return nullptr;

  auto* host = static_cast<WasmEffect*>(env);
  results[0] = MakeI32Value(host->ReadGuestResourceSize(args[0].of.i32));
  return nullptr;
}

extern "C" wasm_trap_t* HostReadResourceByteRaw(void* env,
                                                 wasmtime_caller_t* caller,
                                                 const wasmtime_val_t* args,
                                                 size_t nargs,
                                                 wasmtime_val_t* results,
                                                 size_t nresults)
{
  (void)caller;

  if (nresults > 0)
    results[0] = MakeI32Value(0);

  if (!env || nargs < 2 || nresults < 1)
    return nullptr;

  auto* host = static_cast<WasmEffect*>(env);
  results[0] = MakeI32Value(host->ReadGuestResourceByte(args[0].of.i32, args[1].of.i32));
  return nullptr;
}

} // namespace

WasmEffect::WasmEffect()
{
  ResetGuestDescriptor();
  mEngine = wasm_engine_new();
  if (!mEngine)
    SetError("Failed to create Wasmtime engine.");
}

WasmEffect::~WasmEffect()
{
  TeardownRuntime();

  if (mEngine)
  {
    wasm_engine_delete(mEngine);
    mEngine = nullptr;
  }
}

std::optional<WasmModuleDescriptor> WasmEffect::InspectModuleFile(const std::filesystem::path& modulePath,
                                                                  std::string* error)
{
  if (!std::filesystem::exists(modulePath))
  {
    if (error)
      *error = "WASM module file not found: " + modulePath.string();
    return std::nullopt;
  }

  WasmEffect probe;
  probe.mModuleBytes = guitarfx::util::ReadFileBytes(modulePath);
  probe.mModulePath = modulePath;
  if (probe.mModuleBytes.empty())
  {
    if (error)
      *error = "WASM module file is empty: " + modulePath.string();
    return std::nullopt;
  }

  if (!probe.BuildRuntimeOnly())
  {
    if (error)
      *error = probe.mLastError;
    return std::nullopt;
  }

  if (!probe.LoadGuestDescriptor())
  {
    if (error)
      *error = probe.mLastError;
    probe.TeardownRuntime();
    return std::nullopt;
  }

  probe.TeardownRuntime();
  return probe.mGuestDescriptor;
}

std::optional<WasmModuleDescriptor> WasmEffect::ParseDescriptorConfig(const std::string& configJson,
                                                                      std::string* error)
{
  try
  {
    const auto parsed = nlohmann::json::parse(configJson);
    if (!parsed.is_array())
    {
      if (error)
        *error = "WASM descriptor config must be a JSON array of { key, value } entries.";
      return std::nullopt;
    }

    std::vector<WasmMetadataEntry> entries;
    for (const auto& item : parsed)
    {
      if (!item.is_object())
        continue;

      WasmMetadataEntry entry;
      entry.key = item.value("key", "");
      entry.value = item.value("value", "");
      if (!entry.key.empty())
        entries.push_back(std::move(entry));
    }

    return BuildDescriptorFromEntries(entries, error);
  }
  catch (const std::exception& ex)
  {
    if (error)
      *error = std::string("Failed to parse WASM descriptor config: ") + ex.what();
    return std::nullopt;
  }
}

std::string WasmEffect::SerializeDescriptorConfig(const std::vector<WasmMetadataEntry>& entries)
{
  nlohmann::json payload = nlohmann::json::array();
  for (const auto& entry : entries)
  {
    payload.push_back({
        { "key", entry.key },
        { "value", entry.value },
    });
  }
  return payload.dump();
}

void WasmEffect::Prepare(double sampleRate, int maxBlockSize)
{
  if (!ValidatePrepare(sampleRate, maxBlockSize))
    return;

  mSampleRate = sampleRate;
  mMaxBlockSize = maxBlockSize;
  mLatencySamples = 0;

  if (!mModuleBytes.empty())
    RebuildRuntime();
}

void WasmEffect::Reset()
{
  if (!mRuntimeReady || !mHasResetFunction || !mContext)
    return;

  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* error = wasmtime_func_call(mContext, &mResetFunction, nullptr, 0, nullptr, 0, &trap))
  {
    SetError(std::string("WASM reset failed: ") + TakeErrorMessage(error));
    mRuntimeReady = false;
    return;
  }

  if (trap)
  {
    SetError(std::string("WASM reset trapped: ") + TakeTrapMessage(trap));
    mRuntimeReady = false;
  }
}

void WasmEffect::Process(float** inputs, float** outputs, int numSamples)
{
  if (!outputs || numSamples <= 0)
    return;

  const float* inputLeft = inputs ? inputs[0] : nullptr;
  const float* inputRight = inputs ? inputs[1] : nullptr;
  float* outputLeft = outputs[0];
  float* outputRight = outputs[1];

  if (!outputLeft && !outputRight)
    return;

  const float wetMix = static_cast<float>(std::clamp(mMix, 0.0, 1.0));
  const float dryMix = 1.0f - wetMix;
  const float inputGain = static_cast<float>(DbToLinear(mInputGainDb));
  const float outputGain = static_cast<float>(DbToLinear(mOutputGainDb));

  for (int sample = 0; sample < numSamples; ++sample)
  {
    // Treat a single live input channel as dual-mono so stereo guest modules
    // behave sensibly on mono sources.
    const float dryLeft = inputLeft ? inputLeft[sample] : (inputRight ? inputRight[sample] : 0.0f);
    const float dryRight = inputRight ? inputRight[sample] : dryLeft;

    float wetLeft = dryLeft * inputGain;
    float wetRight = dryRight * inputGain;

    if (mRuntimeReady)
    {
      if (!InvokeProcess(wetLeft, wetRight, wetLeft, wetRight))
      {
        wetLeft = dryLeft;
        wetRight = dryRight;
      }
    }
    else
    {
      wetLeft = dryLeft;
      wetRight = dryRight;
    }

    const float mixedLeft = (dryLeft * dryMix) + (wetLeft * wetMix);
    const float mixedRight = (dryRight * dryMix) + (wetRight * wetMix);

    if (outputLeft)
      outputLeft[sample] = mixedLeft * outputGain;
    if (outputRight)
      outputRight[sample] = mixedRight * outputGain;
  }
}

void WasmEffect::SetParam(const std::string& key, double value)
{
  if (key == "mix")
  {
    mMix = value;
    return;
  }

  if (key == "inputGainDb")
  {
    mInputGainDb = value;
    return;
  }

  if (key == "outputGainDb")
  {
    mOutputGainDb = value;
    return;
  }

  if (key == "bpm")
  {
    mBpm = value;
    return;
  }

  if (const auto macroIndex = ParseGuestMacroIndex(key))
  {
    mPendingGuestParamValues[key] = value;
    mGuestMacros[*macroIndex] = value;
    return;
  }

  mPendingGuestParamValues[key] = value;
  if (const auto slotIt = mGuestParamSlots.find(key); slotIt != mGuestParamSlots.end())
    mGuestMacros[slotIt->second] = value;
}

double WasmEffect::GetParam(const std::string& key) const
{
  if (key == "mix")
    return mMix;
  if (key == "inputGainDb")
    return mInputGainDb;
  if (key == "outputGainDb")
    return mOutputGainDb;
  if (key == "bpm")
    return mBpm;
  if (const auto macroIndex = ParseGuestMacroIndex(key))
    return mGuestMacros[*macroIndex];
  if (const auto pendingIt = mPendingGuestParamValues.find(key); pendingIt != mPendingGuestParamValues.end())
    return pendingIt->second;
  if (const auto slotIt = mGuestParamSlots.find(key); slotIt != mGuestParamSlots.end())
    return mGuestMacros[slotIt->second];
  return 0.0;
}

void WasmEffect::SetConfig(const std::string& key, const std::string& value)
{
  (void)key;
  (void)value;
}

std::string WasmEffect::GetConfig(const std::string& key) const
{
  if (key == "lastError")
    return mLastError;
  return {};
}

bool WasmEffect::LoadResources(const std::vector<ResourceRef>& refs, const std::vector<std::filesystem::path>& paths)
{
  mModuleBytes.clear();
  mResourceBytesBySlot.clear();
  mModulePath.clear();
  mLatencySamples = 0;
  mRuntimeReady = false;
  mLastError.clear();
  ResetGuestDescriptor();
  TeardownRuntime();

  const std::size_t resourceCount = std::min(refs.size(), paths.size());
  if (resourceCount == 0)
  {
    SetError("WASM effect requires a module resource in slot 0.");
    return false;
  }

  for (std::size_t i = 0; i < resourceCount; ++i)
  {
    const std::size_t slotIndex = ParseResourceSlotIndex(refs[i]).value_or(i);
    const auto fileBytes = guitarfx::util::ReadFileBytes(paths[i]);

    if (mResourceBytesBySlot.size() <= slotIndex)
      mResourceBytesBySlot.resize(slotIndex + 1);

    mResourceBytesBySlot[slotIndex] = fileBytes;
    if (slotIndex == kModuleSlot)
    {
      mModuleBytes = fileBytes;
      mModulePath = paths[i];
    }
  }

  if (mModuleBytes.empty())
  {
    SetError("WASM effect could not load a module from resource slot 0.");
    return false;
  }

  if (!BuildRuntimeOnly())
    return false;

  if (!LoadGuestDescriptor())
  {
    TeardownRuntime();
    return false;
  }

  TeardownRuntime();

  if (mSampleRate > 0.0 && mMaxBlockSize > 0)
    return RebuildRuntime();

  return true;
}

float WasmEffect::ReadGuestParam(int index) const
{
  if (index < 0)
    return 0.0f;

  const std::size_t slot = static_cast<std::size_t>(index);
  if (slot < mGuestMacros.size())
    return static_cast<float>(mGuestMacros[slot]);
  if (slot == kTempoParamIndex)
    return static_cast<float>(mBpm);
  return 0.0f;
}

int WasmEffect::ReadGuestResourceSize(int slot) const
{
  if (slot < 0)
    return 0;

  const std::size_t resourceSlot = static_cast<std::size_t>(slot);
  if (resourceSlot >= mResourceBytesBySlot.size())
    return 0;

  return static_cast<int>(mResourceBytesBySlot[resourceSlot].size());
}

int WasmEffect::ReadGuestResourceByte(int slot, int offset) const
{
  if (slot < 0 || offset < 0)
    return 0;

  const std::size_t resourceSlot = static_cast<std::size_t>(slot);
  const std::size_t resourceOffset = static_cast<std::size_t>(offset);
  if (resourceSlot >= mResourceBytesBySlot.size())
    return 0;

  const auto& resourceBytes = mResourceBytesBySlot[resourceSlot];
  if (resourceOffset >= resourceBytes.size())
    return 0;

  return static_cast<int>(resourceBytes[resourceOffset]);
}

bool WasmEffect::RebuildRuntime()
{
  mRuntimeReady = false;
  mLatencySamples = 0;

  if (!BuildRuntimeOnly())
    return false;

  if (!LoadGuestDescriptor())
  {
    TeardownRuntime();
    return false;
  }

  if (!InvokePrepare())
  {
    TeardownRuntime();
    return false;
  }

  mLatencySamples = QueryLatencySamples();
  mRuntimeReady = true;
  mLastError.clear();
  return true;
}

bool WasmEffect::BuildRuntimeOnly()
{
  TeardownRuntime();

  if (mModuleBytes.empty())
  {
    SetError("WASM effect cannot build a runtime without module bytes.");
    return false;
  }

  if (!mEngine)
  {
    mEngine = wasm_engine_new();
    if (!mEngine)
    {
      SetError("Failed to create Wasmtime engine.");
      return false;
    }
  }

  if (wasmtime_error_t* error = wasmtime_module_new(mEngine, mModuleBytes.data(), mModuleBytes.size(), &mModule))
  {
    SetError(std::string("Failed to compile WASM module: ") + TakeErrorMessage(error));
    TeardownRuntime();
    return false;
  }

  mLinker = wasmtime_linker_new(mEngine);
  if (!mLinker)
  {
    SetError("Failed to create Wasmtime linker.");
    TeardownRuntime();
    return false;
  }

  mStore = wasmtime_store_new(mEngine, this, nullptr);
  if (!mStore)
  {
    SetError("Failed to create Wasmtime store.");
    TeardownRuntime();
    return false;
  }

  mContext = wasmtime_store_context(mStore);
  if (!mContext)
  {
    SetError("Failed to access Wasmtime store context.");
    TeardownRuntime();
    return false;
  }

  const auto defineHostImport = [&](const char* fieldName,
                                    FunctionTypePtr&& type,
                                    wasmtime_func_callback_t callback) -> bool {
    if (!type)
    {
      SetError(std::string("Failed to allocate Wasmtime signature for host import '") + fieldName + "'.");
      return false;
    }

    if (wasmtime_error_t* error = wasmtime_linker_define_func(mLinker,
                                                              "host",
                                                              std::strlen("host"),
                                                              fieldName,
                                                              std::strlen(fieldName),
                                                              type.get(),
                                                              callback,
                                                              this,
                                                              nullptr))
    {
      SetError(std::string("Failed to link host import '") + fieldName + "': " + TakeErrorMessage(error));
      return false;
    }

    return true;
  };

  if (!defineHostImport("read_param", MakeFunctionType(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_f32())), HostReadParamRaw)
      || !defineHostImport("read_resource_size", MakeFunctionType(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32())), HostReadResourceSizeRaw)
      || !defineHostImport("read_resource_byte", MakeFunctionType(wasm_functype_new_2_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32())), HostReadResourceByteRaw))
  {
    TeardownRuntime();
    return false;
  }

  const auto loadAliasedFunctionExport = [&](const char* preferredName,
                                             const char* legacyName,
                                             wasmtime_func_t& outFunction,
                                             bool required,
                                             bool& found) -> bool {
    bool hasPreferred = false;
    if (!LoadFunctionExport(preferredName, outFunction, false, hasPreferred))
      return false;
    if (hasPreferred)
    {
      found = true;
      return true;
    }

    bool hasLegacy = false;
    if (!LoadFunctionExport(legacyName, outFunction, false, hasLegacy))
      return false;
    if (hasLegacy)
    {
      found = true;
      return true;
    }

    found = false;
    if (required)
    {
      SetError(std::string("WASM module is missing required export '") + preferredName
          + "' (legacy '" + legacyName + "' is also accepted).");
      return false;
    }

    return true;
  };

  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* error = wasmtime_linker_instantiate(mLinker, mContext, mModule, &mInstance, &trap))
  {
    SetError(std::string("Failed to instantiate WASM module: ") + TakeErrorMessage(error));
    TeardownRuntime();
    return false;
  }

  if (trap)
  {
    SetError(std::string("Failed to instantiate WASM module: ") + TakeTrapMessage(trap));
    TeardownRuntime();
    return false;
  }

  mHasInstance = true;

  if (!loadAliasedFunctionExport("audiofx_prepare", "guitarfx_prepare", mPrepareFunction, true, mHasPrepareFunction)
      || !loadAliasedFunctionExport("audiofx_reset", "guitarfx_reset", mResetFunction, true, mHasResetFunction)
      || !loadAliasedFunctionExport("audiofx_process", "guitarfx_process", mProcessFunction, true, mHasProcessFunction)
      || !loadAliasedFunctionExport("audiofx_get_latency_samples", "guitarfx_get_latency_samples", mLatencyFunction, false, mHasLatencyFunction))
  {
    TeardownRuntime();
    return false;
  }

  return true;
}

bool WasmEffect::LoadFunctionExport(const char* exportName, wasmtime_func_t& outFunction, bool required, bool& found)
{
  outFunction = {};
  found = false;

  if (!mContext || !mHasInstance)
  {
    SetError("WASM runtime is not ready for export lookup.");
    return false;
  }

  wasmtime_extern_t item{};
  if (!wasmtime_instance_export_get(mContext, &mInstance, exportName, std::strlen(exportName), &item))
  {
    if (required)
    {
      SetError(std::string("WASM module is missing required export '") + exportName + "'.");
      return false;
    }

    return true;
  }

  if (item.kind != WASMTIME_EXTERN_FUNC)
  {
    wasmtime_extern_delete(&item);
    SetError(std::string("WASM export '") + exportName + "' is not a function.");
    return false;
  }

  outFunction = item.of.func;
  found = true;
  wasmtime_extern_delete(&item);
  return true;
}

bool WasmEffect::InvokePrepare()
{
  if (!mHasPrepareFunction || !mContext)
    return false;

  std::array<wasmtime_val_t, 3> args{
      MakeF32Value(static_cast<float>(mSampleRate)),
      MakeI32Value(static_cast<int32_t>(mMaxBlockSize)),
      MakeI32Value(static_cast<int32_t>(mResourceBytesBySlot.size())),
  };
  std::array<wasmtime_val_t, 1> results{};

  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* error = wasmtime_func_call(mContext,
                                                   &mPrepareFunction,
                                                   args.data(),
                                                   args.size(),
                                                   results.data(),
                                                   results.size(),
                                                   &trap))
  {
    SetError(std::string("WASM prepare failed: ") + TakeErrorMessage(error));
    return false;
  }

  if (trap)
  {
    SetError(std::string("WASM prepare trapped: ") + TakeTrapMessage(trap));
    return false;
  }

  if (results[0].kind != WASMTIME_I32)
  {
    SetError("WASM prepare returned an unexpected result type.");
    return false;
  }

  const int32_t status = results[0].of.i32;

  if (status != 0)
  {
    std::ostringstream error;
    error << "WASM prepare returned non-zero status " << status << '.';
    SetError(error.str());
    return false;
  }

  return true;
}

bool WasmEffect::InvokeProcess(float inputLeft, float inputRight, float& outputLeft, float& outputRight)
{
  if (!mHasProcessFunction || !mContext)
    return false;

  std::array<wasmtime_val_t, 2> args{
      MakeF32Value(inputLeft),
      MakeF32Value(inputRight),
  };
  std::array<wasmtime_val_t, 2> results{};

  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* error = wasmtime_func_call(mContext,
                                                   &mProcessFunction,
                                                   args.data(),
                                                   args.size(),
                                                   results.data(),
                                                   results.size(),
                                                   &trap))
  {
    SetError(std::string("WASM process failed: ") + TakeErrorMessage(error));
    mRuntimeReady = false;
    return false;
  }

  if (trap)
  {
    SetError(std::string("WASM process trapped: ") + TakeTrapMessage(trap));
    mRuntimeReady = false;
    return false;
  }

  if (results[0].kind != WASMTIME_F32 || results[1].kind != WASMTIME_F32)
  {
    SetError("WASM process returned unexpected result types.");
    mRuntimeReady = false;
    return false;
  }

  outputLeft = results[0].of.f32;
  outputRight = results[1].of.f32;

  outputLeft = ClampFinite(outputLeft, inputLeft);
  outputRight = ClampFinite(outputRight, inputRight);
  return true;
}

int WasmEffect::QueryLatencySamples()
{
  if (!mHasLatencyFunction || !mContext)
    return 0;

  std::array<wasmtime_val_t, 1> results{};

  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* error = wasmtime_func_call(mContext,
                                                   &mLatencyFunction,
                                                   nullptr,
                                                   0,
                                                   results.data(),
                                                   results.size(),
                                                   &trap))
  {
    SetError(std::string("WASM latency query failed: ") + TakeErrorMessage(error));
    return 0;
  }

  if (trap)
  {
    SetError(std::string("WASM latency query trapped: ") + TakeTrapMessage(trap));
    return 0;
  }

  if (results[0].kind != WASMTIME_I32)
  {
    SetError("WASM latency query returned an unexpected result type.");
    return 0;
  }

  const int32_t latencySamples = results[0].of.i32;

  return std::max(latencySamples, 0);
}

bool WasmEffect::LoadGuestDescriptor()
{
  ResetGuestDescriptor();

  wasmtime_func_t descriptorPtrFunction{};
  wasmtime_func_t descriptorLenFunction{};
  bool hasDescriptorPtr = false;
  bool hasDescriptorLen = false;
  const char* descriptorPtrName = "audiofx_descriptor_ptr";
  const char* descriptorLenName = "audiofx_descriptor_len";

  bool hasAudioDescriptorPtr = false;
  bool hasAudioDescriptorLen = false;
  if (!LoadFunctionExport("audiofx_descriptor_ptr", descriptorPtrFunction, false, hasAudioDescriptorPtr)
      || !LoadFunctionExport("audiofx_descriptor_len", descriptorLenFunction, false, hasAudioDescriptorLen))
  {
    return false;
  }

  if (hasAudioDescriptorPtr != hasAudioDescriptorLen)
  {
    SetError("WASM descriptor exports must provide both 'audiofx_descriptor_ptr' and 'audiofx_descriptor_len'.");
    return false;
  }

  if (hasAudioDescriptorPtr)
  {
    hasDescriptorPtr = true;
    hasDescriptorLen = true;
  }
  else
  {
    if (!LoadFunctionExport("guitarfx_descriptor_ptr", descriptorPtrFunction, false, hasDescriptorPtr)
        || !LoadFunctionExport("guitarfx_descriptor_len", descriptorLenFunction, false, hasDescriptorLen))
    {
      return false;
    }

    if (hasDescriptorPtr != hasDescriptorLen)
    {
      SetError("WASM descriptor exports must provide both 'guitarfx_descriptor_ptr' and 'guitarfx_descriptor_len'.");
      return false;
    }

    if (hasDescriptorPtr)
    {
      descriptorPtrName = "guitarfx_descriptor_ptr";
      descriptorLenName = "guitarfx_descriptor_len";
    }
  }

  if (!hasDescriptorPtr)
  {
    ApplyPendingGuestParamValues();
    return true;
  }

  if (!mContext || !mHasInstance)
  {
    SetError("WASM runtime is not ready for descriptor lookup.");
    return false;
  }

  auto callI32Function = [&](wasmtime_func_t& function, const char* functionName) -> std::optional<int32_t> {
    std::array<wasmtime_val_t, 1> results{};
    wasm_trap_t* trap = nullptr;
    if (wasmtime_error_t* error = wasmtime_func_call(mContext,
                                                     &function,
                                                     nullptr,
                                                     0,
                                                     results.data(),
                                                     results.size(),
                                                     &trap))
    {
      SetError(std::string("WASM descriptor call failed for '") + functionName + "': " + TakeErrorMessage(error));
      return std::nullopt;
    }
    if (trap)
    {
      SetError(std::string("WASM descriptor call trapped for '") + functionName + "': " + TakeTrapMessage(trap));
      return std::nullopt;
    }
    if (results[0].kind != WASMTIME_I32)
    {
      SetError(std::string("WASM descriptor function '") + functionName + "' returned a non-i32 result.");
      return std::nullopt;
    }
    return results[0].of.i32;
  };

  const auto descriptorPtr = callI32Function(descriptorPtrFunction, descriptorPtrName);
  const auto descriptorLen = callI32Function(descriptorLenFunction, descriptorLenName);
  if (!descriptorPtr || !descriptorLen)
    return false;

  if (*descriptorPtr < 0 || *descriptorLen < 0)
  {
    SetError("WASM descriptor pointer and length must be non-negative.");
    return false;
  }

  if (*descriptorLen == 0)
  {
    ApplyPendingGuestParamValues();
    return true;
  }

  wasmtime_extern_t memoryItem{};
  if (!wasmtime_instance_export_get(mContext, &mInstance, "memory", std::strlen("memory"), &memoryItem))
  {
    SetError("WASM descriptor exports require an exported linear memory named 'memory'.");
    return false;
  }
  if (memoryItem.kind != WASMTIME_EXTERN_MEMORY)
  {
    wasmtime_extern_delete(&memoryItem);
    SetError("WASM export 'memory' is not a memory object.");
    return false;
  }

  const size_t memorySize = wasmtime_memory_data_size(mContext, &memoryItem.of.memory);
  std::uint8_t* memoryData = wasmtime_memory_data(mContext, &memoryItem.of.memory);
  const std::size_t descriptorOffset = static_cast<std::size_t>(*descriptorPtr);
  const std::size_t descriptorSize = static_cast<std::size_t>(*descriptorLen);
  if (!memoryData || descriptorOffset + descriptorSize > memorySize)
  {
    wasmtime_extern_delete(&memoryItem);
    SetError("WASM descriptor range falls outside exported guest memory.");
    return false;
  }

  const std::string descriptorBlob(reinterpret_cast<const char*>(memoryData + descriptorOffset), descriptorSize);
  wasmtime_extern_delete(&memoryItem);

  std::string parseError;
  const auto descriptor = ParseDescriptorBlob(descriptorBlob, &parseError);
  if (!descriptor)
  {
    SetError(parseError.empty() ? "Failed to parse WASM descriptor." : parseError);
    return false;
  }

  ApplyGuestDescriptor(*descriptor);
  return true;
}

void WasmEffect::ResetGuestDescriptor()
{
  mGuestDescriptor = {};
  mGuestParamSlots.clear();
  mGuestMacroDefaults.fill(0.0);
  mGuestMacroDefaults[0] = 1.0;
  ResetGuestMacrosToDefaults();
}

void WasmEffect::ApplyGuestDescriptor(const WasmModuleDescriptor& descriptor)
{
  mGuestDescriptor = descriptor;
  mGuestParamSlots.clear();
  mGuestMacroDefaults.fill(0.0);

  for (const auto& guestParam : descriptor.parameters)
  {
    if (guestParam.slot >= mGuestMacroDefaults.size())
      continue;

    mGuestParamSlots[guestParam.definition.id] = guestParam.slot;
    mGuestMacroDefaults[guestParam.slot] = guestParam.definition.defaultValue;
  }

  ResetGuestMacrosToDefaults();
  ApplyPendingGuestParamValues();
}

void WasmEffect::ResetGuestMacrosToDefaults()
{
  mGuestMacros = mGuestMacroDefaults;
}

void WasmEffect::ApplyPendingGuestParamValues()
{
  for (std::size_t slot = 0; slot < kGuestMacroCount; ++slot)
  {
    const std::string genericKey = "param" + std::to_string(slot + 1);
    if (const auto genericIt = mPendingGuestParamValues.find(genericKey); genericIt != mPendingGuestParamValues.end())
      mGuestMacros[slot] = genericIt->second;
  }

  for (const auto& [key, value] : mPendingGuestParamValues)
  {
    if (ParseGuestMacroIndex(key).has_value())
      continue;

    if (const auto slotIt = mGuestParamSlots.find(key); slotIt != mGuestParamSlots.end())
      mGuestMacros[slotIt->second] = value;
  }
}

void WasmEffect::TeardownRuntime()
{
  mPrepareFunction = {};
  mResetFunction = {};
  mProcessFunction = {};
  mLatencyFunction = {};
  mHasInstance = false;
  mHasPrepareFunction = false;
  mHasResetFunction = false;
  mHasProcessFunction = false;
  mHasLatencyFunction = false;
  mRuntimeReady = false;
  mContext = nullptr;

  if (mLinker)
  {
    wasmtime_linker_delete(mLinker);
    mLinker = nullptr;
  }

  if (mStore)
  {
    wasmtime_store_delete(mStore);
    mStore = nullptr;
  }

  if (mModule)
  {
    wasmtime_module_delete(mModule);
    mModule = nullptr;
  }
}

void WasmEffect::SetError(const std::string& message)
{
  mLastError = message;
}

void RegisterWasmEffect()
{
  const auto makeParam = [](std::string id,
                            std::string displayName,
                            double minValue,
                            double maxValue,
                            double defaultValue,
                            std::string unit,
                            double step,
                            std::string group = {},
                            bool advanced = false) {
    ParameterDef param;
    param.id = id;
    param.displayName = displayName;
    param.defaultValue = defaultValue;
    param.minValue = minValue;
    param.maxValue = maxValue;
    param.unit = unit;
    param.step = step;
    param.group = std::move(group);
    param.advanced = advanced;
    return param;
  };

  EffectTypeInfo info;
  info.type = EffectGuids::kWasmHost;
  info.displayName = "Custom Effect";
    info.description = "Runs a stereo WASM effect that can self-describe guest parameters and optional resource slots.";
  info.category = "utility";
  info.requiresResource = true;
  info.requiresTempo = true;
  info.resourceType = "wasm";
  info.aliases = { "wasm_host" };
  info.parameters = {
      makeParam("mix", "Mix", 0.0, 1.0, 1.0, "", 0.01, "Host"),
      makeParam("inputGainDb", "Input Gain", -24.0, 24.0, 0.0, "dB", 0.1, "Host", true),
      makeParam("outputGainDb", "Output Gain", -24.0, 24.0, 0.0, "dB", 0.1, "Host", true),
  };
  info.exposedResources = {
      { "module", "Module", "", "wasm", 0, true, {}, std::nullopt },
  };

  EffectRegistry::Instance().Register(info.type, info, []() {
    return std::make_unique<WasmEffect>();
  });
}

} // namespace guitarfx