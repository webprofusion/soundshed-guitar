#pragma once

// WasmTestGuests.h - The Custom Effect ABI 2 guests the WASM host's tests load: a gain, a stereo
// average, params and resources looked up by id (in either order), tempo, hard-coded and unknown
// handles that must trap or find nothing, resource reads at the edges, a stateful module for
// Reset, and a bare descriptor. Written with WasmModuleWriter.h.

#include "WasmModuleWriter.h"
#include "dsp/effects/WasmEffect.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx::wasm_test
{
// Every fixture is a Custom Effect ABI 2 module laid out the same way: its imports, then prepare,
// reset, process, latency, the descriptor's pointer and length, audiofx_abi_version and an
// optional start function. The descriptor sits at offset 0 of memory, the id strings at kIdOffset.
inline constexpr uint32_t kIdOffset = 4096;

struct GuestSpec
{
    std::vector<FuncTypeDef> importTypes;
    std::vector<ImportFuncDef> imports;
    std::vector<GlobalDef> globals;
    ByteVector descriptor;
    std::vector<std::string> ids;
    ByteVector prepareOps = I32Const(0);
    ByteVector resetOps;
    ByteVector processOps;
    ByteVector startOps;
    std::optional<int32_t> abiVersion = WasmEffect::kAbiVersion;
    uint32_t memoryMinPages = 1;
};

inline ByteVector MakeGuestModule(const GuestSpec& spec)
{
    std::vector<FuncTypeDef> types = spec.importTypes;
    const auto firstType = static_cast<uint32_t>(types.size());
    types.push_back({{WasmValueType::F32, WasmValueType::I32}, {WasmValueType::I32}});
    types.push_back({{}, {}});
    types.push_back({{WasmValueType::F32, WasmValueType::F32}, {WasmValueType::F32, WasmValueType::F32}});
    types.push_back({{}, {WasmValueType::I32}});

    ByteVector processOps = spec.processOps;

    if (processOps.empty())
    {
        AppendOp(processOps, LocalGet(0));
        AppendOp(processOps, LocalGet(1));
    }

    std::vector<DefinedFuncDef> functions = {
        {firstType, spec.prepareOps},
        {firstType + 1, spec.resetOps},
        {firstType + 2, processOps},
        {firstType + 3, I32Const(0)},
        {firstType + 3, I32Const(0)},
        {firstType + 3, I32Const(static_cast<int32_t>(spec.descriptor.size()))},
        {firstType + 3, I32Const(spec.abiVersion.value_or(0))},
    };

    const auto first = static_cast<uint32_t>(spec.imports.size());
    std::vector<std::pair<std::string, uint32_t>> exports = {
        {"audiofx_prepare", first},
        {"audiofx_reset", first + 1},
        {"audiofx_process", first + 2},
        {"audiofx_get_latency_samples", first + 3},
        {"audiofx_descriptor_ptr", first + 4},
        {"audiofx_descriptor_len", first + 5},
    };

    if (spec.abiVersion)
    {
        exports.push_back({"audiofx_abi_version", first + 6});
    }

    std::optional<uint32_t> startFunction;

    if (!spec.startOps.empty())
    {
        functions.push_back({firstType + 1, spec.startOps});
        startFunction = first + 7;
    }

    std::vector<DataSegmentDef> data;

    if (!spec.descriptor.empty())
    {
        data.push_back({0, spec.descriptor});
    }

    ByteVector idBytes;

    for (const auto& id : spec.ids)
    {
        idBytes.insert(idBytes.end(), id.begin(), id.end());
    }

    if (!idBytes.empty())
    {
        data.push_back({kIdOffset, idBytes});
    }

    return MakeModule(types, spec.imports, spec.globals, functions, exports, true, data, spec.memoryMinPages,
                      startFunction);
}

inline constexpr uint32_t kParamHandleImport = 0;
inline constexpr uint32_t kParamValueImport = 1;
inline constexpr uint32_t kTempoImport = 2;

// A module that imports the param readers and keeps one handle per id in globals 0..N-1, looked up
// in prepare as the authoring guide says.
inline GuestSpec ParamGuest(ByteVector descriptor, std::vector<std::string> ids)
{
    GuestSpec spec;
    spec.importTypes = {
        {{WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}},
        {{WasmValueType::I32}, {WasmValueType::F32}},
        {{}, {WasmValueType::F32}},
    };
    spec.imports = {
        {"host", "param_handle", 0},
        {"host", "param_value", 1},
        {"host", "tempo_bpm", 2},
    };
    spec.descriptor = std::move(descriptor);
    spec.ids = std::move(ids);
    spec.prepareOps.clear();

    uint32_t offset = kIdOffset;

    for (uint32_t index = 0; index < spec.ids.size(); ++index)
    {
        const auto length = static_cast<int32_t>(spec.ids[index].size());
        spec.globals.push_back({WasmValueType::I32, true, I32Const(-1)});
        AppendOp(spec.prepareOps, I32Const(static_cast<int32_t>(offset)));
        AppendOp(spec.prepareOps, I32Const(length));
        AppendOp(spec.prepareOps, Call(kParamHandleImport));
        AppendOp(spec.prepareOps, GlobalSet(index));
        offset += static_cast<uint32_t>(length);
    }

    AppendOp(spec.prepareOps, I32Const(0));
    return spec;
}

// param_value(the handle for ids[index]).
inline ByteVector ReadParam(uint32_t index)
{
    ByteVector ops = GlobalGet(index);
    AppendOp(ops, Call(kParamValueImport));
    return ops;
}

// Scales each channel by the 'gain' param.
inline ByteVector MakeGainModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({
                               {"effect.name", "WASM Gain"},
                               {"effect.version", "1.2.3"},
                               {"effect.category", "utility"},
                               {"effect.description", "Simple gain module with self-described guest metadata."},
                               {"effect.thumbnailBase64", "dGVzdA=="},
                               {"param.gain.title", "Gain"},
                               {"param.gain.default", "0.5"},
                               {"param.gain.min", "0.0"},
                               {"param.gain.max", "2.0"},
                               {"param.gain.unit", "amount"},
                           }),
                           {"gain"});

    AppendOp(spec.processOps, LocalGet(0));
    AppendOp(spec.processOps, ReadParam(0));
    AppendU8(spec.processOps, 0x94);
    AppendOp(spec.processOps, LocalGet(1));
    AppendOp(spec.processOps, ReadParam(0));
    AppendU8(spec.processOps, 0x94);
    return MakeGuestModule(spec);
}

// Both outputs are (left + right) / 2 scaled by 'gain'.
inline ByteVector MakeStereoAverageModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({
                               {"effect.name", "Stereo Average"},
                               {"effect.category", "utility"},
                               {"param.gain.title", "Output Gain"},
                               {"param.gain.default", "1.0"},
                               {"param.gain.min", "0.0"},
                               {"param.gain.max", "2.0"},
                           }),
                           {"gain"});

    for (int channel = 0; channel < 2; ++channel)
    {
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        AppendU8(spec.processOps, 0x92);
        AppendOp(spec.processOps, F32Const(0.5f));
        AppendU8(spec.processOps, 0x94);
        AppendOp(spec.processOps, ReadParam(0));
        AppendU8(spec.processOps, 0x94);
    }

    return MakeGuestModule(spec);
}

// Outputs (param 'a', param 'b'), whatever order the descriptor declares them in.
inline ByteVector MakePairModule(bool reversed)
{
    std::vector<std::pair<std::string, std::string>> entries = {
        {"param.a.default", "0.1"},
        {"param.b.default", "0.9"},
    };

    if (reversed)
    {
        std::swap(entries[0], entries[1]);
    }

    auto spec = ParamGuest(BuildDescriptorBlob(entries), {"a", "b"});
    AppendOp(spec.processOps, ReadParam(0));
    AppendOp(spec.processOps, ReadParam(1));
    return MakeGuestModule(spec);
}

// Outputs (param 'last', tempo_bpm()).
inline ByteVector MakeTempoModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({
                               {"effect.name", "Tempo"},
                               {"param.first.default", "0.1"},
                               {"param.last.default", "0.75"},
                           }),
                           {"first", "last"});

    AppendOp(spec.processOps, ReadParam(1));
    AppendOp(spec.processOps, Call(kTempoImport));
    return MakeGuestModule(spec);
}

// Reads params at 0 and 1, the way ABI 1 numbered them, without asking param_handle.
inline ByteVector MakeHardCodedHandleModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({{"param.gain.default", "1.0"}}), {});

    AppendOp(spec.processOps, I32Const(0));
    AppendOp(spec.processOps, Call(kParamValueImport));
    AppendOp(spec.processOps, I32Const(1));
    AppendOp(spec.processOps, Call(kParamValueImport));
    return MakeGuestModule(spec);
}

// Prepare succeeds only if param_handle answers -1 to an undeclared id, an id outside memory, an
// empty id and an over-long one. Process then outputs 0.125 on both channels.
inline ByteVector MakeUnknownIdModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({{"param.gain.default", "1.0"}}), {"missing"});
    spec.globals.clear();
    spec.prepareOps.clear();

    const std::vector<std::pair<int32_t, int32_t>> lookups = {
        {static_cast<int32_t>(kIdOffset), 7},
        {0x7fff0000, 4},
        {static_cast<int32_t>(kIdOffset), 0},
        {static_cast<int32_t>(kIdOffset), static_cast<int32_t>(WasmEffect::kMaxGuestIdLength) + 1},
    };

    for (std::size_t i = 0; i < lookups.size(); ++i)
    {
        AppendOp(spec.prepareOps, I32Const(lookups[i].first));
        AppendOp(spec.prepareOps, I32Const(lookups[i].second));
        AppendOp(spec.prepareOps, Call(kParamHandleImport));
        AppendOp(spec.prepareOps, I32Const(1));
        AppendU8(spec.prepareOps, 0x6a);

        if (i > 0)
        {
            AppendU8(spec.prepareOps, 0x72);
        }
    }

    AppendOp(spec.processOps, F32Const(0.125f));
    AppendOp(spec.processOps, F32Const(0.125f));
    return MakeGuestModule(spec);
}

inline constexpr uint32_t kResourceHandleImport = 0;
inline constexpr uint32_t kResourceSizeImport = 1;
inline constexpr uint32_t kResourceReadImport = 2;
// Where the resource fixtures copy what they read; one page of memory ends at kMemoryEnd.
inline constexpr uint32_t kResourceDataOffset = 8192;
inline constexpr int32_t kMemoryEnd = 65536;

// A module that imports the resource readers and keeps one handle per id in globals 0..N-1, looked
// up in prepare. Its prepare ends with the handle lookups: the caller appends the status.
inline GuestSpec ResourceGuest(ByteVector descriptor, std::vector<std::string> ids)
{
    GuestSpec spec;
    spec.importTypes = {
        {{WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}},
        {{WasmValueType::I32}, {WasmValueType::I32}},
        {{WasmValueType::I32, WasmValueType::I32, WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}},
    };
    spec.imports = {
        {"host", "resource_handle", 0},
        {"host", "resource_size", 1},
        {"host", "resource_read", 2},
    };
    spec.descriptor = std::move(descriptor);
    spec.ids = std::move(ids);
    spec.prepareOps.clear();

    uint32_t offset = kIdOffset;

    for (uint32_t index = 0; index < spec.ids.size(); ++index)
    {
        const auto length = static_cast<int32_t>(spec.ids[index].size());
        spec.globals.push_back({WasmValueType::I32, true, I32Const(-1)});
        AppendOp(spec.prepareOps, I32Const(static_cast<int32_t>(offset)));
        AppendOp(spec.prepareOps, I32Const(length));
        AppendOp(spec.prepareOps, Call(kResourceHandleImport));
        AppendOp(spec.prepareOps, GlobalSet(index));
        offset += static_cast<uint32_t>(length);
    }

    return spec;
}

// resource_read(the handle in global `handleGlobal`, offset, destination, length).
inline ByteVector ReadResource(uint32_t handleGlobal, int32_t offset, int32_t destination, int32_t length)
{
    ByteVector ops = GlobalGet(handleGlobal);
    AppendOp(ops, I32Const(offset));
    AppendOp(ops, I32Const(destination));
    AppendOp(ops, I32Const(length));
    AppendOp(ops, Call(kResourceReadImport));
    return ops;
}

inline ByteVector ResourceSize(uint32_t handleGlobal)
{
    ByteVector ops = GlobalGet(handleGlobal);
    AppendOp(ops, Call(kResourceSizeImport));
    return ops;
}

// i32.load8_u at a fixed address.
inline ByteVector LoadByte(int32_t address)
{
    ByteVector ops = I32Const(address);
    AppendOp(ops, {0x2d, 0x00, 0x00});
    return ops;
}

// Copies the first byte of resources 'a' and 'b' in prepare and outputs (a / 64, b / 64), whatever
// order the descriptor declares them in.
inline ByteVector MakeResourcePairModule(bool reversed)
{
    std::vector<std::pair<std::string, std::string>> entries = {
        {"resource.a.title", "A"},
        {"resource.b.title", "B"},
    };

    if (reversed)
    {
        std::swap(entries[0], entries[1]);
    }

    auto spec = ResourceGuest(BuildDescriptorBlob(entries), {"a", "b"});

    for (uint32_t index = 0; index < 2; ++index)
    {
        AppendOp(spec.prepareOps, ReadResource(index, 0, static_cast<int32_t>(kResourceDataOffset + 16 * index), 16));
        AppendU8(spec.prepareOps, 0x1a);
    }

    AppendOp(spec.prepareOps, I32Const(0));

    for (uint32_t index = 0; index < 2; ++index)
    {
        AppendOp(spec.processOps, LoadByte(static_cast<int32_t>(kResourceDataOffset + 16 * index)));
        AppendU8(spec.processOps, 0xb3);
        AppendOp(spec.processOps, F32Const(64.0f));
        AppendU8(spec.processOps, 0x95);
    }

    return MakeGuestModule(spec);
}

// Prepare checks the resource readers' edges, setting bit i of its status when check i fails, so
// a failure names itself in lastError. 'data' is given the bytes 1, 2, 3, 4; 'empty' no file.
// Process outputs 0.125 on both channels.
inline ByteVector MakeResourceEdgeModule()
{
    auto spec = ResourceGuest(BuildDescriptorBlob({
                                  {"resource.data.title", "Data"},
                                  {"resource.empty.title", "Empty"},
                              }),
                              {"data", "empty"});
    constexpr auto kDst = static_cast<int32_t>(kResourceDataOffset);

    const auto handleOf = [](int32_t pointer, int32_t length) {
        ByteVector ops = I32Const(pointer);
        AppendOp(ops, I32Const(length));
        AppendOp(ops, Call(kResourceHandleImport));
        return ops;
    };

    const std::vector<std::pair<ByteVector, int32_t>> checks = {
        {ResourceSize(0), 4},
        {ReadResource(0, 2, kDst, 100), 2},         // stops at the end of the resource
        {ReadResource(0, 4, kDst + 32, 1), 0},      // offset at the end
        {ReadResource(0, -1, kDst + 32, 1), 0},     // negative offset
        {ReadResource(0, 0, kMemoryEnd - 1, 4), 1}, // stops at the end of guest memory
        {ReadResource(0, 0, kMemoryEnd, 1), 0},     // destination past memory
        {ReadResource(0, 0, kDst + 32, -4), 0},     // negative length
        {ResourceSize(1), 0},                       // declared, no file chosen
        {ReadResource(1, 0, kDst + 32, 4), 0},
        {handleOf(static_cast<int32_t>(kIdOffset), 3), -1}, // "dat": undeclared
        {handleOf(0x7fff0000, 4), -1},                      // id outside memory
        {handleOf(static_cast<int32_t>(kIdOffset), static_cast<int32_t>(WasmEffect::kMaxGuestIdLength) + 1), -1},
        {LoadByte(kDst), 3}, // what the second check copied
        {LoadByte(kDst + 1), 4},
        {LoadByte(kMemoryEnd - 1), 1},
    };

    AppendOp(spec.prepareOps, I32Const(0));

    for (std::size_t i = 0; i < checks.size(); ++i)
    {
        AppendOp(spec.prepareOps, checks[i].first);
        AppendOp(spec.prepareOps, I32Const(checks[i].second));
        AppendU8(spec.prepareOps, 0x47);
        AppendOp(spec.prepareOps, I32Const(1 << i));
        AppendU8(spec.prepareOps, 0x6c);
        AppendU8(spec.prepareOps, 0x72);
    }

    AppendOp(spec.processOps, F32Const(0.125f));
    AppendOp(spec.processOps, F32Const(0.125f));
    return MakeGuestModule(spec);
}

// Asks resource_size about handles 0 and 1 instead of asking resource_handle.
inline ByteVector MakeHardCodedResourceHandleModule()
{
    auto spec = ResourceGuest(BuildDescriptorBlob({{"resource.data.title", "Data"}}), {});
    AppendOp(spec.prepareOps, I32Const(0));

    for (int32_t handle = 0; handle < 2; ++handle)
    {
        AppendOp(spec.processOps, I32Const(handle));
        AppendOp(spec.processOps, Call(kResourceSizeImport));
        AppendU8(spec.processOps, 0xb3);
    }

    return MakeGuestModule(spec);
}

inline ByteVector MakeStatefulBiasModule()
{
    GuestSpec spec;
    spec.globals = {{WasmValueType::F32, true, F32Const(0.0f)}};

    AppendOp(spec.resetOps, F32Const(0.0f));
    AppendOp(spec.resetOps, GlobalSet(0));

    AppendOp(spec.processOps, GlobalGet(0));
    AppendOp(spec.processOps, F32Const(0.01f));
    AppendU8(spec.processOps, 0x92);
    AppendOp(spec.processOps, GlobalSet(0));

    for (uint32_t channel = 0; channel < 2; ++channel)
    {
        AppendOp(spec.processOps, LocalGet(channel));
        AppendOp(spec.processOps, GlobalGet(0));
        AppendU8(spec.processOps, 0x92);
    }

    return MakeGuestModule(spec);
}

inline ByteVector MakeDescriptorModule(const ByteVector& descriptorBlob)
{
    GuestSpec spec;
    spec.descriptor = descriptorBlob;
    return MakeGuestModule(spec);
}
} // namespace guitarfx::wasm_test
