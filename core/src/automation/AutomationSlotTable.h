#pragma once

/**
 * AutomationSlotTable.h — Manages the full set of automation slots.
 *
 * Owns the default + custom slots, handles persistence (automation.json),
 * and provides the apply path that routes normalized values to targets.
 *
 * Threading. The message thread is the only one that changes which slots there are,
 * and it does so under the controller's mDSPMutex, because the audio thread walks
 * the slots under that lock (MIDI, DAW automation). A rebuild from JSON is built off
 * the lock and swapped in under it (BuildSlotsFromJson, CommitSlots).
 *
 * A host reads its parameters on any thread, the audio thread included, so it never
 * walks the slots: each DAW parameter reads a cell of its own that mirrors its slot's
 * value (BindDawParameters, GetDawParameterValue).
 *
 * Applying a node.* slot on the audio thread allocates and locks nothing. The address is
 * resolved when it is set (BindNodeAddress), the node it drives is found without allocating
 * (MultiPresetMixer::FindAutomationTarget), and what it changed is posted to a NodeChangeQueue
 * for the message thread to take (TakeNodeChanges).
 */

#include "automation/AutomationTypes.h"
#include "automation/NodeChangeQueue.h"
#include "automation/ParamRegistry.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace guitarfx
{
class MultiPresetMixer;
class EffectRegistry;

/// Manages all automation slots and the apply dispatch.
class AutomationSlotTable
{
  public:
    AutomationSlotTable();
    ~AutomationSlotTable();

    AutomationSlotTable(const AutomationSlotTable&) = delete;
    AutomationSlotTable& operator=(const AutomationSlotTable&) = delete;

    /// Initialize the param registry with global/setlist entries bound to the mixer.
    /// Called once at startup after the mixer is available.
    void InitializeRegistry(MultiPresetMixer& mixer, const std::function<double()>& getSetlistCursor,
                            const std::function<void(int)>& applySetlistPresetByIndex,
                            const std::function<void(int)>& bankUp, const std::function<void(int)>& bankDown,
                            const std::function<int()>& getSetlistLength,
                            const std::function<int()>& getSetlistBankBase,
                            const std::function<void(int)>& selectSetlistBank,
                            const std::function<int()>& getSetlistBankNumber,
                            const std::function<void(int)>& selectSceneByIndex,
                            const std::function<int()>& getActiveSceneIndex);

    /// Load custom slots + default overrides from automation.json JSON. Replaces the slots in
    /// place, so only where nothing else can be reading them; otherwise BuildSlotsFromJson and
    /// CommitSlots.
    void LoadFromJson(const nlohmann::json& json);

    /// Message thread: the slots automation.json `json` describes, built without touching the
    /// table. Every value starts at 0; LoadValuesInto can fill them in before they go live.
    [[nodiscard]] std::vector<AutomationSlot> BuildSlotsFromJson(const nlohmann::json& json) const;

    /// Message thread, under mDSPMutex: makes `slots` the table's slots and brings every DAW
    /// parameter's value up to date with them. The replaced slots are left in `slots`, to be
    /// freed once the lock is released.
    void CommitSlots(std::vector<AutomationSlot>& slots);

    /// LoadValuesFromJson, for slots that are not live yet (see BuildSlotsFromJson).
    static void LoadValuesInto(std::vector<AutomationSlot>& slots, const nlohmann::json& json);

    /// Message thread, before any other thread reads a parameter: the slot behind each DAW
    /// parameter, in parameter order. From then on every slot's value is mirrored into a cell
    /// for its parameter. An id with no slot, such as a reserved placeholder, reads 0, as does
    /// a parameter whose slot is removed. Only the first call counts: a host keeps the plugin's
    /// parameters for as long as it has the plugin.
    void BindDawParameters(const std::vector<std::string>& slotIds);

    /// Any thread, without a lock: the value of DAW parameter `parameterIndex`, or 0 when there
    /// is no such parameter.
    [[nodiscard]] float GetDawParameterValue(int parameterIndex) const;

    /// Serialize to JSON for persistence.
    [[nodiscard]] nlohmann::json SaveToJson() const;

    /// The slots' current values, for the state a DAW saves with its project. Kept out of
    /// SaveToJson, which is also the machine-wide automation.json and the cross-instance
    /// sync payload: a value belongs to this instance, not to the user's mappings.
    [[nodiscard]] nlohmann::json SaveValuesToJson() const;

    /// Restores values written by SaveValuesToJson without applying them. Reopening a
    /// project must not fire a setlist or scene trigger, and continuous targets such as
    /// the input and output trims restore from their own state. A slot the object does not
    /// name goes back to 0, so a reused instance cannot keep a value the project never had.
    void LoadValuesFromJson(const nlohmann::json& json);

    /// Get all slots (default + custom) for UI.
    [[nodiscard]] nlohmann::json GetSlotsJson() const;

    /// Get the param registry info for UI.
    [[nodiscard]] std::vector<ParamRegistryInfo> GetRegistryInfo() const;

    /// Find a slot by ID. Returns nullptr if not found.
    AutomationSlot* FindSlot(const std::string& slotId);
    [[nodiscard]] const AutomationSlot* FindSlot(const std::string& slotId) const;

    /// Add or update a custom slot. Returns false if slotId is a default or max exceeded.
    bool SetCustomSlot(const std::string& slotId, const std::optional<std::string>& label,
                       const std::optional<std::string>& address, const std::optional<std::string>& nodeSelector,
                       const std::optional<MidiControlMap>& midiMap,
                       const std::optional<std::vector<KeyboardMap>>& keyMaps);

    /// Update a default slot's editable fields (label, midiMap, keyMaps only — not address).
    bool SetDefaultSlotOverrides(const std::string& slotId, const std::optional<std::string>& label,
                                 const std::optional<MidiControlMap>& midiMap,
                                 const std::optional<std::vector<KeyboardMap>>& keyMaps);

    /// Remove a custom or per-preset slot by ID. Returns false if not found or is default.
    bool RemoveCustomSlot(const std::string& slotId);

    /// Add or update a per-preset MIDI mapping. A new slot needs a presetId and room under
    /// kMaxPresetSlotsPerPreset. Returns false for a slot that is not a per-preset one.
    bool SetPresetSlot(const std::string& slotId, const std::string& presetId, const std::optional<std::string>& label,
                       const std::optional<std::string>& address, const std::optional<MidiControlMap>& midiMap);

    /// Remove every per-preset mapping a preset holds. Returns how many were removed.
    int RemovePresetSlots(const std::string& presetId);

    /// Number of per-preset mappings a preset holds.
    [[nodiscard]] int CountPresetSlots(const std::string& presetId) const;

    /// The preset whose per-preset mappings are live. Set it from the message thread under
    /// mDSPMutex; HandleMidi reads it on the audio thread under the same lock, and the
    /// message thread, its only writer, may read it without the lock.
    void SetActivePresetId(const std::string& presetId)
    {
        mActivePresetId = presetId;
    }

    [[nodiscard]] const std::string& GetActivePresetId() const
    {
        return mActivePresetId;
    }

    /// Slot IDs that are DAW parameters, in order (defaults first, then custom). Per-preset
    /// slots are left out: they are MIDI only.
    [[nodiscard]] std::vector<std::string> GetSlotIds() const;

    /// Apply a normalized value to a slot's target. Must be called under mDSPMutex.
    /// Returns true if the apply was performed (target existed).
    bool ApplyAutomationLocked(const std::string& slotId, float normalized, AutomationSource src);

    /// Resolve a node.* address to a concrete nodeId in the active graph.
    /// Returns empty string if no matching enabled node found.
    [[nodiscard]] std::string ResolveNodeAddress(const std::string& address, const std::string& nodeSelector) const;

    /// Set the mixer reference for node.* apply dispatch (called at init).
    void SetMixer(MultiPresetMixer* mixer)
    {
        mMixer = mixer;
    }

    /// Set the effect registry for node.* type resolution, and resolve every slot's address with
    /// it. At initialisation, before any other thread reads the slots.
    void SetEffectRegistry(const EffectRegistry* registry);

    /// Message thread: `address` resolved for applying (see NodeAddressBinding), or null when it
    /// is not a well-formed node.* address.
    [[nodiscard]] std::shared_ptr<const NodeAddressBinding> BindNodeAddress(const std::string& address) const;

    /// Every slot's binding, in slot order, and the registry generation they were resolved at.
    struct NodeBindings
    {
        std::uint64_t registryGeneration = 0;
        std::vector<std::shared_ptr<const NodeAddressBinding>> bySlot;
    };

    /// Message thread: whether effect types have been registered or removed since the slots were
    /// last resolved, which a composite does when it is saved, so a slot may map onto a range its
    /// type no longer declares. ResolveNodeBindings and CommitNodeBindings bring them up to date.
    [[nodiscard]] bool NodeBindingsStale() const;

    /// Message thread: every slot's address resolved afresh, without touching the table.
    [[nodiscard]] NodeBindings ResolveNodeBindings() const;

    /// Message thread, under mDSPMutex: makes `bindings` the slots' own, unless the slots have
    /// changed since they were resolved. The replaced ones are left in `bindings`, to be released
    /// once the lock is.
    void CommitNodeBindings(NodeBindings& bindings);

    /// Message thread: hands `take` each NodeChangeQueue::Change node.* automation has made since
    /// the last call, the latest for each node parameter and each bypassed effect type.
    template <typename Fn> void TakeNodeChanges(Fn&& take)
    {
        mNodeChanges.Take(std::forward<Fn>(take));
    }

    /// Any thread, one atomic load: whether TakeNodeChanges may have anything to hand over.
    [[nodiscard]] bool HasNodeChanges() const
    {
        return mNodeChanges.HasChanges();
    }

    /// Message thread: how many node changes found the queue full since the last call.
    [[nodiscard]] std::size_t TakeDroppedNodeChangeCount()
    {
        return mNodeChanges.TakeDroppedCount();
    }

    /// Handle a MIDI event — matches against slot MIDI maps and applies.
    void HandleMidi(const MidiEvent& ev);

    /// Check if any slot has a MIDI learn armed.
    [[nodiscard]] bool IsMidiLearnArmed() const
    {
        return mMidiLearnSlotId.has_value();
    }

    /// Arm MIDI learn for a slot. Empty string disarms. Either way a capture made for the slot
    /// armed before is dropped, so it cannot land on this one.
    void ArmMidiLearn(const std::string& slotId)
    {
        if (slotId.empty())
        {
            mMidiLearnSlotId.reset();
        }
        else
        {
            mMidiLearnSlotId = slotId;
        }

        mMidiLearnCapture.reset();
    }

    /// Get the slot ID currently armed for MIDI learn (empty if none).
    [[nodiscard]] std::string GetMidiLearnSlot() const
    {
        return mMidiLearnSlotId.value_or("");
    }

    /// Poll for a captured MIDI learn result. Returns nullopt if none captured.
    /// If a result is returned, learn is automatically disarmed.
    [[nodiscard]] std::optional<MidiControlMap> PollMidiLearnCapture();

  private:
    /// The default slots, from kDefaultSlots, with no overrides.
    static std::vector<AutomationSlot> MakeDefaultSlots();

    /// Apply a slot's value to its target address. Called under lock.
    bool ApplySlotLocked(AutomationSlot& slot);

    /// ApplySlotLocked for a slot with a node.* binding. Allocates nothing.
    bool ApplyNodeSlotLocked(AutomationSlot& slot);

    /// The registry node.* addresses are resolved with.
    [[nodiscard]] const EffectRegistry& TypeRegistry() const;

    /// Joins `slot` to its DAW parameter's cell, if it has one, and publishes its value there.
    void AttachDawValue(AutomationSlot& slot);

    /// A slot on its way out: its DAW parameter, if it has one, reads 0 from now on.
    static void DetachDawValue(AutomationSlot& slot);

    ParamRegistry mRegistry;
    std::vector<AutomationSlot> mSlots;

    /// One cell per DAW parameter, allocated once by BindDawParameters and never moved, so a
    /// host thread can read one while the message thread replaces the slots.
    std::unique_ptr<std::atomic<float>[]> mDawValues;
    std::size_t mDawValueCount = 0;
    std::unordered_map<std::string, std::atomic<float>*> mDawValueBySlotId; ///< message thread
    MultiPresetMixer* mMixer = nullptr;
    const EffectRegistry* mEffectRegistry = nullptr;

    // Callbacks for setlist operations
    std::function<double()> mGetSetlistCursor;
    std::function<void(int)> mApplySetlistPresetByIndex;
    std::function<void(int)> mBankUp;
    std::function<void(int)> mBankDown;
    std::function<int()> mGetSetlistLength;
    std::function<int()> mGetSetlistBankBase;
    std::function<void(int)> mSelectSetlistBank;
    std::function<int()> mGetSetlistBankNumber;

    // Callbacks for scene selection within the active preset
    std::function<void(int)> mSelectSceneByIndex;
    std::function<int()> mGetActiveSceneIndex;

    // MIDI learn state
    std::optional<std::string> mMidiLearnSlotId;
    std::optional<MidiControlMap> mMidiLearnCapture;

    // Per-preset mappings answer MIDI only while this preset is active
    std::string mActivePresetId;

    /// The EffectRegistry generation the slots' bindings were last all resolved at.
    std::uint64_t mNodeBindingsGeneration = 0;

    /// What node.* slots changed, for the message thread (TakeNodeChanges).
    NodeChangeQueue mNodeChanges;
};
} // namespace guitarfx
