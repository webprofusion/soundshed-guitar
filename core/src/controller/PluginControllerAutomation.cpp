/**
 * PluginControllerAutomation.cpp - Host automation slots, MIDI learn, setlists
 * and scenes.
 *
 * The *Direct variants of the setlist and scene switches run on the caller's
 * thread; the plain ones defer to the message pump. MIDI arrives on the audio
 * thread and is queued rather than acted on there.
 */

#include "PluginController.h"

#include "controller/ControlSurfaceQueue.h"

#include "controller/internal/HostedPluginSupport.h"

#include <algorithm>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

void PluginController::HandleGetAutomationRequest()
{
    nlohmann::json msg;
    msg["type"] = "automation";
    msg["slots"] = mAutomationSlots.GetSlotsJson();

    nlohmann::json registry = nlohmann::json::array();
    for (const auto& info : mAutomationSlots.GetRegistryInfo())
    {
        registry.push_back({{"address", info.address},
                            {"label", info.label},
                            {"unit", info.unit},
                            {"min", info.minValue},
                            {"max", info.maxValue},
                            {"isStepped", info.isStepped},
                            {"isTrigger", info.isTrigger}});
    }
    msg["registry"] = std::move(registry);
    msg["maxCustomSlots"] = kMaxCustomSlots;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetAutomationSlotRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    if (slotId.empty())
    {
        return;
    }

    const auto* existing = mAutomationSlots.FindSlot(slotId);
    const bool isDefault = existing && existing->isDefault;

    std::optional<std::string> label;
    if (payload.contains("label") && payload["label"].is_string())
    {
        label = payload["label"].get<std::string>();
    }

    std::optional<std::string> address;
    if (payload.contains("address") && payload["address"].is_string())
    {
        address = payload["address"].get<std::string>();
    }

    std::optional<std::string> nodeSelector;
    if (payload.contains("nodeSelector") && payload["nodeSelector"].is_string())
    {
        nodeSelector = payload["nodeSelector"].get<std::string>();
    }

    std::optional<MidiControlMap> midiMap;
    bool clearMidiMap = false;
    if (payload.contains("midiMap") && payload["midiMap"].is_object())
    {
        const auto& mm = payload["midiMap"];
        MidiControlMap m;
        m.eventType = static_cast<MidiControlMap::EventType>(mm.value("eventType", 0));
        m.channel = mm.value("channel", 0);
        m.controller = mm.value("controller", 0);
        m.mode = static_cast<MidiControlMap::Mode>(mm.value("mode", 0));
        m.sensitivity = mm.value("sensitivity", 0.1f);
        m.pickupRange = mm.value("pickupRange", 0.1f);
        midiMap = m;
    }
    else if (payload.contains("midiMap") && payload["midiMap"].is_null())
    {
        clearMidiMap = true;
    }

    std::optional<std::vector<KeyboardMap>> keyMaps;
    bool clearKeyMap = false;
    if (payload.contains("keyMap") && payload["keyMap"].is_array())
    {
        std::vector<KeyboardMap> kms;
        for (const auto& k : payload["keyMap"])
        {
            KeyboardMap km;
            km.key = k.value("key", "");
            km.mode = static_cast<KeyboardMap::Mode>(k.value("mode", 0));
            km.value = k.value("value", 0.0f);
            kms.push_back(std::move(km));
        }
        keyMaps = std::move(kms);
    }
    else if (payload.contains("keyMap") && payload["keyMap"].is_null())
    {
        clearKeyMap = true;
    }

    {
        // Lock the DSP mutex around structural slot mutations so we never
        // realloc/erase mSlots while the audio thread iterates it in HandleMidi.
        // Keep the critical section short: no disk I/O or UI sends under the lock.
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (isDefault)
        {
            mAutomationSlots.SetDefaultSlotOverrides(slotId, label, midiMap, keyMaps);
        }
        else
        {
            mAutomationSlots.SetCustomSlot(slotId, label, address, nodeSelector, midiMap, keyMaps);
        }

        if (clearMidiMap)
        {
            auto* slot = mAutomationSlots.FindSlot(slotId);
            if (slot)
            {
                slot->midiMap.reset();
            }
        }
        if (clearKeyMap)
        {
            auto* slot = mAutomationSlots.FindSlot(slotId);
            if (slot)
            {
                slot->keyMaps.clear();
            }
        }
    }

    SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
    HandleGetAutomationRequest();
}

void PluginController::HandleRemoveAutomationSlotRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mAutomationSlots.RemoveCustomSlot(slotId);
    }
    SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
    HandleGetAutomationRequest();
}

void PluginController::HandleSetAutomationValueRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    const float value = payload.value("value", 0.0f);
    const std::string sourceStr = payload.value("source", "ui");
    auto src = sourceStr == "keyboard" ? AutomationSource::Keyboard : AutomationSource::UI;

    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ApplyAutomationLocked(slotId, value, src);
}

void PluginController::ApplyAutomationFromDAW(const std::string& slotId, float normalized)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ApplyAutomationLocked(slotId, normalized, AutomationSource::DAW);
}

float PluginController::GetAutomationSlotValue(const std::string& slotId) const
{
    const auto* slot = mAutomationSlots.FindSlot(slotId);
    return slot ? slot->value.load() : 0.0f;
}

void PluginController::HandleArmMidiLearnRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    // mMidiLearnSlotId is read by the audio thread (under mDSPMutex) in HandleMidi.
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ArmMidiLearn(slotId);
}

void PluginController::HandleCancelMidiLearnRequest()
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ArmMidiLearn("");
}

void PluginController::EnqueueMidi(const MidiEvent& ev)
{
    mControlSurface->EnqueueMidi(ev);
}

void PluginController::ProcessQueuedMidi()
{
    // Audio thread. Drain queued MIDI events under the DSP lock without ever
    // blocking: if the lock is held (e.g. a preset load on the message thread),
    // leave the events queued and retry on the next block.
    if (!mControlSurface->HasMidiToApply())
    {
        return;
    }

    std::unique_lock<std::mutex> lock(mDSPMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return;
    }

    mControlSurface->DrainMidiForApply([this](const MidiEvent& event) { mAutomationSlots.HandleMidi(event); });
}

void PluginController::SetMidiLogEnabled(bool enabled)
{
    mControlSurface->SetMidiLogEnabled(enabled);
}

void PluginController::ApplySetlistPresetByIndex(int index)
{
    // This method may be called from the audio thread (via automation/MIDI apply,
    // already holding mDSPMutex) or from the UI thread (not holding the lock).
    // ApplyActivePresetById needs to acquire mDSPMutex, so when we're already
    // holding it we must defer the actual preset swap to OnIdle.
    //
    // We detect this by trying to lock mDSPMutex non-blocking. If it fails,
    // we're on the audio thread (or another locked context) and must defer.

    if (mDSPMutex.try_lock())
    {
        // We got the lock — not currently held, safe to proceed directly.
        mDSPMutex.unlock();
        ApplySetlistPresetByIndexDirect(index);
    }
    else
    {
        // Lock is held (audio thread under DSP lock) — defer to OnIdle.
        mControlSurface->RequestSetlistPreset(index);
    }
}

void PluginController::ApplySetlistPresetByIndexDirect(int index)
{
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
    {
        return;
    }

    // Resolve the active setlist by activeSetlistId (first setlist as fallback)
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    const nlohmann::json* activeSlots = nullptr;
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            activeSlots = &sl["slots"];
            break;
        }
    }
    if (!activeSlots || !activeSlots->is_array())
    {
        return;
    }

    if (index < 0 || index >= static_cast<int>(activeSlots->size()))
    {
        return;
    }

    const auto& slot = (*activeSlots)[index];
    const std::string presetId = slot.value("presetId", "");
    if (presetId.empty())
    {
        return;
    }

    mSetlistCursorIndex = index;

    // Persist cursor
    auto toStore = setlistsData;
    toStore["cursorIndex"] = index;
    SaveUiStorageJson("setlists.json", toStore);

    // Change to the preset. A setlist step is a *switch*, not a Multi-Rig add: it must swap
    // the mixer down to this one preset (gapless, via ApplyPreset's crossfade) rather than
    // stacking another instance on top of whatever is already playing.
    ApplyActivePresetById(presetId);

    // Notify the UI that the setlist cursor changed so it can update its display
    // and load the preset into the main preset chooser.
    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = activeSetlistId;
    msg["cursorIndex"] = index;
    msg["presetId"] = presetId;
    SendMessageToUI(msg.dump());
}

void PluginController::SetlistBankUp(int steps)
{
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SetlistBankChangeDirect(steps);
    }
    else
    {
        mControlSurface->AddSetlistBankDelta(steps);
    }
}

int PluginController::GetActiveSceneIndex() const
{
    if (!mActivePreset)
    {
        return -1;
    }

    const std::string activeSceneId = GetResolvedActiveSceneId();
    for (std::size_t i = 0; i < mActivePreset->scenes.size(); ++i)
    {
        if (mActivePreset->scenes[i].id == activeSceneId)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void PluginController::SelectSceneByIndex(int index)
{
    // Same threading contract as ApplySetlistPresetByIndex: reachable from the
    // audio thread via automation/MIDI apply (already holding mDSPMutex) or from
    // the UI thread. SelectSceneByIndexDirect ends up in ApplyPreset, which takes
    // mDSPMutex itself, so defer to OnIdle when the lock is already held.
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SelectSceneByIndexDirect(index);
    }
    else
    {
        mControlSurface->RequestScene(index);
    }
}

void PluginController::SelectSceneByIndexDirect(int index)
{
    if (!mActivePreset)
    {
        return;
    }

    // Must run while the outgoing scene is still the active one, so its live plugin state
    // is written back to that scene rather than leaking into the one being switched to.
    CaptureLiveHostedPluginStateIntoActivePreset();

    NormalizePresetScenes(*mActivePreset);

    // Out-of-range is a no-op rather than a clamp: a footswitch mapped to scene 4
    // should do nothing on a two-scene preset, not silently jump to scene 2.
    if (index < 0 || index >= static_cast<int>(mActivePreset->scenes.size()))
    {
        return;
    }

    const std::string targetSceneId = mActivePreset->scenes[static_cast<std::size_t>(index)].id;
    if (targetSceneId == GetResolvedActiveSceneId())
    {
        return;
    }

    if (!SetPresetActiveScene(*mActivePreset, targetSceneId, &mActiveSceneId))
    {
        return;
    }

    SyncActivePresetSceneGraph();
    ApplyPreset(*mActivePreset);

    // Report the switch on the same "presetLoaded" channel a UI-driven scene change
    // uses, so an open editor tracks the change and a closed one simply misses a
    // message it was never going to receive.
    nlohmann::json loaded;
    loaded["type"] = "presetLoaded";
    loaded["preset"] = SerializePresetForUi(*mActivePreset);
    nlohmann::json activeIds = nlohmann::json::array();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        activeIds.push_back(id);
    }
    loaded["activePresetIds"] = activeIds;
    loaded["sceneId"] = GetResolvedActiveSceneId();
    SendMessageToUI(loaded.dump());
}

void PluginController::SetlistBankDown(int steps)
{
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SetlistBankChangeDirect(-steps);
    }
    else
    {
        mControlSurface->AddSetlistBankDelta(-steps);
    }
}

void PluginController::SetlistBankChangeDirect(int delta)
{
    if (delta == 0)
    {
        return;
    }

    // A "bank" is a whole setlist. Bank up/down moves the active setlist to the
    // next/previous one in UI list order, clamped at the first/last setlist.
    auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    const int count = static_cast<int>(setlists.size());
    if (count == 0)
    {
        return;
    }

    // Resolve the current active setlist index (by id), defaulting to the first.
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    int currentIndex = 0;
    if (!activeSetlistId.empty())
    {
        for (int i = 0; i < count; ++i)
        {
            if (setlists[i].value("id", "") == activeSetlistId)
            {
                currentIndex = i;
                break;
            }
        }
    }

    const int newIndex = std::clamp(currentIndex + delta, 0, count - 1);
    if (newIndex == currentIndex)
    {
        return;
    }

    const std::string newId = setlists[newIndex].value("id", "");

    // Switch the active setlist ("bank"). Reset the preset cursor to the first
    // slot. No preset is loaded on a bank change — preset selection is a
    // separate MIDI action.
    mSetlistCursorIndex = 0;

    setlistsData["activeSetlistId"] = newId;
    setlistsData["cursorIndex"] = 0;
    SaveUiStorageJson("setlists.json", setlistsData);

    // Notify the UI so the setlist (bank) list highlights the new active setlist
    // and shows its slots.
    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = newId;
    msg["cursorIndex"] = 0;
    SendMessageToUI(msg.dump());
}

void PluginController::SelectSetlistBank(int bankNumber)
{
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SelectSetlistBankDirect(bankNumber);
    }
    else
    {
        mControlSurface->RequestSetlistBankSelect(bankNumber);
    }
}

void PluginController::SelectSetlistBankDirect(int bankNumber)
{
    // Select the setlist whose `bank` number matches `bankNumber` and make it
    // the active setlist ("bank"). No-op (with a log) if no setlist claims it.
    auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
    {
        return;
    }

    std::string targetId;
    for (const auto& sl : setlists)
    {
        if (sl.contains("bank") && sl["bank"].is_number_integer() && sl["bank"].get<int>() == bankNumber)
        {
            targetId = sl.value("id", "");
            break;
        }
    }

    if (targetId.empty())
    {
        AppendSessionLog("[Automation] Select Bank " + std::to_string(bankNumber) +
                         ": no setlist mapped to this bank number");
        return;
    }

    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    if (targetId == activeSetlistId)
    {
        return;
    }

    // Switch the active setlist. Reset the preset cursor to the first slot.
    // No preset is loaded — preset selection is a separate MIDI action.
    mSetlistCursorIndex = 0;

    setlistsData["activeSetlistId"] = targetId;
    setlistsData["cursorIndex"] = 0;
    SaveUiStorageJson("setlists.json", setlistsData);

    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = targetId;
    msg["cursorIndex"] = 0;
    SendMessageToUI(msg.dump());
}

int PluginController::GetSetlistLength() const
{
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
    {
        return 0;
    }

    // Resolve the active setlist by activeSetlistId (first setlist as fallback)
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            const auto& slots = sl.value("slots", nlohmann::json::array());
            return static_cast<int>(slots.size());
        }
    }
    return 0;
}

int PluginController::GetSetlistBankBase() const
{
    // A "bank" is a whole setlist, so MIDI preset slots 1..N map directly onto
    // the active setlist's slots starting at index 0.
    return 0;
}

int PluginController::GetSetlistBankNumber() const
{
    // Return the bank number of the active setlist, or 0 if none/unassigned.
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
    {
        return 0;
    }

    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            if (sl.contains("bank") && sl["bank"].is_number_integer())
            {
                return sl["bank"].get<int>();
            }
            return 0;
        }
    }
    return 0;
}

std::string PluginController::GetSetlistSlotPresetId(int index) const
{
    if (index < 0)
    {
        return "";
    }

    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
    {
        return "";
    }

    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            const auto& slots = sl.value("slots", nlohmann::json::array());
            if (index >= static_cast<int>(slots.size()))
            {
                return "";
            }
            return slots[index].value("presetId", "");
        }
    }
    return "";
}

} // namespace guitarfx
