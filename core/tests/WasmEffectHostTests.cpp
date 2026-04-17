#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/WasmEffect.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx {
namespace {

using ByteVector = std::vector<uint8_t>;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 8;
constexpr float kTolerance = 1.0e-4f;

enum class WasmValueType : uint8_t {
  I32 = 0x7f,
  F32 = 0x7d,
};

struct FuncTypeDef {
  std::vector<WasmValueType> params;
  std::vector<WasmValueType> results;
};

struct ImportFuncDef {
  std::string moduleName;
  std::string fieldName;
  uint32_t typeIndex = 0;
};

struct GlobalDef {
  WasmValueType valueType = WasmValueType::F32;
  bool isMutable = false;
  ByteVector initExpr;
};

struct DefinedFuncDef {
  uint32_t typeIndex = 0;
  ByteVector ops;
};

struct DataSegmentDef {
  uint32_t offset = 0;
  ByteVector bytes;
};

struct TempDir {
  TempDir()
  {
    root = std::filesystem::temp_directory_path() / "soundshed_guitar_wasm_host_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }

  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  std::filesystem::path root;
};

void AppendU8(ByteVector& out, uint8_t value)
{
  out.push_back(value);
}

void AppendBytes(ByteVector& out, const ByteVector& bytes)
{
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendU32Leb(ByteVector& out, uint32_t value)
{
  do
  {
    uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
    value >>= 7u;
    if (value != 0)
      byte |= 0x80u;
    out.push_back(byte);
  } while (value != 0);
}

void AppendI32Leb(ByteVector& out, int32_t value)
{
  bool more = true;
  while (more)
  {
    uint8_t byte = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    const bool signBitSet = (byte & 0x40u) != 0;
    more = !((value == 0 && !signBitSet) || (value == -1 && signBitSet));
    if (more)
      byte |= 0x80u;
    out.push_back(byte);
  }
}

void AppendF32(ByteVector& out, float value)
{
  std::array<uint8_t, sizeof(float)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(float));
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendString(ByteVector& out, const std::string& value)
{
  AppendU32Leb(out, static_cast<uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

ByteVector MakeSection(uint8_t sectionId, const ByteVector& payload)
{
  ByteVector section;
  AppendU8(section, sectionId);
  AppendU32Leb(section, static_cast<uint32_t>(payload.size()));
  AppendBytes(section, payload);
  return section;
}

ByteVector LocalGet(uint32_t index)
{
  ByteVector ops{ 0x20 };
  AppendU32Leb(ops, index);
  return ops;
}

ByteVector GlobalGet(uint32_t index)
{
  ByteVector ops{ 0x23 };
  AppendU32Leb(ops, index);
  return ops;
}

ByteVector GlobalSet(uint32_t index)
{
  ByteVector ops{ 0x24 };
  AppendU32Leb(ops, index);
  return ops;
}

ByteVector Call(uint32_t index)
{
  ByteVector ops{ 0x10 };
  AppendU32Leb(ops, index);
  return ops;
}

ByteVector I32Const(int32_t value)
{
  ByteVector ops{ 0x41 };
  AppendI32Leb(ops, value);
  return ops;
}

ByteVector F32Const(float value)
{
  ByteVector ops{ 0x43 };
  AppendF32(ops, value);
  return ops;
}

void AppendOp(ByteVector& target, const ByteVector& op)
{
  target.insert(target.end(), op.begin(), op.end());
}

ByteVector MakeModule(const std::vector<FuncTypeDef>& types,
                      const std::vector<ImportFuncDef>& imports,
                      const std::vector<GlobalDef>& globals,
                      const std::vector<DefinedFuncDef>& definedFunctions,
                      const std::vector<std::pair<std::string, uint32_t>>& exports,
                      bool exportMemory = false,
                      const std::vector<DataSegmentDef>& dataSegments = {})
{
  ByteVector module{ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };

  ByteVector typePayload;
  AppendU32Leb(typePayload, static_cast<uint32_t>(types.size()));
  for (const auto& type : types)
  {
    AppendU8(typePayload, 0x60);
    AppendU32Leb(typePayload, static_cast<uint32_t>(type.params.size()));
    for (const auto valueType : type.params)
      AppendU8(typePayload, static_cast<uint8_t>(valueType));
    AppendU32Leb(typePayload, static_cast<uint32_t>(type.results.size()));
    for (const auto valueType : type.results)
      AppendU8(typePayload, static_cast<uint8_t>(valueType));
  }
  AppendBytes(module, MakeSection(1, typePayload));

  if (!imports.empty())
  {
    ByteVector importPayload;
    AppendU32Leb(importPayload, static_cast<uint32_t>(imports.size()));
    for (const auto& import : imports)
    {
      AppendString(importPayload, import.moduleName);
      AppendString(importPayload, import.fieldName);
      AppendU8(importPayload, 0x00);
      AppendU32Leb(importPayload, import.typeIndex);
    }
    AppendBytes(module, MakeSection(2, importPayload));
  }

  if (!definedFunctions.empty())
  {
    ByteVector functionPayload;
    AppendU32Leb(functionPayload, static_cast<uint32_t>(definedFunctions.size()));
    for (const auto& function : definedFunctions)
      AppendU32Leb(functionPayload, function.typeIndex);
    AppendBytes(module, MakeSection(3, functionPayload));
  }

  if (exportMemory || !dataSegments.empty())
  {
    ByteVector memoryPayload;
    AppendU32Leb(memoryPayload, 1);
    AppendU8(memoryPayload, 0x00);
    AppendU32Leb(memoryPayload, 1);
    AppendBytes(module, MakeSection(5, memoryPayload));
  }

  if (!globals.empty())
  {
    ByteVector globalPayload;
    AppendU32Leb(globalPayload, static_cast<uint32_t>(globals.size()));
    for (const auto& global : globals)
    {
      AppendU8(globalPayload, static_cast<uint8_t>(global.valueType));
      AppendU8(globalPayload, global.isMutable ? 0x01 : 0x00);
      AppendBytes(globalPayload, global.initExpr);
      AppendU8(globalPayload, 0x0b);
    }
    AppendBytes(module, MakeSection(6, globalPayload));
  }

  ByteVector exportPayload;
  AppendU32Leb(exportPayload, static_cast<uint32_t>(exports.size() + (exportMemory ? 1 : 0)));
  for (const auto& [name, functionIndex] : exports)
  {
    AppendString(exportPayload, name);
    AppendU8(exportPayload, 0x00);
    AppendU32Leb(exportPayload, functionIndex);
  }
  if (exportMemory)
  {
    AppendString(exportPayload, "memory");
    AppendU8(exportPayload, 0x02);
    AppendU32Leb(exportPayload, 0);
  }
  AppendBytes(module, MakeSection(7, exportPayload));

  ByteVector codePayload;
  AppendU32Leb(codePayload, static_cast<uint32_t>(definedFunctions.size()));
  for (const auto& function : definedFunctions)
  {
    ByteVector body;
    AppendU32Leb(body, 0);
    AppendBytes(body, function.ops);
    AppendU8(body, 0x0b);
    AppendU32Leb(codePayload, static_cast<uint32_t>(body.size()));
    AppendBytes(codePayload, body);
  }
  AppendBytes(module, MakeSection(10, codePayload));

  if (!dataSegments.empty())
  {
    ByteVector dataPayload;
    AppendU32Leb(dataPayload, static_cast<uint32_t>(dataSegments.size()));
    for (const auto& segment : dataSegments)
    {
      AppendU8(dataPayload, 0x00);
      AppendOp(dataPayload, I32Const(static_cast<int32_t>(segment.offset)));
      AppendU8(dataPayload, 0x0b);
      AppendU32Leb(dataPayload, static_cast<uint32_t>(segment.bytes.size()));
      AppendBytes(dataPayload, segment.bytes);
    }
    AppendBytes(module, MakeSection(11, dataPayload));
  }

  return module;
}

ByteVector BuildDescriptorBlob(std::initializer_list<std::pair<std::string, std::string>> entries)
{
  ByteVector blob;
  for (const auto& [key, value] : entries)
  {
    blob.insert(blob.end(), key.begin(), key.end());
    blob.push_back('=');
    blob.insert(blob.end(), value.begin(), value.end());
    blob.push_back('\n');
  }
  return blob;
}

ByteVector MakeGainModule()
{
  const std::vector<FuncTypeDef> types = {
      { { WasmValueType::I32 }, { WasmValueType::F32 } },
      { { WasmValueType::F32, WasmValueType::I32, WasmValueType::I32 }, { WasmValueType::I32 } },
      { {}, {} },
      { { WasmValueType::F32, WasmValueType::F32 }, { WasmValueType::F32, WasmValueType::F32 } },
      { {}, { WasmValueType::I32 } },
  };

  const std::vector<ImportFuncDef> imports = {
      { "host", "read_param", 0 },
  };

  const ByteVector descriptorBlob = BuildDescriptorBlob({
      { "effect.name", "WASM Gain" },
      { "effect.category", "utility" },
      { "effect.description", "Simple gain module with self-described guest metadata." },
      { "effect.thumbnailBase64", "dGVzdA==" },
      { "param.0.id", "gain" },
      { "param.0.title", "Gain" },
      { "param.0.slot", "0" },
      { "param.0.default", "0.5" },
      { "param.0.min", "0.0" },
      { "param.0.max", "2.0" },
      { "param.0.unit", "amount" },
  });

  ByteVector prepareOps;
  AppendOp(prepareOps, I32Const(0));

  ByteVector processOps;
  AppendOp(processOps, LocalGet(0));
  AppendOp(processOps, I32Const(0));
  AppendOp(processOps, Call(0));
  AppendU8(processOps, 0x94);
  AppendOp(processOps, LocalGet(1));
  AppendOp(processOps, I32Const(0));
  AppendOp(processOps, Call(0));
  AppendU8(processOps, 0x94);

  ByteVector latencyOps;
  AppendOp(latencyOps, I32Const(0));

    ByteVector descriptorPtrOps;
    AppendOp(descriptorPtrOps, I32Const(0));

    ByteVector descriptorLenOps;
    AppendOp(descriptorLenOps, I32Const(static_cast<int32_t>(descriptorBlob.size())));

  const std::vector<DefinedFuncDef> functions = {
      { 1, prepareOps },
      { 2, {} },
      { 3, processOps },
      { 4, latencyOps },
      { 4, descriptorPtrOps },
      { 4, descriptorLenOps },
  };

  return MakeModule(types, imports, {}, functions,
                    {
                        { "audiofx_prepare", 1 },
                        { "audiofx_reset", 2 },
                        { "audiofx_process", 3 },
                        { "audiofx_get_latency_samples", 4 },
              { "audiofx_descriptor_ptr", 5 },
              { "audiofx_descriptor_len", 6 },
            },
            true,
            { { 0, descriptorBlob } });
}

ByteVector MakeStereoAverageModule()
{
  const std::vector<FuncTypeDef> types = {
      { { WasmValueType::I32 }, { WasmValueType::F32 } },
      { { WasmValueType::F32, WasmValueType::I32, WasmValueType::I32 }, { WasmValueType::I32 } },
      { {}, {} },
      { { WasmValueType::F32, WasmValueType::F32 }, { WasmValueType::F32, WasmValueType::F32 } },
      { {}, { WasmValueType::I32 } },
  };

  const std::vector<ImportFuncDef> imports = {
      { "host", "read_param", 0 },
  };

  const ByteVector descriptorBlob = BuildDescriptorBlob({
      { "effect.name", "Stereo Average" },
      { "effect.category", "utility" },
      { "param.0.id", "gain" },
      { "param.0.title", "Output Gain" },
      { "param.0.slot", "0" },
      { "param.0.default", "1.0" },
      { "param.0.min", "0.0" },
      { "param.0.max", "2.0" },
      { "param.0.unit", "amount" },
  });

  ByteVector prepareOps;
  AppendOp(prepareOps, I32Const(0));

  ByteVector processOps;
  AppendOp(processOps, LocalGet(0));
  AppendOp(processOps, LocalGet(1));
  AppendU8(processOps, 0x92);
  AppendOp(processOps, F32Const(0.5f));
  AppendU8(processOps, 0x94);
  AppendOp(processOps, I32Const(0));
  AppendOp(processOps, Call(0));
  AppendU8(processOps, 0x94);

  AppendOp(processOps, LocalGet(1));
  AppendOp(processOps, LocalGet(0));
  AppendU8(processOps, 0x92);
  AppendOp(processOps, F32Const(0.5f));
  AppendU8(processOps, 0x94);
  AppendOp(processOps, I32Const(0));
  AppendOp(processOps, Call(0));
  AppendU8(processOps, 0x94);

  ByteVector latencyOps;
  AppendOp(latencyOps, I32Const(0));

    ByteVector descriptorPtrOps;
    AppendOp(descriptorPtrOps, I32Const(0));

    ByteVector descriptorLenOps;
    AppendOp(descriptorLenOps, I32Const(static_cast<int32_t>(descriptorBlob.size())));

  const std::vector<DefinedFuncDef> functions = {
      { 1, prepareOps },
      { 2, {} },
      { 3, processOps },
      { 4, latencyOps },
      { 4, descriptorPtrOps },
      { 4, descriptorLenOps },
  };

  return MakeModule(types, imports, {}, functions,
                    {
                        { "guitarfx_prepare", 1 },
                        { "guitarfx_reset", 2 },
                        { "guitarfx_process", 3 },
                        { "guitarfx_get_latency_samples", 4 },
              { "guitarfx_descriptor_ptr", 5 },
              { "guitarfx_descriptor_len", 6 },
            },
            true,
            { { 0, descriptorBlob } });
}

ByteVector MakeResourceScalerModule()
{
  const std::vector<FuncTypeDef> types = {
      { { WasmValueType::I32, WasmValueType::I32 }, { WasmValueType::I32 } },
      { { WasmValueType::F32, WasmValueType::I32, WasmValueType::I32 }, { WasmValueType::I32 } },
      { {}, {} },
      { { WasmValueType::F32, WasmValueType::F32 }, { WasmValueType::F32, WasmValueType::F32 } },
      { {}, { WasmValueType::I32 } },
  };

  const std::vector<ImportFuncDef> imports = {
      { "host", "read_resource_byte", 0 },
  };

  ByteVector prepareOps;
  AppendOp(prepareOps, I32Const(0));

  ByteVector processOps;
  AppendOp(processOps, LocalGet(0));
  AppendOp(processOps, I32Const(1));
  AppendOp(processOps, I32Const(0));
  AppendOp(processOps, Call(0));
  AppendU8(processOps, 0xb2);
  AppendOp(processOps, F32Const(64.0f));
  AppendU8(processOps, 0x95);
  AppendU8(processOps, 0x94);

  AppendOp(processOps, LocalGet(1));
  AppendOp(processOps, I32Const(1));
  AppendOp(processOps, I32Const(0));
  AppendOp(processOps, Call(0));
  AppendU8(processOps, 0xb2);
  AppendOp(processOps, F32Const(64.0f));
  AppendU8(processOps, 0x95);
  AppendU8(processOps, 0x94);

  ByteVector latencyOps;
  AppendOp(latencyOps, I32Const(0));

  const std::vector<DefinedFuncDef> functions = {
      { 1, prepareOps },
      { 2, {} },
      { 3, processOps },
      { 4, latencyOps },
  };

  return MakeModule(types, imports, {}, functions,
                    {
              { "audiofx_prepare", 1 },
              { "audiofx_reset", 2 },
              { "audiofx_process", 3 },
              { "audiofx_get_latency_samples", 4 },
                    });
}

ByteVector MakeStatefulBiasModule()
{
  const std::vector<FuncTypeDef> types = {
      { { WasmValueType::F32, WasmValueType::I32, WasmValueType::I32 }, { WasmValueType::I32 } },
      { {}, {} },
      { { WasmValueType::F32, WasmValueType::F32 }, { WasmValueType::F32, WasmValueType::F32 } },
      { {}, { WasmValueType::I32 } },
  };

  ByteVector prepareOps;
  AppendOp(prepareOps, I32Const(0));

  ByteVector resetOps;
  AppendOp(resetOps, F32Const(0.0f));
  AppendOp(resetOps, GlobalSet(0));

  ByteVector processOps;
  AppendOp(processOps, GlobalGet(0));
  AppendOp(processOps, F32Const(0.01f));
  AppendU8(processOps, 0x92);
  AppendOp(processOps, GlobalSet(0));

  AppendOp(processOps, LocalGet(0));
  AppendOp(processOps, GlobalGet(0));
  AppendU8(processOps, 0x92);

  AppendOp(processOps, LocalGet(1));
  AppendOp(processOps, GlobalGet(0));
  AppendU8(processOps, 0x92);

  ByteVector latencyOps;
  AppendOp(latencyOps, I32Const(0));

  const std::vector<GlobalDef> globals = {
      { WasmValueType::F32, true, F32Const(0.0f) },
  };

  const std::vector<DefinedFuncDef> functions = {
      { 0, prepareOps },
      { 1, resetOps },
      { 2, processOps },
      { 3, latencyOps },
  };

  return MakeModule(types, {}, globals, functions,
                    {
              { "audiofx_prepare", 0 },
              { "audiofx_reset", 1 },
              { "audiofx_process", 2 },
              { "audiofx_get_latency_samples", 3 },
                    });
}

bool WriteBytes(const std::filesystem::path& path, const ByteVector& bytes)
{
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open())
    return false;
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return stream.good();
}

bool NearlyEqual(float actual, float expected, float tolerance = kTolerance)
{
  return std::fabs(actual - expected) <= tolerance;
}

std::unique_ptr<EffectProcessor> CreateWasmHost()
{
  return EffectRegistry::Instance().Create(EffectGuids::kWasmHost);
}

bool TestRegistryMetadata()
{
  const auto info = EffectRegistry::Instance().GetTypeInfo(EffectGuids::kWasmHost);
  if (!info)
  {
    std::cerr << "WASM host effect was not registered.\n";
    return false;
  }

  if (!info->requiresResource || !info->requiresTempo || info->exposedResources.size() != 1 || info->parameters.size() != 3)
  {
    std::cerr << "WASM host metadata is missing required resource or slot information.\n";
    return false;
  }

  return true;
}

bool TestDescriptorInspection(const TempDir& tempDir)
{
  const auto modulePath = tempDir.root / "gain_descriptor.wasm";
  if (!WriteBytes(modulePath, MakeGainModule()))
  {
    std::cerr << "Failed to write descriptor-enabled gain module.\n";
    return false;
  }

  std::string error;
  const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);
  if (!descriptor)
  {
    std::cerr << "Failed to inspect WASM descriptor: " << error << "\n";
    return false;
  }

  if (descriptor->displayName != "WASM Gain"
      || descriptor->thumbnailDataUrl != "data:image/png;base64,dGVzdA=="
      || descriptor->parameters.size() != 1
      || descriptor->parameters.front().definition.id != "gain")
  {
    std::cerr << "Descriptor inspection returned unexpected metadata.\n";
    return false;
  }

  return true;
}

bool TestDirectGainModule(const TempDir& tempDir)
{
  const auto modulePath = tempDir.root / "gain.wasm";
  if (!WriteBytes(modulePath, MakeGainModule()))
  {
    std::cerr << "Failed to write gain WASM module.\n";
    return false;
  }

  auto effect = CreateWasmHost();
  if (!effect)
  {
    std::cerr << "Failed to create WASM effect instance.\n";
    return false;
  }

  ResourceRef moduleRef;
  moduleRef.resourceType = "wasm";
  moduleRef.filePath = modulePath.string();

  if (!effect->LoadResources({ moduleRef }, { modulePath }))
  {
    std::cerr << "Gain module failed to load.\n";
    return false;
  }

  effect->SetParam("gain", 0.5);
  effect->Prepare(kSampleRate, kBlockSize);

  std::array<float, kBlockSize> leftIn{};
  std::array<float, kBlockSize> rightIn{};
  std::array<float, kBlockSize> leftOut{};
  std::array<float, kBlockSize> rightOut{};
  leftIn[0] = 1.0f;
  rightIn[0] = -0.5f;

  float* inputs[] = { leftIn.data(), rightIn.data() };
  float* outputs[] = { leftOut.data(), rightOut.data() };
  effect->Process(inputs, outputs, kBlockSize);

  if (!NearlyEqual(leftOut[0], 0.5f) || !NearlyEqual(rightOut[0], -0.25f))
  {
    std::cerr << "Gain module output mismatch.\n";
    return false;
  }

  return true;
}

bool TestMonoInputToStereoGuestUsesDualMono(const TempDir& tempDir)
{
  const auto modulePath = tempDir.root / "stereo_average_mono_input.wasm";
  if (!WriteBytes(modulePath, MakeStereoAverageModule()))
  {
    std::cerr << "Failed to write stereo-average WASM module for mono-input test.\n";
    return false;
  }

  auto effect = CreateWasmHost();
  if (!effect)
  {
    std::cerr << "Failed to create WASM effect instance.\n";
    return false;
  }

  ResourceRef moduleRef;
  moduleRef.resourceType = "wasm";
  moduleRef.filePath = modulePath.string();

  if (!effect->LoadResources({ moduleRef }, { modulePath }))
  {
    std::cerr << "Stereo-average module failed to load for mono-input test.\n";
    return false;
  }

  effect->SetParam("gain", 1.0);
  effect->Prepare(kSampleRate, kBlockSize);

  std::array<float, kBlockSize> leftIn{};
  std::array<float, kBlockSize> leftOut{};
  std::array<float, kBlockSize> rightOut{};
  leftIn[0] = 1.0f;

  float* inputs[] = { leftIn.data(), nullptr };
  float* outputs[] = { leftOut.data(), rightOut.data() };
  effect->Process(inputs, outputs, kBlockSize);

  if (!NearlyEqual(leftOut[0], 1.0f) || !NearlyEqual(rightOut[0], 1.0f))
  {
    std::cerr << "Mono input was not mirrored to dual-mono for stereo WASM guest processing.\n";
    return false;
  }

  return true;
}

bool TestSignalGraphStereoModule(const TempDir& tempDir)
{
  const auto modulePath = tempDir.root / "stereo_average.wasm";
  if (!WriteBytes(modulePath, MakeStereoAverageModule()))
  {
    std::cerr << "Failed to write stereo-average WASM module.\n";
    return false;
  }

  SignalGraph graph;
  graph.nodes.push_back(GraphNode{ "__input__", kNodeTypeInput, "utility", "Input", true });

  GraphNode wasmNode;
  wasmNode.id = "wasm";
  wasmNode.type = EffectGuids::kWasmHost;
  wasmNode.category = "utility";
  wasmNode.label = "WASM Host";
  wasmNode.params["gain"] = 1.0;
  wasmNode.resources.resize(1);
  wasmNode.resources[0].resourceType = "wasm";
  wasmNode.resources[0].filePath = modulePath.string();
  graph.nodes.push_back(wasmNode);

  graph.nodes.push_back(GraphNode{ "__output__", kNodeTypeOutput, "utility", "Output", true });

  graph.edges.push_back(GraphEdge{ "__input__", "wasm", 0, 0, 1.0 });
  graph.edges.push_back(GraphEdge{ "wasm", "__output__", 0, 0, 1.0 });

  ResourceLibrary resourceLibrary;
  SignalGraphExecutor executor;
  executor.SetResourceLibrary(&resourceLibrary);
  executor.SetGraph(graph);
  executor.Prepare(kSampleRate, kBlockSize);

  std::array<float, kBlockSize> leftIn{};
  std::array<float, kBlockSize> rightIn{};
  std::array<float, kBlockSize> leftOut{};
  std::array<float, kBlockSize> rightOut{};
  std::fill(leftIn.begin(), leftIn.end(), 1.0f);
  std::fill(rightIn.begin(), rightIn.end(), 0.0f);

  float* inputs[] = { leftIn.data(), rightIn.data() };
  float* outputs[] = { leftOut.data(), rightOut.data() };
  executor.Process(inputs, outputs, kBlockSize);

  if (!NearlyEqual(leftOut[0], 0.5f) || !NearlyEqual(rightOut[0], 0.5f))
  {
    std::cerr << "Signal graph stereo WASM node output mismatch.\n";
    return false;
  }

  return true;
}

bool TestResourceBackedModule(const TempDir& tempDir)
{
  const auto modulePath = tempDir.root / "resource_scaler.wasm";
  const auto dataPath = tempDir.root / "scale.bin";
  if (!WriteBytes(modulePath, MakeResourceScalerModule()) || !WriteBytes(dataPath, { 32u }))
  {
    std::cerr << "Failed to write resource-scaler test files.\n";
    return false;
  }

  auto effect = CreateWasmHost();
  if (!effect)
  {
    std::cerr << "Failed to create WASM effect instance.\n";
    return false;
  }

  ResourceRef moduleRef;
  moduleRef.resourceType = "wasm";
  moduleRef.filePath = modulePath.string();
  moduleRef.metadata["resourceSlotIndex"] = "0";

  ResourceRef dataRef;
  dataRef.resourceType = "blob";
  dataRef.filePath = dataPath.string();
  dataRef.metadata["resourceSlotIndex"] = "1";

  if (!effect->LoadResources({ moduleRef, dataRef }, { modulePath, dataPath }))
  {
    std::cerr << "Resource-backed module failed to load.\n";
    return false;
  }

  effect->Prepare(kSampleRate, kBlockSize);

  std::array<float, kBlockSize> leftIn{};
  std::array<float, kBlockSize> rightIn{};
  std::array<float, kBlockSize> leftOut{};
  std::array<float, kBlockSize> rightOut{};
  leftIn[0] = 1.0f;
  rightIn[0] = 1.0f;

  float* inputs[] = { leftIn.data(), rightIn.data() };
  float* outputs[] = { leftOut.data(), rightOut.data() };
  effect->Process(inputs, outputs, kBlockSize);

  if (!NearlyEqual(leftOut[0], 0.5f) || !NearlyEqual(rightOut[0], 0.5f))
  {
    std::cerr << "Resource-backed module output mismatch.\n";
    return false;
  }

  return true;
}

bool TestResetStatefulModule(const TempDir& tempDir)
{
  const auto modulePath = tempDir.root / "stateful_bias.wasm";
  if (!WriteBytes(modulePath, MakeStatefulBiasModule()))
  {
    std::cerr << "Failed to write stateful-bias WASM module.\n";
    return false;
  }

  auto effect = CreateWasmHost();
  if (!effect)
  {
    std::cerr << "Failed to create WASM effect instance.\n";
    return false;
  }

  ResourceRef moduleRef;
  moduleRef.resourceType = "wasm";
  moduleRef.filePath = modulePath.string();

  if (!effect->LoadResources({ moduleRef }, { modulePath }))
  {
    std::cerr << "Stateful module failed to load.\n";
    return false;
  }

  effect->Prepare(kSampleRate, kBlockSize);

  std::array<float, kBlockSize> leftIn{};
  std::array<float, kBlockSize> rightIn{};
  std::array<float, kBlockSize> leftOut{};
  std::array<float, kBlockSize> rightOut{};

  float* inputs[] = { leftIn.data(), rightIn.data() };
  float* outputs[] = { leftOut.data(), rightOut.data() };

  effect->Process(inputs, outputs, kBlockSize);
  const float firstRunValue = leftOut[0];

  effect->Process(inputs, outputs, kBlockSize);
  const float secondRunValue = leftOut[0];

  effect->Reset();
  effect->Process(inputs, outputs, kBlockSize);
  const float resetRunValue = leftOut[0];

  if (!(secondRunValue > firstRunValue) || !NearlyEqual(resetRunValue, firstRunValue))
  {
    std::cerr << "Stateful module reset behavior mismatch.\n";
    return false;
  }

  return true;
}

} // namespace

} // namespace guitarfx

int main()
{
  guitarfx::RegisterAllEffects();

  guitarfx::TempDir tempDir;
  bool allPassed = true;
  allPassed = guitarfx::TestRegistryMetadata() && allPassed;
  allPassed = guitarfx::TestDescriptorInspection(tempDir) && allPassed;
  allPassed = guitarfx::TestDirectGainModule(tempDir) && allPassed;
  allPassed = guitarfx::TestMonoInputToStereoGuestUsesDualMono(tempDir) && allPassed;
  allPassed = guitarfx::TestSignalGraphStereoModule(tempDir) && allPassed;
  allPassed = guitarfx::TestResourceBackedModule(tempDir) && allPassed;
  allPassed = guitarfx::TestResetStatefulModule(tempDir) && allPassed;

  if (!allPassed)
  {
    std::cerr << "WasmEffectHostTests failed.\n";
    return 1;
  }

  std::cout << "WasmEffectHostTests passed.\n";
  return 0;
}