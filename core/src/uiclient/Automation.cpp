/**
 * Automation.cpp - MIDI mapping for the native UIs (see Automation.h).
 */

#include "uiclient/Automation.h"

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectGuids.h"
#include "uiclient/UiCommands.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace guitarfx::uiclient
{
namespace
{
/// The control-bar knobs and the global addresses they set.
const std::pair<const char*, const char*> kGlobalKnobAddresses[] = {{"input", "global.inputTrim"},
                                                                    {"output", "global.outputTrim"}};

const char* const kMidiEventNames[] = {"CC", "PC", "NoteOn", "NoteOff", "PBend"};
const char* const kMidiModeNames[] = {"Abs", "Rel", "Toggle", "Pickup"};

/// The chain's Input and Output nodes: hidden from the catalog, but their gain is a node
/// address like any other (presetV2.ts ROUTING_NODE_EFFECTS).
struct RoutingNode
{
    const char* type;
    const char* name;
};

const RoutingNode kRoutingNodes[] = {{"input", "Input"}, {"output", "Output"}};

/// Effect types whose parameters belong to what a node has loaded, not to its type.
bool HasPerNodeParameters(const std::string& type)
{
    return type == EffectGuids::kPluginHost || type == EffectGuids::kWasmHost;
}

std::string NextNumberedSlotId(const std::vector<AutomationSlotInfo>& slots, const std::string& prefix)
{
    int highest = 0;

    for (const auto& slot : slots)
    {
        if (slot.slotId.size() <= prefix.size() + 1 || slot.slotId.compare(0, prefix.size() + 1, prefix + ".") != 0)
        {
            continue;
        }

        const std::string digits = slot.slotId.substr(prefix.size() + 1);

        if (!digits.empty() &&
            std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        {
            highest = std::max(highest, std::stoi(digits));
        }
    }

    return prefix + "." + std::to_string(highest + 1);
}

/// "node.<type>.<param>" split, or false.
bool SplitNodeAddress(const std::string& address, std::string& type, std::string& param)
{
    if (address.rfind("node.", 0) != 0)
    {
        return false;
    }

    const auto dot = address.find('.', 5);

    if (dot == std::string::npos || dot == 5 || dot + 1 >= address.size())
    {
        return false;
    }

    type = address.substr(5, dot - 5);
    param = address.substr(dot + 1);
    return true;
}

std::string EffectName(const ClientState& state, const std::string& type)
{
    for (const auto& routing : kRoutingNodes)
    {
        if (type == routing.type)
        {
            return routing.name;
        }
    }

    const auto* info = state.FindEffectType(type);
    return info != nullptr && !info->name.empty() ? info->name : type;
}
} // namespace

const AutomationSlotInfo* AutomationState::FindSlot(const std::string& slotId) const
{
    const auto it =
        std::find_if(slots.begin(), slots.end(), [&](const AutomationSlotInfo& s) { return s.slotId == slotId; });
    return it != slots.end() ? &*it : nullptr;
}

const AutomationRegistryEntry* AutomationState::FindRegistryEntry(const std::string& address) const
{
    const auto it = std::find_if(registry.begin(), registry.end(),
                                 [&](const AutomationRegistryEntry& e) { return e.address == address; });
    return it != registry.end() ? &*it : nullptr;
}

std::vector<AutomationSlotInfo> ParseAutomationSlots(const nlohmann::json& slots)
{
    std::vector<AutomationSlotInfo> parsed;

    if (!slots.is_array())
    {
        return parsed;
    }

    for (const auto& s : slots)
    {
        if (!s.is_object() || !s.contains("slotId") || !s["slotId"].is_string())
        {
            continue;
        }

        AutomationSlotInfo slot;
        slot.slotId = s["slotId"].get<std::string>();
        slot.label = s.value("label", std::string());
        slot.address = s.value("address", std::string());
        slot.nodeSelector = s.value("nodeSelector", std::string());
        slot.isDefault = s.value("isDefault", false);
        slot.presetId = s.value("presetId", std::string());
        slot.value = s.value("value", 0.0);

        if (const auto map = s.find("midiMap"); map != s.end() && map->is_object())
        {
            AutomationMidiMap m;
            m.eventType = map->value("eventType", 0);
            m.channel = map->value("channel", 0);
            m.controller = map->value("controller", 0);
            m.mode = map->value("mode", 0);
            m.sensitivity = map->value("sensitivity", 0.1);
            m.pickupRange = map->value("pickupRange", 0.1);
            slot.midiMap = m;
        }

        if (const auto keys = s.find("keyMap"); keys != s.end() && keys->is_array())
        {
            for (const auto& k : *keys)
            {
                if (k.is_object())
                {
                    slot.keyMap.push_back({k.value("key", std::string()), k.value("mode", 0), k.value("value", 0.0)});
                }
            }
        }

        parsed.push_back(std::move(slot));
    }

    return parsed;
}

std::vector<AutomationRegistryEntry> ParseAutomationRegistry(const nlohmann::json& registry)
{
    std::vector<AutomationRegistryEntry> parsed;

    if (!registry.is_array())
    {
        return parsed;
    }

    for (const auto& e : registry)
    {
        if (!e.is_object() || !e.contains("address") || !e["address"].is_string())
        {
            continue;
        }

        AutomationRegistryEntry entry;
        entry.address = e["address"].get<std::string>();
        entry.label = e.value("label", entry.address);
        entry.unit = e.value("unit", std::string());
        entry.min = e.value("min", 0.0);
        entry.max = e.value("max", 1.0);
        entry.isStepped = e.value("isStepped", false);
        entry.isTrigger = e.value("isTrigger", false);
        parsed.push_back(std::move(entry));
    }

    return parsed;
}

// ── What a control drives ────────────────────────────────────────────────────────

std::optional<MidiLearnTarget> RegistryLearnTarget(const AutomationState& automation, const std::string& address)
{
    const auto* entry = automation.FindRegistryEntry(address);
    return entry != nullptr ? std::optional<MidiLearnTarget>(MidiLearnTarget{address, entry->label}) : std::nullopt;
}

std::optional<MidiLearnTarget> GlobalKnobLearnTarget(const AutomationState& automation, const std::string& knob)
{
    for (const auto& [name, address] : kGlobalKnobAddresses)
    {
        if (knob == name)
        {
            return RegistryLearnTarget(automation, address);
        }
    }

    return std::nullopt;
}

std::optional<MidiLearnTarget> NodeParamLearnTarget(const EffectTypeInfo* type, const std::string& paramKey)
{
    // Only a parameter the type declares has a range the engine can map a MIDI value onto.
    const auto* param = type != nullptr && !HasPerNodeParameters(type->type) ? type->FindParam(paramKey) : nullptr;

    if (param == nullptr)
    {
        return std::nullopt;
    }

    return MidiLearnTarget{"node." + type->type + "." + paramKey, (type->name.empty() ? type->type : type->name) +
                                                                      ": " +
                                                                      (param->name.empty() ? paramKey : param->name)};
}

std::optional<MidiLearnTarget> SetlistPadLearnTarget(const AutomationState& automation, int slotIndex)
{
    return slotIndex >= 0 ? RegistryLearnTarget(automation, "setlist.preset" + std::to_string(slotIndex + 1))
                          : std::nullopt;
}

// ── Slots ────────────────────────────────────────────────────────────────────────

const AutomationSlotInfo* FindSlotForAddress(const std::vector<AutomationSlotInfo>& slots, const std::string& address)
{
    for (const auto& slot : slots)
    {
        if (slot.isDefault && slot.address == address)
        {
            return &slot;
        }
    }

    for (const auto& slot : slots)
    {
        if (slot.presetId.empty() && slot.address == address)
        {
            return &slot;
        }
    }

    return nullptr;
}

const AutomationSlotInfo* FindPresetSlotForAddress(const std::vector<AutomationSlotInfo>& slots,
                                                   const std::string& presetId, const std::string& address)
{
    for (const auto& slot : slots)
    {
        if (!presetId.empty() && slot.presetId == presetId && slot.address == address)
        {
            return &slot;
        }
    }

    return nullptr;
}

bool AllowsPresetMapping(const std::string& address)
{
    if (address.rfind("node.", 0) == 0)
    {
        return true;
    }

    return std::any_of(std::begin(kGlobalKnobAddresses), std::end(kGlobalKnobAddresses),
                       [&](const auto& knob) { return address == knob.second; });
}

int CountCustomSlots(const std::vector<AutomationSlotInfo>& slots)
{
    return static_cast<int>(std::count_if(
        slots.begin(), slots.end(), [](const AutomationSlotInfo& s) { return !s.isDefault && s.presetId.empty(); }));
}

int CountPresetSlots(const std::vector<AutomationSlotInfo>& slots, const std::string& presetId)
{
    return static_cast<int>(
        std::count_if(slots.begin(), slots.end(), [&](const AutomationSlotInfo& s) { return s.presetId == presetId; }));
}

std::string NextCustomSlotId(const std::vector<AutomationSlotInfo>& slots)
{
    return NextNumberedSlotId(slots, "custom");
}

std::string NextPresetSlotId(const std::vector<AutomationSlotInfo>& slots)
{
    return NextNumberedSlotId(slots, "preset");
}

// ── How mappings read ────────────────────────────────────────────────────────────

std::string FormatMidiChannel(int channel)
{
    return channel < 0 ? "any" : std::to_string(channel + 1);
}

std::string DescribeMidiMap(int eventType, int channel, int controller, std::optional<int> mode)
{
    const bool knownEvent = eventType >= 0 && eventType < static_cast<int>(std::size(kMidiEventNames));
    std::string text = std::string(knownEvent ? kMidiEventNames[eventType] : "CC") + " " + std::to_string(controller) +
                       " ch" + FormatMidiChannel(channel);

    if (mode)
    {
        const bool knownMode = *mode >= 0 && *mode < static_cast<int>(std::size(kMidiModeNames));
        text += std::string(" ") + (knownMode ? kMidiModeNames[*mode] : "Abs");
    }

    return text;
}

std::string DescribeMidiMap(const AutomationMidiMap& map)
{
    return DescribeMidiMap(map.eventType, map.channel, map.controller, map.mode);
}

std::string DescribeKeyMaps(const AutomationSlotInfo& slot)
{
    if (slot.keyMap.empty())
    {
        return "\xE2\x80\x94";
    }

    std::string text;

    for (const auto& key : slot.keyMap)
    {
        char value[32];
        std::snprintf(value, sizeof(value), "=%.2f", key.value);
        text += (text.empty() ? "" : ", ") + (key.key == " " ? std::string("Space") : key.key) + "(" +
                (key.mode == 0 ? std::string("trig") : std::string(value)) + ")";
    }

    return text;
}

std::string DescribeRange(const AutomationRegistryEntry* entry)
{
    if (entry == nullptr)
    {
        return {};
    }

    const auto number = [](double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", v);
        return std::string(text);
    };
    return "(" + number(entry->min) + ".." + number(entry->max) + (entry->unit.empty() ? "" : " " + entry->unit) + ")";
}

bool KeyIsMapped(const AutomationState& automation, const std::string& key)
{
    const std::string pressed = AutomationSlotTable::NormalizeMappedKey(key);

    if (pressed.empty() || AutomationSlotTable::IsUnmappableKey(pressed))
    {
        return false;
    }

    return std::any_of(automation.slots.begin(), automation.slots.end(), [&](const AutomationSlotInfo& slot) {
        return std::any_of(slot.keyMap.begin(), slot.keyMap.end(), [&](const AutomationKeyMap& map) {
            return AutomationSlotTable::NormalizeMappedKey(map.key) == pressed;
        });
    });
}

// ── The right-click menu ─────────────────────────────────────────────────────────

std::vector<MidiLearnMenuItem> BuildMidiLearnMenuItems(const AutomationState& automation, const MidiLearnTarget& target,
                                                       const std::string& activePresetId)
{
    const auto* slot = FindSlotForAddress(automation.slots, target.address);
    const auto* presetSlot = FindPresetSlotForAddress(automation.slots, activePresetId, target.address);
    const auto& armed = automation.midiLearnSlotId;
    const bool listening = !armed.empty() && ((slot != nullptr && slot->slotId == armed) ||
                                              (presetSlot != nullptr && presetSlot->slotId == armed));
    std::vector<MidiLearnMenuItem> items;

    if (listening)
    {
        items.push_back({MidiLearnMenuAction::Cancel, "Cancel MIDI Learn", "Listening\xE2\x80\xA6", false});
    }
    else
    {
        // A control no slot drives yet needs a free slot to learn into.
        const bool noCustomSlot = slot == nullptr && CountCustomSlots(automation.slots) >= automation.maxCustomSlots;
        items.push_back(
            {MidiLearnMenuAction::Learn, "MIDI Learn\xE2\x80\xA6",
             noCustomSlot ? "All " + std::to_string(automation.maxCustomSlots) + " custom slots in use" : "",
             noCustomSlot});

        if (!activePresetId.empty() && AllowsPresetMapping(target.address))
        {
            const bool noPresetSlot = presetSlot == nullptr &&
                                      CountPresetSlots(automation.slots, activePresetId) >= automation.maxPresetSlots;
            items.push_back(
                {MidiLearnMenuAction::LearnPreset, "MIDI Learn for this preset\xE2\x80\xA6",
                 noPresetSlot ? "All " + std::to_string(automation.maxPresetSlots) + " preset slots in use" : "",
                 noPresetSlot});
        }
    }

    if (presetSlot != nullptr && presetSlot->midiMap)
    {
        items.push_back({MidiLearnMenuAction::Clear, "Clear Mapping",
                         DescribeMidiMap(*presetSlot->midiMap) + " \xC2\xB7 this preset", false});
    }
    else if (slot != nullptr && slot->midiMap)
    {
        items.push_back({MidiLearnMenuAction::Clear, "Clear Mapping", DescribeMidiMap(*slot->midiMap), false});
    }

    return items;
}

std::string MidiLearnMenuTitle(const AutomationState& automation, const MidiLearnTarget& target,
                               const std::string& activePresetId)
{
    if (const auto* slot = FindSlotForAddress(automation.slots, target.address);
        slot != nullptr && !slot->label.empty())
    {
        return slot->label;
    }

    if (const auto* slot = FindPresetSlotForAddress(automation.slots, activePresetId, target.address);
        slot != nullptr && !slot->label.empty())
    {
        return slot->label;
    }

    return target.label;
}

std::string RunMidiLearnMenuAction(UiCommands& commands, const ClientState& state, MidiLearnMenuAction action,
                                   const MidiLearnTarget& target)
{
    const auto& automation = state.automation;
    const std::string& presetId = state.activePresetId;
    const auto* slot = FindSlotForAddress(automation.slots, target.address);
    const auto* presetSlot = FindPresetSlotForAddress(automation.slots, presetId, target.address);

    switch (action)
    {
    case MidiLearnMenuAction::Cancel:
        commands.CancelMidiLearn();
        return {};

    case MidiLearnMenuAction::Clear:

        // The active preset's own mapping goes first; the global one is left for the next clear.
        if (presetSlot != nullptr && presetSlot->midiMap)
        {
            // A per-preset slot is nothing but its mapping, so the slot goes with it; one still
            // listening would leave the engine a learn with nowhere to land.
            const std::string id = presetSlot->slotId;
            const std::string label = presetSlot->label;

            if (automation.midiLearnSlotId == id)
            {
                commands.CancelMidiLearn();
            }

            commands.RemoveAutomationSlot(id);
            const auto* preset = state.FindPresetSummary(presetId);
            return "Cleared MIDI mapping for " + label + " in " + (preset != nullptr ? preset->name : presetId);
        }

        if (slot != nullptr)
        {
            // The slot stays, with any keys and DAW automation, for a later learn to fill again.
            commands.ClearAutomationMidiMap(slot->slotId);
            return "Cleared MIDI mapping for " + slot->label;
        }

        return {};

    case MidiLearnMenuAction::Learn:
    case MidiLearnMenuAction::LearnPreset:
        break;
    }

    const bool forPreset = action == MidiLearnMenuAction::LearnPreset;

    if (forPreset && presetId.empty())
    {
        return {};
    }

    const auto* existing = forPreset ? presetSlot : slot;
    std::string slotId = existing != nullptr ? existing->slotId : std::string();

    // The engine handles messages in order, so a slot made here exists by the time learn is armed on it.
    if (slotId.empty() && forPreset)
    {
        if (CountPresetSlots(automation.slots, presetId) >= automation.maxPresetSlots)
        {
            return "MIDI Learn unavailable: this preset already has " + std::to_string(automation.maxPresetSlots) +
                   " mappings";
        }

        slotId = NextPresetSlotId(automation.slots);
        commands.SetAutomationSlot(slotId, target.label, target.address, presetId);
    }
    else if (slotId.empty())
    {
        if (CountCustomSlots(automation.slots) >= automation.maxCustomSlots)
        {
            return "MIDI Learn unavailable: all " + std::to_string(automation.maxCustomSlots) +
                   " custom slots are in use";
        }

        slotId = NextCustomSlotId(automation.slots);
        commands.SetAutomationSlot(slotId, target.label, target.address);
    }

    commands.ArmMidiLearn(slotId);
    return {};
}

// ── The panel ─────────────────────────────────────────────────────────────────────

bool IsBypassAddress(const std::string& address)
{
    std::string type, param;
    return SplitNodeAddress(address, type, param) && param.find('.') == std::string::npos &&
           (param == "bypassed" || param == "bypass" || param == "enabled");
}

std::string DeriveSlotLabel(const ClientState& state, const std::string& address)
{
    if (const auto* entry = state.automation.FindRegistryEntry(address))
    {
        return entry->label;
    }

    std::string type, param;

    if (SplitNodeAddress(address, type, param))
    {
        const std::string effect = EffectName(state, type);

        if (param == "enabled")
        {
            return effect + ": On/Off";
        }

        if (param == "bypassed" || param == "bypass")
        {
            return effect + ": Bypass";
        }

        const bool routing = type == "input" || type == "output";
        const auto* info = routing ? nullptr : state.FindEffectType(type);
        const auto* p = info != nullptr ? info->FindParam(param) : nullptr;
        const std::string paramName = routing && param == "gainDb"       ? "Gain"
                                      : p != nullptr && !p->name.empty() ? p->name
                                                                         : param;
        return effect + ": " + paramName;
    }

    return "Custom";
}

bool ShowsTestButton(const AutomationState& automation, const AutomationSlotInfo& slot)
{
    const auto* entry = automation.FindRegistryEntry(slot.address);
    return (entry != nullptr && entry->isTrigger) || !slot.isDefault;
}

double TestValue(const AutomationState& automation, const AutomationSlotInfo& slot)
{
    const auto* entry = automation.FindRegistryEntry(slot.address);

    if (IsBypassAddress(slot.address) || (slot.midiMap && slot.midiMap->mode == 2))
    {
        return slot.value < 0.5 ? 1.0 : 0.0;
    }

    if (entry == nullptr || !entry->isTrigger)
    {
        return slot.value < 0.99 ? 1.0 : 0.0;
    }

    return 1.0;
}

std::vector<AutomationTargetOption> BuildTargetOptions(const ClientState& state)
{
    std::vector<AutomationTargetOption> options;

    for (const auto& entry : state.automation.registry)
    {
        const std::string group = entry.address.rfind("global.", 0) == 0    ? "Global"
                                  : entry.address.rfind("setlist.", 0) == 0 ? "Setlist"
                                                                            : "Other";
        options.push_back({entry.address, entry.label, group});
    }

    for (const auto& routing : kRoutingNodes)
    {
        options.push_back(
            {std::string("node.") + routing.type + ".gainDb", std::string(routing.name) + ": Gain", "Signal Chain"});
    }

    // As the web UI's picker: every effect the catalog offers with parameters, by category.
    std::vector<const EffectTypeInfo*> effects;

    for (const auto& info : state.catalog)
    {
        if (info.parameters.empty() || info.type == EffectGuids::kAmpNamBlend ||
            info.type == EffectGuids::kPickupMatch || info.type.rfind("composite:", 0) == 0)
        {
            continue;
        }

        effects.push_back(&info);
    }

    std::sort(effects.begin(), effects.end(), [](const EffectTypeInfo* a, const EffectTypeInfo* b) {
        if (a->category != b->category)
        {
            return a->category < b->category;
        }

        return (a->name.empty() ? a->type : a->name) < (b->name.empty() ? b->type : b->name);
    });

    for (const auto* info : effects)
    {
        const std::string group = info->category.empty() ? "Effects" : info->category;
        const std::string name = info->name.empty() ? info->type : info->name;
        // Both senses of on/off, as the web UI offers them: "enabled" turns the effect on with
        // a high value, "bypassed" turns it off.
        options.push_back({"node." + info->type + ".enabled", name + ": On/Off", group});
        options.push_back({"node." + info->type + ".bypassed", name + ": Bypass", group});

        for (const auto& param : info->parameters)
        {
            options.push_back({"node." + info->type + "." + param.key,
                               name + ": " + (param.name.empty() ? param.key : param.name), group});
        }
    }

    return options;
}

std::vector<AutomationTargetOption> WithCurrentTargetOption(std::vector<AutomationTargetOption> options,
                                                            const std::string& address, const std::string& label)
{
    if (address.empty() || std::any_of(options.begin(), options.end(),
                                       [&](const AutomationTargetOption& o) { return o.address == address; }))
    {
        return options;
    }

    options.insert(options.begin(), {address, label, "Current"});
    return options;
}
} // namespace guitarfx::uiclient
