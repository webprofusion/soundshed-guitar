#pragma once

/**
 * AutomationSlotTable.h — Manages the full set of automation slots.
 *
 * Owns the default + custom slots, handles persistence (automation.json),
 * and provides the apply path that routes normalized values to targets.
 */

#include "automation/AutomationTypes.h"
#include "automation/ParamRegistry.h"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

    /// Initialize the param registry with global/setlist entries bound to the mixer.
    /// Called once at startup after the mixer is available.
    void InitializeRegistry(MultiPresetMixer& mixer,
                            const std::function<double()>& getSetlistCursor,
                            const std::function<void(int)>& applySetlistPresetByIndex,
                            const std::function<void(int)>& bankUp,
                            const std::function<void(int)>& bankDown,
                            const std::function<int()>& getSetlistLength,
                            const std::function<int()>& getSetlistBankBase,
                            const std::function<void(int)>& selectSetlistBank,
                            const std::function<int()>& getSetlistBankNumber,
                            const std::function<void(int)>& selectSceneByIndex,
                            const std::function<int()>& getActiveSceneIndex);

    /// Load custom slots + default overrides from automation.json JSON.
    void LoadFromJson(const nlohmann::json& json);

    /// Serialize to JSON for persistence.
    [[nodiscard]] nlohmann::json SaveToJson() const;

    /// Get all slots (default + custom) for UI.
    [[nodiscard]] nlohmann::json GetSlotsJson() const;

    /// Get the param registry info for UI.
    [[nodiscard]] std::vector<ParamRegistryInfo> GetRegistryInfo() const;

    /// Find a slot by ID. Returns nullptr if not found.
    AutomationSlot* FindSlot(const std::string& slotId);
    [[nodiscard]] const AutomationSlot* FindSlot(const std::string& slotId) const;

    /// Add or update a custom slot. Returns false if slotId is a default or max exceeded.
    bool SetCustomSlot(const std::string& slotId, const std::optional<std::string>& label,
                       const std::optional<std::string>& address,
                       const std::optional<std::string>& nodeSelector,
                       const std::optional<MidiControlMap>& midiMap,
                       const std::optional<std::vector<KeyboardMap>>& keyMaps);

    /// Update a default slot's editable fields (label, midiMap, keyMaps only — not address).
    bool SetDefaultSlotOverrides(const std::string& slotId,
                                 const std::optional<std::string>& label,
                                 const std::optional<MidiControlMap>& midiMap,
                                 const std::optional<std::vector<KeyboardMap>>& keyMaps);

    /// Remove a custom slot by ID. Returns false if not found or is default.
    bool RemoveCustomSlot(const std::string& slotId);

    /// Get the list of all slot IDs in order (defaults first, then custom).
    [[nodiscard]] std::vector<std::string> GetSlotIds() const;

    /// Apply a normalized value to a slot's target. Must be called under mDSPMutex.
    /// Returns true if the apply was performed (target existed).
    bool ApplyAutomationLocked(const std::string& slotId, float normalized, AutomationSource src);

    /// Resolve a node.* address to a concrete nodeId in the active graph.
    /// Returns empty string if no matching enabled node found.
    [[nodiscard]] std::string ResolveNodeAddress(const std::string& address,
                                                  const std::string& nodeSelector) const;

    /// Set the mixer reference for node.* apply dispatch (called at init).
    void SetMixer(MultiPresetMixer* mixer) { mMixer = mixer; }

    /// Set the effect registry for node.* type resolution.
    void SetEffectRegistry(const EffectRegistry* registry) { mEffectRegistry = registry; }

    /// Callback invoked after a node.* param is successfully applied.
    /// Receives (effectType, paramId, nativeValue). Called under mDSPMutex.
    void SetOnNodeParamApplied(std::function<void(const std::string&, const std::string&, double)> cb)
    { mOnNodeParamApplied = std::move(cb); }

    /// Callback invoked after a node.* bypass target is applied.
    /// Receives (effectType, enabled). Called under mDSPMutex.
    void SetOnNodeBypassApplied(std::function<void(const std::string&, bool)> cb)
    { mOnNodeBypassApplied = std::move(cb); }

    /// Handle a MIDI event — matches against slot MIDI maps and applies.
    void HandleMidi(const MidiEvent& ev);

    /// Check if any slot has a MIDI learn armed.
    [[nodiscard]] bool IsMidiLearnArmed() const { return mMidiLearnSlotId.has_value(); }

    /// Arm MIDI learn for a slot. Empty string disarms.
    void ArmMidiLearn(const std::string& slotId) { mMidiLearnSlotId = slotId; }

    /// Get the slot ID currently armed for MIDI learn (empty if none).
    [[nodiscard]] std::string GetMidiLearnSlot() const { return mMidiLearnSlotId.value_or(""); }

    /// Poll for a captured MIDI learn result. Returns nullopt if none captured.
    /// If a result is returned, learn is automatically disarmed.
    [[nodiscard]] std::optional<MidiControlMap> PollMidiLearnCapture();

private:
    /// Build default slots from kDefaultSlots.
    void InitializeDefaultSlots();

    /// Apply a slot's value to its target address. Called under lock.
    bool ApplySlotLocked(AutomationSlot& slot);

    ParamRegistry mRegistry;
    std::vector<AutomationSlot> mSlots;
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

    // Callback fired after a node.* param apply succeeds
    std::function<void(const std::string&, const std::string&, double)> mOnNodeParamApplied;
    std::function<void(const std::string&, bool)> mOnNodeBypassApplied;
};

} // namespace guitarfx
