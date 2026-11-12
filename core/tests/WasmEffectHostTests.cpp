#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
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
        !NearlyEqual(left, 0.75f) || !NearlyEqual(right, 120.0f))
    {
        std::cerr << "Param default or tempo read back wrong: " << left << ", " << right << "\n";
        return false;
    }

    effect->SetParam("last", 0.25);
    effect->SetParam("bpm", 90.0);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, 90.0f) || effect->GetParam("last") != 0.25)
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

bool TestHardCodedResourceHandleTraps(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "hard_coded_resource.wasm", MakeHardCodedResourceHandleModule());

    if (!effect)
    {
        return false;
    }

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

// Modules that misbehave on purpose. A module is code from whoever shared it, so each of these
// must end in a trap or a refused load, never a hang or a crash.
enum class HostileKind
{
    LoopingProcess,
    RecursiveProcess,
    LoopingStart,
    HugeMemory,
};

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
        // One page past the store's 256 MiB limit.
        spec.memoryMinPages = 4097;
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

    // The block budget is 0.2 s; allow for a slow CI machine, but not for a hang.
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
    // freeze it. The setup budget is 5 s.
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
} // namespace
} // namespace guitarfx

int main()
{
    guitarfx::RegisterAllEffects();
    // The app sets this from the Custom Effects feature setting.
    guitarfx::WasmEffect::SetHostingEnabled(true);

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
    allPassed =
        guitarfx::TestHostileProcessIsStopped(tempDir, guitarfx::HostileKind::LoopingProcess, "looping_process") &&
        allPassed;
    allPassed =
        guitarfx::TestHostileProcessIsStopped(tempDir, guitarfx::HostileKind::RecursiveProcess, "recursive_process") &&
        allPassed;
    allPassed = guitarfx::TestLoopingStartFunctionIsStopped(tempDir) && allPassed;
    allPassed = guitarfx::TestHugeMemoryIsRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestNetworkPathsAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestHostingGate(tempDir) && allPassed;
    allPassed = guitarfx::TestHostileDescriptorIsSanitised(tempDir) && allPassed;

    if (!allPassed)
    {
        std::cerr << "WasmEffectHostTests failed.\n";
        return 1;
    }

    std::cout << "WasmEffectHostTests passed.\n";
    return 0;
}
