#pragma once

/**
 * AutomationTypes.h — Data structures for the automation system.
 *
 * Defines the slot model, MIDI/keyboard control maps, and the ParamAddress
 * grammar used to address any automatable parameter via a generic string.
 */

#include "dsp/ParamTaper.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
/// Normalized value source — used for reconciliation and UI feedback.
enum class AutomationSource
{
    UI,
    DAW,
    MIDI,
    Keyboard,
};

/// MIDI event crossing the JUCE/core boundary (POD, no JUCE dependency).
struct MidiEvent
{
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    int sampleOffset = 0;
};

/// MIDI control-message mapping for a slot.
struct MidiControlMap
{
    enum class EventType
    {
        CC,
        ProgramChange,
        NoteOn,
        NoteOff,
        PitchBend
    };
    EventType eventType = EventType::CC;
    int channel = 0;    ///< 0-15, or -1 for "any channel"
    int controller = 0; ///< CC number (0-127) or note number
    enum class Mode
    {
        Absolute,
        Relative,
        Toggle,
        Pickup
    } mode = Mode::Absolute;
    float sensitivity = 0.1f; ///< Relative mode increment scale
    float pickupRange = 0.1f; ///< Pickup mode tolerance
};

/// Keyboard mapping for a slot (multiple keys can target the same slot).
struct KeyboardMap
{
    enum class Mode
    {
        Trigger,
        SetValue
    };
    std::string key; ///< KeyboardEvent.key value, e.g. "1", "a", "PageUp"
    Mode mode = Mode::Trigger;
    float value = 0.0f; ///< Normalized 0..1 for SetValue; ignored for Trigger
};

/// A `node.<effectType>.<paramId>` address, resolved on the message thread when a slot is given it
/// (AutomationSlotTable::BindNodeAddress). MIDI and DAW automation apply slots on the audio
/// thread, so everything that needs parsing or a registry lookup is worked out here, once.
struct NodeAddressBinding
{
    /// The canonical type (EffectRegistry::Resolve), whichever form the address names it in:
    /// nodes are matched by it, so an alias finds a node stored under the UUID and vice versa.
    std::string effectType;
    std::string paramId;

    /// "bypassed", "bypass" or "enabled": switches every node of the type rather than setting a
    /// parameter. For "enabled" a high value enables, for the others it bypasses.
    bool isBypass = false;
    bool highMeansEnabled = false;

    /// The range a slot's 0..1 value maps onto, snapped to `step` (or to a whole index for an
    /// enum) along `taper`. Without one, the parameter is one its effect does not declare, and
    /// it gets the 0..1 value as it is.
    bool hasRange = false;
    double minValue = 0.0;
    double maxValue = 1.0;
    double step = 0.0;
    bool isEnum = false;
    ParamTaper taper = ParamTaper::Linear;
    /// The effect declares the range, so the node driven may narrow it with its own settings
    /// (EffectProcessor::GetAutomationRange), as a pitch shift does.
    bool nodeMayNarrowRange = false;
};

/// A single automation slot (default or custom).
struct AutomationSlot
{
    std::string slotId;       ///< Stable ID: "default.inputLevel", "custom.3"
    std::string label;        ///< User-facing label
    std::string address;      ///< ParamAddress: "global.inputTrim", "node.amp_nam.inputGain", "setlist.preset"
    std::string nodeSelector; ///< Optional selector for node.* addresses (empty = first)
    bool isDefault = false;   ///< Default slots can't be deleted/readdressed
    std::string presetId;     ///< Set for a per-preset mapping: MIDI only, live only while that preset is active

    std::optional<MidiControlMap> midiMap;
    std::vector<KeyboardMap> keyMaps;

    /// `address` resolved, when it is a node.* one; null otherwise. Replaced whenever the address
    /// is, on the message thread under mDSPMutex. Shared and never changed once made, so a
    /// notification of what the slot changed can hold it past the slot's own lifetime.
    std::shared_ptr<const NodeAddressBinding> nodeBinding;

    // Runtime state (audio-thread accessible via atomics). Write `value` through StoreValue()
    // or SetValue(), never directly, so a DAW parameter's copy of it stays current.
    std::atomic<float> value{0.0f};
    std::atomic<int> lastSource{static_cast<int>(AutomationSource::UI)};
    std::atomic<float> lastNormalized{0.0f}; ///< For trigger edge detection
    std::atomic<bool> lastToggleGate{false}; ///< For MIDI toggle edge detection
    std::atomic<bool> pendingApply{false};

    /// While the slot is in its table and is a DAW parameter: the table's cell that mirrors
    /// `value` for the host to read on any thread (see AutomationSlotTable::BindDawParameters).
    std::atomic<float>* dawValue = nullptr;

    AutomationSlot() = default;
    AutomationSlot(const AutomationSlot& other);
    AutomationSlot& operator=(const AutomationSlot& other);
    AutomationSlot(AutomationSlot&&) noexcept = default;
    AutomationSlot& operator=(AutomationSlot&&) noexcept = default;

    /// Set value from a source; marks pending apply.
    void SetValue(float normalized, AutomationSource src);

    /// Sets `value`, and the DAW parameter's copy of it.
    void StoreValue(float normalized);

    /// Check if this slot has any input mapping (MIDI or keyboard).
    [[nodiscard]] bool HasInputMapping() const
    {
        return midiMap.has_value() || !keyMaps.empty();
    }
};

/// Default slot definitions (data, not code cases).
struct DefaultSlotDef
{
    const char* slotId;
    const char* address;
    const char* label;
};

/// The default automation slots shipped with the app.
inline constexpr DefaultSlotDef kDefaultSlots[] = {
    {"default.setlistPreset1", "setlist.preset1", "Setlist Preset 1"},
    {"default.setlistPreset2", "setlist.preset2", "Setlist Preset 2"},
    {"default.setlistPreset3", "setlist.preset3", "Setlist Preset 3"},
    {"default.setlistPreset4", "setlist.preset4", "Setlist Preset 4"},
    {"default.setlistPreset5", "setlist.preset5", "Setlist Preset 5"},
    {"default.setlistPreset6", "setlist.preset6", "Setlist Preset 6"},
    {"default.setlistPreset7", "setlist.preset7", "Setlist Preset 7"},
    {"default.setlistPreset8", "setlist.preset8", "Setlist Preset 8"},
    {"default.bankUp", "setlist.bankUp", "Bank Up"},
    {"default.bankDown", "setlist.bankDown", "Bank Down"},
    {"default.bankSelect", "setlist.bankSelect", "Select Bank"},
    {"default.inputLevel", "global.inputTrim", "Input Level"},
    {"default.outputLevel", "global.outputTrim", "Output Level"},
    {"default.scene1", "scene.select1", "Scene 1"},
    {"default.scene2", "scene.select2", "Scene 2"},
    {"default.scene3", "scene.select3", "Scene 3"},
    {"default.scene4", "scene.select4", "Scene 4"},
};

/// Maximum number of custom slots (reserved in the DAW parameter layout).
inline constexpr int kMaxCustomSlots = 16;

/// Maximum number of per-preset MIDI mappings one preset can hold. Per-preset slots are not
/// DAW parameters, so they do not count against kMaxCustomSlots.
inline constexpr int kMaxPresetSlotsPerPreset = 16;

/// Number of entries at the front of kDefaultSlots that belong to the DAW
/// parameter layout as originally shipped.
///
/// Never change this value, and only ever append to kDefaultSlots. The VST3
/// ParamID of a slot is derived from its parameter index, so anything inserted
/// before the custom-slot block rebinds automation in existing DAW projects.
/// Entries past this index are registered after the reserved custom slots
/// instead — see PluginProcessorAdapter::registerAutomationParameters.
inline constexpr int kLayoutStableDefaultSlots = 13;

static_assert(std::size(kDefaultSlots) >= static_cast<std::size_t>(kLayoutStableDefaultSlots),
              "kDefaultSlots must not shrink below the shipped DAW parameter layout: "
              "removing an entry shifts every custom slot and rebinds automation in "
              "existing DAW projects.");

/// Number of setlist preset slots per bank (also the number of DAW parameters).
inline constexpr int kSetlistPresetsPerBank = 8;

/// Number of directly selectable scene slots exposed to MIDI/automation.
inline constexpr int kSceneSelectSlots = 4;
} // namespace guitarfx
