/**
 * AutomationSlotTable.cpp — Manages all automation slots and the apply dispatch.
 *
 * Loading and saving the slots as JSON is in AutomationSlotTableJson.cpp.
 */

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectProcessor.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
namespace
{
bool IsBypassParam(const std::string& paramId)
{
    return paramId == "bypassed" || paramId == "bypass" || paramId == "enabled";
}

/// Whether `node.input.gainDb` / `node.output.gainDb` — the chain's Input and Output nodes.
/// They are routing, not effects, so the EffectRegistry declares nothing for them and the
/// range for their one parameter comes from PresetTypes.h instead.
bool IsBoundaryGainNodeParam(const std::string& effectType, const std::string& paramId)
{
    return paramId == kBoundaryGainParam && (effectType == kNodeTypeInput || effectType == kNodeTypeOutput);
}

/// Maps a slot's 0..1 value onto a parameter's range along its taper, snapped to the range's
/// step, or to a whole index for an enum. A log taper sweeps a frequency by octaves, so an
/// expression pedal spends its travel evenly across the range as it is heard.
double DenormalizeNodeParam(const ParamRange& range, bool isEnum, double normalized,
                            ParamTaper taper = ParamTaper::Linear)
{
    double native = TaperPositionToValue(taper, range.minValue, range.maxValue, normalized);

    if (range.step > 0.0)
    {
        native = range.minValue + std::round((native - range.minValue) / range.step) * range.step;
    }
    else if (isEnum)
    {
        native = std::round(native);
    }

    return std::clamp(native, std::min(range.minValue, range.maxValue), std::max(range.minValue, range.maxValue));
}

/// Whether a mapping listens to this MIDI control. A NoteOn mapping also hears its key's
/// release, which arrives as NoteOff.
bool ListensTo(const MidiControlMap& map, MidiControlMap::EventType eventType, int channel, int controller)
{
    const bool typeMatches = map.eventType == eventType || (map.eventType == MidiControlMap::EventType::NoteOn &&
                                                            eventType == MidiControlMap::EventType::NoteOff);
    return typeMatches && map.controller == controller && (map.channel == -1 || map.channel == channel);
}
} // namespace

// ── AutomationSlot copy helpers ──────────────────────────────────────────
//
// A copy is not in the table, so it does not take dawValue: the constructor leaves it unset and
// assignment keeps the target's. A move does take it, which is what keeps a slot joined to its
// DAW parameter as the vector reallocates or erases around it.

AutomationSlot::AutomationSlot(const AutomationSlot& other)
    : slotId(other.slotId), label(other.label), address(other.address), nodeSelector(other.nodeSelector),
      isDefault(other.isDefault), presetId(other.presetId), midiMap(other.midiMap), keyMaps(other.keyMaps),
      nodeBinding(other.nodeBinding), value(other.value.load()), lastSource(other.lastSource.load()),
      lastNormalized(other.lastNormalized.load()), lastToggleGate(other.lastToggleGate.load()),
      pendingApply(other.pendingApply.load())
{
}

AutomationSlot& AutomationSlot::operator=(const AutomationSlot& other)
{
    if (this != &other)
    {
        slotId = other.slotId;
        label = other.label;
        address = other.address;
        nodeSelector = other.nodeSelector;
        isDefault = other.isDefault;
        presetId = other.presetId;
        midiMap = other.midiMap;
        keyMaps = other.keyMaps;
        nodeBinding = other.nodeBinding;
        StoreValue(other.value.load());
        lastSource.store(other.lastSource.load());
        lastNormalized.store(other.lastNormalized.load());
        lastToggleGate.store(other.lastToggleGate.load());
        pendingApply.store(other.pendingApply.load());
    }

    return *this;
}

void AutomationSlot::SetValue(float normalized, AutomationSource src)
{
    StoreValue(normalized);
    lastSource.store(static_cast<int>(src));
    pendingApply.store(true);
}

void AutomationSlot::StoreValue(float normalized)
{
    value.store(normalized);

    if (dawValue)
    {
        dawValue->store(normalized, std::memory_order_relaxed);
    }
}

// ── AutomationSlotTable ───────────────────────────────────────────────────

std::vector<AutomationSlot> AutomationSlotTable::MakeDefaultSlots()
{
    std::vector<AutomationSlot> slots;
    slots.reserve(std::size(kDefaultSlots));

    for (const auto& def : kDefaultSlots)
    {
        AutomationSlot slot;
        slot.slotId = def.slotId;
        slot.address = def.address;
        slot.label = def.label;
        slot.isDefault = true;
        slots.push_back(std::move(slot));
    }

    return slots;
}

AutomationSlotTable::AutomationSlotTable() : mSlots(MakeDefaultSlots())
{
}

AutomationSlotTable::~AutomationSlotTable() = default;

void AutomationSlotTable::InitializeRegistry(
    MultiPresetMixer& mixer, const std::function<double()>& getSetlistCursor,
    const std::function<void(int)>& applySetlistPresetByIndex, const std::function<void(int)>& bankUp,
    const std::function<void(int)>& bankDown, const std::function<int()>& getSetlistLength,
    const std::function<int()>& getSetlistBankBase, const std::function<void(int)>& selectSetlistBank,
    const std::function<int()>& getSetlistBankNumber, const std::function<void(int)>& selectSceneByIndex,
    const std::function<int()>& getActiveSceneIndex)
{
    mMixer = &mixer;
    mGetSetlistCursor = getSetlistCursor;
    mApplySetlistPresetByIndex = applySetlistPresetByIndex;
    mBankUp = bankUp;
    mBankDown = bankDown;
    mGetSetlistLength = getSetlistLength;
    mGetSetlistBankBase = getSetlistBankBase;
    mSelectSetlistBank = selectSetlistBank;
    mGetSetlistBankNumber = getSetlistBankNumber;
    mSelectSceneByIndex = selectSceneByIndex;
    mGetActiveSceneIndex = getActiveSceneIndex;

    // global.inputTrim
    {
        ParamRegistryEntry e;
        e.address = "global.inputTrim";
        e.label = "Input Trim";
        e.unit = "dB";
        e.minValue = -40.0;
        e.maxValue = 20.0;
        e.get = [this]() { return mMixer ? mMixer->GetGlobalChainConfig().inputGain : 0.0; };
        e.apply = [this](double v, bool) {
            if (mMixer)
            {
                mMixer->SetGlobalInputGain(v);
            }
        };
        mRegistry.Register(e);
    }

    // global.outputTrim
    {
        ParamRegistryEntry e;
        e.address = "global.outputTrim";
        e.label = "Output Trim";
        e.unit = "dB";
        e.minValue = -40.0;
        e.maxValue = 20.0;
        e.get = [this]() { return mMixer ? mMixer->GetGlobalChainConfig().outputGain : 0.0; };
        e.apply = [this](double v, bool) {
            if (mMixer)
            {
                mMixer->SetGlobalOutputGain(v);
            }
        };
        mRegistry.Register(e);
    }

    // setlist.preset1..N — one trigger parameter per preset slot in the current bank
    for (int i = 1; i <= kSetlistPresetsPerBank; ++i)
    {
        ParamRegistryEntry e;
        e.address = "setlist.preset" + std::to_string(i);
        e.label = "Setlist Preset " + std::to_string(i);
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        const int slotOffset = i; // captured by value
        e.get = [this, slotOffset]() -> double {
            if (!mGetSetlistBankBase || !mGetSetlistCursor)
            {
                return 0.0;
            }

            const int base = mGetSetlistBankBase();
            const int cursor = static_cast<int>(std::round(mGetSetlistCursor()));
            return (cursor == base + (slotOffset - 1)) ? 1.0 : 0.0;
        };
        e.apply = [this, slotOffset](double, bool) {
            if (!mGetSetlistBankBase || !mApplySetlistPresetByIndex)
            {
                return;
            }

            const int base = mGetSetlistBankBase();
            mApplySetlistPresetByIndex(base + (slotOffset - 1));
        };
        mRegistry.Register(e);
    }

    // setlist.bankUp
    {
        ParamRegistryEntry e;
        e.address = "setlist.bankUp";
        e.label = "Bank Up";
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        e.get = []() { return 0.0; };
        e.apply = [this](double, bool) {
            if (mBankUp)
            {
                mBankUp(1);
            }
        };
        mRegistry.Register(e);
    }

    // setlist.bankDown
    {
        ParamRegistryEntry e;
        e.address = "setlist.bankDown";
        e.label = "Bank Down";
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        e.get = []() { return 0.0; };
        e.apply = [this](double, bool) {
            if (mBankDown)
            {
                mBankDown(1);
            }
        };
        mRegistry.Register(e);
    }

    // setlist.bankSelect — select a specific bank number (0..127) directly.
    // The value maps onto the bank number, selecting the setlist whose `bank`
    // field matches. A MIDI CC (0..127) therefore picks the bank directly.
    {
        ParamRegistryEntry e;
        e.address = "setlist.bankSelect";
        e.label = "Select Bank";
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 127.0;
        e.isStepped = true;
        e.get = [this]() -> double {
            return mGetSetlistBankNumber ? static_cast<double>(mGetSetlistBankNumber()) : 0.0;
        };
        e.apply = [this](double v, bool) {
            if (mSelectSetlistBank)
            {
                mSelectSetlistBank(static_cast<int>(std::round(v)));
            }
        };
        mRegistry.Register(e);
    }

    // scene.select1..N — one trigger parameter per scene of the *active* preset.
    // Selection is handled entirely in the controller so a footswitch still
    // switches scenes when the plugin editor is closed and no UI exists.
    for (int i = 1; i <= kSceneSelectSlots; ++i)
    {
        ParamRegistryEntry e;
        e.address = "scene.select" + std::to_string(i);
        e.label = "Scene " + std::to_string(i);
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        const int sceneOffset = i; // captured by value
        e.get = [this, sceneOffset]() -> double {
            if (!mGetActiveSceneIndex)
            {
                return 0.0;
            }

            return mGetActiveSceneIndex() == (sceneOffset - 1) ? 1.0 : 0.0;
        };
        e.apply = [this, sceneOffset](double, bool) {
            if (mSelectSceneByIndex)
            {
                mSelectSceneByIndex(sceneOffset - 1);
            }
        };
        mRegistry.Register(e);
    }
}

void AutomationSlotTable::CommitSlots(std::vector<AutomationSlot>& slots)
{
    mSlots.swap(slots);

    // Each new slot publishes its value, then a parameter whose slot has gone reads 0. In that
    // order, and one store per cell, so a host reading meanwhile never sees a value in between.
    for (auto& slot : mSlots)
    {
        AttachDawValue(slot);
    }

    for (auto& replaced : slots)
    {
        const bool claimed = std::any_of(mSlots.begin(), mSlots.end(), [&replaced](const AutomationSlot& slot) {
            return slot.dawValue == replaced.dawValue;
        });

        if (!claimed)
        {
            DetachDawValue(replaced);
        }

        // No longer in the table, though only freed later.
        replaced.dawValue = nullptr;
    }
}

// ── DAW parameters ───────────────────────────────────────────────────────

void AutomationSlotTable::BindDawParameters(const std::vector<std::string>& slotIds)
{
    if (mDawValues)
    {
        return;
    }

    mDawValueCount = slotIds.size();
    mDawValues = std::make_unique<std::atomic<float>[]>(mDawValueCount);

    for (std::size_t i = 0; i < mDawValueCount; ++i)
    {
        mDawValueBySlotId.emplace(slotIds[i], &mDawValues[i]);
    }

    for (auto& slot : mSlots)
    {
        AttachDawValue(slot);
    }
}

float AutomationSlotTable::GetDawParameterValue(int parameterIndex) const
{
    if (parameterIndex < 0 || static_cast<std::size_t>(parameterIndex) >= mDawValueCount)
    {
        return 0.0f;
    }

    return mDawValues[static_cast<std::size_t>(parameterIndex)].load(std::memory_order_relaxed);
}

void AutomationSlotTable::AttachDawValue(AutomationSlot& slot)
{
    const auto it = mDawValueBySlotId.find(slot.slotId);
    slot.dawValue = it != mDawValueBySlotId.end() ? it->second : nullptr;
    slot.StoreValue(slot.value.load());
}

void AutomationSlotTable::DetachDawValue(AutomationSlot& slot)
{
    if (slot.dawValue)
    {
        slot.dawValue->store(0.0f, std::memory_order_relaxed);
        slot.dawValue = nullptr;
    }
}

// ── Slot queries ─────────────────────────────────────────────────────────

AutomationSlot* AutomationSlotTable::FindSlot(const std::string& slotId)
{
    for (auto& s : mSlots)
    {
        if (s.slotId == slotId)
        {
            return &s;
        }
    }

    return nullptr;
}

const AutomationSlot* AutomationSlotTable::FindSlot(const std::string& slotId) const
{
    for (const auto& s : mSlots)
    {
        if (s.slotId == slotId)
        {
            return &s;
        }
    }

    return nullptr;
}

std::vector<std::string> AutomationSlotTable::GetSlotIds() const
{
    std::vector<std::string> ids;
    ids.reserve(mSlots.size());

    for (const auto& s : mSlots)
    {
        if (s.presetId.empty())
        {
            ids.push_back(s.slotId);
        }
    }

    return ids;
}

std::vector<ParamRegistryInfo> AutomationSlotTable::GetRegistryInfo() const
{
    return mRegistry.GetAllInfo();
}

// ── Slot management ──────────────────────────────────────────────────────

bool AutomationSlotTable::SetCustomSlot(const std::string& slotId, const std::optional<std::string>& label,
                                        const std::optional<std::string>& address,
                                        const std::optional<std::string>& nodeSelector,
                                        const std::optional<MidiControlMap>& midiMap,
                                        const std::optional<std::vector<KeyboardMap>>& keyMaps)
{
    auto* slot = FindSlot(slotId);

    if (slot && (slot->isDefault || !slot->presetId.empty()))
    {
        return false; // Defaults keep their address; per-preset slots go through SetPresetSlot
    }

    if (!slot)
    {
        // Check max custom slots
        int customCount = 0;

        for (const auto& s : mSlots)
        {
            if (!s.isDefault && s.presetId.empty())
            {
                ++customCount;
            }
        }

        if (customCount >= kMaxCustomSlots)
        {
            return false;
        }

        AutomationSlot newSlot;
        newSlot.slotId = slotId;
        newSlot.isDefault = false;

        if (label)
        {
            newSlot.label = *label;
        }

        if (address)
        {
            newSlot.address = *address;
            newSlot.nodeBinding = BindNodeAddress(*address);
        }

        if (nodeSelector)
        {
            newSlot.nodeSelector = *nodeSelector;
        }

        if (midiMap)
        {
            newSlot.midiMap = *midiMap;
        }

        if (keyMaps)
        {
            newSlot.keyMaps = *keyMaps;
        }

        mSlots.push_back(std::move(newSlot));
        AttachDawValue(mSlots.back());
        return true;
    }

    if (label)
    {
        slot->label = *label;
    }

    if (address)
    {
        slot->address = *address;
        slot->nodeBinding = BindNodeAddress(*address);
    }

    if (nodeSelector)
    {
        slot->nodeSelector = *nodeSelector;
    }

    if (midiMap)
    {
        slot->midiMap = *midiMap;
    }

    if (keyMaps)
    {
        slot->keyMaps = *keyMaps;
    }

    return true;
}

bool AutomationSlotTable::SetDefaultSlotOverrides(const std::string& slotId, const std::optional<std::string>& label,
                                                  const std::optional<MidiControlMap>& midiMap,
                                                  const std::optional<std::vector<KeyboardMap>>& keyMaps)
{
    auto* slot = FindSlot(slotId);

    if (!slot || !slot->isDefault)
    {
        return false;
    }

    if (label)
    {
        slot->label = *label;
    }

    if (midiMap)
    {
        slot->midiMap = *midiMap;
    }

    if (keyMaps)
    {
        slot->keyMaps = *keyMaps;
    }

    return true;
}

bool AutomationSlotTable::RemoveCustomSlot(const std::string& slotId)
{
    for (auto it = mSlots.begin(); it != mSlots.end(); ++it)
    {
        if (it->slotId == slotId && !it->isDefault)
        {
            DetachDawValue(*it);
            mSlots.erase(it);
            return true;
        }
    }

    return false;
}

bool AutomationSlotTable::SetPresetSlot(const std::string& slotId, const std::string& presetId,
                                        const std::optional<std::string>& label,
                                        const std::optional<std::string>& address,
                                        const std::optional<MidiControlMap>& midiMap)
{
    auto* slot = FindSlot(slotId);

    if (!slot)
    {
        if (slotId.empty() || presetId.empty() || CountPresetSlots(presetId) >= kMaxPresetSlotsPerPreset)
        {
            return false;
        }

        AutomationSlot newSlot;
        newSlot.slotId = slotId;
        newSlot.presetId = presetId;
        mSlots.push_back(std::move(newSlot));
        slot = &mSlots.back();
        AttachDawValue(*slot);
    }
    else if (slot->presetId.empty())
    {
        return false; // A default or custom slot does not become a per-preset one
    }

    if (label)
    {
        slot->label = *label;
    }

    if (address)
    {
        slot->address = *address;
        slot->nodeBinding = BindNodeAddress(*address);
    }

    if (midiMap)
    {
        slot->midiMap = *midiMap;
    }

    return true;
}

int AutomationSlotTable::RemovePresetSlots(const std::string& presetId)
{
    if (presetId.empty())
    {
        return 0;
    }

    for (auto& slot : mSlots)
    {
        if (slot.presetId == presetId)
        {
            DetachDawValue(slot);
        }
    }

    return static_cast<int>(
        std::erase_if(mSlots, [&presetId](const AutomationSlot& slot) { return slot.presetId == presetId; }));
}

int AutomationSlotTable::CountPresetSlots(const std::string& presetId) const
{
    return static_cast<int>(std::count_if(mSlots.begin(), mSlots.end(), [&presetId](const AutomationSlot& slot) {
        return !presetId.empty() && slot.presetId == presetId;
    }));
}

// ── Apply path ────────────────────────────────────────────────────────────

bool AutomationSlotTable::ApplyAutomationLocked(const std::string& slotId, float normalized, AutomationSource src)
{
    auto* slot = FindSlot(slotId);

    if (!slot)
    {
        return false;
    }

    slot->SetValue(normalized, src);
    return ApplySlotLocked(*slot);
}

bool AutomationSlotTable::ApplySlotLocked(AutomationSlot& slot)
{
    if (slot.address.empty())
    {
        return false; // Unmapped slot
    }

    if (slot.nodeBinding)
    {
        return ApplyNodeSlotLocked(slot);
    }

    // Static registry entry (global.* / setlist.*). A node.* address with no binding did not
    // parse, and the registry holds none.
    const auto* entry = mRegistry.Find(slot.address);

    if (!entry)
    {
        return false;
    }

    const float normalized = slot.value.load();

    const double maxVal = entry->maxValue;

    const double native = entry->isStepped ? std::round(entry->minValue + normalized * (maxVal - entry->minValue))
                                           : entry->minValue + normalized * (maxVal - entry->minValue);

    // Trigger edge detection
    if (entry->isTrigger)
    {
        const float prev = slot.lastNormalized.load();
        slot.lastNormalized.store(normalized);
        const bool fire = (prev < 0.5f) && (normalized >= 0.5f);

        if (!fire)
        {
            return false;
        }

        // Reset both value and lastNormalized so the trigger can fire again
        // on the next rising edge. Without this, a sustained/high MIDI value
        // or repeated Test button presses would prevent retriggering.
        slot.StoreValue(0.0f);
        slot.lastNormalized.store(0.0f);
    }
    else
    {
        slot.lastNormalized.store(normalized);
    }

    if (entry->apply)
    {
        entry->apply(native, true);
        return true;
    }

    return false;
}

bool AutomationSlotTable::ApplyNodeSlotLocked(AutomationSlot& slot)
{
    if (!mMixer)
    {
        return false;
    }

    const NodeAddressBinding& binding = *slot.nodeBinding;
    const float normalized = slot.value.load();

    if (binding.isBypass)
    {
        const bool enabled = binding.highMeansEnabled ? normalized >= 0.5f : normalized < 0.5f;

        if (!mMixer->SetAutomatedNodesEnabled(binding.effectType, enabled))
        {
            return false;
        }

        mNodeChanges.Post(slot.nodeBinding, nullptr, enabled ? 1.0 : 0.0);
        return true;
    }

    const auto target = mMixer->FindAutomationTarget(binding.effectType);

    if (!target)
    {
        return false;
    }

    // Every source hands a slot a 0..1 value, but an effect takes its parameters in native
    // units. A parameter the effect does not declare has no range to map onto, so it keeps the
    // 0..1 value. The node may narrow the declared range with its own settings, as a pitch shift
    // does to bound an expression pedal's sweep; the declared taper then applies across the
    // narrowed range.
    double native = static_cast<double>(normalized);

    if (binding.hasRange)
    {
        ParamRange range{binding.minValue, binding.maxValue, binding.step};

        if (binding.nodeMayNarrowRange && target.processor)
        {
            (void)target.processor->GetAutomationRange(binding.paramId, range); // Leaves it alone if not
        }

        native = DenormalizeNodeParam(range, binding.isEnum, native, binding.taper);
    }

    SignalGraphExecutor::SetAutomationTargetParam(target, binding.paramId, native);
    mNodeChanges.Post(slot.nodeBinding, target.id, native);
    return true;
}

// ── node.* address binding ───────────────────────────────────────────────

const EffectRegistry& AutomationSlotTable::TypeRegistry() const
{
    // Without one of its own, the table resolves types as the executor matches them.
    return mEffectRegistry ? *mEffectRegistry : EffectRegistry::Instance();
}

void AutomationSlotTable::SetEffectRegistry(const EffectRegistry* registry)
{
    mEffectRegistry = registry;

    for (auto& slot : mSlots)
    {
        slot.nodeBinding = BindNodeAddress(slot.address);
    }

    mNodeBindingsGeneration = TypeRegistry().GetGeneration();
}

std::shared_ptr<const NodeAddressBinding> AutomationSlotTable::BindNodeAddress(const std::string& address) const
{
    std::string effectType;
    std::string paramId;

    if (!ParamRegistry::ParseNodeAddress(address, effectType, paramId))
    {
        return nullptr;
    }

    auto binding = std::make_shared<NodeAddressBinding>();
    binding->effectType = TypeRegistry().Resolve(effectType);
    binding->paramId = paramId;

    if (IsBypassParam(paramId))
    {
        binding->isBypass = true;
        binding->highMeansEnabled = paramId == "enabled";
        return binding;
    }

    if (const auto* def = mEffectRegistry ? mEffectRegistry->FindParameter(binding->effectType, paramId) : nullptr)
    {
        binding->hasRange = true;
        binding->minValue = def->minValue;
        binding->maxValue = def->maxValue;
        binding->step = def->step;
        binding->isEnum = !def->labels.empty();
        binding->taper = def->taper;
        binding->nodeMayNarrowRange = true;
    }
    else if (IsBoundaryGainNodeParam(binding->effectType, paramId))
    {
        binding->hasRange = true;
        binding->minValue = kBoundaryGainMinDb;
        binding->maxValue = kBoundaryGainMaxDb;
    }

    return binding;
}

bool AutomationSlotTable::NodeBindingsStale() const
{
    return TypeRegistry().GetGeneration() != mNodeBindingsGeneration;
}

AutomationSlotTable::NodeBindings AutomationSlotTable::ResolveNodeBindings() const
{
    NodeBindings bindings;
    bindings.registryGeneration = TypeRegistry().GetGeneration();
    bindings.bySlot.reserve(mSlots.size());

    for (const auto& slot : mSlots)
    {
        bindings.bySlot.push_back(BindNodeAddress(slot.address));
    }

    return bindings;
}

void AutomationSlotTable::CommitNodeBindings(NodeBindings& bindings)
{
    // The message thread is the only one that changes the slots, so this only guards against
    // a caller that changed them in between.
    if (bindings.bySlot.size() != mSlots.size())
    {
        return;
    }

    for (std::size_t i = 0; i < mSlots.size(); ++i)
    {
        mSlots[i].nodeBinding.swap(bindings.bySlot[i]);
    }

    mNodeBindingsGeneration = bindings.registryGeneration;
}

std::string AutomationSlotTable::ResolveNodeAddress(const std::string& address, const std::string& nodeSelector) const
{
    if (!mMixer)
    {
        return {};
    }

    std::string effectType, paramId;

    if (!ParamRegistry::ParseNodeAddress(address, effectType, paramId))
    {
        return {};
    }

    if (mEffectRegistry)
    {
        effectType = mEffectRegistry->Resolve(effectType);
    }

    const auto found = mMixer->FindFirstEnabledNodeOfType(effectType);

    if (!found)
    {
        return {};
    }

    // v1: nodeSelector is ignored — always returns first. Future: interpret selector.
    return found->second;
}

// ── MIDI handling ─────────────────────────────────────────────────────────

void AutomationSlotTable::HandleMidi(const MidiEvent& ev)
{
    const int channel = ev.status & 0x0F;
    const int statusType = (ev.status >> 4) & 0x0F;

    MidiControlMap::EventType eventType;
    int controller = 0;
    int dataValue = 0;

    switch (statusType)
    {
    case 0x0B: // CC
        eventType = MidiControlMap::EventType::CC;
        controller = ev.data1;
        dataValue = ev.data2;
        break;
    case 0x0C: // Program Change
        eventType = MidiControlMap::EventType::ProgramChange;
        controller = ev.data1;
        dataValue = ev.data2; // typically 0 for PC
        break;
    case 0x09: // Note On
        // MIDI spec: Note On with velocity 0 is equivalent to Note Off.
        eventType = (ev.data2 == 0) ? MidiControlMap::EventType::NoteOff : MidiControlMap::EventType::NoteOn;
        controller = ev.data1;
        dataValue = ev.data2; // velocity
        break;
    case 0x08: // Note Off
        eventType = MidiControlMap::EventType::NoteOff;
        controller = ev.data1;
        dataValue = ev.data2;
        break;
    case 0x0E: // Pitch Bend
        eventType = MidiControlMap::EventType::PitchBend;
        controller = 0;
        dataValue = (ev.data2 << 7) | ev.data1;
        break;
    default:
        return;
    }

    // MIDI learn: capture the next incoming event for the armed slot
    if (mMidiLearnSlotId.has_value() && !mMidiLearnCapture.has_value())
    {
        // Only capture CC, ProgramChange, NoteOn, and PitchBend for learning
        // (NoteOff is not useful as a controller)
        if (eventType == MidiControlMap::EventType::NoteOff)
        {
            return;
        }

        MidiControlMap captured;
        captured.eventType = eventType;
        captured.channel = channel;
        captured.controller = controller;
        captured.mode = MidiControlMap::Mode::Absolute;
        captured.sensitivity = 0.1f;
        captured.pickupRange = 0.1f;
        mMidiLearnCapture = captured;
        return;
    }

    // A mapping made for the active preset takes its MIDI control over from any global mapping
    // on the same control, so one pedal can do a different job in each preset.
    const auto isLivePresetSlot = [this](const AutomationSlot& slot) {
        return !slot.presetId.empty() && slot.presetId == mActivePresetId;
    };
    const bool presetOwnsControl = std::any_of(mSlots.begin(), mSlots.end(), [&](const AutomationSlot& slot) {
        return isLivePresetSlot(slot) && slot.midiMap && ListensTo(*slot.midiMap, eventType, channel, controller);
    });

    // Match against all slot MIDI maps
    for (auto& slot : mSlots)
    {
        if (!slot.midiMap.has_value())
        {
            continue;
        }

        const auto& mm = slot.midiMap.value();

        if (slot.presetId.empty() ? presetOwnsControl && ListensTo(mm, eventType, channel, controller)
                                  : !isLivePresetSlot(slot))
        {
            continue;
        }

        if (mm.channel != -1 && mm.channel != channel)
        {
            continue;
        }

        if (mm.controller != controller)
        {
            continue;
        }

        const bool isNoteOnToggleRelease = (mm.mode == MidiControlMap::Mode::Toggle) &&
                                           (mm.eventType == MidiControlMap::EventType::NoteOn) &&
                                           (eventType == MidiControlMap::EventType::NoteOff);

        if (isNoteOnToggleRelease)
        {
            // NoteOn mappings receive releases as NoteOff/NoteOn(vel=0):
            // reset the gate so the next key press can fire, but do not toggle on release.
            slot.lastToggleGate.store(false);
            continue;
        }

        if (mm.eventType != eventType)
        {
            continue;
        }

        float normalized = 0.0f;
        const bool isBypassAddress = slot.nodeBinding && slot.nodeBinding->isBypass;

        switch (mm.mode)
        {
        case MidiControlMap::Mode::Absolute:

            if (isBypassAddress && (eventType == MidiControlMap::EventType::NoteOn ||
                                    eventType == MidiControlMap::EventType::ProgramChange))
            {
                // Discrete events commonly carry only "on" values. For bypass
                // targets, flip state so both directions work from a single press.
                normalized = (slot.value.load() < 0.5f) ? 1.0f : 0.0f;
            }
            else if (eventType == MidiControlMap::EventType::PitchBend)
            {
                normalized = static_cast<float>(dataValue) / 16383.0f;
            }
            else if (eventType == MidiControlMap::EventType::ProgramChange)
            {
                // Program Change carries the program number in data1 (controller).
                normalized = static_cast<float>(controller) / 127.0f;
            }
            else
            {
                normalized = static_cast<float>(dataValue) / 127.0f;
            }

            break;
        case MidiControlMap::Mode::Relative: {
            const float current = slot.value.load();
            const float delta = (static_cast<float>(dataValue) - 64.0f) / 64.0f * mm.sensitivity;
            normalized = std::clamp(current + delta, 0.0f, 1.0f);
            break;
        }
        case MidiControlMap::Mode::Toggle: {
            bool gateHigh = false;

            if (eventType == MidiControlMap::EventType::ProgramChange)
            {
                // Program change events are already discrete, so every match toggles once.
                gateHigh = true;
            }
            else if (eventType == MidiControlMap::EventType::NoteOff)
            {
                // Releasing a key must not fire a second toggle.
                slot.lastToggleGate.store(false);
                continue;
            }
            else if (eventType == MidiControlMap::EventType::NoteOn)
            {
                // For NoteOn mappings, treat each NoteOn press as a discrete toggle
                // so re-activation works even when release events are not delivered.
                gateHigh = dataValue > 0;

                if (!gateHigh)
                {
                    slot.lastToggleGate.store(false);
                    continue;
                }

                slot.lastToggleGate.store(true);
                normalized = (slot.value.load() < 0.5f) ? 1.0f : 0.0f;
                break;
            }
            else if (eventType == MidiControlMap::EventType::PitchBend)
            {
                gateHigh = dataValue >= 8192;
            }
            else
            {
                gateHigh = dataValue >= 64;
            }

            const bool fire = (eventType == MidiControlMap::EventType::ProgramChange)
                                  ? true
                                  : (!slot.lastToggleGate.load() && gateHigh);
            slot.lastToggleGate.store(gateHigh);

            if (!fire)
            {
                continue;
            }

            normalized = (slot.value.load() < 0.5f) ? 1.0f : 0.0f;
            break;
        }
        case MidiControlMap::Mode::Pickup: {
            const float target = static_cast<float>(dataValue) / 127.0f;

            if (std::abs(slot.value.load() - target) > mm.pickupRange)
            {
                continue; // Too far away, don't jump
            }

            normalized = target;
            break;
        }
        }

        slot.SetValue(normalized, AutomationSource::MIDI);
        ApplySlotLocked(slot);
    }
}

std::optional<MidiControlMap> AutomationSlotTable::PollMidiLearnCapture()
{
    if (!mMidiLearnCapture.has_value())
    {
        return std::nullopt;
    }

    auto result = std::move(mMidiLearnCapture);
    mMidiLearnCapture.reset();
    mMidiLearnSlotId.reset();
    return result;
}
} // namespace guitarfx
