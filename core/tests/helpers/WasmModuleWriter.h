#pragma once

// WasmModuleWriter.h - Writes WebAssembly binaries by hand for the WASM host's tests: LEB128 and
// float encodings, sections, a few opcodes, a whole module from its types, imports, globals,
// functions and data, and a Custom Effect descriptor blob. WasmTestGuests.h builds the test
// guests with it; WasmEffectHostTests.cpp runs them.

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx::wasm_test
{
using ByteVector = std::vector<uint8_t>;

enum class WasmValueType : uint8_t
{
    I32 = 0x7f,
    F32 = 0x7d,
};

struct FuncTypeDef
{
    std::vector<WasmValueType> params;
    std::vector<WasmValueType> results;
};

struct ImportFuncDef
{
    std::string moduleName;
    std::string fieldName;
    uint32_t typeIndex = 0;
};

struct GlobalDef
{
    WasmValueType valueType = WasmValueType::F32;
    bool isMutable = false;
    ByteVector initExpr;
};

struct DefinedFuncDef
{
    uint32_t typeIndex = 0;
    ByteVector ops;
};

struct DataSegmentDef
{
    uint32_t offset = 0;
    ByteVector bytes;
};

inline void AppendU8(ByteVector& out, uint8_t value)
{
    out.push_back(value);
}

inline void AppendBytes(ByteVector& out, const ByteVector& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void AppendU32Leb(ByteVector& out, uint32_t value)
{
    do
    {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
        value >>= 7u;

        if (value != 0)
        {
            byte |= 0x80u;
        }

        out.push_back(byte);
    } while (value != 0);
}

inline void AppendI32Leb(ByteVector& out, int32_t value)
{
    bool more = true;

    while (more)
    {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        const bool signBitSet = (byte & 0x40u) != 0;
        more = !((value == 0 && !signBitSet) || (value == -1 && signBitSet));

        if (more)
        {
            byte |= 0x80u;
        }

        out.push_back(byte);
    }
}

inline void AppendF32(ByteVector& out, float value)
{
    std::array<uint8_t, sizeof(float)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(float));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void AppendString(ByteVector& out, const std::string& value)
{
    AppendU32Leb(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

inline ByteVector MakeSection(uint8_t sectionId, const ByteVector& payload)
{
    ByteVector section;
    AppendU8(section, sectionId);
    AppendU32Leb(section, static_cast<uint32_t>(payload.size()));
    AppendBytes(section, payload);
    return section;
}

inline ByteVector LocalGet(uint32_t index)
{
    ByteVector ops{0x20};
    AppendU32Leb(ops, index);
    return ops;
}

inline ByteVector GlobalGet(uint32_t index)
{
    ByteVector ops{0x23};
    AppendU32Leb(ops, index);
    return ops;
}

inline ByteVector GlobalSet(uint32_t index)
{
    ByteVector ops{0x24};
    AppendU32Leb(ops, index);
    return ops;
}

inline ByteVector Call(uint32_t index)
{
    ByteVector ops{0x10};
    AppendU32Leb(ops, index);
    return ops;
}

inline ByteVector I32Const(int32_t value)
{
    ByteVector ops{0x41};
    AppendI32Leb(ops, value);
    return ops;
}

inline ByteVector F32Const(float value)
{
    ByteVector ops{0x43};
    AppendF32(ops, value);
    return ops;
}

inline void AppendOp(ByteVector& target, const ByteVector& op)
{
    target.insert(target.end(), op.begin(), op.end());
}

inline ByteVector MakeModule(const std::vector<FuncTypeDef>& types, const std::vector<ImportFuncDef>& imports,
                      const std::vector<GlobalDef>& globals, const std::vector<DefinedFuncDef>& definedFunctions,
                      const std::vector<std::pair<std::string, uint32_t>>& exports, bool exportMemory = false,
                      const std::vector<DataSegmentDef>& dataSegments = {}, uint32_t memoryMinPages = 1,
                      std::optional<uint32_t> startFunction = std::nullopt)
{
    ByteVector module{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

    ByteVector typePayload;
    AppendU32Leb(typePayload, static_cast<uint32_t>(types.size()));

    for (const auto& type : types)
    {
        AppendU8(typePayload, 0x60);
        AppendU32Leb(typePayload, static_cast<uint32_t>(type.params.size()));

        for (const auto valueType : type.params)
        {
            AppendU8(typePayload, static_cast<uint8_t>(valueType));
        }

        AppendU32Leb(typePayload, static_cast<uint32_t>(type.results.size()));

        for (const auto valueType : type.results)
        {
            AppendU8(typePayload, static_cast<uint8_t>(valueType));
        }
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
        {
            AppendU32Leb(functionPayload, function.typeIndex);
        }

        AppendBytes(module, MakeSection(3, functionPayload));
    }

    if (exportMemory || !dataSegments.empty())
    {
        ByteVector memoryPayload;
        AppendU32Leb(memoryPayload, 1);
        AppendU8(memoryPayload, 0x00);
        AppendU32Leb(memoryPayload, memoryMinPages);
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

    if (startFunction)
    {
        ByteVector startPayload;
        AppendU32Leb(startPayload, *startFunction);
        AppendBytes(module, MakeSection(8, startPayload));
    }

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

inline ByteVector BuildDescriptorBlob(const std::vector<std::pair<std::string, std::string>>& entries)
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
} // namespace guitarfx::wasm_test
