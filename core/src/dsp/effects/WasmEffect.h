#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <wasmtime.h>

namespace guitarfx {

struct WasmMetadataEntry {
  std::string key;
  std::string value;
};

struct WasmGuestParameterDescriptor {
  std::size_t slot = 0;
  ParameterDef definition;
};

struct WasmModuleDescriptor {
  std::vector<WasmMetadataEntry> entries;
  std::string displayName;
  std::string version;
  std::string description;
  std::string category = "utility";
  std::string thumbnailDataUrl;
  std::vector<WasmGuestParameterDescriptor> parameters;
  std::vector<ExposedResource> exposedResources;
};

class WasmEffect final : public EffectProcessor {
public:
  static constexpr std::size_t kGuestMacroCount = 8;
  static constexpr const char* kDescriptorConfigKey = "wasmGuestDescriptor";

  WasmEffect();
  ~WasmEffect() override;

  [[nodiscard]] static std::optional<WasmModuleDescriptor> InspectModuleFile(const std::filesystem::path& modulePath,
                                                                             std::string* error = nullptr);
  [[nodiscard]] static std::optional<WasmModuleDescriptor> ParseDescriptorConfig(const std::string& configJson,
                                                                                  std::string* error = nullptr);
  [[nodiscard]] static std::string SerializeDescriptorConfig(const std::vector<WasmMetadataEntry>& entries);

  void Prepare(double sampleRate, int maxBlockSize) override;
  void Reset() override;
  void Process(float** inputs, float** outputs, int numSamples) override;

  void SetParam(const std::string& key, double value) override;
  double GetParam(const std::string& key) const override;

  void SetConfig(const std::string& key, const std::string& value) override;
  std::string GetConfig(const std::string& key) const override;

  bool LoadResources(const std::vector<ResourceRef>& refs, const std::vector<std::filesystem::path>& paths) override;
  bool RequiresResource() const override { return true; }
  bool HasResource() const override { return !mModuleBytes.empty(); }
  std::filesystem::path GetResourcePath() const override { return mModulePath; }
  int GetLatencySamples() const override { return mLatencySamples; }

  std::string GetType() const override { return "wasm_host"; }
  std::string GetCategory() const override { return "utility"; }

  float ReadGuestParam(int index) const;
  int ReadGuestResourceSize(int slot) const;
  int ReadGuestResourceByte(int slot, int offset) const;

private:
  bool LoadGuestDescriptor();
  bool RebuildRuntime();
  bool BuildRuntimeOnly();
  bool LoadFunctionExport(const char* exportName, wasmtime_func_t& outFunction, bool required, bool& found);
  bool InvokePrepare();
  bool InvokeProcess(float inputLeft, float inputRight, float& outputLeft, float& outputRight);
  int QueryLatencySamples();
  void ResetGuestDescriptor();
  void ApplyGuestDescriptor(const WasmModuleDescriptor& descriptor);
  void ResetGuestMacrosToDefaults();
  void ApplyPendingGuestParamValues();
  void TeardownRuntime();
  void SetError(const std::string& message);

  std::array<double, kGuestMacroCount> mGuestMacros{};
  std::array<double, kGuestMacroCount> mGuestMacroDefaults{};
  double mMix = 1.0;
  double mInputGainDb = 0.0;
  double mOutputGainDb = 0.0;
  double mBpm = 120.0;
  std::unordered_map<std::string, std::size_t> mGuestParamSlots;
  std::unordered_map<std::string, double> mPendingGuestParamValues;
  WasmModuleDescriptor mGuestDescriptor;

  std::vector<uint8_t> mModuleBytes;
  std::vector<std::vector<uint8_t>> mResourceBytesBySlot;
  std::filesystem::path mModulePath;

  wasm_engine_t* mEngine = nullptr;
  wasmtime_store_t* mStore = nullptr;
  wasmtime_context_t* mContext = nullptr;
  wasmtime_linker_t* mLinker = nullptr;
  wasmtime_module_t* mModule = nullptr;
  wasmtime_instance_t mInstance{};
  wasmtime_func_t mPrepareFunction{};
  wasmtime_func_t mResetFunction{};
  wasmtime_func_t mProcessFunction{};
  wasmtime_func_t mLatencyFunction{};
  bool mHasInstance = false;
  bool mHasPrepareFunction = false;
  bool mHasResetFunction = false;
  bool mHasProcessFunction = false;
  bool mHasLatencyFunction = false;

  int mLatencySamples = 0;
  bool mRuntimeReady = false;
  std::string mLastError;
};

void RegisterWasmEffect();

} // namespace guitarfx