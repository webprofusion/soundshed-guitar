#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/WasmDescriptor.h"
#include "dsp/effects/WasmEffect.h"
#include "resources/ResourceLibrary.h"

#include "WasmModuleWriter.h"
#include "WasmTestGuests.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace guitarfx
{
namespace
{
using namespace wasm_test;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 8;
constexpr float kTolerance = 1.0e-4f;

struct TempDir
{
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

bool WriteBytes(const std::filesystem::path& path, const ByteVector& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);

    if (!stream.is_open())
    {
        return false;
    }

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

std::unique_ptr<EffectProcessor> LoadModuleFile(const std::filesystem::path& modulePath, bool& loaded)
{
    auto effect = CreateWasmHost();
    loaded = false;

    if (!effect)
    {
        return nullptr;
    }

    ResourceRef moduleRef;
    moduleRef.resourceType = "wasm";
    moduleRef.filePath = modulePath.string();
    loaded = effect->LoadResources({moduleRef}, {modulePath});
    return effect;
}

// Writes the module, loads it into a fresh effect and prepares it; null if any step fails.
std::unique_ptr<EffectProcessor> LoadPrepared(const TempDir& tempDir, const char* fileName, const ByteVector& module)
{
    const auto modulePath = tempDir.root / fileName;

    if (!WriteBytes(modulePath, module))
    {
        std::cerr << "Failed to write " << fileName << ".\n";
        return nullptr;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (!effect || !loaded)
    {
        std::cerr << fileName << " failed to load: " << (effect ? effect->GetConfig("lastError") : std::string{})
                  << "\n";
        return nullptr;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return effect;
}

// One block of the given constant input; returns the first output sample of each channel.
std::pair<float, float> ProcessBlock(EffectProcessor& effect, float left, float right)
{
    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    leftIn.fill(left);
    rightIn.fill(right);
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    effect.Process(inputs, outputs, kBlockSize);
    return {leftOut[0], rightOut[0]};
}

bool TestRegistryMetadata()
{
    const auto info = EffectRegistry::Instance().GetTypeInfo(EffectGuids::kWasmHost);

    if (!info)
    {
        std::cerr << "WASM host effect was not registered.\n";
        return false;
    }

    if (!info->requiresResource || !info->requiresTempo || info->exposedResources.size() != 1 ||
        info->parameters.size() != 3)
    {
        std::cerr << "WASM host metadata is missing required resource or slot information.\n";
        return false;
    }

    for (const auto& param : info->parameters)
    {
        if (!WasmEffect::IsHostParamId(param.id))
        {
            std::cerr << "Host param '" << param.id << "' is missing from WasmEffect::kHostParamIds.\n";
            return false;
        }
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

    if (descriptor->displayName != "WASM Gain" || descriptor->version != "1.2.3" ||
        descriptor->thumbnailDataUrl != "data:image/png;base64,dGVzdA==" || descriptor->parameters.size() != 1)
    {
        std::cerr << "Descriptor inspection returned unexpected metadata.\n";
        return false;
    }

    const auto& gain = descriptor->parameters.front().definition;

    if (gain.id != "gain" || gain.displayName != "Gain" || gain.defaultValue != 0.5 || gain.maxValue != 2.0 ||
        gain.unit != "amount")
    {
        std::cerr << "The 'gain' param's fields were not read from param.gain.*.\n";
        return false;
    }

    return true;
}

bool TestDirectGainModule(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "gain.wasm", MakeGainModule());

    if (!effect)
    {
        return false;
    }

    // The descriptor's default first, then a value set after the load.
    if (const auto [left, right] = ProcessBlock(*effect, 1.0f, -0.5f);
        !NearlyEqual(left, 0.5f) || !NearlyEqual(right, -0.25f))
    {
        std::cerr << "Gain module did not read its default through its handle: " << left << ", " << right << "\n";
        return false;
    }

    effect->SetParam("gain", 2.0);

    if (const auto [left, right] = ProcessBlock(*effect, 1.0f, -0.5f);
        !NearlyEqual(left, 2.0f) || !NearlyEqual(right, -1.0f))
    {
        std::cerr << "Gain module did not follow SetParam: " << left << ", " << right << "\n";
        return false;
    }

    // A rebuild keeps the value.
    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 1.0f, -0.5f); !NearlyEqual(left, 2.0f))
    {
        std::cerr << "Gain lost its value across Prepare: " << left << "\n";
        return false;
    }

    // An id the module didn't declare is not kept once a descriptor has loaded.
    effect->SetParam("notDeclared", 1.0);

    if (effect->GetParam("notDeclared") != 0.0 || effect->GetParam("gain") != 2.0)
    {
        std::cerr << "SetParam kept a value for an id the module never declared.\n";
        return false;
    }

    return true;
}

// The tempo has its own import; it never shares a numbering with params again.
bool TestTempoImport(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "tempo.wasm", MakeTempoModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.75f) || !NearlyEqual(right, 1.2f))
    {
        std::cerr << "Param default or tempo read back wrong: " << left << ", " << right << "\n";
        return false;
    }

    effect->SetParam("last", 0.25);
    effect->SetParam("bpm", 90.0);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, 0.9f) || effect->GetParam("last") != 0.25)
    {
        std::cerr << "Param or tempo did not follow SetParam: " << left << ", " << right << "\n";
        return false;
    }

    return true;
}

// A new version of a module that declares the same ids in another order reads the same values:
// nothing depends on where a param sits in the descriptor.
bool TestReorderedDescriptorKeepsValuesById(const TempDir& tempDir)
{
    const auto firstPath = tempDir.root / "pair.wasm";
    const auto reorderedPath = tempDir.root / "pair_reordered.wasm";

    if (!WriteBytes(firstPath, MakePairModule(false)) || !WriteBytes(reorderedPath, MakePairModule(true)))
    {
        std::cerr << "Failed to write the pair modules.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(firstPath, loaded);

    if (!effect || !loaded)
    {
        std::cerr << "The pair module failed to load.\n";
        return false;
    }

    effect->SetParam("a", 0.2);
    effect->SetParam("b", 0.7);

    ResourceRef moduleRef;
    moduleRef.resourceType = "wasm";
    moduleRef.filePath = reorderedPath.string();

    if (!effect->LoadResources({moduleRef}, {reorderedPath}))
    {
        std::cerr << "The reordered pair module failed to load.\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.2f) || !NearlyEqual(right, 0.7f))
    {
        std::cerr << "Reordering the descriptor moved values between params: " << left << ", " << right << "\n";
        return false;
    }

    return true;
}

// A module that reads 0 and 1 instead of asking param_handle traps, and the node passes dry.
bool TestHardCodedHandleTraps(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "hard_coded.wasm", MakeHardCodedHandleModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.25f, -0.5f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f))
    {
        std::cerr << "A hard-coded handle did not leave the node passing dry: " << left << ", " << right << "\n";
        return false;
    }

    if (effect->GetConfig("lastError").find("param_handle") == std::string::npos)
    {
        std::cerr << "A hard-coded handle's trap was not reported: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

bool TestUnknownIdsHaveNoHandle(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "unknown_id.wasm", MakeUnknownIdModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.125f) || !NearlyEqual(right, 0.125f))
    {
        std::cerr << "param_handle gave a handle for an undeclared, out-of-bounds, empty or over-long id: "
                  << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

// Each must be refused before it runs, with a message that says why.
bool TestOtherInterfacesAreRefused(const TempDir& tempDir)
{
    GuestSpec abi1;
    abi1.importTypes = {{{WasmValueType::I32}, {WasmValueType::F32}}};
    abi1.imports = {{"host", "read_param", 0}};
    abi1.abiVersion = std::nullopt;

    GuestSpec unversioned;
    unversioned.abiVersion = std::nullopt;

    GuestSpec futureAbi;
    futureAbi.abiVersion = 3;

    GuestSpec abi1Resources;
    abi1Resources.importTypes = {{{WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}}};
    abi1Resources.imports = {{"host", "read_resource_byte", 0}};

    GuestSpec foreignImport;
    foreignImport.importTypes = {{{WasmValueType::F32}, {WasmValueType::F32}}};
    foreignImport.imports = {{"env", "sinf", 0}};

    const std::vector<std::tuple<const char*, GuestSpec, const char*>> cases = {
        {"abi1.wasm", abi1, "host.read_param"},
        {"abi1_resources.wasm", abi1Resources, "ABI 1 (it imports host.read_resource_byte)"},
        {"unversioned.wasm", unversioned, "audiofx_abi_version"},
        {"future_abi.wasm", futureAbi, "ABI 3"},
        {"foreign_import.wasm", foreignImport, "env.sinf"},
    };

    for (const auto& [fileName, spec, expected] : cases)
    {
        const auto modulePath = tempDir.root / fileName;

        if (!WriteBytes(modulePath, MakeGuestModule(spec)))
        {
            std::cerr << "Failed to write " << fileName << ".\n";
            return false;
        }

        bool loaded = false;
        auto effect = LoadModuleFile(modulePath, loaded);
        const std::string error = effect ? effect->GetConfig("lastError") : std::string{};

        if (loaded || error.find(expected) == std::string::npos)
        {
            std::cerr << fileName << " was not refused with a message naming '" << expected << "': " << error << "\n";
            return false;
        }
    }

    return true;
}

bool TestBadDescriptorsAreRefused(const TempDir& tempDir)
{
    std::vector<std::pair<std::string, std::string>> tooMany;

    for (std::size_t i = 0; i <= WasmEffect::kMaxGuestParams; ++i)
    {
        tooMany.push_back({"param.p" + std::to_string(i) + ".default", "0"});
    }

    std::vector<std::pair<std::string, std::string>> tooManyResources;

    for (std::size_t i = 0; i <= WasmEffect::kMaxGuestResources; ++i)
    {
        tooManyResources.push_back({"resource.r" + std::to_string(i) + ".type", "blob"});
    }

    const std::vector<std::pair<std::vector<std::pair<std::string, std::string>>, const char*>> cases = {
        {{{"param.0.id", "gain"}}, "numbers its param"},
        {{{"param.gain.slot", "0"}}, "no 'slot'"},
        {{{"param.gain.id", "gain"}}, "no 'id'"},
        {{{"param.mix.title", "Guest Mix"}}, "host's own controls"},
        {{{"param.bpm.title", "Guest Tempo"}}, "host's own controls"},
        {{{"param.9lives.title", "Lives"}}, "must be a letter"},
        {{{"param.gain", "1"}}, "names no field"},
        {tooMany, "the limit is 64"},
        {{{"resource.0.id", "impulse"}}, "numbers its resource"},
        {{{"resource.ir.slot", "1"}}, "no 'slot'"},
        {{{"resource.ir.id", "ir"}}, "no 'id'"},
        {{{"resource.module.title", "Another Module"}}, "module's own resource"},
        {{{"resource._ir.title", "Impulse"}}, "must be a letter"},
        {{{"resource.ir", "x"}}, "names no field"},
        {tooManyResources, "the limit is 16"},
    };

    for (const auto& [entries, expected] : cases)
    {
        const auto modulePath = tempDir.root / "bad_descriptor.wasm";

        if (!WriteBytes(modulePath, MakeDescriptorModule(BuildDescriptorBlob(entries))))
        {
            std::cerr << "Failed to write a bad-descriptor module.\n";
            return false;
        }

        std::string error;

        if (WasmEffect::InspectModuleFile(modulePath, &error) || error.find(expected) == std::string::npos)
        {
            std::cerr << "A descriptor starting '" << entries.front().first << "' was not refused with '" << expected
                      << "': " << error << "\n";
            return false;
        }
    }

    return true;
}

bool TestMonoInputToStereoGuestUsesDualMono(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "stereo_average_mono_input.wasm", MakeStereoAverageModule());

    if (!effect)
    {
        return false;
    }

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    leftIn[0] = 1.0f;

    float* inputs[] = {leftIn.data(), nullptr};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    effect->Process(inputs, outputs, kBlockSize);

    if (!NearlyEqual(leftOut[0], 1.0f) || !NearlyEqual(rightOut[0], 1.0f))
    {
        std::cerr << "Mono input was not mirrored to dual-mono for stereo WASM guest processing.\n";
        return false;
    }

    return true;
}

// The node's params reach the module through the executor, by id.
bool TestSignalGraphStereoModule(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "stereo_average.wasm";

    if (!WriteBytes(modulePath, MakeStereoAverageModule()))
    {
        std::cerr << "Failed to write stereo-average WASM module.\n";
        return false;
    }

    SignalGraph graph;
    graph.nodes.push_back(GraphNode{"__input__", kNodeTypeInput, "utility", "Input", true});

    GraphNode wasmNode;
    wasmNode.id = "wasm";
    wasmNode.type = EffectGuids::kWasmHost;
    wasmNode.category = "utility";
    wasmNode.label = "WASM Host";
    wasmNode.params["gain"] = 0.5;
    wasmNode.resources.resize(1);
    wasmNode.resources[0].resourceType = "wasm";
    wasmNode.resources[0].filePath = modulePath.string();
    graph.nodes.push_back(wasmNode);

    graph.nodes.push_back(GraphNode{"__output__", kNodeTypeOutput, "utility", "Output", true});

    graph.edges.push_back(GraphEdge{"__input__", "wasm", 0, 0, 1.0});
    graph.edges.push_back(GraphEdge{"wasm", "__output__", 0, 0, 1.0});

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

    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    executor.Process(inputs, outputs, kBlockSize);

    if (!NearlyEqual(leftOut[0], 0.25f) || !NearlyEqual(rightOut[0], 0.25f))
    {
        std::cerr << "Signal graph stereo WASM node output mismatch: " << leftOut[0] << ", " << rightOut[0] << "\n";
        return false;
    }

    return true;
}

ResourceRef FileRef(const std::filesystem::path& path, const char* type, std::size_t slot)
{
    ResourceRef ref;
    ref.resourceType = type;
    ref.filePath = path.string();
    ref.metadata["resourceSlotIndex"] = std::to_string(slot);
    return ref;
}

// The pair modules and two one-byte files: 'a' holds 16 (0.25 out) and 'b' 48 (0.75 out).
struct ResourcePairFiles
{
    std::filesystem::path module;
    std::filesystem::path reversedModule;
    std::filesystem::path a;
    std::filesystem::path b;
};

std::optional<ResourcePairFiles> WriteResourcePairFiles(const TempDir& tempDir)
{
    ResourcePairFiles files{tempDir.root / "resource_pair.wasm", tempDir.root / "resource_pair_reversed.wasm",
                            tempDir.root / "a.bin", tempDir.root / "b.bin"};

    if (!WriteBytes(files.module, MakeResourcePairModule(false)) ||
        !WriteBytes(files.reversedModule, MakeResourcePairModule(true)) || !WriteBytes(files.a, {16u}) ||
        !WriteBytes(files.b, {48u}))
    {
        std::cerr << "Failed to write the resource-pair test files.\n";
        return std::nullopt;
    }

    return files;
}

// Loads `module` with `first` and `second` at node resources 1 and 2, after `resourceIds` if any.
std::optional<std::pair<float, float>> RunResourcePair(const std::filesystem::path& module,
                                                       const std::filesystem::path& first,
                                                       const std::filesystem::path& second,
                                                       const std::string& resourceIds = {})
{
    auto effect = CreateWasmHost();

    if (!resourceIds.empty())
    {
        effect->SetConfig(WasmEffect::kResourceIdsConfigKey, resourceIds);
    }

    if (!effect->LoadResources({FileRef(module, "wasm", 0), FileRef(first, "blob", 1), FileRef(second, "blob", 2)},
                               {module, first, second}))
    {
        std::cerr << "The resource-pair module failed to load: " << effect->GetConfig("lastError") << "\n";
        return std::nullopt;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return ProcessBlock(*effect, 0.0f, 0.0f);
}

bool TestResourcesById(const TempDir& tempDir)
{
    const auto files = WriteResourcePairFiles(tempDir);

    if (!files)
    {
        return false;
    }

    // With no ids saved, each file is the resource declared at its place.
    if (const auto out = RunResourcePair(files->module, files->a, files->b);
        !out || !NearlyEqual(out->first, 0.25f) || !NearlyEqual(out->second, 0.75f))
    {
        std::cerr << "Resources read back wrong by declaration order.\n";
        return false;
    }

    // Saved ids win over places: here the module declares b first, and the files are where a
    // module declaring a first put them.
    if (const auto out = RunResourcePair(files->reversedModule, files->a, files->b, R"(["","a","b"])");
        !out || !NearlyEqual(out->first, 0.25f) || !NearlyEqual(out->second, 0.75f))
    {
        std::cerr << "Saved resource ids did not decide which file a resource reads.\n";
        return false;
    }

    // An id the module doesn't declare takes no resource, and a bad one is ignored.
    if (const auto out = RunResourcePair(files->module, files->a, files->b, R"(["","gone","9bad"])");
        !out || !NearlyEqual(out->first, 0.0f) || !NearlyEqual(out->second, 0.75f))
    {
        std::cerr << "An undeclared or malformed saved resource id still took a file.\n";
        return false;
    }

    return true;
}

// The executor sets the node's config before it loads its resources, so saved ids reach the
// binding.
bool TestSignalGraphResourcesById(const TempDir& tempDir)
{
    const auto files = WriteResourcePairFiles(tempDir);

    if (!files)
    {
        return false;
    }

    SignalGraph graph;
    graph.nodes.push_back(GraphNode{"__input__", kNodeTypeInput, "utility", "Input", true});

    GraphNode wasmNode;
    wasmNode.id = "wasm";
    wasmNode.type = EffectGuids::kWasmHost;
    wasmNode.category = "utility";
    wasmNode.params["mix"] = 1.0;
    wasmNode.config[WasmEffect::kResourceIdsConfigKey] = R"(["","b","a"])";
    wasmNode.resources = {FileRef(files->module, "wasm", 0), FileRef(files->b, "blob", 1),
                          FileRef(files->a, "blob", 2)};

    // The executor stamps slots itself.
    for (auto& resource : wasmNode.resources)
    {
        resource.metadata.clear();
    }

    graph.nodes.push_back(wasmNode);
    graph.nodes.push_back(GraphNode{"__output__", kNodeTypeOutput, "utility", "Output", true});
    graph.edges.push_back(GraphEdge{"__input__", "wasm", 0, 0, 1.0});
    graph.edges.push_back(GraphEdge{"wasm", "__output__", 0, 0, 1.0});

    ResourceLibrary resourceLibrary;
    SignalGraphExecutor executor;
    executor.SetResourceLibrary(&resourceLibrary);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlockSize);

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    executor.Process(inputs, outputs, kBlockSize);

    if (!NearlyEqual(leftOut[0], 0.25f) || !NearlyEqual(rightOut[0], 0.75f))
    {
        std::cerr << "The executor did not bind resources by their saved ids: " << leftOut[0] << ", " << rightOut[0]
                  << "\n";
        return false;
    }

    return true;
}

WasmModuleDescriptor DescriptorWithResources(std::vector<std::string> ids)
{
    WasmModuleDescriptor descriptor;

    for (auto& id : ids)
    {
        ExposedResource resource;
        resource.resourceId = std::move(id);
        descriptor.exposedResources.push_back(std::move(resource));
    }

    return descriptor;
}

ResourceRef NamedFile(const char* name)
{
    ResourceRef ref;
    ref.resourceType = "blob";
    ref.filePath = name;
    return ref;
}

// What the controller does to a node when a new version of its module is chosen or a preset loads.
bool TestRekeyNodeResources()
{
    GraphNode node;
    node.resources = {NamedFile("module.wasm"), NamedFile("a.bin"), NamedFile("b.bin")};

    // Chosen against a module declaring a, b; its new version declares b, a and then c.
    auto dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"b", "a", "c"}),
                                                  DescriptorWithResources({"a", "b"}));

    if (!dropped.empty() || node.resources.size() != 3 || node.resources[0].filePath != "module.wasm" ||
        node.resources[1].filePath != "b.bin" || node.resources[2].filePath != "a.bin" ||
        node.config[WasmEffect::kResourceIdsConfigKey] != R"(["","b","a"])")
    {
        std::cerr << "Reordered resources did not keep their files by id.\n";
        return false;
    }

    // Saved ids now say which file is whose, whatever `placedBy` claims. The next version drops a.
    dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"b"}), DescriptorWithResources({"x", "y"}));

    if (dropped != std::vector<std::string>{"a"} || node.resources.size() != 2 ||
        node.resources[1].filePath != "b.bin" || node.config[WasmEffect::kResourceIdsConfigKey] != R"(["","b"])")
    {
        std::cerr << "A resource the module no longer declares was not dropped, or b moved.\n";
        return false;
    }

    // A gap stays empty, the trailing empty place goes, and with no guest files the ids go too.
    node.resources = {NamedFile("module.wasm"), ResourceRef{}, NamedFile("b.bin")};
    node.config.erase(WasmEffect::kResourceIdsConfigKey);
    dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"a", "b", "c"}),
                                             DescriptorWithResources({"a", "b", "c"}));

    if (!dropped.empty() || node.resources.size() != 3 || node.resources[1].IsValid() ||
        node.resources[2].filePath != "b.bin")
    {
        std::cerr << "A gap between chosen files did not stay where it was.\n";
        return false;
    }

    node.resources.resize(1);
    dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"a"}), DescriptorWithResources({"a"}));

    if (!dropped.empty() || node.resources.size() != 1 || node.config.contains(WasmEffect::kResourceIdsConfigKey))
    {
        std::cerr << "A node with no guest files kept its saved resource ids.\n";
        return false;
    }

    return true;
}

bool TestResourceReadEdges(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "resource_edges.wasm";
    const auto dataPath = tempDir.root / "edges.bin";

    if (!WriteBytes(modulePath, MakeResourceEdgeModule()) || !WriteBytes(dataPath, {1u, 2u, 3u, 4u}))
    {
        std::cerr << "Failed to write the resource-edge test files.\n";
        return false;
    }

    auto effect = CreateWasmHost();

    if (!effect->LoadResources({FileRef(modulePath, "wasm", 0), FileRef(dataPath, "blob", 1)}, {modulePath, dataPath}))
    {
        std::cerr << "The resource-edge module failed to load: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.125f) || !NearlyEqual(right, 0.125f))
    {
        std::cerr << "A resource reader edge check failed (bit i of the status is check i): "
                  << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

// Prepare is where resources are read, so a bad handle there fails the load itself.
bool TestHardCodedResourceHandleTraps(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "hard_coded_resource.wasm";

    if (!WriteBytes(modulePath, MakeHardCodedResourceHandleModule()))
    {
        std::cerr << "Failed to write the hard-coded resource module.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (!effect || loaded)
    {
        std::cerr << "A hard-coded resource handle in prepare did not fail the load.\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 0.25f, -0.5f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f) ||
        effect->GetConfig("lastError").find("resource_handle") == std::string::npos)
    {
        std::cerr << "A hard-coded resource handle did not trap: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

bool TestResetStatefulModule(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "stateful_bias.wasm", MakeStatefulBiasModule());

    if (!effect)
    {
        return false;
    }

    const float firstRunValue = ProcessBlock(*effect, 0.0f, 0.0f).first;
    const float secondRunValue = ProcessBlock(*effect, 0.0f, 0.0f).first;
    effect->Reset();
    const float resetRunValue = ProcessBlock(*effect, 0.0f, 0.0f).first;

    if (!(secondRunValue > firstRunValue) || !NearlyEqual(resetRunValue, firstRunValue))
    {
        std::cerr << "Stateful module reset behavior mismatch.\n";
        return false;
    }

    return true;
}

// A host block longer than the guest was prepared for goes through in guest-sized pieces, and the
// host may hand the same buffers in as input and output: every sample must still come back from
// its own place, on its own channel.
bool TestLongInPlaceBlock(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "gain_long_block.wasm", MakeGainModule());

    if (!effect)
    {
        return false;
    }

    constexpr int kFrames = kBlockSize * 2 + 5;
    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};

    for (int i = 0; i < kFrames; ++i)
    {
        left[i] = 0.01f * static_cast<float>(i + 1);
        right[i] = -0.02f * static_cast<float>(i + 1);
    }

    float* buffers[] = {left.data(), right.data()};
    effect->Process(buffers, buffers, kFrames);

    // The gain module's default gain is 0.5.
    for (int i = 0; i < kFrames; ++i)
    {
        if (!NearlyEqual(left[i], 0.005f * static_cast<float>(i + 1)) ||
            !NearlyEqual(right[i], -0.01f * static_cast<float>(i + 1)))
        {
            std::cerr << "A long in-place block came back wrong at frame " << i << ": " << left[i] << ", " << right[i]
                      << "\n";
            return false;
        }
    }

    return true;
}

// The per-sample audiofx_process ABI 2 first had, and buffers that don't fit the module's memory,
// are refused before audio runs through them, with a message that says what is wrong.
bool TestBadProcessShapesAreRefused(const TempDir& tempDir)
{
    GuestSpec perSample;
    perSample.perSampleProcess = true;
    const auto perSamplePath = tempDir.root / "per_sample_process.wasm";

    if (!WriteBytes(perSamplePath, MakeGuestModule(perSample)))
    {
        std::cerr << "Failed to write the per-sample module.\n";
        return false;
    }

    bool loaded = false;
    auto perSampleEffect = LoadModuleFile(perSamplePath, loaded);
    const std::string perSampleError = perSampleEffect ? perSampleEffect->GetConfig("lastError") : std::string{};

    if (loaded || perSampleError.find("must take (i32)") == std::string::npos ||
        perSampleError.find("audiofx_buffer_ptr") == std::string::npos)
    {
        std::cerr << "A per-sample audiofx_process was not refused by its signature: " << perSampleError << "\n";
        return false;
    }

    // One page of memory: 65536 bytes, and an 8-frame block needs 64 of them.
    for (const int32_t pointer : {-4, 2, 65536 - 32, 65536})
    {
        GuestSpec spec;
        spec.bufferPtr = pointer;
        const std::string fileName = "bad_buffer_" + std::to_string(pointer + 4) + ".wasm";
        const auto modulePath = tempDir.root / fileName;

        if (!WriteBytes(modulePath, MakeGuestModule(spec)))
        {
            std::cerr << "Failed to write " << fileName << ".\n";
            return false;
        }

        auto effect = LoadModuleFile(modulePath, loaded);

        if (!effect)
        {
            return false;
        }

        effect->Prepare(kSampleRate, kBlockSize);
        const std::string error = effect->GetConfig("lastError");
        const auto [left, right] = ProcessBlock(*effect, 0.25f, -0.5f);

        if (error.find("audiofx_buffer_ptr returned " + std::to_string(pointer)) == std::string::npos ||
            !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f))
        {
            std::cerr << "A buffer at " << pointer << " was not refused, or the node did not pass dry: " << error
                      << "\n";
            return false;
        }
    }

    return true;
}

// Modules that misbehave on purpose. A module is code from whoever shared it, so each of these
// must end in a trap or a refused load, never a hang or a crash.
enum class HostileKind
{
    LoopingProcess,
    RecursiveProcess,
    LoopingStart,
    HugeMemory,
    SlowProcess,
    GrowingProcess,
    LookupInProcess,
    HugeOutput,
};

// Iterations of an empty loop per sample in SlowProcess: well over real time for an 8-frame
// block, well under the 30 ms block deadline.
constexpr int32_t kSlowProcessIterations = 1000000;

ByteVector MakeHostileModule(HostileKind kind)
{
    // loop (br 0) end; unreachable — the unreachable makes the body valid for any result type.
    const ByteVector foreverOps = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x00};

    GuestSpec spec;

    switch (kind)
    {
    case HostileKind::LoopingProcess:
        spec.processOps = foreverOps;
        break;
    case HostileKind::RecursiveProcess:
        // Process is function 2 in a module with no imports.
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        AppendOp(spec.processOps, Call(2));
        break;
    case HostileKind::LoopingStart:
        spec.startOps = foreverOps;
        break;
    case HostileKind::HugeMemory:
        // One page past the store's limit.
        spec.memoryMinPages = static_cast<uint32_t>(WasmEffect::kMaxGuestMemoryBytes / 65536) + 1;
        break;
    case HostileKind::SlowProcess:
        // i32 local 2 counts down from kSlowProcessIterations; then the input goes out unchanged.
        spec.processLocals = {WasmValueType::I32};
        AppendOp(spec.processOps, I32Const(kSlowProcessIterations));
        AppendOp(spec.processOps, LocalSet(2));
        AppendOp(spec.processOps, {0x03, 0x40}); // loop
        AppendOp(spec.processOps, LocalGet(2));
        AppendOp(spec.processOps, I32Const(1));
        AppendOp(spec.processOps, {0x6b, 0x22, 0x02, 0x0d, 0x00, 0x0b}); // i32.sub, local.tee 2, br_if 0, end
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        break;
    case HostileKind::GrowingProcess:
        // memory.grow by a page every sample.
        AppendOp(spec.processOps, I32Const(1));
        AppendOp(spec.processOps, {0x40, 0x00, 0x1a}); // memory.grow 0, drop
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        break;
    case HostileKind::LookupInProcess:
        // param_handle on the audio thread, where it may not be called.
        spec = ParamGuest(BuildDescriptorBlob({{"param.gain.default", "1.0"}}), {"gain"});
        AppendOp(spec.processOps, I32Const(static_cast<int32_t>(kIdOffset)));
        AppendOp(spec.processOps, I32Const(4));
        AppendOp(spec.processOps, Call(kParamHandleImport));
        AppendU8(spec.processOps, 0x1a); // drop
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        break;
    case HostileKind::HugeOutput:
        AppendOp(spec.processOps, F32Const(1.0e30f));
        AppendOp(spec.processOps, F32Const(-1.0e30f));
        break;
    }

    return MakeGuestModule(spec);
}

double SecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The guest's process runs on the audio thread; one that never returns must be stopped by its
// deadline and the node must pass the input through dry from then on.
bool TestHostileProcessIsStopped(const TempDir& tempDir, HostileKind kind, const char* label)
{
    auto effect = LoadPrepared(tempDir, (std::string(label) + ".wasm").c_str(), MakeHostileModule(kind));

    if (!effect)
    {
        return false;
    }

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    leftIn.fill(0.25f);
    rightIn.fill(-0.5f);
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};

    const auto start = std::chrono::steady_clock::now();
    effect->Process(inputs, outputs, kBlockSize);
    effect->Process(inputs, outputs, kBlockSize);
    const double elapsed = SecondsSince(start);

    // The block budget is 30 ms; allow for a slow CI machine, but not for a hang.
    if (elapsed > 2.0)
    {
        std::cerr << "The " << label << " module held Process for " << elapsed << " s.\n";
        return false;
    }

    if (!NearlyEqual(leftOut[kBlockSize - 1], 0.25f) || !NearlyEqual(rightOut[kBlockSize - 1], -0.5f))
    {
        std::cerr << "The " << label << " module did not leave the node passing dry.\n";
        return false;
    }

    if (effect->GetConfig("lastError").empty())
    {
        std::cerr << "The " << label << " module's trap was not reported.\n";
        return false;
    }

    return true;
}

bool TestLoopingStartFunctionIsStopped(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "looping_start.wasm";

    if (!WriteBytes(modulePath, MakeHostileModule(HostileKind::LoopingStart)))
    {
        std::cerr << "Failed to write the looping-start module.\n";
        return false;
    }

    // Inspection runs on the message thread when a preset loads; the start function must not
    // freeze it. A load's setup budget is 2 s.
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);
    const double elapsed = SecondsSince(start);

    if (descriptor || error.find("time limit") == std::string::npos)
    {
        std::cerr << "A looping start function was not stopped by its deadline: " << error << "\n";
        return false;
    }

    if (elapsed > 15.0)
    {
        std::cerr << "A looping start function held inspection for " << elapsed << " s.\n";
        return false;
    }

    // Once out of time, the module is refused at once for the rest of the session, by inspection
    // and by a node alike, so a preset full of it can't freeze the app once per node.
    const auto again = std::chrono::steady_clock::now();
    std::string secondError;
    const auto secondDescriptor = WasmEffect::InspectModuleFile(modulePath, &secondError);
    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);
    const double secondElapsed = SecondsSince(again);

    if (secondDescriptor || loaded || secondError.find("earlier in this session") == std::string::npos || !effect ||
        effect->GetConfig("lastError").find("earlier in this session") == std::string::npos)
    {
        std::cerr << "A module that ran out of time loading was tried again: " << secondError << "\n";
        return false;
    }

    if (secondElapsed > 0.5)
    {
        std::cerr << "Refusing a module that ran out of time loading took " << secondElapsed << " s.\n";
        return false;
    }

    return true;
}

// One block of 0.25 / -0.5 in, through a fresh output buffer; the last output frame.
std::pair<float, float> ProcessConstantBlock(EffectProcessor& effect, int frames = kBlockSize)
{
    std::vector<float> leftIn(static_cast<std::size_t>(frames), 0.25f);
    std::vector<float> rightIn(static_cast<std::size_t>(frames), -0.5f);
    std::vector<float> leftOut(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> rightOut(static_cast<std::size_t>(frames), 0.0f);
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    effect.Process(inputs, outputs, frames);
    return {leftOut.back(), rightOut.back()};
}

// A module that finishes every block, but takes longer than the block lasts, is stopped by the
// CPU watchdog; while a host renders offline, it is left to run.
bool TestSlowProcessIsStoppedByWatchdog(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "slow_process.wasm", MakeHostileModule(HostileKind::SlowProcess));

    if (!effect)
    {
        return false;
    }

    WasmEffect::NoteOfflineRendering(true);

    for (int block = 0; block < 40; ++block)
    {
        ProcessConstantBlock(*effect);
    }

    WasmEffect::NoteOfflineRendering(false);

    if (!effect->GetConfig("lastError").empty())
    {
        std::cerr << "The watchdog stopped a slow module during an offline render: " << effect->GetConfig("lastError")
                  << "\n";
        return false;
    }

    std::string error;

    for (int block = 0; block < 200 && error.empty(); ++block)
    {
        ProcessConstantBlock(*effect);
        error = effect->GetConfig("lastError");
    }

    if (error.find("real time") == std::string::npos)
    {
        std::cerr << "A module slower than real time was not stopped by the watchdog: " << error << "\n";
        return false;
    }

    if (const auto [left, right] = ProcessConstantBlock(*effect);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f))
    {
        std::cerr << "A module the watchdog stopped did not leave the node passing dry.\n";
        return false;
    }

    return true;
}

// A module stopped while running is not revived by the host preparing again (a DAW prepares
// often); loading it again is what gives it another go.
bool TestStoppedModuleStaysStopped(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "stays_stopped.wasm";

    if (!WriteBytes(modulePath, MakeHostileModule(HostileKind::LoopingProcess)))
    {
        std::cerr << "Failed to write the stays-stopped module.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (!effect || !loaded)
    {
        std::cerr << "The stays-stopped module failed to load.\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    ProcessConstantBlock(*effect);

    if (effect->GetConfig("lastError").find("time limit") == std::string::npos)
    {
        std::cerr << "A looping process was not stopped: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    const auto start = std::chrono::steady_clock::now();
    const auto [left, right] = ProcessConstantBlock(*effect);
    const double elapsed = SecondsSince(start);

    if (elapsed > 0.02 || !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f) ||
        effect->GetConfig("lastError").empty())
    {
        std::cerr << "Prepare revived a stopped module (" << elapsed << " s for a block).\n";
        return false;
    }

    ResourceRef moduleRef;
    moduleRef.resourceType = "wasm";
    moduleRef.filePath = modulePath.string();

    if (!effect->LoadResources({moduleRef}, {modulePath}) || !effect->GetConfig("lastError").empty())
    {
        std::cerr << "Loading a stopped module again did not give it another go: " << effect->GetConfig("lastError")
                  << "\n";
        return false;
    }

    return true;
}

// On the audio thread a module may only read params and tempo, and may not grow its memory.
bool TestAudioThreadRules(const TempDir& tempDir)
{
    const std::vector<std::tuple<HostileKind, const char*, const char*>> cases = {
        {HostileKind::LookupInProcess, "lookup_in_process.wasm", "param_handle was called from audiofx_process"},
        {HostileKind::GrowingProcess, "growing_process.wasm", "grew the module's memory"},
    };

    for (const auto& [kind, fileName, expected] : cases)
    {
        auto effect = LoadPrepared(tempDir, fileName, MakeHostileModule(kind));

        if (!effect)
        {
            return false;
        }

        const auto [left, right] = ProcessConstantBlock(*effect);
        const std::string error = effect->GetConfig("lastError");

        if (error.find(expected) == std::string::npos || !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f))
        {
            std::cerr << fileName << " was not stopped as it should be: " << error << "\n";
            return false;
        }
    }

    return true;
}

bool TestOutputIsClamped(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "huge_output.wasm", MakeHostileModule(HostileKind::HugeOutput));

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessConstantBlock(*effect);
        !NearlyEqual(left, WasmEffect::kMaxGuestOutputSample) ||
        !NearlyEqual(right, -WasmEffect::kMaxGuestOutputSample))
    {
        std::cerr << "A huge finite output was not clamped: " << left << ", " << right << "\n";
        return false;
    }

    return true;
}

bool TestLatencyIsCapped(const TempDir& tempDir)
{
    const auto limit = WasmEffect::kMaxGuestLatencySamples;

    for (const int32_t latency : {limit, limit + 1, std::numeric_limits<int32_t>::max()})
    {
        GuestSpec spec;
        spec.latencySamples = latency;
        const auto modulePath = tempDir.root / ("latency_" + std::to_string(latency) + ".wasm");

        if (!WriteBytes(modulePath, MakeGuestModule(spec)))
        {
            std::cerr << "Failed to write the latency module.\n";
            return false;
        }

        // A latency past the limit fails the load itself, and every prepare after it.
        bool loaded = false;
        auto effect = LoadModuleFile(modulePath, loaded);

        if (!effect)
        {
            return false;
        }

        effect->Prepare(kSampleRate, kBlockSize);
        const bool allowed = latency <= limit;
        const std::string error = effect->GetConfig("lastError");

        if (allowed ? (!loaded || effect->GetLatencySamples() != latency || !error.empty())
                    : (loaded || effect->GetLatencySamples() != 0 || error.find("latency") == std::string::npos))
        {
            std::cerr << "A latency of " << latency << " was " << (allowed ? "refused" : "accepted") << ": " << error
                      << "\n";
            return false;
        }
    }

    return true;
}

// A host block longer than kMaxGuestBlockFrames reaches the module in pieces it was prepared for.
bool TestGuestBlockIsCapped(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "gain_big_block.wasm";

    if (!WriteBytes(modulePath, MakeGainModule()))
    {
        std::cerr << "Failed to write the big-block module.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);
    constexpr int kHostBlock = WasmEffect::kMaxGuestBlockFrames * 2 + 3;

    if (!effect || !loaded)
    {
        return false;
    }

    // The fixture's buffer holds 2 x 4096 frames, so a prepare at the host's block would not fit.
    effect->Prepare(kSampleRate, kHostBlock);

    if (const auto [left, right] = ProcessConstantBlock(*effect, kHostBlock);
        !effect->GetConfig("lastError").empty() || !NearlyEqual(left, 0.125f) || !NearlyEqual(right, -0.25f))
    {
        std::cerr << "A long host block did not go through in guest-sized pieces: " << effect->GetConfig("lastError")
                  << "\n";
        return false;
    }

    return true;
}

bool TestOversizedModuleIsRefused(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "oversized.wasm";
    ByteVector module = MakeGainModule();
    module.resize(WasmEffect::kMaxModuleBytes + 1, 0);

    if (!WriteBytes(modulePath, module))
    {
        std::cerr << "Failed to write the oversized module.\n";
        return false;
    }

    std::string error;
    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (WasmEffect::InspectModuleFile(modulePath, &error) || error.find("limit") == std::string::npos || loaded ||
        !effect || effect->GetConfig("lastError").find("limit") == std::string::npos)
    {
        std::cerr << "A module over the size limit was not refused: " << error << "\n";
        return false;
    }

    return true;
}

// The process-wide limits: how many Custom Effects may run, and how much memory they may share.
bool TestProcessWideLimits(const TempDir& tempDir)
{
    const auto gainPath = tempDir.root / "gain_many.wasm";
    const auto bigPath = tempDir.root / "big_memory.wasm";
    GuestSpec big;
    big.memoryMinPages = static_cast<uint32_t>(WasmEffect::kMaxGuestMemoryBytes / 65536);

    if (!WriteBytes(gainPath, MakeGainModule()) || !WriteBytes(bigPath, MakeGuestModule(big)))
    {
        std::cerr << "Failed to write the process-wide limit modules.\n";
        return false;
    }

    std::vector<std::unique_ptr<EffectProcessor>> effects;
    bool loaded = true;

    while (loaded && effects.size() <= WasmEffect::kMaxLiveInstances)
    {
        effects.push_back(LoadModuleFile(gainPath, loaded));
    }

    if (loaded || effects.size() != WasmEffect::kMaxLiveInstances + 1 ||
        effects.back()->GetConfig("lastError").find("at once") == std::string::npos)
    {
        std::cerr << "The instance limit did not hold at " << WasmEffect::kMaxLiveInstances << ": "
                  << effects.back()->GetConfig("lastError") << "\n";
        return false;
    }

    // One gone makes room for one more.
    effects.erase(effects.begin());
    effects.back() = LoadModuleFile(gainPath, loaded);

    if (!loaded)
    {
        std::cerr << "An instance freed no room: " << effects.back()->GetConfig("lastError") << "\n";
        return false;
    }

    effects.clear();

    // Each of these keeps all the memory one may have, so the shared budget runs out first.
    const auto fits = static_cast<std::size_t>(WasmEffect::kMaxTotalGuestBytes / WasmEffect::kMaxGuestMemoryBytes);
    std::string error;

    while (error.empty() && effects.size() <= fits)
    {
        effects.push_back(LoadModuleFile(bigPath, loaded));
        effects.back()->Prepare(kSampleRate, kBlockSize);
        error = effects.back()->GetConfig("lastError");
    }

    if (error.find("share") == std::string::npos || effects.size() + 1 < fits || effects.size() > fits)
    {
        std::cerr << "The shared memory budget did not hold (" << effects.size() << " of " << fits << " fit): " << error
                  << "\n";
        return false;
    }

    return true;
}

bool TestDescriptorSizeLimits()
{
    std::vector<std::pair<std::string, std::string>> manyEntries;

    for (std::size_t i = 0; i <= wasm_descriptor::kMaxDescriptorEntries; ++i)
    {
        manyEntries.emplace_back("x", "y");
    }

    std::string labels;

    for (std::size_t i = 0; i <= wasm_descriptor::kMaxParamLabels; ++i)
    {
        labels += (i ? "|" : "") + std::string("a");
    }

    const auto blobText = [](const std::vector<std::pair<std::string, std::string>>& entries) {
        const ByteVector blob = BuildDescriptorBlob(entries);
        return std::string(blob.begin(), blob.end());
    };

    const std::vector<std::pair<std::string, std::string>> cases = {
        {blobText(manyEntries), "entries"},
        {blobText({{"param.choice.labels", labels}}), "labels"},
    };

    for (const auto& [blob, expected] : cases)
    {
        std::string error;

        if (wasm_descriptor::ParseDescriptorBlob(blob, &error) || error.find(expected) == std::string::npos)
        {
            std::cerr << "A descriptor over its " << expected << " limit was not refused: " << error << "\n";
            return false;
        }
    }

    return true;
}

bool TestHugeMemoryIsRefused(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "huge_memory.wasm";

    if (!WriteBytes(modulePath, MakeHostileModule(HostileKind::HugeMemory)))
    {
        std::cerr << "Failed to write the huge-memory module.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (loaded)
    {
        std::cerr << "A module asking for more than the memory limit was instantiated.\n";
        return false;
    }

    return true;
}

bool TestNetworkPathsAreRefused(const TempDir& tempDir)
{
    const std::vector<std::pair<std::filesystem::path, bool>> cases = {
        {std::filesystem::path(L"\\\\server\\share\\fx.wasm"), true},
        {std::filesystem::path(L"//server/share/fx.wasm"), true},
        {std::filesystem::path(L"\\\\?\\UNC\\server\\share\\fx.wasm"), true},
        {std::filesystem::path(L"\\\\?\\C:\\fx.wasm"), false},
        {std::filesystem::path(L"C:\\fx.wasm"), false},
        {tempDir.root / "fx.wasm", false},
    };

    for (const auto& [path, network] : cases)
    {
        if (WasmEffect::IsNetworkPath(path) != network)
        {
            std::cerr << "IsNetworkPath got " << path.string() << " wrong.\n";
            return false;
        }
    }

    bool loaded = false;
    auto effect = LoadModuleFile(std::filesystem::path(L"\\\\server.invalid\\share\\fx.wasm"), loaded);

    if (loaded || !effect || effect->GetConfig("lastError").find("network") == std::string::npos)
    {
        std::cerr << "A module on a network share was not refused up front.\n";
        return false;
    }

    std::string error;

    if (WasmEffect::InspectModuleFile(std::filesystem::path(L"\\\\server.invalid\\share\\fx.wasm"), &error) ||
        error.find("network") == std::string::npos)
    {
        std::cerr << "Inspecting a module on a network share was not refused up front.\n";
        return false;
    }

    return true;
}

bool TestHostingGate(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "gated_gain.wasm";

    if (!WriteBytes(modulePath, MakeGainModule()))
    {
        std::cerr << "Failed to write the gated gain module.\n";
        return false;
    }

    WasmEffect::SetHostingEnabled(false);
    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);
    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);
    WasmEffect::SetHostingEnabled(true);

    if (loaded || descriptor)
    {
        std::cerr << "A module loaded while Custom Effects were turned off.\n";
        return false;
    }

    auto enabledEffect = LoadModuleFile(modulePath, loaded);

    if (!loaded)
    {
        std::cerr << "The gain module did not load once Custom Effects were turned back on.\n";
        return false;
    }

    return true;
}

// A module's category and thumbnail end up in UI markup and in the Custom Effect library.
bool TestHostileDescriptorIsSanitised(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "hostile_descriptor.wasm";
    const ByteVector blob = BuildDescriptorBlob({
        {"effect.name", "Looks Normal"},
        {"effect.category", "<iframe src=\"https://example.invalid\"></iframe>"},
        {"effect.thumbnailDataUrl", "https://example.invalid/pixel.png"},
        {"param.amount.title", "Amount"},
    });

    if (!WriteBytes(modulePath, MakeDescriptorModule(blob)))
    {
        std::cerr << "Failed to write the hostile-descriptor module.\n";
        return false;
    }

    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);

    if (!descriptor)
    {
        std::cerr << "Failed to inspect the hostile-descriptor module: " << error << "\n";
        return false;
    }

    if (descriptor->category != "utility" || !descriptor->thumbnailDataUrl.empty())
    {
        std::cerr << "A hostile category or thumbnail URL survived descriptor parsing.\n";
        return false;
    }

    for (const auto& entry : descriptor->entries)
    {
        if ((entry.key == "effect.category" && entry.value != "utility") || entry.key.rfind("effect.thumbnail", 0) == 0)
        {
            std::cerr << "The stored descriptor entries still carry '" << entry.key << "' unsanitised.\n";
            return false;
        }
    }

    return true;
}

// The output level of a module run through the host.
struct ModuleRunReport
{
    float peak = 0.0f;
    float rms = 0.0f;
};

// Runs a module file the way a Custom Effect node does: loaded with the given resource files (in
// the order the descriptor declares the resources), prepared at 48 kHz for 512-frame blocks, then
// two seconds of a 440 Hz sine at half scale in host blocks of 1, 64, 480 and 512 frames. Prints
// the descriptor when `verbose`. False, with the reason on stderr, if the module fails to load or
// run, or produces a non-finite sample.
bool RunModuleFile(const std::filesystem::path& modulePath, const std::vector<std::filesystem::path>& resourcePaths,
                   bool verbose, ModuleRunReport& report)
{
    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);

    if (!descriptor)
    {
        std::cerr << modulePath.string() << ": " << error << "\n";
        return false;
    }

    if (verbose)
    {
        std::cout << modulePath.filename().string() << ": " << descriptor->displayName << " (" << descriptor->category
                  << ")\n";

        for (const auto& param : descriptor->parameters)
        {
            const auto& definition = param.definition;
            std::cout << "  param " << definition.id << ": default " << definition.defaultValue << ", "
                      << definition.minValue << " to " << definition.maxValue << "\n";
        }

        for (const auto& resource : descriptor->exposedResources)
        {
            std::cout << "  resource " << resource.resourceId << " (" << resource.resourceType << ")\n";
        }
    }

    std::vector<ResourceRef> refs{FileRef(modulePath, "wasm", 0)};
    std::vector<std::filesystem::path> paths{modulePath};

    for (std::size_t i = 0; i < resourcePaths.size(); ++i)
    {
        const char* type =
            i < descriptor->exposedResources.size() ? descriptor->exposedResources[i].resourceType.c_str() : "blob";
        refs.push_back(FileRef(resourcePaths[i], type, i + 1));
        paths.push_back(resourcePaths[i]);
    }

    auto effect = CreateWasmHost();

    if (!effect->LoadResources(refs, paths))
    {
        std::cerr << modulePath.string() << ": " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    constexpr int kMaxBlock = 512;
    // A prepare is a whole load's setup (instantiation, descriptor, prepare, buffer, latency), the
    // calls that share the setup budget; the module itself compiled in LoadResources.
    const auto prepareStart = std::chrono::steady_clock::now();
    effect->Prepare(kSampleRate, kMaxBlock);
    const double prepareSeconds = SecondsSince(prepareStart);

    if (const auto lastError = effect->GetConfig("lastError"); !lastError.empty())
    {
        std::cerr << modulePath.string() << ": " << lastError << "\n";
        return false;
    }

    if (verbose)
    {
        std::cout << "  setup took " << static_cast<int>(prepareSeconds * 1000.0) << " ms of a load's "
                  << (WasmEffect::kSmallAddressSpace ? 8 : 2) << " s budget\n";
    }

    std::vector<float> leftIn(kMaxBlock);
    std::vector<float> rightIn(kMaxBlock);
    std::vector<float> leftOut(kMaxBlock);
    std::vector<float> rightOut(kMaxBlock);
    constexpr std::array<int, 4> kHostBlocks{1, 64, 480, kMaxBlock};
    const int totalFrames = static_cast<int>(kSampleRate) * 2;
    double sumSquares = 0.0;
    int frame = 0;

    for (std::size_t blockIndex = 0; frame < totalFrames; ++blockIndex)
    {
        const int frames = std::min(kHostBlocks[blockIndex % kHostBlocks.size()], totalFrames - frame);

        for (int i = 0; i < frames; ++i)
        {
            const float sample =
                0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * (frame + i) / kSampleRate));
            leftIn[static_cast<std::size_t>(i)] = sample;
            rightIn[static_cast<std::size_t>(i)] = sample;
        }

        float* inputs[] = {leftIn.data(), rightIn.data()};
        float* outputs[] = {leftOut.data(), rightOut.data()};
        effect->Process(inputs, outputs, frames);

        if (const auto lastError = effect->GetConfig("lastError"); !lastError.empty())
        {
            std::cerr << modulePath.string() << ": stopped at frame " << frame << ": " << lastError << "\n";
            return false;
        }

        for (int i = 0; i < frames; ++i)
        {
            for (const float value : {leftOut[static_cast<std::size_t>(i)], rightOut[static_cast<std::size_t>(i)]})
            {
                if (!std::isfinite(value))
                {
                    std::cerr << modulePath.string() << ": output at frame " << frame + i << " is not finite.\n";
                    return false;
                }

                report.peak = std::max(report.peak, std::fabs(value));
                sumSquares += static_cast<double>(value) * value;
            }
        }

        frame += frames;
    }

    report.rms = static_cast<float>(std::sqrt(sumSquares / (2.0 * totalFrames)));

    if (verbose)
    {
        std::cout << "  ran " << totalFrames << " frames: peak " << report.peak << ", RMS " << report.rms << "\n";
    }

    return true;
}

// The committed phaser sample (tools/wasm-samples) still loads and runs. At its defaults a phaser
// passes most of a sine through.
bool TestCommittedPhaserSample()
{
    const auto modulePath =
        std::filesystem::path(GUITARFX_WASM_SAMPLES_DIR) / "audiofx-phaser-module" / "dist" / "ai_phaser.wasm";
    ModuleRunReport report;

    if (!RunModuleFile(modulePath, {}, false, report))
    {
        std::cerr << "The committed phaser sample failed to run.\n";
        return false;
    }

    if (report.rms < 0.05f || report.peak > 2.0f)
    {
        std::cerr << "The committed phaser sample's output is implausible: peak " << report.peak << ", RMS "
                  << report.rms << ".\n";
        return false;
    }

    return true;
}
} // namespace
} // namespace guitarfx

// WasmEffectHostTests --check <module.wasm> [resource files...] runs one module through the host
// and reports what it declares and how it ran, for checking a module by hand.
static int RunCheck(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: WasmEffectHostTests --check <module.wasm> [resource files...]\n";
        return 2;
    }

    std::vector<std::filesystem::path> resources;

    for (int i = 3; i < argc; ++i)
    {
        resources.emplace_back(argv[i]);
    }

    guitarfx::ModuleRunReport report;
    return guitarfx::RunModuleFile(argv[2], resources, true, report) ? 0 : 1;
}

int main(int argc, char** argv)
{
    guitarfx::RegisterAllEffects();
    guitarfx::RegisterAllEffects();
    // The app sets this from the Custom Effects feature setting.
    guitarfx::WasmEffect::SetHostingEnabled(true);

    if (argc >= 2 && std::string(argv[1]) == "--check")
    {
        return RunCheck(argc, argv);
    }

    guitarfx::TempDir tempDir;
    bool allPassed = true;
    allPassed = guitarfx::TestRegistryMetadata() && allPassed;
    allPassed = guitarfx::TestDescriptorInspection(tempDir) && allPassed;
    allPassed = guitarfx::TestDirectGainModule(tempDir) && allPassed;
    allPassed = guitarfx::TestTempoImport(tempDir) && allPassed;
    allPassed = guitarfx::TestReorderedDescriptorKeepsValuesById(tempDir) && allPassed;
    allPassed = guitarfx::TestHardCodedHandleTraps(tempDir) && allPassed;
    allPassed = guitarfx::TestUnknownIdsHaveNoHandle(tempDir) && allPassed;
    allPassed = guitarfx::TestOtherInterfacesAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestBadDescriptorsAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestMonoInputToStereoGuestUsesDualMono(tempDir) && allPassed;
    allPassed = guitarfx::TestSignalGraphStereoModule(tempDir) && allPassed;
    allPassed = guitarfx::TestResourcesById(tempDir) && allPassed;
    allPassed = guitarfx::TestSignalGraphResourcesById(tempDir) && allPassed;
    allPassed = guitarfx::TestRekeyNodeResources() && allPassed;
    allPassed = guitarfx::TestResourceReadEdges(tempDir) && allPassed;
    allPassed = guitarfx::TestHardCodedResourceHandleTraps(tempDir) && allPassed;
    allPassed = guitarfx::TestResetStatefulModule(tempDir) && allPassed;
    allPassed = guitarfx::TestLongInPlaceBlock(tempDir) && allPassed;
    allPassed = guitarfx::TestBadProcessShapesAreRefused(tempDir) && allPassed;
    allPassed =
        guitarfx::TestHostileProcessIsStopped(tempDir, guitarfx::HostileKind::LoopingProcess, "looping_process") &&
        allPassed;
    allPassed =
        guitarfx::TestHostileProcessIsStopped(tempDir, guitarfx::HostileKind::RecursiveProcess, "recursive_process") &&
        allPassed;
    allPassed = guitarfx::TestLoopingStartFunctionIsStopped(tempDir) && allPassed;
    allPassed = guitarfx::TestHugeMemoryIsRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestSlowProcessIsStoppedByWatchdog(tempDir) && allPassed;
    allPassed = guitarfx::TestStoppedModuleStaysStopped(tempDir) && allPassed;
    allPassed = guitarfx::TestAudioThreadRules(tempDir) && allPassed;
    allPassed = guitarfx::TestOutputIsClamped(tempDir) && allPassed;
    allPassed = guitarfx::TestLatencyIsCapped(tempDir) && allPassed;
    allPassed = guitarfx::TestGuestBlockIsCapped(tempDir) && allPassed;
    allPassed = guitarfx::TestOversizedModuleIsRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestProcessWideLimits(tempDir) && allPassed;
    allPassed = guitarfx::TestDescriptorSizeLimits() && allPassed;
    allPassed = guitarfx::TestNetworkPathsAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestHostingGate(tempDir) && allPassed;
    allPassed = guitarfx::TestHostileDescriptorIsSanitised(tempDir) && allPassed;
    allPassed = guitarfx::TestCommittedPhaserSample() && allPassed;

    if (!allPassed)
    {
        std::cerr << "WasmEffectHostTests failed.\n";
        return 1;
    }

    std::cout << "WasmEffectHostTests passed.\n";
    return 0;
}
