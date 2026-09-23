/**
 * AutomationSlotTableJson.cpp - The slot table as JSON: automation.json, the slot
 * values a DAW saves with its project, and the slot list the UI shows.
 *
 * Loading builds a new slot list without touching the table (BuildSlotsFromJson);
 * the caller swaps it in under the DSP lock (CommitSlots, in AutomationSlotTable.cpp).
 */

#include "automation/AutomationSlotTable.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>

namespace guitarfx
{
namespace
{
nlohmann::json MidiMapToJson(const MidiControlMap& map)
{
    nlohmann::json json = nlohmann::json::object();
    json["eventType"] = static_cast<int>(map.eventType);
    json["channel"] = map.channel;
    json["controller"] = map.controller;
    json["mode"] = static_cast<int>(map.mode);
    json["sensitivity"] = map.sensitivity;
    json["pickupRange"] = map.pickupRange;
    return json;
}

MidiControlMap MidiMapFromJson(const nlohmann::json& json)
{
    MidiControlMap map;
    map.eventType = static_cast<MidiControlMap::EventType>(json.value("eventType", 0));
    map.channel = json.value("channel", 0);
    map.controller = json.value("controller", 0);
    map.mode = static_cast<MidiControlMap::Mode>(json.value("mode", 0));
    map.sensitivity = json.value("sensitivity", 0.1f);
    map.pickupRange = json.value("pickupRange", 0.1f);
    return map;
}

AutomationSlot* FindIn(std::vector<AutomationSlot>& slots, const std::string& slotId)
{
    const auto it = std::find_if(slots.begin(), slots.end(),
                                 [&slotId](const AutomationSlot& slot) { return slot.slotId == slotId; });
    return it != slots.end() ? &*it : nullptr;
}

std::vector<KeyboardMap> KeyMapsFromJson(const nlohmann::json& json)
{
    std::vector<KeyboardMap> keyMaps;

    for (const auto& k : json)
    {
        KeyboardMap km;
        km.key = k.value("key", "");
        km.mode = static_cast<KeyboardMap::Mode>(k.value("mode", 0));
        km.value = k.value("value", 0.0f);
        keyMaps.push_back(std::move(km));
    }

    return keyMaps;
}
} // namespace

// ── Serialization ─────────────────────────────────────────────────────────

nlohmann::json AutomationSlotTable::SaveToJson() const
{
    nlohmann::json j = nlohmann::json::object();
    j["schemaVersion"] = 1;

    // Default slot overrides (midiMap, keyMaps, label)
    nlohmann::json overrides = nlohmann::json::object();

    for (const auto& slot : mSlots)
    {
        if (!slot.isDefault)
        {
            continue;
        }

        if (!slot.midiMap && slot.keyMaps.empty() && slot.label.empty())
        {
            continue;
        }

        nlohmann::json o = nlohmann::json::object();

        // Only store label if it differs from the default
        for (const auto& def : kDefaultSlots)
        {
            if (def.slotId == slot.slotId && def.label != slot.label)
            {
                o["label"] = slot.label;
                break;
            }
        }

        if (slot.midiMap)
        {
            o["midiMap"] = MidiMapToJson(*slot.midiMap);
        }

        if (!slot.keyMaps.empty())
        {
            nlohmann::json km = nlohmann::json::array();

            for (const auto& k : slot.keyMaps)
            {
                km.push_back({{"key", k.key}, {"mode", static_cast<int>(k.mode)}, {"value", k.value}});
            }

            o["keyMap"] = std::move(km);
        }

        overrides[slot.slotId] = std::move(o);
    }

    j["defaultSlotOverrides"] = std::move(overrides);

    // Custom slots
    nlohmann::json customs = nlohmann::json::array();

    for (const auto& slot : mSlots)
    {
        if (slot.isDefault || !slot.presetId.empty())
        {
            continue;
        }

        nlohmann::json s = nlohmann::json::object();
        s["slotId"] = slot.slotId;
        s["label"] = slot.label;
        s["address"] = slot.address;

        if (!slot.nodeSelector.empty())
        {
            s["nodeSelector"] = slot.nodeSelector;
        }

        if (slot.midiMap)
        {
            s["midiMap"] = MidiMapToJson(*slot.midiMap);
        }

        if (!slot.keyMaps.empty())
        {
            nlohmann::json km = nlohmann::json::array();

            for (const auto& k : slot.keyMaps)
            {
                km.push_back({{"key", k.key}, {"mode", static_cast<int>(k.mode)}, {"value", k.value}});
            }

            s["keyMap"] = std::move(km);
        }

        customs.push_back(std::move(s));
    }

    j["customSlots"] = std::move(customs);

    // Per-preset MIDI mappings. Written only when there are some, so a document from before
    // they existed saves back unchanged.
    nlohmann::json presetSlots = nlohmann::json::array();

    for (const auto& slot : mSlots)
    {
        if (slot.presetId.empty())
        {
            continue;
        }

        nlohmann::json s = nlohmann::json::object();
        s["slotId"] = slot.slotId;
        s["presetId"] = slot.presetId;
        s["label"] = slot.label;
        s["address"] = slot.address;

        if (slot.midiMap)
        {
            s["midiMap"] = MidiMapToJson(*slot.midiMap);
        }

        presetSlots.push_back(std::move(s));
    }

    if (!presetSlots.empty())
    {
        j["presetSlots"] = std::move(presetSlots);
    }

    return j;
}

void AutomationSlotTable::LoadFromJson(const nlohmann::json& j)
{
    auto slots = BuildSlotsFromJson(j);
    CommitSlots(slots);
}

std::vector<AutomationSlot> AutomationSlotTable::BuildSlotsFromJson(const nlohmann::json& j) const
{
    auto slots = MakeDefaultSlots();

    // Load default slot overrides
    if (j.contains("defaultSlotOverrides") && j["defaultSlotOverrides"].is_object())
    {
        for (auto it = j["defaultSlotOverrides"].begin(); it != j["defaultSlotOverrides"].end(); ++it)
        {
            const auto& slotId = it.key();
            const auto& o = it.value();
            auto* slot = FindIn(slots, slotId);

            if (!slot || !slot->isDefault)
            {
                continue;
            }

            if (o.contains("label") && o["label"].is_string())
            {
                slot->label = o["label"].get<std::string>();
            }

            if (o.contains("midiMap") && o["midiMap"].is_object())
            {
                slot->midiMap = MidiMapFromJson(o["midiMap"]);
            }

            if (o.contains("keyMap") && o["keyMap"].is_array())
            {
                slot->keyMaps = KeyMapsFromJson(o["keyMap"]);
            }
        }
    }

    // Load custom slots
    if (j.contains("customSlots") && j["customSlots"].is_array())
    {
        for (const auto& cs : j["customSlots"])
        {
            AutomationSlot slot;
            slot.slotId = cs.value("slotId", "");
            slot.label = cs.value("label", "");
            slot.address = cs.value("address", "");
            slot.nodeSelector = cs.value("nodeSelector", "");
            slot.isDefault = false;

            if (cs.contains("midiMap") && cs["midiMap"].is_object())
            {
                slot.midiMap = MidiMapFromJson(cs["midiMap"]);
            }

            if (cs.contains("keyMap") && cs["keyMap"].is_array())
            {
                slot.keyMaps = KeyMapsFromJson(cs["keyMap"]);
            }

            if (!slot.slotId.empty())
            {
                slots.push_back(std::move(slot));
            }
        }
    }

    // Per-preset MIDI mappings
    if (j.contains("presetSlots") && j["presetSlots"].is_array())
    {
        for (const auto& ps : j["presetSlots"])
        {
            if (!ps.is_object())
            {
                continue;
            }

            AutomationSlot slot;
            slot.slotId = ps.value("slotId", "");
            slot.presetId = ps.value("presetId", "");
            slot.label = ps.value("label", "");
            slot.address = ps.value("address", "");

            if (ps.contains("midiMap") && ps["midiMap"].is_object())
            {
                slot.midiMap = MidiMapFromJson(ps["midiMap"]);
            }

            if (!slot.slotId.empty() && !slot.presetId.empty() && !FindIn(slots, slot.slotId))
            {
                slots.push_back(std::move(slot));
            }
        }
    }

    for (auto& slot : slots)
    {
        slot.nodeBinding = BindNodeAddress(slot.address);
    }

    return slots;
}

// ── Host state: slot values ──────────────────────────────────────────────

nlohmann::json AutomationSlotTable::SaveValuesToJson() const
{
    // A json object is an ordered map, so the same values always dump to the same bytes.
    // Hosts, and clap-validator's state tests, compare saved states byte for byte.
    nlohmann::json values = nlohmann::json::object();

    for (const auto& slot : mSlots)
    {
        values[slot.slotId] = slot.value.load();
    }

    return values;
}

void AutomationSlotTable::LoadValuesFromJson(const nlohmann::json& json)
{
    LoadValuesInto(mSlots, json);
}

void AutomationSlotTable::LoadValuesInto(std::vector<AutomationSlot>& slots, const nlohmann::json& json)
{
    for (auto& slot : slots)
    {
        float value = 0.0f;

        if (const auto it = json.find(slot.slotId); it != json.end() && it->is_number())
        {
            const auto stored = it->get<double>();

            // std::clamp passes a NaN straight through, so a non-finite value is rejected
            // before clamping rather than after.
            if (IsFinite(stored))
            {
                value = static_cast<float>(std::clamp(stored, 0.0, 1.0));
            }
        }

        slot.StoreValue(value);
        // As if this value had been applied: a host re-sending it is then not a rising
        // edge, and does not fire the trigger.
        slot.lastNormalized.store(value);
        slot.pendingApply.store(false);
    }
}

nlohmann::json AutomationSlotTable::GetSlotsJson() const
{
    nlohmann::json slots = nlohmann::json::array();

    for (const auto& s : mSlots)
    {
        nlohmann::json sj = nlohmann::json::object();
        sj["slotId"] = s.slotId;
        sj["label"] = s.label;
        sj["address"] = s.address;

        if (!s.nodeSelector.empty())
        {
            sj["nodeSelector"] = s.nodeSelector;
        }

        sj["isDefault"] = s.isDefault;

        if (!s.presetId.empty())
        {
            sj["presetId"] = s.presetId;
        }

        sj["value"] = s.value.load();

        if (s.midiMap)
        {
            sj["midiMap"] = MidiMapToJson(*s.midiMap);
        }

        if (!s.keyMaps.empty())
        {
            nlohmann::json km = nlohmann::json::array();

            for (const auto& k : s.keyMaps)
            {
                km.push_back({{"key", k.key}, {"mode", static_cast<int>(k.mode)}, {"value", k.value}});
            }

            sj["keyMap"] = std::move(km);
        }

        slots.push_back(std::move(sj));
    }

    return slots;
}
} // namespace guitarfx
