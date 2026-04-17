#include "dsp/effects/WasmEffect.h"

#include "util/FileIO.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

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
  mGuestMacros[0] = 1.0;
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
    const float dryLeft = inputLeft ? inputLeft[sample] : 0.0f;
    const float dryRight = inputRight ? inputRight[sample] : 0.0f;

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
    mGuestMacros[*macroIndex] = value;
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

  if (!LoadFunctionExport("guitarfx_prepare", mPrepareFunction, true, mHasPrepareFunction)
      || !LoadFunctionExport("guitarfx_reset", mResetFunction, true, mHasResetFunction)
      || !LoadFunctionExport("guitarfx_process", mProcessFunction, true, mHasProcessFunction)
      || !LoadFunctionExport("guitarfx_get_latency_samples", mLatencyFunction, false, mHasLatencyFunction))
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
                            double step) {
    ParameterDef param;
    param.id = id;
    param.displayName = displayName;
    param.defaultValue = defaultValue;
    param.minValue = minValue;
    param.maxValue = maxValue;
    param.unit = unit;
    param.step = step;
    return param;
  };

  EffectTypeInfo info;
  info.type = EffectGuids::kWasmHost;
  info.displayName = "WASM Host";
  info.description = "Runs a generic stereo WASM effect with fixed params and resource slots.";
  info.category = "utility";
  info.requiresResource = true;
  info.requiresTempo = true;
  info.resourceType = "wasm";
  info.aliases = { "wasm_host" };
  info.parameters = {
      makeParam("mix", "Mix", 0.0, 1.0, 1.0, "", 0.01),
      makeParam("inputGainDb", "Input Gain", -24.0, 24.0, 0.0, "dB", 0.1),
      makeParam("outputGainDb", "Output Gain", -24.0, 24.0, 0.0, "dB", 0.1),
      makeParam("param1", "Param 1", -4.0, 4.0, 1.0, "", 0.01),
      makeParam("param2", "Param 2", -4.0, 4.0, 0.0, "", 0.01),
      makeParam("param3", "Param 3", -4.0, 4.0, 0.0, "", 0.01),
      makeParam("param4", "Param 4", -4.0, 4.0, 0.0, "", 0.01),
      makeParam("param5", "Param 5", -4.0, 4.0, 0.0, "", 0.01),
      makeParam("param6", "Param 6", -4.0, 4.0, 0.0, "", 0.01),
      makeParam("param7", "Param 7", -4.0, 4.0, 0.0, "", 0.01),
      makeParam("param8", "Param 8", -4.0, 4.0, 0.0, "", 0.01),
  };
  info.exposedResources = {
      { "module", "Module", "", "wasm", 0, true, {}, std::nullopt },
      { "data_a", "Data A", "", "blob", 1, true, {}, std::nullopt },
      { "data_b", "Data B", "", "blob", 2, true, {}, std::nullopt },
      { "data_c", "Data C", "", "blob", 3, true, {}, std::nullopt },
  };

  EffectRegistry::Instance().Register(info.type, info, []() {
    return std::make_unique<WasmEffect>();
  });
}

} // namespace guitarfx