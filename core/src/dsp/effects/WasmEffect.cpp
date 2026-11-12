#include "dsp/effects/WasmEffect.h"

#include "dsp/effects/WasmDescriptor.h"

#include "dsp/FiniteCheck.h"
#include "resources/CustomEffectLibrary.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
namespace
{
using wasm_descriptor::BuildDescriptorFromEntries;
using wasm_descriptor::IsAsciiDigit;
using wasm_descriptor::IsAsciiLetter;
using wasm_descriptor::kAbiMigrationHint;
using wasm_descriptor::ParseDescriptorBlob;

constexpr std::size_t kModuleSlot = 0;

// The shared engine's epoch advances once per tick; a deadline is a number of ticks.
constexpr std::chrono::milliseconds kEpochTick{10};
// Instantiation (the start function), the descriptor exports, prepare and the latency query run
// off the audio thread and may do real work, such as parsing a resource.
constexpr double kSetupBudgetSeconds = 5.0;
// A block gets four times its own duration, and never less than this. Offline renders may run
// slower than real time, so the budget is there to stop a hang, not to enforce real time.
constexpr double kProcessMinBudgetSeconds = 0.2;
constexpr double kProcessBudgetBlockMultiple = 4.0;

// The descriptor a module exports reaches the UI and presets; bound its size.
constexpr std::size_t kMaxDescriptorBytes = 2u * 1024u * 1024u;

std::atomic<bool> gHostingEnabled{false};

float ClampFinite(float value, float fallback)
{
    return IsFinite(value) ? value : fallback;
}

double DbToLinear(double value)
{
    return std::pow(10.0, value / 20.0);
}

std::optional<std::size_t> ParseResourceSlotIndex(const ResourceRef& ref)
{
    const auto it = ref.metadata.find("resourceSlotIndex");

    if (it == ref.metadata.end())
    {
        return std::nullopt;
    }

    try
    {
        return static_cast<std::size_t>(std::stoul(it->second));
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
    {
        return {};
    }

    std::size_t size = message.size;

    if (size > 0 && message.data[size - 1] == '\0')
    {
        --size;
    }

    return std::string(message.data, size);
}

std::string TakeErrorMessage(wasmtime_error_t* error)
{
    if (!error)
    {
        return {};
    }

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
    {
        return {};
    }

    if (wasmtime_trap_code_t code = 0; wasmtime_trap_code(trap, &code) && code == WASMTIME_TRAP_CODE_INTERRUPT)
    {
        wasm_trap_delete(trap);
        return "the module ran past its time limit and was stopped";
    }

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

// (i32, i32, i32, i32) -> i32; the C API has no four-param shorthand.
FunctionTypePtr MakeResourceReadType()
{
    wasm_valtype_vec_t params;
    wasm_valtype_vec_t results;
    std::array<wasm_valtype_t*, 4> paramTypes{wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(),
                                              wasm_valtype_new_i32()};
    std::array<wasm_valtype_t*, 1> resultTypes{wasm_valtype_new_i32()};
    wasm_valtype_vec_new(&params, paramTypes.size(), paramTypes.data());
    wasm_valtype_vec_new(&results, resultTypes.size(), resultTypes.data());
    return MakeFunctionType(wasm_functype_new(&params, &results));
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

struct GuestMemoryView
{
    std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// The calling guest's exported memory, valid until it next runs. Looked up on every call:
// memory.grow can move it.
std::optional<GuestMemoryView> GuestMemory(wasmtime_caller_t* caller)
{
    if (!caller)
    {
        return std::nullopt;
    }

    wasmtime_extern_t memoryItem{};

    if (!wasmtime_caller_export_get(caller, "memory", std::strlen("memory"), &memoryItem))
    {
        return std::nullopt;
    }

    if (memoryItem.kind != WASMTIME_EXTERN_MEMORY)
    {
        wasmtime_extern_delete(&memoryItem);
        return std::nullopt;
    }

    wasmtime_context_t* context = wasmtime_caller_context(caller);
    GuestMemoryView view{wasmtime_memory_data(context, &memoryItem.of.memory),
                         wasmtime_memory_data_size(context, &memoryItem.of.memory)};
    wasmtime_extern_delete(&memoryItem);

    if (!view.data)
    {
        return std::nullopt;
    }

    return view;
}

// The guest's bytes at [pointer, pointer + length) in its exported memory, valid until it next runs.
std::optional<std::string_view> ReadGuestString(wasmtime_caller_t* caller, int32_t pointer, int32_t length,
                                                std::size_t maxLength)
{
    if (pointer < 0 || length <= 0 || static_cast<std::size_t>(length) > maxLength)
    {
        return std::nullopt;
    }

    const auto memory = GuestMemory(caller);
    const auto offset = static_cast<std::size_t>(pointer);
    const auto size = static_cast<std::size_t>(length);

    if (!memory || offset > memory->size || size > memory->size - offset)
    {
        return std::nullopt;
    }

    return std::string_view(reinterpret_cast<const char*>(memory->data + offset), size);
}

wasm_trap_t* BadHandleTrap(const char* import, const char* lookup, int32_t handle)
{
    const std::string message =
        std::string(import) + " was given handle " + std::to_string(handle) + ", which " + lookup + " did not return";
    return wasmtime_trap_new(message.data(), message.size());
}

// param_handle(idPtr, idLen) -> handle, or -1 for an id the descriptor doesn't declare.
extern "C" wasm_trap_t* HostParamHandleRaw(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
                                           size_t nargs, wasmtime_val_t* results, size_t nresults)
{
    if (nresults > 0)
    {
        results[0] = MakeI32Value(-1);
    }

    if (!env || nargs < 2 || nresults < 1)
    {
        return nullptr;
    }

    if (const auto id = ReadGuestString(caller, args[0].of.i32, args[1].of.i32, WasmEffect::kMaxGuestIdLength))
    {
        results[0] = MakeI32Value(static_cast<WasmEffect*>(env)->GuestParamHandle(*id));
    }

    return nullptr;
}

// param_value(handle) -> value. A handle param_handle never gave traps: reading the wrong knob
// quietly is what ABI 1 did, so a bad handle stops the module instead.
extern "C" wasm_trap_t* HostParamValueRaw(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
                                          size_t nargs, wasmtime_val_t* results, size_t nresults)
{
    (void)caller;

    if (nresults > 0)
    {
        results[0] = MakeF32Value(0.0f);
    }

    if (!env || nargs < 1 || nresults < 1)
    {
        return nullptr;
    }

    float value = 0.0f;

    if (!static_cast<WasmEffect*>(env)->ReadGuestParamHandle(args[0].of.i32, value))
    {
        return BadHandleTrap("param_value", "param_handle", args[0].of.i32);
    }

    results[0] = MakeF32Value(value);
    return nullptr;
}

extern "C" wasm_trap_t* HostTempoBpmRaw(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t nargs,
                                        wasmtime_val_t* results, size_t nresults)
{
    (void)caller;
    (void)args;
    (void)nargs;

    if (nresults < 1)
    {
        return nullptr;
    }

    results[0] = MakeF32Value(env ? static_cast<WasmEffect*>(env)->GuestTempoBpm() : 120.0f);
    return nullptr;
}

// resource_handle(idPtr, idLen) -> handle, or -1 for an id the descriptor doesn't declare.
extern "C" wasm_trap_t* HostResourceHandleRaw(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
                                              size_t nargs, wasmtime_val_t* results, size_t nresults)
{
    if (nresults > 0)
    {
        results[0] = MakeI32Value(-1);
    }

    if (!env || nargs < 2 || nresults < 1)
    {
        return nullptr;
    }

    if (const auto id = ReadGuestString(caller, args[0].of.i32, args[1].of.i32, WasmEffect::kMaxGuestIdLength))
    {
        results[0] = MakeI32Value(static_cast<WasmEffect*>(env)->GuestResourceHandle(*id));
    }

    return nullptr;
}

// resource_size(handle) -> bytes, 0 when no file is chosen. A bad handle traps, as in param_value.
extern "C" wasm_trap_t* HostResourceSizeRaw(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
                                            size_t nargs, wasmtime_val_t* results, size_t nresults)
{
    (void)caller;

    if (nresults > 0)
    {
        results[0] = MakeI32Value(0);
    }

    if (!env || nargs < 1 || nresults < 1)
    {
        return nullptr;
    }

    const auto* bytes = static_cast<WasmEffect*>(env)->GuestResourceBytes(args[0].of.i32);

    if (!bytes)
    {
        return BadHandleTrap("resource_size", "resource_handle", args[0].of.i32);
    }

    // LoadResources refuses a file larger than guest memory can hold, so this fits an i32.
    results[0] = MakeI32Value(static_cast<int32_t>(bytes->size()));
    return nullptr;
}

// resource_read(handle, offset, dstPtr, len) -> bytes copied: the resource's bytes from offset, as
// many as len, the rest of the resource and guest memory from dstPtr allow. 0 for a negative
// argument or an offset past the end. A bad handle traps.
extern "C" wasm_trap_t* HostResourceReadRaw(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
                                            size_t nargs, wasmtime_val_t* results, size_t nresults)
{
    if (nresults > 0)
    {
        results[0] = MakeI32Value(0);
    }

    if (!env || nargs < 4 || nresults < 1)
    {
        return nullptr;
    }

    const int32_t handle = args[0].of.i32;
    const int32_t offset = args[1].of.i32;
    const int32_t destination = args[2].of.i32;
    const int32_t length = args[3].of.i32;
    const auto* bytes = static_cast<WasmEffect*>(env)->GuestResourceBytes(handle);

    if (!bytes)
    {
        return BadHandleTrap("resource_read", "resource_handle", handle);
    }

    const auto memory = GuestMemory(caller);

    if (!memory || offset < 0 || destination < 0 || length <= 0 || static_cast<std::size_t>(offset) >= bytes->size() ||
        static_cast<std::size_t>(destination) >= memory->size)
    {
        return nullptr;
    }

    const std::size_t count =
        std::min({static_cast<std::size_t>(length), bytes->size() - static_cast<std::size_t>(offset),
                  memory->size - static_cast<std::size_t>(destination)});
    std::memcpy(memory->data + destination, bytes->data() + offset, count);
    results[0] = MakeI32Value(static_cast<int32_t>(count));
    return nullptr;
}
} // namespace

// One engine for every Custom Effect in the process, and a thread that advances its epoch each
// tick so the deadline on every guest call means something. Held by shared_ptr from each effect:
// the last one out stops the thread and frees the engine, so nothing outlives a plugin unload.
class WasmSharedRuntime
{
  public:
    static std::shared_ptr<WasmSharedRuntime> Acquire()
    {
        static std::mutex acquireMutex;
        static std::weak_ptr<WasmSharedRuntime> current;
        std::lock_guard<std::mutex> lock(acquireMutex);

        if (auto runtime = current.lock())
        {
            return runtime;
        }

        auto runtime = std::shared_ptr<WasmSharedRuntime>(new WasmSharedRuntime());
        current = runtime;
        return runtime;
    }

    WasmSharedRuntime(const WasmSharedRuntime&) = delete;
    WasmSharedRuntime& operator=(const WasmSharedRuntime&) = delete;

    ~WasmSharedRuntime()
    {
        {
            std::lock_guard<std::mutex> lock(mTickMutex);
            mStopping = true;
        }

        mTickWake.notify_all();

        if (mTicker.joinable())
        {
            mTicker.join();
        }

        if (mEngine)
        {
            wasm_engine_delete(mEngine);
        }
    }

    [[nodiscard]] wasm_engine_t* Engine() const
    {
        return mEngine;
    }

  private:
    WasmSharedRuntime()
    {
        wasm_config_t* config = wasm_config_new();

        if (!config)
        {
            return;
        }

        wasmtime_config_epoch_interruption_set(config, true);
        wasmtime_config_max_wasm_stack_set(config, WasmEffect::kMaxGuestStackBytes);
        // Cranelift only: the April 2026 sandbox escape in the Winch backend is why.
        wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
        // Proposals an audio effect has no use for are off, so their runtime code is out of reach.
        // GC, exceptions and stack switching have carried most of 2026's advisories.
        wasmtime_config_wasm_threads_set(config, false);
        wasmtime_config_shared_memory_set(config, false);
        wasmtime_config_wasm_gc_set(config, false);
        wasmtime_config_wasm_exceptions_set(config, false);
        wasmtime_config_wasm_stack_switching_set(config, false);
        wasmtime_config_wasm_memory64_set(config, false);

        mEngine = wasm_engine_new_with_config(config);

        if (mEngine)
        {
            mTicker = std::thread([this]() { TickLoop(); });
        }
    }

    void TickLoop()
    {
        std::unique_lock<std::mutex> lock(mTickMutex);

        while (!mTickWake.wait_for(lock, kEpochTick, [this]() { return mStopping; }))
        {
            wasmtime_engine_increment_epoch(mEngine);
        }
    }

    wasm_engine_t* mEngine = nullptr;
    std::mutex mTickMutex;
    std::condition_variable mTickWake;
    bool mStopping = false;
    std::thread mTicker;
};

void WasmEffect::SetHostingEnabled(bool enabled)
{
    gHostingEnabled.store(enabled, std::memory_order_relaxed);
}

bool WasmEffect::IsHostingEnabled()
{
    return gHostingEnabled.load(std::memory_order_relaxed);
}

bool WasmEffect::IsNetworkPath(const std::filesystem::path& path)
{
    // \\server\share, //server/share and the \\?\UNC\ long form. A plain \\?\C:\ is local.
    const std::wstring text = path.wstring();

    if (text.size() < 2 || (text[0] != L'\\' && text[0] != L'/') || (text[1] != L'\\' && text[1] != L'/'))
    {
        return false;
    }

    std::wstring upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t ch) { return std::towupper(ch); });
    const bool deviceLocal = (upper.rfind(L"\\\\?\\", 0) == 0 || upper.rfind(L"\\\\.\\", 0) == 0) &&
                             upper.rfind(L"\\\\?\\UNC\\", 0) != 0 && upper.rfind(L"\\\\.\\UNC\\", 0) != 0;
    return !deviceLocal;
}

WasmEffect::WasmEffect()
{
    ResetGuestDescriptor();
    mRuntime = WasmSharedRuntime::Acquire();
    mEngine = mRuntime ? mRuntime->Engine() : nullptr;

    if (!mEngine)
    {
        SetError("Failed to create Wasmtime engine.");
    }
}

WasmEffect::~WasmEffect()
{
    TeardownRuntime();
    // The engine is the shared runtime's; dropping mRuntime releases it with the last effect.
    mEngine = nullptr;
}

void WasmEffect::ArmGuestDeadline(double budgetSeconds)
{
    if (!mContext)
    {
        return;
    }

    const double tickSeconds = std::chrono::duration<double>(kEpochTick).count();
    // One more tick than the budget covers, since the current tick is already partly gone.
    const auto ticks = static_cast<std::uint64_t>(std::ceil(budgetSeconds / tickSeconds)) + 1u;
    wasmtime_context_set_epoch_deadline(mContext, ticks);
}

std::optional<WasmModuleDescriptor> WasmEffect::InspectModuleFile(const std::filesystem::path& modulePath,
                                                                  std::string* error)
{
    if (!IsHostingEnabled())
    {
        if (error)
        {
            *error = "Custom Effects are turned off in Settings, so this module was not inspected.";
        }

        return std::nullopt;
    }

    if (IsNetworkPath(modulePath))
    {
        if (error)
        {
            *error = "WASM modules are not loaded from network shares: " + util::PathToUtf8(modulePath);
        }

        return std::nullopt;
    }

    if (!std::filesystem::exists(modulePath))
    {
        if (error)
        {
            *error = "WASM module file not found: " + util::PathToUtf8(modulePath);
        }

        return std::nullopt;
    }

    WasmEffect probe;
    probe.mModuleBytes = guitarfx::util::ReadFileBytes(modulePath);
    probe.mModulePath = modulePath;

    if (probe.mModuleBytes.empty())
    {
        if (error)
        {
            *error = "WASM module file is empty: " + util::PathToUtf8(modulePath);
        }

        return std::nullopt;
    }

    if (!probe.BuildRuntimeOnly())
    {
        if (error)
        {
            *error = probe.mLastError;
        }

        return std::nullopt;
    }

    if (!probe.LoadGuestDescriptor())
    {
        if (error)
        {
            *error = probe.mLastError;
        }

        probe.TeardownRuntime();
        return std::nullopt;
    }

    probe.TeardownRuntime();
    return probe.mGuestDescriptor;
}

std::optional<WasmModuleDescriptor> WasmEffect::ParseDescriptorConfig(const std::string& configJson, std::string* error)
{
    try
    {
        const auto parsed = nlohmann::json::parse(configJson);

        if (!parsed.is_array())
        {
            if (error)
            {
                *error = "WASM descriptor config must be a JSON array of { key, value } entries.";
            }

            return std::nullopt;
        }

        std::vector<WasmMetadataEntry> entries;

        for (const auto& item : parsed)
        {
            if (!item.is_object())
            {
                continue;
            }

            WasmMetadataEntry entry;
            entry.key = item.value("key", "");
            entry.value = item.value("value", "");

            if (!entry.key.empty())
            {
                entries.push_back(std::move(entry));
            }
        }

        return BuildDescriptorFromEntries(entries, error);
    }
    catch (const std::exception& ex)
    {
        if (error)
        {
            *error = std::string("Failed to parse WASM descriptor config: ") + ex.what();
        }

        return std::nullopt;
    }
}

std::string WasmEffect::SerializeDescriptorConfig(const std::vector<WasmMetadataEntry>& entries)
{
    nlohmann::json payload = nlohmann::json::array();

    for (const auto& entry : entries)
    {
        payload.push_back({
            {"key", entry.key},
            {"value", entry.value},
        });
    }

    return payload.dump();
}

std::vector<std::string> WasmEffect::ParseResourceIdsConfig(const std::string& configJson)
{
    std::vector<std::string> ids;

    try
    {
        const auto parsed = nlohmann::json::parse(configJson);

        if (!parsed.is_array())
        {
            return ids;
        }

        for (const auto& item : parsed)
        {
            ids.push_back(item.is_string() && IsValidGuestId(item.get<std::string>()) ? item.get<std::string>() : "");
        }
    }
    catch (...)
    {
        ids.clear();
    }

    return ids;
}

std::string WasmEffect::SerializeResourceIdsConfig(const std::vector<std::string>& ids)
{
    return nlohmann::json(ids).dump();
}

std::vector<std::string> WasmEffect::RekeyNodeResources(GraphNode& node, const WasmModuleDescriptor& descriptor,
                                                        const WasmModuleDescriptor& placedBy)
{
    if (node.resources.empty())
    {
        return {};
    }

    std::vector<std::string> savedIds;

    if (const auto savedIt = node.config.find(kResourceIdsConfigKey); savedIt != node.config.end())
    {
        savedIds = ParseResourceIdsConfig(savedIt->second);
    }

    std::map<std::string, ResourceRef> filesById;
    std::vector<std::string> dropped;

    for (std::size_t slot = kModuleSlot + 1; slot < node.resources.size(); ++slot)
    {
        if (!node.resources[slot].IsValid())
        {
            continue;
        }

        std::string id = slot < savedIds.size() ? savedIds[slot] : std::string{};

        if (id.empty() && slot - 1 < placedBy.exposedResources.size())
        {
            id = placedBy.exposedResources[slot - 1].resourceId;
        }

        if (id.empty() || !filesById.emplace(id, node.resources[slot]).second)
        {
            dropped.push_back(id.empty() ? "#" + std::to_string(slot) : id);
        }
    }

    std::vector<ResourceRef> resources{node.resources[kModuleSlot]};
    std::vector<std::string> ids{""};

    for (const auto& resource : descriptor.exposedResources)
    {
        const auto fileIt = filesById.find(resource.resourceId);
        resources.push_back(fileIt != filesById.end() ? fileIt->second : ResourceRef{});
        ids.push_back(resource.resourceId);

        if (fileIt != filesById.end())
        {
            filesById.erase(fileIt);
        }
    }

    while (resources.size() > 1 && !resources.back().IsValid())
    {
        resources.pop_back();
        ids.pop_back();
    }

    for (const auto& entry : filesById)
    {
        dropped.push_back(entry.first);
    }

    node.resources = std::move(resources);

    if (ids.size() > 1)
    {
        node.config[kResourceIdsConfigKey] = SerializeResourceIdsConfig(ids);
    }
    else
    {
        node.config.erase(kResourceIdsConfigKey);
    }

    return dropped;
}

// A letter, then up to 31 letters, digits or underscores: safe in a preset key, a MIDI mapping and
// a descriptor key, and never a number, so a numbered ABI 1 block can't pass for one.
bool WasmEffect::IsValidGuestId(std::string_view id)
{
    if (id.empty() || id.size() > kMaxGuestIdLength || !IsAsciiLetter(id.front()))
    {
        return false;
    }

    return std::all_of(id.begin(), id.end(),
                       [](char ch) { return IsAsciiLetter(ch) || IsAsciiDigit(ch) || ch == '_'; });
}

void WasmEffect::Prepare(double sampleRate, int maxBlockSize)
{
    if (!ValidatePrepare(sampleRate, maxBlockSize))
    {
        return;
    }

    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mLatencySamples = 0;

    if (!mModuleBytes.empty())
    {
        RebuildRuntime();
    }
}

void WasmEffect::Reset()
{
    if (!mRuntimeReady || !mHasResetFunction || !mContext)
    {
        return;
    }

    wasm_trap_t* trap = nullptr;

    // Reset can come from the audio thread, so it gets a block's budget, not setup's.
    ArmGuestDeadline(kProcessMinBudgetSeconds);

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
    {
        return;
    }

    const float* inputLeft = inputs ? inputs[0] : nullptr;
    const float* inputRight = inputs ? inputs[1] : nullptr;
    float* outputLeft = outputs[0];
    float* outputRight = outputs[1];

    if (!outputLeft && !outputRight)
    {
        return;
    }

    const float wetMix = static_cast<float>(std::clamp(mMix, 0.0, 1.0));
    const float dryMix = 1.0f - wetMix;
    const float inputGain = static_cast<float>(DbToLinear(mInputGainDb));
    const float outputGain = static_cast<float>(DbToLinear(mOutputGainDb));

    if (mRuntimeReady)
    {
        // One deadline for the whole block: a guest that hangs traps, and the rest of the block
        // and every block after it pass dry (InvokeProcess clears mRuntimeReady).
        const double blockSeconds = mSampleRate > 0.0 ? static_cast<double>(numSamples) / mSampleRate : 0.0;
        ArmGuestDeadline(std::max(kProcessMinBudgetSeconds, blockSeconds * kProcessBudgetBlockMultiple));
    }

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
        {
            outputLeft[sample] = mixedLeft * outputGain;
        }

        if (outputRight)
        {
            outputRight[sample] = mixedRight * outputGain;
        }
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

    if (const auto indexIt = mGuestParamIndex.find(key); indexIt != mGuestParamIndex.end())
    {
        mGuestValues[indexIt->second] = value;
        return;
    }

    // Automation runs SetParam on the audio thread, and only reaches ids the module declared; an
    // unknown id is kept only until a descriptor has loaded to say which ids there are.
    if (!mHasGuestDescriptor)
    {
        mPendingGuestParamValues[key] = value;
    }
}

double WasmEffect::GetParam(const std::string& key) const
{
    if (key == "mix")
    {
        return mMix;
    }

    if (key == "inputGainDb")
    {
        return mInputGainDb;
    }

    if (key == "outputGainDb")
    {
        return mOutputGainDb;
    }

    if (key == "bpm")
    {
        return mBpm;
    }

    if (const auto indexIt = mGuestParamIndex.find(key); indexIt != mGuestParamIndex.end())
    {
        return mGuestValues[indexIt->second];
    }

    if (const auto pendingIt = mPendingGuestParamValues.find(key); pendingIt != mPendingGuestParamValues.end())
    {
        return pendingIt->second;
    }

    return 0.0;
}

// The executor sets the node's config before it loads the node's resources, so the ids are here
// when LoadResources binds the files to them.
void WasmEffect::SetConfig(const std::string& key, const std::string& value)
{
    if (key == kResourceIdsConfigKey)
    {
        mResourceIdsBySlot = ParseResourceIdsConfig(value);
    }
}

std::string WasmEffect::GetConfig(const std::string& key) const
{
    if (key == "lastError")
    {
        return mLastError;
    }

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

    if (!IsHostingEnabled())
    {
        SetError("Custom Effects are turned off in Settings, so this module was not loaded.");
        return false;
    }

    for (std::size_t i = 0; i < resourceCount; ++i)
    {
        if (IsNetworkPath(paths[i]))
        {
            SetError("WASM effect resources are not loaded from network shares: " + util::PathToUtf8(paths[i]));
            return false;
        }
    }

    for (std::size_t i = 0; i < resourceCount; ++i)
    {
        const std::size_t slotIndex = ParseResourceSlotIndex(refs[i]).value_or(i);

        if (slotIndex > kMaxGuestResources)
        {
            continue;
        }

        // A resource is read into guest memory, so one that could never fit is refused unread.
        if (std::error_code sizeError;
            slotIndex != kModuleSlot &&
            std::filesystem::file_size(paths[i], sizeError) > static_cast<std::uintmax_t>(kMaxGuestMemoryBytes) &&
            !sizeError)
        {
            SetError("WASM effect resource is larger than the " + std::to_string(kMaxGuestMemoryBytes / (1024 * 1024)) +
                     " MB a module's memory can hold: " + util::PathToUtf8(paths[i]));
            return false;
        }

        const auto fileBytes = guitarfx::util::ReadFileBytes(paths[i]);

        if (mResourceBytesBySlot.size() <= slotIndex)
        {
            mResourceBytesBySlot.resize(slotIndex + 1);
        }

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
    {
        return false;
    }

    if (!LoadGuestDescriptor())
    {
        TeardownRuntime();
        return false;
    }

    TeardownRuntime();

    if (mSampleRate > 0.0 && mMaxBlockSize > 0)
    {
        return RebuildRuntime();
    }

    return true;
}

bool WasmEffect::IsHostParamId(std::string_view id)
{
    return std::find(kHostParamIds.begin(), kHostParamIds.end(), id) != kHostParamIds.end();
}

std::int32_t WasmEffect::GuestParamHandle(std::string_view id) const
{
    if (!mHasGuestDescriptor)
    {
        return -1;
    }

    for (std::size_t index = 0; index < mGuestParamCount; ++index)
    {
        if (mGuestDescriptor.parameters[index].definition.id == id)
        {
            return mGuestHandleBase + static_cast<std::int32_t>(index);
        }
    }

    return -1;
}

bool WasmEffect::ReadGuestParamHandle(std::int32_t handle, float& value) const
{
    const std::int64_t index = static_cast<std::int64_t>(handle) - mGuestHandleBase;

    if (!mHasGuestDescriptor || index < 0 || index >= static_cast<std::int64_t>(mGuestParamCount))
    {
        return false;
    }

    value = static_cast<float>(mGuestValues[static_cast<std::size_t>(index)]);
    return true;
}

float WasmEffect::GuestTempoBpm() const
{
    return static_cast<float>(mBpm);
}

std::int32_t WasmEffect::GuestResourceHandle(std::string_view id) const
{
    if (!mHasGuestDescriptor)
    {
        return -1;
    }

    for (std::size_t index = 0; index < mGuestResourceCount; ++index)
    {
        if (mGuestDescriptor.exposedResources[index].resourceId == id)
        {
            return mGuestResourceHandleBase + static_cast<std::int32_t>(index);
        }
    }

    return -1;
}

const std::vector<uint8_t>* WasmEffect::GuestResourceBytes(std::int32_t handle) const
{
    static const std::vector<uint8_t> kNoFile;
    const std::int64_t index = static_cast<std::int64_t>(handle) - mGuestResourceHandleBase;

    if (!mHasGuestDescriptor || index < 0 || index >= static_cast<std::int64_t>(mGuestResourceCount))
    {
        return nullptr;
    }

    const int slot = mGuestResourceSlots[static_cast<std::size_t>(index)];
    return slot > 0 ? &mResourceBytesBySlot[static_cast<std::size_t>(slot)] : &kNoFile;
}

// Each guest resource takes the node resource saved under its id. A slot with no saved id is taken
// as the resource declared at its place, which is where the controller puts each one.
void WasmEffect::BindGuestResources()
{
    mGuestResourceSlots.fill(-1);
    mGuestResourceCount = std::min(mGuestDescriptor.exposedResources.size(), kMaxGuestResources);

    for (std::size_t slot = kModuleSlot + 1; slot < mResourceBytesBySlot.size(); ++slot)
    {
        std::string_view id;

        // A gap between chosen files, which reads as no file anyway.
        if (mResourceBytesBySlot[slot].empty())
        {
            continue;
        }

        if (slot < mResourceIdsBySlot.size() && !mResourceIdsBySlot[slot].empty())
        {
            id = mResourceIdsBySlot[slot];
        }
        else if (slot - 1 < mGuestResourceCount)
        {
            id = mGuestDescriptor.exposedResources[slot - 1].resourceId;
        }

        for (std::size_t index = 0; !id.empty() && index < mGuestResourceCount; ++index)
        {
            if (mGuestResourceSlots[index] < 0 && mGuestDescriptor.exposedResources[index].resourceId == id)
            {
                mGuestResourceSlots[index] = static_cast<int>(slot);
                break;
            }
        }
    }
}

bool WasmEffect::RebuildRuntime()
{
    mRuntimeReady = false;
    mLatencySamples = 0;

    if (!BuildRuntimeOnly())
    {
        return false;
    }

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

    if (!IsHostingEnabled())
    {
        SetError("Custom Effects are turned off in Settings, so this module was not loaded.");
        return false;
    }

    if (!mEngine)
    {
        SetError("Failed to create Wasmtime engine.");
        return false;
    }

    if (mModuleBytes.size() > kMaxModuleBytes)
    {
        SetError("WASM module is larger than the " + std::to_string(kMaxModuleBytes / (1024u * 1024u)) + " MB limit.");
        return false;
    }

    if (wasmtime_error_t* error = wasmtime_module_new(mEngine, mModuleBytes.data(), mModuleBytes.size(), &mModule))
    {
        SetError(std::string("Failed to compile WASM module: ") + TakeErrorMessage(error));
        TeardownRuntime();
        return false;
    }

    if (!CheckModuleImports())
    {
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

    // One instance with bounded memory and tables. Growth past a limit fails inside the guest
    // (memory.grow returns -1); a module that declares more than the limit fails to instantiate.
    wasmtime_store_limiter(mStore, kMaxGuestMemoryBytes, kMaxGuestTableElements, 1, 4, 1);

    mContext = wasmtime_store_context(mStore);

    if (!mContext)
    {
        SetError("Failed to access Wasmtime store context.");
        TeardownRuntime();
        return false;
    }

    const auto defineHostImport = [&](const char* fieldName, FunctionTypePtr&& type,
                                      wasmtime_func_callback_t callback) -> bool {
        if (!type)
        {
            SetError(std::string("Failed to allocate Wasmtime signature for host import '") + fieldName + "'.");
            return false;
        }

        if (wasmtime_error_t* error =
                wasmtime_linker_define_func(mLinker, "host", std::strlen("host"), fieldName, std::strlen(fieldName),
                                            type.get(), callback, this, nullptr))
        {
            SetError(std::string("Failed to link host import '") + fieldName + "': " + TakeErrorMessage(error));
            return false;
        }

        return true;
    };

    if (!defineHostImport("param_handle",
                          MakeFunctionType(wasm_functype_new_2_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(),
                                                                 wasm_valtype_new_i32())),
                          HostParamHandleRaw) ||
        !defineHostImport("param_value",
                          MakeFunctionType(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_f32())),
                          HostParamValueRaw) ||
        !defineHostImport("tempo_bpm", MakeFunctionType(wasm_functype_new_0_1(wasm_valtype_new_f32())),
                          HostTempoBpmRaw) ||
        !defineHostImport("resource_handle",
                          MakeFunctionType(wasm_functype_new_2_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(),
                                                                 wasm_valtype_new_i32())),
                          HostResourceHandleRaw) ||
        !defineHostImport("resource_size",
                          MakeFunctionType(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32())),
                          HostResourceSizeRaw) ||
        !defineHostImport("resource_read", MakeResourceReadType(), HostResourceReadRaw))
    {
        TeardownRuntime();
        return false;
    }

    wasm_trap_t* trap = nullptr;

    // Instantiation runs the module's start function, so it is the first call that needs a deadline.
    ArmGuestDeadline(kSetupBudgetSeconds);

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

    if (!CheckAbiVersion() || !LoadFunctionExport("audiofx_prepare", mPrepareFunction, true, mHasPrepareFunction) ||
        !LoadFunctionExport("audiofx_reset", mResetFunction, true, mHasResetFunction) ||
        !LoadFunctionExport("audiofx_process", mProcessFunction, true, mHasProcessFunction) ||
        !LoadFunctionExport("audiofx_get_latency_samples", mLatencyFunction, false, mHasLatencyFunction))
    {
        TeardownRuntime();
        return false;
    }

    return true;
}

// Before linking, so a module written for another interface is refused by name instead of by
// Wasmtime's "unknown import".
bool WasmEffect::CheckModuleImports()
{
    static constexpr std::array<std::string_view, 6> kHostImports{"param_handle",    "param_value",   "tempo_bpm",
                                                                  "resource_handle", "resource_size", "resource_read"};
    static constexpr std::array<std::string_view, 3> kAbi1Imports{"read_param", "read_resource_size",
                                                                  "read_resource_byte"};

    wasm_importtype_vec_t imports;
    wasmtime_module_imports(mModule, &imports);
    std::string problem;

    for (std::size_t i = 0; i < imports.size && problem.empty(); ++i)
    {
        const std::string moduleName = CopyWasmMessage(*wasm_importtype_module(imports.data[i]));
        const std::string fieldName = CopyWasmMessage(*wasm_importtype_name(imports.data[i]));

        if (moduleName == "host" &&
            std::find(kAbi1Imports.begin(), kAbi1Imports.end(), fieldName) != kAbi1Imports.end())
        {
            problem = "WASM module was written for Custom Effect ABI 1 (it imports host." + fieldName + ")." +
                      kAbiMigrationHint;
        }
        else if (moduleName != "host" ||
                 std::find(kHostImports.begin(), kHostImports.end(), fieldName) == kHostImports.end())
        {
            problem = "WASM module imports '" + moduleName + "." + fieldName +
                      "', which a Custom Effect is not given. The host provides host.param_handle, "
                      "host.param_value, host.tempo_bpm, host.resource_handle, host.resource_size and "
                      "host.resource_read.";
        }
    }

    wasm_importtype_vec_delete(&imports);

    if (!problem.empty())
    {
        SetError(problem);
        return false;
    }

    return true;
}

bool WasmEffect::CheckAbiVersion()
{
    wasmtime_func_t abiFunction{};
    bool hasAbiFunction = false;

    if (!LoadFunctionExport("audiofx_abi_version", abiFunction, false, hasAbiFunction))
    {
        return false;
    }

    if (!hasAbiFunction)
    {
        SetError(std::string("WASM module does not export audiofx_abi_version, so it was written for Custom Effect "
                             "ABI 1.") +
                 kAbiMigrationHint);
        return false;
    }

    const auto version = CallGuestI32(abiFunction, "audiofx_abi_version");

    if (!version)
    {
        return false;
    }

    if (*version != kAbiVersion)
    {
        SetError("WASM module declares Custom Effect ABI " + std::to_string(*version) + "; this host runs ABI " +
                 std::to_string(kAbiVersion) + ".");
        return false;
    }

    return true;
}

std::optional<std::int32_t> WasmEffect::CallGuestI32(wasmtime_func_t& function, const char* functionName)
{
    std::array<wasmtime_val_t, 1> results{};
    wasm_trap_t* trap = nullptr;
    ArmGuestDeadline(kSetupBudgetSeconds);

    if (wasmtime_error_t* error =
            wasmtime_func_call(mContext, &function, nullptr, 0, results.data(), results.size(), &trap))
    {
        SetError(std::string("WASM call to '") + functionName + "' failed: " + TakeErrorMessage(error));
        return std::nullopt;
    }

    if (trap)
    {
        SetError(std::string("WASM call to '") + functionName + "' trapped: " + TakeTrapMessage(trap));
        return std::nullopt;
    }

    if (results[0].kind != WASMTIME_I32)
    {
        SetError(std::string("WASM function '") + functionName + "' returned a non-i32 result.");
        return std::nullopt;
    }

    return results[0].of.i32;
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
    {
        return false;
    }

    std::array<wasmtime_val_t, 2> args{
        MakeF32Value(static_cast<float>(mSampleRate)),
        MakeI32Value(static_cast<int32_t>(mMaxBlockSize)),
    };
    std::array<wasmtime_val_t, 1> results{};

    wasm_trap_t* trap = nullptr;
    ArmGuestDeadline(kSetupBudgetSeconds);

    if (wasmtime_error_t* error = wasmtime_func_call(mContext, &mPrepareFunction, args.data(), args.size(),
                                                     results.data(), results.size(), &trap))
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
    {
        return false;
    }

    std::array<wasmtime_val_t, 2> args{
        MakeF32Value(inputLeft),
        MakeF32Value(inputRight),
    };
    std::array<wasmtime_val_t, 2> results{};

    wasm_trap_t* trap = nullptr;

    if (wasmtime_error_t* error = wasmtime_func_call(mContext, &mProcessFunction, args.data(), args.size(),
                                                     results.data(), results.size(), &trap))
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
    {
        return 0;
    }

    std::array<wasmtime_val_t, 1> results{};

    wasm_trap_t* trap = nullptr;
    ArmGuestDeadline(kSetupBudgetSeconds);

    if (wasmtime_error_t* error =
            wasmtime_func_call(mContext, &mLatencyFunction, nullptr, 0, results.data(), results.size(), &trap))
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

    // Every module describes itself: the descriptor is the only place its param ids come from.
    wasmtime_func_t descriptorPtrFunction{};
    wasmtime_func_t descriptorLenFunction{};
    bool hasDescriptorPtr = false;
    bool hasDescriptorLen = false;

    if (!LoadFunctionExport("audiofx_descriptor_ptr", descriptorPtrFunction, true, hasDescriptorPtr) ||
        !LoadFunctionExport("audiofx_descriptor_len", descriptorLenFunction, true, hasDescriptorLen))
    {
        return false;
    }

    const auto descriptorPtr = CallGuestI32(descriptorPtrFunction, "audiofx_descriptor_ptr");
    const auto descriptorLen = CallGuestI32(descriptorLenFunction, "audiofx_descriptor_len");

    if (!descriptorPtr || !descriptorLen)
    {
        return false;
    }

    if (*descriptorPtr < 0 || *descriptorLen < 0)
    {
        SetError("WASM descriptor pointer and length must be non-negative.");
        return false;
    }

    if (*descriptorLen == 0)
    {
        ApplyGuestDescriptor({});
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

    if (descriptorSize > kMaxDescriptorBytes)
    {
        wasmtime_extern_delete(&memoryItem);
        SetError("WASM descriptor is larger than the 2 MB limit.");
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

// Runs only off the audio thread (construction, LoadResources, rebuilding the runtime). The
// values set so far are kept by id, so a rebuild, or a new version of the module that keeps an
// id, picks them up again in ApplyGuestDescriptor.
void WasmEffect::ResetGuestDescriptor()
{
    for (std::size_t index = 0; mHasGuestDescriptor && index < mGuestParamCount; ++index)
    {
        mPendingGuestParamValues[mGuestDescriptor.parameters[index].definition.id] = mGuestValues[index];
    }

    mGuestDescriptor = {};
    mGuestParamIndex.clear();
    mGuestParamCount = 0;
    mGuestResourceCount = 0;
    mGuestResourceSlots.fill(-1);
    mHasGuestDescriptor = false;
    mGuestValues.fill(0.0);
}

void WasmEffect::ApplyGuestDescriptor(const WasmModuleDescriptor& descriptor)
{
    ResetGuestDescriptor();
    mGuestDescriptor = descriptor;
    mGuestParamCount = std::min(descriptor.parameters.size(), kMaxGuestParams);

    for (std::size_t index = 0; index < mGuestParamCount; ++index)
    {
        const auto& definition = descriptor.parameters[index].definition;
        const auto pendingIt = mPendingGuestParamValues.find(definition.id);
        mGuestValues[index] = pendingIt != mPendingGuestParamValues.end() ? pendingIt->second : definition.defaultValue;
        mGuestParamIndex[definition.id] = index;
    }

    mPendingGuestParamValues.clear();
    BindGuestResources();

    // Fresh bases for every instance; ids are what stay the same.
    std::random_device device;
    std::uniform_int_distribution<std::int32_t> base(1 << 16, 1 << 30);
    mGuestHandleBase = base(device);

    do
    {
        mGuestResourceHandleBase = base(device);
    } while (std::abs(static_cast<std::int64_t>(mGuestResourceHandleBase) - mGuestHandleBase) <
             static_cast<std::int64_t>(kMaxGuestParams + kMaxGuestResources));

    mHasGuestDescriptor = true;
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
    const auto makeParam = [](std::string id, std::string displayName, double minValue, double maxValue,
                              double defaultValue, std::string unit, double step, std::string group = {},
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
    info.description = "Runs a stereo WASM effect that describes its own parameters and optional resources.";
    info.category = "utility";
    info.requiresResource = true;
    info.requiresTempo = true;
    info.resourceType = "wasm";
    info.aliases = {"wasm_host"};
    info.parameters = {
        makeParam("mix", "Mix", 0.0, 1.0, 1.0, "", 0.01, "Host"),
        makeParam("inputGainDb", "Input Gain", -24.0, 24.0, 0.0, "dB", 0.1, "Host", true),
        makeParam("outputGainDb", "Output Gain", -24.0, 24.0, 0.0, "dB", 0.1, "Host", true),
    };
    info.exposedResources = {
        {"module", "Module", "", "wasm", 0, true, {}, std::nullopt},
    };

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<WasmEffect>(); });
}
} // namespace guitarfx