/**
 * PluginControllerAutomation.cpp - Host automation slots, MIDI learn, setlists
 * and scenes, and what is sent back to the MIDI device: the preset name for the
 * controller's display, and the bank and slot picked in the app.
 *
 * The *Direct variants of the setlist and scene switches run on the caller's
 * thread; the plain ones defer to the message pump. MIDI arrives on the audio
 * thread and is queued rather than acted on there.
 */

#include "PluginController.h"

#include "controller/ControlSurfaceQueue.h"
#include "controller/ControllerDisplayFeed.h"
#include "controller/HostStateRelay.h"

#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/SettingsKeys.h"

#include <algorithm>
#include <utility>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// The `bank` number of the active setlist in setlists.json (the first setlist when none is
/// marked active), or nothing when there are no setlists or it has no bank number.
std::optional<int> FindActiveSetlistBank(const nlohmann::json& setlistsData)
{
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");

    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            if (sl.contains("bank") && sl["bank"].is_number_integer())
            {
                return sl["bank"].get<int>();
            }

            return std::nullopt;
        }
    }

    return std::nullopt;
}
} // namespace

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
    msg["maxPresetSlots"] = kMaxPresetSlotsPerPreset;
    msg["keyLearnSlotId"] = mKeyLearnSlotId;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleArmKeyLearnRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    const auto* slot = slotId.empty() ? nullptr : mAutomationSlots.FindSlot(slotId);
    // Keys are global: a per-preset mapping is MIDI only.
    mKeyLearnSlotId = slot != nullptr && slot->presetId.empty() ? slotId : std::string{};
    HandleGetAutomationRequest();
}

void PluginController::HandleAutomationKeyRequest(const nlohmann::json& payload)
{
    const std::string key = payload.value("key", "");

    if (key.empty())
    {
        return;
    }

    // A key learn takes the next key. Escape backs out of it; Space and a bare modifier are
    // waited through (the key a modifier is held for comes next).
    if (!mKeyLearnSlotId.empty())
    {
        if (key == "Escape")
        {
            mKeyLearnSlotId.clear();
            HandleGetAutomationRequest();
            return;
        }

        if (AutomationSlotTable::IsUnmappableKey(key))
        {
            return;
        }

        const std::string slotId = std::exchange(mKeyLearnSlotId, std::string{});
        const auto* slot = mAutomationSlots.FindSlot(slotId);

        if (slot == nullptr)
        {
            HandleGetAutomationRequest();
            return;
        }

        nlohmann::json keyMap = nlohmann::json::array();

        for (const auto& map : mAutomationSlots.LearnedKeyMaps(*slot, key))
        {
            keyMap.push_back({{"key", map.key}, {"mode", static_cast<int>(map.mode)}, {"value", map.value}});
        }

        // Saved and announced as any other edit of the slot.
        HandleSetAutomationSlotRequest({{"slotId", slotId}, {"keyMap", std::move(keyMap)}});
        return;
    }

    // A chord is a shortcut (Ctrl+Z is undo, not "z"), and leaves the mappings alone.
    if (payload.value("ctrl", false) || payload.value("alt", false) || payload.value("meta", false))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mDSPMutex);

    if (const auto match = mAutomationSlots.MatchKey(key, payload.value("repeat", false)))
    {
        mAutomationSlots.ApplyAutomationLocked(match->first, match->second, AutomationSource::Keyboard);
    }
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

    // A per-preset MIDI mapping: an existing one keeps its preset, a new one names it.
    std::string presetId = existing ? existing->presetId : std::string{};

    if (!existing && payload.contains("presetId") && payload["presetId"].is_string())
    {
        presetId = payload["presetId"].get<std::string>();
    }

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
        else if (!presetId.empty())
        {
            mAutomationSlots.SetPresetSlot(slotId, presetId, label, address, midiMap);
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

void PluginController::ForgetPresetAutomation(const std::string& presetId)
{
    int removed = 0;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        removed = mAutomationSlots.RemovePresetSlots(presetId);
    }

    if (removed > 0)
    {
        SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
        HandleGetAutomationRequest();
    }
}

void PluginController::SyncAutomationActivePreset()
{
    // The id the state broadcast reports as activePresetId, so "this preset" is the same
    // preset to the engine and to the UI. Locked only on a change: this runs every idle tick,
    // and the audio thread leaves MIDI queued for a block whenever it finds the lock held.
    const std::string activeId = mActivePreset ? mActivePresetId : std::string{};

    if (activeId != mAutomationSlots.GetActivePresetId())
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mAutomationSlots.SetActivePresetId(activeId);
    }
}

void PluginController::SyncAutomationSceneValue()
{
    // The Scene parameter reports the scene playing. Stored when the scene changes, not whenever
    // the value differs: a lane can ask for a scene the preset does not have, and putting the
    // value back would have the host send it again on every block, each one parking a switch.
    // Locked only on a change, as above.
    const int sceneIndex = GetActiveSceneIndex();

    if (sceneIndex < 0 || sceneIndex == mAutomationSceneIndexStored)
    {
        return;
    }

    mAutomationSceneIndexStored = sceneIndex;
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.StoreSceneSelectValue(sceneIndex);
}

void PluginController::RefreshAutomationBindings()
{
    // A node.* slot keeps the range its effect type declared when the address was set, so the
    // audio thread has nothing to look up. Composites register their types as they are saved
    // and removed, which can change that range; the check is an atomic load when nothing has.
    if (!mAutomationSlots.NodeBindingsStale())
    {
        return;
    }

    auto bindings = mAutomationSlots.ResolveNodeBindings();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mAutomationSlots.CommitNodeBindings(bindings);
    }

    // `bindings` now holds the replaced ones, released here, after the lock.
}

void PluginController::SyncControllerDisplay()
{
    // Every tick, but the feed compares the name with the one it last queued, so only a
    // preset switch, a rename, the setting coming back on or a refresh sends anything.
    const auto setting = mAppSettings.find(kControllerDisplayPresetNameSettingKey);
    const bool enabled = setting == mAppSettings.end() || !setting->is_boolean() || setting->get<bool>();

    mControllerDisplay->Update(mActivePreset ? std::string_view(mActivePreset->name) : std::string_view{}, enabled);
}

std::size_t PluginController::TakeControllerDisplaySysEx(std::span<std::uint8_t> out)
{
    return mControllerDisplay->TakeSysEx(out);
}

void PluginController::SendSetlistPickToMidi(std::optional<int> slotIndex)
{
    const auto enabled = mAppSettings.find(kSetlistMidiOutEnabledSettingKey);

    if (enabled == mAppSettings.end() || !enabled->is_boolean() || !enabled->get<bool>())
    {
        return;
    }

    const auto channel = mAppSettings.find(kSetlistMidiOutChannelSettingKey);
    const auto bankSelect = mAppSettings.find(kSetlistMidiOutBankSelectSettingKey);
    const int channelNumber = channel != mAppSettings.end() && channel->is_number_integer() ? channel->get<int>() : 1;
    const auto mode = SetlistMidiOut::ParseBankSelect(
        bankSelect != mAppSettings.end() && bankSelect->is_string() ? bankSelect->get<std::string>() : std::string{});
    const auto bank = FindActiveSetlistBank(LoadUiStorageJson("setlists.json", nlohmann::json::object()));

    // Nothing to say: a setlist with no bank number picked on its own, or no bank select sent.
    if (!slotIndex.has_value() && (!bank.has_value() || mode == SetlistMidiOut::BankSelect::None))
    {
        return;
    }

    if (!mSetlistMidiOut->Queue(channelNumber, mode, bank, slotIndex))
    {
        AppendSessionLog("[MIDI out] Setlist pick not sent: bank " + (bank ? std::to_string(*bank) : "none") +
                         ", slot " + (slotIndex ? std::to_string(*slotIndex + 1) : "none") +
                         " cannot be sent on channel " + std::to_string(channelNumber) +
                         " with this bank select setting");
    }
}

std::size_t PluginController::TakeSetlistMidiOut(std::span<SetlistMidiOut::ShortMessage> out)
{
    return mSetlistMidiOut->Take(out);
}

void PluginController::HandleSetSetlistCursorRequest(const nlohmann::json& payload)
{
    // UI messages arrive on the message thread. After anything the host queued earlier, as
    // ApplySetlistPresetByIndex does.
    const int index = payload.value("cursorIndex", 0);
    mHostStateRelay->ApplyQueued();

    if (ApplySetlistPresetByIndexDirect(index))
    {
        SendSetlistPickToMidi(index);
    }
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

bool PluginController::ApplyAutomationFromDAW(std::span<const std::pair<int, float>> changes,
                                              std::span<const std::string> slotIds, bool mayBlock)
{
    std::unique_lock<std::mutex> lock(mDSPMutex, std::defer_lock);

    if (mayBlock)
    {
        lock.lock();
    }
    else if (!lock.try_lock())
    {
        return false;
    }

    for (const auto& [index, normalized] : changes)
    {
        if (index >= 0 && static_cast<std::size_t>(index) < slotIds.size())
        {
            mAutomationSlots.ApplyAutomationLocked(slotIds[static_cast<std::size_t>(index)], normalized,
                                                   AutomationSource::DAW);
        }
    }

    return true;
}

void PluginController::BindDawParameters(const std::vector<std::string>& slotIds)
{
    // Joins the live slots to their parameters' cells, which the audio thread writes through.
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.BindDawParameters(slotIds);
}

void PluginController::ReplaceAutomationSlots(const nlohmann::json& automation, const nlohmann::json* values)
{
    // The audio thread walks the slots under mDSPMutex (MIDI, DAW automation), so they are
    // swapped in under it, having been built, and given their values, off it. A host reads
    // its parameters' own cells rather than the slots, and CommitSlots updates those.
    auto slots = mAutomationSlots.BuildSlotsFromJson(automation);

    if (values != nullptr)
    {
        AutomationSlotTable::LoadValuesInto(slots, *values);
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mAutomationSlots.CommitSlots(slots);
    }

    // The new slots' Scene values are whatever was saved, or 0: the next drain stores the
    // scene playing.
    mAutomationSceneIndexStored = -1;

    // `slots` now holds the ones replaced, freed here, after the lock.
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
    // Without ever blocking: if the lock is held (a preset load on the message thread, say),
    // the events stay queued for the next block, which drains them under its own lock.
    if (!mControlSurface->HasMidiToApply())
    {
        return;
    }

    std::unique_lock<std::mutex> lock(mDSPMutex, std::try_to_lock);

    if (!lock.owns_lock())
    {
        return;
    }

    DrainQueuedMidiLocked();
}

void PluginController::SetMidiLogEnabled(bool enabled)
{
    mControlSurface->SetMidiLogEnabled(enabled);
}

void PluginController::ApplySetlistPresetByIndex(int index)
{
    // Never with mDSPMutex held: the preset load takes it. Automation, which applies under it,
    // parks its setlist steps for the message thread instead (see the registry wiring in
    // Initialize, and DrainControlSurfaceRequests).
    //
    // The host's own program change can come from any thread: AU sets a factory preset, and LV2
    // restores a program preset, on whatever thread the host used. A preset load belongs to the
    // message thread as much as a restore does, so it is handed over the same way.
    if (!mHost.IsMessageThread())
    {
        (void)RunHostChangeOnMessageThread([this, index] { (void)ApplySetlistPresetByIndexDirect(index); }, false,
                                           std::nullopt);
        return;
    }

    // After anything the host queued earlier, a restore especially, which would replace it.
    mHostStateRelay->ApplyQueued();
    ApplySetlistPresetByIndexDirect(index);
}

bool PluginController::ApplySetlistPresetByIndexDirect(int index)
{
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());

    if (setlists.empty())
    {
        return false;
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
        return false;
    }

    if (index < 0 || index >= static_cast<int>(activeSlots->size()))
    {
        return false;
    }

    const auto& slot = (*activeSlots)[index];
    const std::string presetId = slot.value("presetId", "");

    if (presetId.empty())
    {
        return false;
    }

    mSetlistCursorIndex = index;

    // Persist cursor
    auto toStore = setlistsData;
    toStore["cursorIndex"] = index;
    SaveUiStorageJson("setlists.json", toStore);

    // A step onto the preset that is already playing on its own only moves the cursor.
    // Reloading it would put the stored copy back over every edit not yet saved — a
    // footswitch, a mapped key or a pad tapped twice silently undid the chain — and
    // crossfade to a chain identical to the one playing.
    const auto activeIds = SnapshotActivePresetIds();
    const bool alreadyPlaying =
        mActivePreset && mActivePresetId == presetId && activeIds.size() == 1 && activeIds.front() == presetId;

    // Otherwise change to the preset. A setlist step is a *switch*, not a Multi-Rig add: it
    // must swap the mixer down to this one preset (gapless, via ApplyPreset's crossfade)
    // rather than stacking another instance on top of whatever is already playing.
    if (!alreadyPlaying)
    {
        ApplyActivePresetById(presetId);
    }

    // Notify the UI that the setlist cursor changed so it can update its display
    // and load the preset into the main preset chooser.
    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = activeSetlistId;
    msg["cursorIndex"] = index;
    msg["presetId"] = presetId;
    SendMessageToUI(msg.dump());
    return true;
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

    // In a multi-preset mix only this preset's slot changes; the others keep playing.
    ApplyActivePresetInItsSlot();

    // Report the switch on the same "presetLoaded" channel a UI-driven scene change
    // uses, so an open editor tracks the change and a closed one simply misses a
    // message it was never going to receive.
    nlohmann::json loaded;
    loaded["type"] = "presetLoaded";
    loaded["preset"] = SerializePresetForUi(*mActivePreset);
    loaded["activePresetIds"] = SnapshotActivePresetIds();
    loaded["sceneId"] = GetResolvedActiveSceneId();
    SendMessageToUI(loaded.dump());
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
    // The bank number of the active setlist, or 0 if none/unassigned.
    return FindActiveSetlistBank(LoadUiStorageJson("setlists.json", nlohmann::json::object())).value_or(0);
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
