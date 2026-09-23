# Effect Parameter Automation & MIDI Mapping — Implementation Plan

## Goals

Expose a fixed set of **default automation slots** (setlist preset selection, bank up/down, signal-chain input level, signal-chain output level) plus a configurable set of **custom automation slots** that the user can map to any automatable parameter in the app — global mixer controls (e.g. `global.inputTrim`, `global.outputTrim`, and future additions like `global.transpose`), per-node effect parameters (e.g. NAM amp `inputGain`, reverb `mix`), or setlist structural controls. Custom targets may reference parameters on nodes that are not currently present or enabled in the signal chain; the mapping is sticky and reactivates when the node appears.

Each automation slot has a normalized 0..1 value and is drivable from three independent sources:

1. **Host DAW** — via JUCE automatable parameters (VST3/AU/CLAP/AAX).
2. **MIDI** — via user-configured MIDI control-message mappings (CC/Program Change/Note) managed in the UI.
3. **Keyboard** — via user-configured keyboard mappings (e.g. `"1"` / `"a"`) active when the main window has focus but no specific input control is focused.

The three sources are reconciled per slot (see §6), and the slot's normalized value is mapped to the target parameter's native range.

## Design Principle: Generic String-Addressed Parameters

There is **one** representation for anything an automation slot can drive: a **`ParamAddress` string** and a **`ParamRegistry`** entry. There are no per-parameter enum members, no `AutomationTarget` discriminated union, no slot "kinds." The default slots are simply pre-populated data entries whose `address` happens to point at `global.inputTrim` / `setlist.preset` / etc. — they use the exact same generic apply path as custom slots.

Adding a new automatable parameter (e.g. `global.transpose` tomorrow, or some future mixer control) is a **one-line registry entry**, not a code change to the slot model, the JUCE parameter layout, or the apply switch. The legacy `PluginController::ParameterId` enum and `mParamValues` array are left untouched for now (internal shadow storage for the existing trim knobs) and are *not* extended by this work; new code routes through the registry. Future cleanup can deprecate that enum entirely.

### ParamAddress

A single string with a small, fixed prefix vocabulary:

| Prefix | Example | Resolved by |
|---|---|---|
| `global.` | `global.inputTrim`, `global.outputTrim`, `global.transpose` | `ParamRegistry` static table of global mixer controls |
| `node.<effectType>.<paramId>` | `node.amp_nam.inputGain`, `node.reverb_room.mix`, `node.delay_digital.bypassed` | Lazy lookup: node of `effectType` in the active graph. Parameter targets use first-enabled-node semantics; bypass target applies to all matching nodes of the type. |
| `setlist.<control>` | `setlist.preset1..8`, `setlist.bankUp`, `setlist.bankDown`, `setlist.bankSelect` | Setlist cursor / structural controls |

The prefix vocabulary is the *only* special case, and it is a fixed three-token grammar — not a per-feature enum. New global mixer params reuse `global.*`; new setlist structural controls reuse `setlist.*`; new node params need no registration at all (the effect's `EffectTypeInfo::parameters` already declares them).

### Node address resolution

`node.*` addresses refer to an **effect type**, not a specific node instance. At apply time the resolver collects all nodes in the active graph (current scene, topological order) whose type matches `effectType`, then picks one using the slot's optional **`nodeSelector`** field. This deliberately mirrors how a guitarist thinks about a rig ("the reverb mix", "the amp gain") rather than how the graph is wired internally.

- `effectType` is the effect's canonical UUID from `EffectGuids.h` (e.g. `2eb53b40-...` for `amp_nam`). The legacy human-readable alias (`amp_nam`) is accepted on load and normalized to the UUID via `EffectRegistry::Resolve`, mirroring preset deserialization.
- `nodeSelector` is an optional string on the slot (absent/empty = default). **v1 implements only the default**, which selects the first matching enabled node. The field is reserved for future selector expressions so multi-instance and custom node selection can be added without changing the address format or the slot model. Reserved (not-yet-implemented) grammar:
  - `"2"` — the Nth matching node (1-based, topological order).
  - `"last"` — the final matching node.
  - `"label:Lead"` — first matching node whose user-set `label` equals the given string.
  - `"path:pre"` — first matching node inside a named signal-chain region (e.g. pre-chain vs post-chain).
  - The grammar is extensible; unrecognized selectors fall back to the default (first) and the UI flags the slot as "selector not supported in this version".
- If no node of the type is present (or no enabled node satisfies the selector), the apply is a **no-op** — the slot retains its value and reactivates the moment a matching enabled node appears. This is the "may or may not be enabled in the current signal chain" behavior, for free.
- Because the address names a type (optionally refined by a selector expression), **node identity is not part of the mapping**. There is no node-id migration problem when presets are re-imported or node IDs change — the slot keeps working as long as some node of that type satisfying the selector exists.

### ParamRegistry

Populated at `PluginController::Initialize()` time. Each entry is:

```cpp
struct ParamRegistryEntry {
    std::string address;              // e.g. "global.inputTrim"
    std::string label;                 // "Input Trim"
    std::string unit;                  // "dB", "%", "st", ""
    double   minValue, maxValue;       // native range
    bool     isStepped;               // continuous vs discrete (setlist preset)
    bool     isTrigger;              // edge-triggered (bank up/down)
    std::function<double()> get;                            // read current value
    std::function<void(double, bool alreadyLocked)> apply;  // write; alreadyLocked = caller holds mDSPMutex
};
```

`global.*` and `setlist.*` entries are **static** — registered once at startup and live for the controller's lifetime. Example registrations (illustrative; the real list is built next to the existing `MultiPresetMixer` setters):

```cpp
registry.Register("global.inputTrim", "Input Trim", "dB", -40, 20, false, false,
    [this]{ return mPresetMixer.GetGlobalInputGain(); },
    [this](double v, bool alreadyLocked){ ApplyGlobalInputGainLocked(v, alreadyLocked); });
registry.Register("global.outputTrim","Output Trim","dB", -40, 20, false, false, /*get*/, /*apply*/);
registry.Register("global.transpose","Transpose","st", -24, 12, true, false, /*get*/, /*apply*/);   // future, no slot model change
registry.Register("setlist.preset", "Setlist Preset", "", 0, 0, true, false,
    [this]{ return (double)GetSetlistCursorIndex(); },
    [this](double v, bool){ ApplySetlistPresetByIndex((int)std::round(v)); });
registry.Register("setlist.bankUp",   "Bank Up",   "", 0, 1, false, true,  /*get*/, /*apply*/);
registry.Register("setlist.bankDown", "Bank Down", "", 0, 1, false, true,  /*get*/, /*apply*/);
```

`node.*` entries are **virtual**: the registry does not enumerate every possible `(effectType, paramId)` pair. Instead, when a slot's address is `node.*`, a single prefix handler:
1. Splits the address into `(effectType, paramId)`.
2. Resolves `effectType` to its canonical UUID via `EffectRegistry::Resolve` (accepts legacy aliases).
3. Collects all enabled nodes in the active graph (current scene, topological order) whose type matches.
4. Picks one using the slot's `nodeSelector` (default = first).
5. If a node is selected, calls the existing per-node setter (`MultiPresetMixer::SetNodeParam(nodeId, paramId, value)` — a new method that does *not* take `mDSPMutex` because the caller already holds it; it reuses the same code path as today's `HandleUpdateSignalPathNodeParamRequest`).
5. If no matching enabled node is found, the apply is a **no-op** — slot retains its value.

The universe of *valid* `node.*` targets (for the UI picker) is derived from `EffectRegistry::GetAllTypes()` and each type's `EffectTypeInfo::parameters`. Validation on slot save: `effectType` must resolve to a registered effect UUID, `paramId` must be in that type's `parameters`. No registry entry is created for node targets; they are pure convention + lazy resolution.

This means the **registry contains zero per-effect entries**. Effect parameters are discovered from `EffectTypeInfo`, which already exists. Adding a new effect or a new parameter on an existing effect requires *no* automation-side change.

## Current State (gap analysis)

- `core/src/PluginController.h:105` defines a `ParameterId` enum (legacy global params: `kParamInputTrim`, `kParamDrive`, `kParamEQLowGain`, …) routed by `ApplyParamChangeLocked` (`core/src/PluginController.cpp:3350`). These are **not** exposed to the host DAW as JUCE parameters. This enum is not extended by this work.
- `juce/source/PluginProcessorAdapter.cpp:180` returns `acceptsMidi() = false` and `processBlock` does `juce::ignoreUnused(midiMessages)` — MIDI is fully discarded today.
- No `juce::AudioProcessorParameter`s are registered; `getNumPrograms` returns 1 and `setCurrentProgram` is a no-op (`PluginProcessorAdapter.cpp:184` / `:186`), so DAW preset/bank selection is not wired up.
- Per-node effect parameters today route via `HandleUpdateSignalPathNodeParamRequest` (UI message `updateSignalPathNodeParam {nodeId, paramId, value}`) — no automation path to them.
- Setlists already exist as UI-storage JSON: `HandleGetSetlistsRequest` / `HandleSetSetlistsRequest` (`PluginController.cpp:8713`); the multi-preset mixer exposes `AddActivePresetById` / `RemoveActivePreset` for slot management.
- The effect registry exposes per-type parameter metadata (`EffectTypeInfo::parameters`), so the universe of automatable node targets is discoverable at runtime, independent of which nodes are currently in the graph.
- `MultiPresetMixer` already has setters for the legacy global controls (`SetGlobalInputGain`, `SetGlobalOutputGain`, `SetGlobalTranspose`, `SetGlobalGateEnabled`, …). These become the bodies of the `global.*` registry entries' `apply` callbacks — no new DSP code, just a different dispatch route.

## 1. Default Automation Slots

Default slots are **data**, not code cases. A static const table ships with the app:

```cpp
struct DefaultSlot {
    std::string slotId;     // "default.inputLevel"
    std::string address;    // "global.inputTrim"
    std::string label;      // "Input Level"
};

static constexpr DefaultSlot kDefaultSlots[] = {
    { "default.setlistPreset1..8", "setlist.preset1..8", "Setlist Preset 1..8" },
    { "default.bankUp",        "setlist.bankUp",   "Bank Up" },
    { "default.bankDown",     "setlist.bankDown", "Bank Down" },
    { "default.bankSelect",   "setlist.bankSelect", "Select Bank" },
    { "default.inputLevel",   "global.inputTrim",  "Input Level" },
    { "default.outputLevel",  "global.outputTrim", "Output Level" },
};
```

That's the entire default-slot definition. `isDefault=true` is just a flag the UI uses to forbid deletion/address-rebind (label, MIDI map, and keyboard map remain editable). Functionally, a default slot is identical to a custom slot — same `address` field, same generic apply path. If we later ship `global.transpose` as a default automation, we add one row here and one row in the registry. No slot-model change, no JUCE layout change (the reserved custom range absorbs it — see §3), no apply switch change.

Default-slot value semantics are inherited from the address's registry entry (`isStepped` / `isTrigger`):
- **Bank = setlist.** A "bank" is a whole setlist; each setlist owns a unique optional `bank` number (see `preset-library-organization.md`).
- `setlist.preset1..8` are triggers → fire on the 0→0.5+ rising edge; each selects the corresponding slot (0..7) of the **active setlist** and loads its preset (`ApplySetlistPresetByIndex`). A slot holding the preset already playing on its own only moves the cursor: reloading it would replace unsaved edits with the stored copy.
- `setlist.bankUp`/`bankDown` are triggers → fire on the rising edge; switch the active setlist to the next/previous one in UI list order, clamped at the first/last setlist. No preset is loaded on a bank change.
- `setlist.bankSelect` is stepped (0..127) → selects the setlist whose `bank` number equals the value (a MIDI CC 0..127 picks the bank directly). No-op with a log entry if no setlist claims that bank number.
- `global.inputTrim`/`outputTrim` are continuous dB.

`cursorIndex` (the selected slot within the active setlist) is added to the existing setlists UI-storage JSON (`setlists.json`), reusing the existing `HandleSetSetlistsRequest` save path. A bank change also persists `activeSetlistId`.

## 2. Custom Automation Slots

User-defined slots, stored in the existing UI-storage layer (see §7) as `automation.json`:

```json
{
  "schemaVersion": 1,
  "defaultSlotOverrides": {
    "default.setlistPreset": {
      "keyMap": [
        {"key": "1", "mode": "setValue", "value": 0.0},
        {"key": "2", "mode": "setValue", "value": 0.125},
        {"key": "3", "mode": "setValue", "value": 0.25}
      ]
    },
    "default.bankUp": {
      "midiMap": {"channel": 0, "eventType": "CC", "controller": 14, "mode": "absolute"},
      "keyMap": [{"key": "PageUp", "mode": "trigger"}]
    },
    "default.bankDown": {
      "midiMap": {"channel": 0, "eventType": "CC", "controller": 15, "mode": "absolute"},
      "keyMap": [{"key": "PageDown", "mode": "trigger"}]
    }
  },
  "customSlots": [
    {
      "slotId": "custom.1",
      "label": "NAM Gain",
      "address": "node.amp_nam.inputGain",
      "midiMap": { "channel": 0, "eventType": "CC", "controller": 7, "mode": "absolute" }
    },
    {
      "slotId": "custom.2",
      "label": "Reverb Mix (2nd reverb)",
      "address": "node.reverb_room.mix",
      "nodeSelector": "2",
      "midiMap": { "channel": 0, "eventType": "CC", "controller": 74, "mode": "absolute" }
    },
    {
      "slotId": "custom.3",
      "label": "Transpose",
      "address": "global.transpose",
      "midiMap": { "channel": 0, "eventType": "CC", "controller": 5, "mode": "absolute" },
      "keyMap": [{"key": "t", "mode": "trigger"}]
    }
  ]
}
```

- `defaultSlotOverrides` stores user-configured MIDI/keyboard mappings for default slots (the address and label are not overridable — they come from `kDefaultSlots`). Absent entries = no mapping configured.
- `nodeSelector` is optional and only meaningful for `node.*` addresses. Absent/empty = default (first matching enabled node). v1 stores and round-trips the field but only implements the default; future versions interpret selector expressions (see "Node address resolution" above). Unknown selectors fall back to default and the UI flags the slot.
- The `address` for node targets uses the effect's human-readable alias (`amp_nam`, `reverb_room`) for readability in the JSON; on load it is resolved to the canonical UUID via `EffectRegistry::Resolve` and stored internally as the UUID form. Either form is accepted on load.
- A fixed maximum of `kMaxCustomSlots` (default 16) is reserved in the JUCE parameter layout so DAW project state stays stable across reloads even if the user removes a slot. Removed custom slots keep their JUCE parameter but become "unmapped" — DAW writes are stored but apply nowhere (no address). This is also exactly how a future *default* slot would look if we ever shipped one the user could opt out of: address cleared, parameter stays.
- The UI target picker lists, for `node.*` targets, every effect type from `EffectRegistry::GetAllTypes()` and every parameter in `EffectTypeInfo::parameters` for the chosen type. The user picks an effect type + parameter; the slot stores the address string. There is no per-instance binding step — resolution is automatic against whatever node of that type is first in the graph at apply time.

The `custom.3 transpose` example above is the proof of the generic model: a future global parameter needs no slot-side code, no new enum, no new slot kind. Register `"global.transpose"` in the `ParamRegistry` and the user can immediately map a custom slot to it (or we can ship it as a default row in `kDefaultSlots` later).

## 3. DAW Host Exposure (JUCE adapter layer)

`juce/source/PluginProcessorAdapter` changes:

1. **Parameter layout is slot-driven, not target-driven.** In the constructor, after `mController.Initialize()`, the adapter asks the controller for the slot table (5 defaults + the reserved custom range of 16) and registers one `AutomationSlotParameter` (a `juce::AudioProcessorParameter` subclass) per slot. Each parameter's `getParameterID()` returns the stable slot ID (`"default.inputLevel"`, `"custom.7"`). The parameter's display label/unit is pulled from the slot's resolved `ParamRegistryEntry` via the controller. This keeps DAW automation lanes stable across mapping edits.
2. `acceptsMidi()` → `true`. `producesMidi()` is `true` too, but only for the preset name sent to the controller's display (§5); `isMidiEffect()` stays false.
3. In `processBlock`, before DSP:
   - The JUCE wrapper has already applied host automation to the `AutomationSlotParameter` objects by this point. Iterate the slot parameters, and for each whose value differs from the slot's last-applied value, call `controller.ApplyAutomationFromDAW(slotId, normalized)`.
   - Drain the `juce::MidiBuffer` and pass each `juce::MidiMessage` to `controller.HandleMidi(ev)` (see §5).
4. Replace the stubbed program API:
   - `getNumPrograms()` returns the active setlist length (or 1 if no setlist) — this maps the DAW's program/bank selector onto the active setlist via the existing `setlist.preset` address, giving a second standard DAW surface for setlist selection in addition to the `default.setlistPreset` slot.
   - `setCurrentProgram(int i)` → `controller.ApplySetlistPresetByIndex(i)` (existing `ApplyPreset` path).
   - `getProgramName(int)` reads from the setlist entry.
5. `getStateInformation` / `setStateInformation` already serialize controller state; extend `PluginController::SerializeState` to include the slot table (so DAW-saved state restores MIDI mappings even if the user's `~/.guitarfx/automation.json` is absent).

`AutomationSlotParameter` notes:
- `getText(value, length)` / `getValueForText(text)` delegate to the slot's `ParamRegistryEntry` for native-unit formatting (dB for trims, % for mix, index for setlist, st for transpose). Adding a new global param needs no parameter-subclass change — it just has a new registry entry with its own `unit`.
- Trigger slots (`setlist.bankUp`/`bankDown`) use JUCE's discrete/stepped flag; the host writes 0→1→0 to fire, and the slot's edge-detection (driven by the registry's `isTrigger` flag) handles the fire. This flag lives on the registry entry, not on a slot subclass.
- `getValue()` runs on whatever thread the host likes. That includes the audio thread: JUCE's VST3 wrapper compares against it while applying the host's automation. It also runs while the message thread is replacing the slot list, so it never looks at the slots. The adapter binds its parameter order once, at construction (`PluginController::BindDawParameters`). From then on `AutomationSlotTable` keeps one atomic cell per parameter, and every write to a slot's value is mirrored into it (`AutomationSlot::StoreValue`). `GetDawParameterValue(index)` reads that cell without a lock. A parameter whose slot is gone, and a reserved placeholder, read 0.
- The slot list itself changes only on the message thread, under `mDSPMutex`, because the audio thread walks it under that lock to apply MIDI and DAW automation. Startup, a shared-sync reload and a host-state restore each rebuild it from JSON. The new list is built off the lock, with a restore's values already in it, and swapped in under the lock (`BuildSlotsFromJson`, `CommitSlots`); the old one is freed after the lock is released. `AutomationSlotThreadTests` covers both this and the parameter cells.

## 4. Apply Path (the only dispatch logic)

The entire apply path is one function; there is no `switch (paramIdx)` and no `switch (targetKind)`:

```cpp
void PluginController::ApplyAutomationLocked(const std::string& slotId, double normalized) {
    auto& slot = *mAutomationSlots.Find(slotId);
    const auto& entry = mParamRegistry.Find(slot.address);
    if (!entry) return; // unmapped slot (removed custom, or unbound default) — store value, apply nothing

    const double native = entry.isStepped
        ? std::round(entry.min + normalized * (entry.max - entry.min))
        : entry.min + normalized * (entry.max - entry.min);

    if (entry.isTrigger) {
        const bool fire = (slot.lastNormalized < 0.5) && (normalized >= 0.5);
        slot.lastNormalized = normalized;
        if (!fire) return;
    } else {
        slot.lastNormalized = normalized;
    }

    entry.apply(native, /*alreadyLocked=*/true);
    // For node.* addresses, entry.apply is a thin shim that splits the address
    // into (effectType, paramId), collects enabled nodes of that type in
    // topological order, picks one via slot.nodeSelector (default=first), and
    // calls mPresetMixer.SetNodeParam(nodeId, paramId, value).
    // The shim is created on the fly by a single registry "node.*" handler, not per-effect.
}
```

The `node.*` case is handled by *one* registry handler registered at startup that pattern-matches the `node.` prefix, not by per-node entries. That handler does the graph lookup and the `SetNodeParam` call. So the registry stays tiny (global + setlist entries only), and node targets are convention-based.

A slot value is 0..1 whichever source wrote it, but effects take parameters in native units. For a `node.*` target the value is mapped onto the range the effect declares for that parameter (`ParameterDef::minValue..maxValue`, snapped to `step`, or to a whole index for an enum) before `SetNodeParam`, so CC 0..127 sweeps a ±24 dB gain knob end to end. A parameter the effect does not declare gets the 0..1 value unchanged. `AutomationNodeParamRangeTests` covers it.

MIDI and DAW automation run the apply path on the audio thread, so for a `node.*` slot it allocates nothing and takes no lock beyond the `mDSPMutex` try-lock it runs under:

- **The address is resolved when it is set, not per event.** `AutomationSlotTable::BindNodeAddress` parses it, resolves the effect type from any alias to its canonical id, and copies the declared range, step, enum-ness and taper into an immutable `NodeAddressBinding` the slot holds (`AutomationSlot::nodeBinding`). It runs on the message thread when a slot is created or re-addressed and when slots are loaded from JSON. Effect types come and go at run time (a composite registers its type as it is saved), so `EffectRegistry` counts registrations, and on the idle tick after one `PluginController::RefreshAutomationBindings` re-resolves every slot off the lock and swaps the bindings in under it.
- **The node is found without allocating.** Which node a slot drives still depends on the running graphs, which preset switches and bypasses change, so it is looked up per event, by `MultiPresetMixer::FindAutomationTarget`: the first enabled node of the canonical type in execution order, in the first preset instance that has one — the node `FindFirstEnabledNodeOfType` names. Each executor resolves its nodes' types once, when it builds its plan, and keeps them in execution order for this walk. Bypass addresses switch every node of the type through `SetAutomatedNodesEnabled`.
- **The executor's copy of the graph is written in place.** It only takes a value for a key the node already holds, since inserting one allocates. Nothing reads that copy back except the Input and Output nodes' `gainDb`, and `SignalGraphExecutor::SetGraph` gives those nodes the key (0 dB, what a missing key reads as).
- **What changed goes to the message thread through a `NodeChangeQueue`** (`core/src/automation/NodeChangeQueue.h`), a fixed set of cells the audio thread claims by compare-exchange. `PluginController::FoldAutomationNodeChanges` takes them and patches the working copy of the preset, in the active scene's graph as well as the preset's (a state broadcast copies the one over the other), then sends the UI `signalPathNodeParamUpdated`. `OnIdle` calls it, but only an open editor drives `OnIdle`, so `DrainControlSurfaceRequests` calls it too, off the plugin's own 30 Hz timer: the working copy, and so what a host saves, stays current with the editor closed. A change replaces any not yet taken for the same node id and parameter, or for a bypass the same effect type, so the queue holds the latest per target, and a poll with nothing waiting is one atomic load. The strings a change names (the node id, the binding) are shared pointers the audio thread only copies; the message thread releases them. `ControlSurfaceDrainTests` covers the closed-editor path.

`AutomationNodeRealtimeTests` counts what the audio thread allocates for one CC on a `node.*` slot (a parameter, a bypass, the Input gain), and checks the lookups behave as the name-based ones did across aliases, bypasses and preset switches.

`MultiPresetMixer::SetNodeParam(nodeId, paramId, value, alreadyLocked)` is a new method that reuses the existing per-node parameter application code (today routed through `HandleUpdateSignalPathNodeParamRequest`), with the mutex assumption made explicit. No new DSP code.

## 5. MIDI Handling

### Core: `PluginController::HandleMidi`

Core stays JUCE-free. A small POD crosses the boundary:

```cpp
struct MidiEvent { uint8_t status; uint8_t data1; uint8_t data2; int sampleOffset; };
void PluginController::HandleMidi(const MidiEvent& ev);
```

Algorithm (audio thread, inside the existing `mDSPMutex` try-lock in `ProcessAudio`):

1. Decode channel + event type (Note/CC/PC/Pitch Bend).
2. Iterate slot MIDI maps; match on `(channel, eventType, controller)`.
3. On match, compute new normalized value per `mode`:
   - `absolute`: `data2/127.f` (CC), `1.0` on Note On velocity>0 (for trigger slots).
   - `relative`: `value += (data2-64)/64.f * sensitivity`.
   - `toggle`: flip on Note On edge.
   - `pickup`: only if `|value - data2/127| < pickupRange`.
4. `slot.SetValue(normalized, Source::MIDI)` → routes through `ApplyAutomationLocked`.

MIDI events with no matching map are ignored in v1. (Future: route to hosted-plugin nodes — `fx-library.md` notes MIDI routing to `plugin_host` is not yet implemented; out of scope here.)

### `MidiControlMap`

```cpp
struct MidiControlMap {
    enum class EventType { CC, ProgramChange, NoteOn, NoteOff, PitchBend };
    EventType eventType;
    int channel;       // 0-15, or -1 for "any"
    int controller;    // CC number or note number
    enum class Mode { Absolute, Relative, Toggle, Pickup } mode;
    float sensitivity; // relative
    float pickupRange;  // pickup
};
```

### MIDI device plumbing

Standalone: JUCE's `AudioDeviceManager` lists the MIDI inputs once `acceptsMidi()=true`, and Settings → Audio & MIDI → MIDI Devices enables and disables them (`juce/source/StandaloneAudioSettings.cpp`; on Android and iOS the holder also opens new ones automatically); the `MidiBuffer` arrives in `processBlock`. Plugin builds receive MIDI from the host track via the same `MidiBuffer`.

The same section offers a MIDI output because `producesMidi()` is true (`NEEDS_MIDI_OUTPUT` in `juce/CMakeLists.txt` — the two must agree). The standalone sends whatever `processBlock` leaves in the buffer to the output chosen there. Every incoming message is cleared before that, including while the host bypasses the plugin (`processBlockBypassed`), so nothing is echoed back to the controller.

### Preset name on the controller's display

The active preset's name is sent to the controller as SysEx, so a control surface with a screen shows what is loaded. The message is the Soundshed Go's `SET_TEXT` (soundshed-go `docs/04-config-protocol.md`), on display line 0:

```
F0 7D 53 47 07 00 <name, up to 32 ASCII bytes> F7
```

`7D` is the manufacturer id reserved for non-commercial use and `53 47` is `"SG"`, so other devices ignore it. The name is converted for a 7-bit display — accents dropped, typographic dashes, quotes and ellipses made plain, anything else `?` — then trimmed and cut to 32 characters (`ControllerDisplayFeed::ToDisplayText`). No active preset sends an empty line, which clears it. The controller answers each message with a `RESULT` SysEx, which the adapter drops along with all other incoming SysEx.

`core/src/controller/ControllerDisplayFeed` owns it. `PluginController::SyncControllerDisplay` hands it the active preset's name every idle tick, and it queues a message only when the name has changed. The adapter collects that in `processBlock` through `TakeControllerDisplaySysEx`, which never blocks or allocates, and adds it to the block's MIDI output. Only the newest message is kept. The device does not store the text, so `PluginController::Prepare` asks for a resend: a stream that starts, or restarts because a different MIDI output was chosen, puts the name back on a display that may be blank.

Where it goes:

- **Standalone** — the MIDI output chosen in Settings → Audio & MIDI → MIDI Devices. Nothing is sent until one is chosen.
- **VST3, AU, AAX, LV2** — the plugin's MIDI output, wherever the host routes it.
- **CLAP** — the wrapper forwards only two- and three-byte messages on its own, so the adapter implements clap-juce-extensions' `addOutboundEventsToQueue` and pushes SysEx as `CLAP_EVENT_MIDI_SYSEX`. The event points at its bytes and the host reads them after `process()` returns, so they are copied into an arena on the adapter that lasts until the next `process()` call.

The name is picked up on the idle tick, which the editor's timer drives. In a plugin whose editor is closed, a preset change reaches the display when the editor next opens.

The *Mappings* tab of the MIDI & Automation panel has a checkbox for it, backed by the app setting `midi.controllerDisplay.presetName` (absent = on). Turned off, nothing is sent; turned back on, the name is sent again.

### MIDI learn

Each slot row has a "Learn" button. When armed, the next matching MIDI event becomes that slot's `MidiControlMap`. The engine forwards the raw event to the UI as `midiLearnCapture {slotId, eventType, channel, controller}` so the UI can show the captured values and let the user confirm/cancel.

The same learn is reachable from the control itself: right-click → **MIDI Learn…** on

- the input and output level knobs (`global.inputTrim`, `global.outputTrim`),
- any effect parameter its effect type declares (`node.<effectType>.<paramKey>`) — knobs, toggles and sliders, in the default and custom layouts,
- the Gain on the chain's own Input and Output nodes (`node.input.gainDb`, `node.output.gainDb`), which trims the preset on top of the global input and output levels,
- a setlist pad or a slot in the setlist editor (`setlist.preset1`..`8`), the bank arrows (`setlist.bankUp`/`bankDown`) and the current-bank panel (`setlist.bankSelect`).

`core/ui/ts/midiLearnMenu.ts` arms the slot that already drives that address — a default slot first, then a custom one — or, for an effect parameter nothing drives yet, creates a custom slot for it (within `kMaxCustomSlots`) and arms that. While it listens an indicator offers Cancel, and Escape cancels too. The captured mapping lands in the slot table the panel edits, and learning again replaces it. When the slot has a MIDI mapping the menu also offers **Clear Mapping**, which names the mapping and does what the panel's clear button does — `setAutomationSlot {slotId, midiMap: null}` — leaving the slot, its keyboard mapping and any DAW automation in place, so a later learn on the control reuses it. The menu's items come from `buildMidiLearnMenuItems()` in `midiMapping.ts`.

**MIDI Learn for this preset…** makes a per-preset mapping. The menu offers it only on the signal chain's controls — the input and output level knobs, effect parameters and the Input/Output node gains (`allowsPresetMapping()` in `midiMapping.ts`) — and not on the setlist pads or bank controls, which pick the bank and preset rather than shape the preset that is playing. Clear Mapping still removes a per-preset mapping from any control that has one. A per-preset mapping is a slot that carries the active preset's id, created with `setAutomationSlot {slotId: "preset.N", label, address, presetId}`. The engine matches it only while that preset is active — `PluginController::SyncAutomationActivePreset` hands `AutomationSlotTable` the id the state broadcast reports as `activePresetId`, checking once per idle tick — and while it is live it takes its MIDI control over from any global mapping on the same control, so one expression pedal can drive a different target in each preset. A control a preset does not map is untouched: its global mapping still answers. Per-preset slots are MIDI only. They are not DAW parameters (`GetSlotIds` leaves them out), take no keyboard keys, do not count against `kMaxCustomSlots` (each preset may hold `kMaxPresetSlotsPerPreset`, 16), and are stored under `presetSlots` in `automation.json` rather than in the preset — so they do not travel with an exported or shared preset, and a duplicated preset does not copy them. Deleting a preset deletes its mappings. **Clear Mapping** removes the active preset's own mapping for the control first (`removeAutomationSlot`, since a per-preset slot is nothing but its mapping) and the global mapping on the next clear. The MIDI panel lists per-preset slots under *Preset Mappings*. Which address an element drives is `resolveMidiLearnTarget()` in `core/ui/ts/midiMapping.ts`. No message was added for this: the menu uses `setAutomationSlot` and `armMidiLearn`, and the learn state lives in `automationPanel.ts` so the panel's Learn button and the menu never disagree about which slot is listening.

Two consequences of existing rules are worth knowing. A `node.*` address names an effect type, so learning on the second of two identical effects drives the first (see §2). A custom slot created at runtime is not a DAW parameter until the plugin is next instantiated — the same as one added from the panel, since the parameter layout is fixed at construction (§3). Parameters of hosted plugins and WASM modules belong to the loaded module rather than the effect type, so a `node.*` address cannot say which one it means and they are not offered.

The chain's Input and Output nodes are routing, not effects: nothing registers them with the engine, so `EffectRegistry::FindParameter` has no range to map a slot's 0..1 value onto and the apply path would hand the node 0..1 dB. `AutomationSlotTable` recognises `node.input.gainDb` and `node.output.gainDb` and maps them onto `kBoundaryGainMinDb`..`kBoundaryGainMaxDb` from `presets/PresetTypes.h` instead — the same −24..+24 dB the UI draws the knob with in `ROUTING_NODE_EFFECTS` (`presetV2.ts`). The two are separate declarations of one range; move one and move the other. Everything past that is the ordinary `node.*` path: the address resolves to the boundary node of the first preset voice playing, `SignalGraphExecutor` sums the node's `gainDb` with the global trim, and the change comes back to the UI as `signalPathNodeParamUpdated`. The MIDI panel's target picker lists the two under *Signal Chain* so a slot learned from the knob can still be retargeted by hand; there is no bypass address for a boundary node.

The automation state (slots plus registry) is requested from `main.ts` once `IPlugReceiveData` is registered. Requested earlier, the reply was lost; the `state` broadcast carries slots but not the registry, so the registry stayed empty until the MIDI panel was first opened — and the menu, which needs the registry, did not appear.

`channel` is 0-based everywhere in the engine, storage, and messaging (0-15, or -1 for "any"), matching the MIDI status byte. The UI displays it 1-based (1-16, or "any") — the convention users see on hardware — via `formatMidiChannel()` in `core/ui/ts/midiMapping.ts`. Keep the conversion at the display boundary only.

## 5b. Keyboard Handling

Keyboard input arrives at the WebView as DOM `KeyboardEvent`s — it never enters the audio thread or the JUCE `MidiBuffer`. The keyboard mapping layer is therefore **UI-resident**: a document-level `keydown` listener (capture phase, mirroring the existing pattern in `core/ui/ts/signalPath.ts:1530`) intercepts keys and sends `setAutomationValue` messages to the engine, which reconciles them as UI-source writes (see §6).

### `KeyboardMap`

A slot may have an array of keyboard bindings (multiple keys can target the same slot):

```cpp
struct KeyboardMap {
    std::string key;        // e.g. "1", "a", "PageUp", "F5"
    enum class Mode { Trigger, SetValue } mode;
    float value;            // for SetValue: normalized 0..1; for Trigger: ignored
};
```

- **`trigger`**: fires the slot (same semantics as a MIDI trigger or DAW 0→0.5+ edge). Used for `setlist.bankUp`/`bankDown` and any trigger-address slot.
- **`setValue`**: sets the slot to a fixed normalized value. Used for direct selection like setlist presets: key `"1"` → value `0.0`, key `"2"` → `1/(N-1)`, etc.

### Focus rules

The keydown listener activates only when **no specific input control is focused** — i.e. the window has focus but the user is not typing into a text field, number input, or contenteditable element. This mirrors the existing `isTextEntryElement` / `isSignalPathShortcutSuppressedElement` checks in `signalPath.ts:1542`:

1. If `document.activeElement` is an `<input>`, `<textarea>`, `[contenteditable]`, or a known suppress-on class, bail (let the keystroke reach the control).
2. Otherwise, look up `event.key` (case-insensitive for single letters) in the keyboard-map table across all slots.
3. If found, send `setAutomationValue {slotId, value, source:"keyboard"}` to the engine and call `event.preventDefault()`.

This lets the user press `"1"` to jump to the first setlist preset while not interfering with typing a preset name in a search box.

### Key capture format

`key` uses `KeyboardEvent.key` values (e.g. `"1"`, `"a"`, `"PageUp"`, `"Enter"`, `" "` for space). Single-character keys are matched case-insensitively. Modifier combinations are not supported in v1 (keep it simple for performance-style triggers).

## 6. Source Reconciliation & Threading

Each `AutomationSlot`: `atomic<float> value`, `atomic<int> lastSource`, `atomic<float> lastNormalized` (for trigger edge detection). All writes go through:

```cpp
void AutomationSlot::SetValue(float v, Source src) {
    value.store(v);
    lastSource.store((int)src);
    pendingApply.store(true);
}
```

Apply is deferred to the audio thread's `ProcessAudio` prologue under `mDSPMutex`:

- DAW and MIDI writes within a block: last writer wins per slot per block (matches JUCE host behavior).
- Keyboard writes arrive as UI messages (same path as UI knob drags); a UI/keyboard write that conflicts with the most recent DAW/MIDI write within the last ~50 ms is dropped (UI is optimistic; the host wins if it just touched the slot — matches the existing "engine value is authoritative" rule in `docs/user-interface.md`).
- The applied value is broadcast back to the UI via the periodic `BroadcastState` so UI knobs reflect DAW/MIDI activity.
- Setlist steps, bank changes and scene switches cannot be applied there. They load a preset or rewrite `setlists.json`, and a preset load takes `mDSPMutex`. So they are parked in `ControlSurfaceQueue`, which keeps only the newest request of each kind but adds up bank steps. `PluginController::DrainControlSurfaceRequests` then applies them on the message thread, after any restore or program change the host queued earlier (`HostStateRelay::ApplyQueued`). The editor's idle tick calls it, and so does a 30 Hz timer on the JUCE adapter, so a footswitch still changes preset or scene when the editor is closed. `ControlSurfaceDrainTests` covers this.

## 7. Persistence & Migration

- **User data**: stored via the existing `PluginController::SaveUiStorageJson("automation.json", ...)` / `LoadUiStorageJson("automation.json", ...)` methods, which resolve to the platform-standard user data directory through `FileSystem::ResolveUiStoragePath` → `ResolveSettingsDirectory` → `ResolveDataV1Directory` → `ResolvePlatformRootDirectory`. Concretely:
  - **Windows**: `%APPDATA%\Soundshed Guitar\data\v1\settings\ui\automation.json`
  - **macOS**: `~/Library/Soundshed Guitar/data/v1/settings/ui/automation.json`
  - **Linux**: `~/.config/Soundshed Guitar/data/v1/settings/ui/automation.json`

  This is the same location pattern used by `setlists.json`, `preset-folders.json`, `preset-favorites.json`, and `preset-ratings.json` — no new storage mechanism. Schema-versioned (`schemaVersion: 1`).
- **DAW plugin state**: `PluginController::SerializeState` packs the slot table into the existing state JSON; `DeserializeState` restores it and rebuilds the slot table before re-applying the active preset. The same state JSON also carries `mixer.presetData` — a full copy of every non-focused mixer slot's preset — so slots restore from the project rather than being reloaded by id from the machine's preset library.
- **Presets**: automation mappings are *not* part of `PresetV2` (app-level state, independent of which preset is loaded). This keeps presets portable and avoids preset-thrashing the user's DAW project mappings.
- **DAW project stability**: the JUCE parameter layout is fixed at construction (5 defaults + 16 reserved custom). Removing a custom slot in the UI keeps the JUCE parameter alive but marks it `unmapped` (address cleared); adding a new custom slot reuses the lowest free reserved index. DAW automation lanes don't drift when the user reconfigures mappings within a saved project.
- **Node target stability**: because `node.*` addresses name an effect type (resolved to its canonical UUID), not a node instance, there is **no node-id migration problem**. Presets can be re-imported, node IDs can change, scenes can swap — the slot keeps working as long as some enabled node of that type exists in the active graph. No `automationSlotUnbound` rebinding prompt is needed for node-ID changes; the slot simply resumes applying once a matching node appears.
- **Unregistered effect type**: if a `node.*` address refers to an effect UUID no longer in `EffectRegistry` (e.g. an effect removed in a newer build), the slot is marked invalid, the UI shows a warning, and the slot stays in the table so the user can rebind.
- **Legacy enum (removed)**: `PluginController::ParameterId`, `mParamValues`, `OnParamChange` and `GetParamValue` are gone. Nothing was ever bound to them — the DAW-visible parameters are the automation slots above — and the flat array was a partial mirror of the global chain that silently reverted untracked values (noise gate, EQ, doubler) when a DAW project reloaded. `GlobalSignalChainConfig` on the mixer is now the single source of truth; the `setParameter` message survives as a flat-name alias onto `setGlobalChainParam`.

## 8. Validation & Edge Cases

- `node.*` address refers to an effect UUID no longer in `EffectRegistry` → slot marked invalid, UI shows a warning, slot stays in the table (no destructive auto-removal) so the user can rebind.
- `node.*` address with `nodeSelector` set to a value the running version doesn't understand → falls back to default (first), UI flags the slot as "selector not supported in this version".
- `node.*` address with `nodeSelector` selecting a node that doesn't currently exist (e.g. `"2"` when only one matching node is present): apply is a no-op, value retained; reactivates when the selected node appears.
- Two custom slots mapped to the same address (+ selector) → allowed (last writer wins per block); UI shows an info badge.
- MIDI mapping collision (two slots on the same CC/channel/mode): allowed; both slots update. UI warns.
- Bank up/down at setlist boundary: clamp at 0 / (N-1); no wraparound in v1.
- Setlist empty: `setlist.*` slots are no-ops; DAW program API returns `getNumPrograms=1`.
- Host writes a default slot whose address's underlying feature is unavailable (e.g. `setlist.preset` with no setlist selected): slot stores the value, applies nothing, broadcasts unchanged state.
- Custom slot address cleared (unmapped): registry `Find` returns null, apply is a no-op, slot retains its value.

## 9. UI

New view in the WebView SPA: **Automation & MIDI Mapping** panel.

```
┌─ Automation Slots ───────────────────────────────────────────────────────┐
│ Slot          Target (address)                  MIDI          Key    Learn │
│ ─────────────────────────────────────────────────────────────────────── │
│ inputLevel    global.inputTrim  (-40..+20 dB)   CC7  ch0             [Learn] │
│ outputLevel   global.outputTrim (-40..+20 dB)   CC74 ch0             [Learn] │
│ setlistPreset setlist.preset    (8 items)        Prgm 1 ch0  1/2/3    [Learn] │
│ bankUp (edge)  setlist.bankUp                    CC14 ch0 tr  PageUp  [Learn] │
│ bankDown (ed) setlist.bankDown                  CC15 ch0 tr  PageDn  [Learn] │
│ ── Custom (N/16 used) ─────────────────────────────────────────────────  │
│ custom.1 NAM Gain    node.amp_nam.inputGain     CC7  ch0 abs          [Learn] │
│ custom.2 Reverb Mix node.reverb_room.mix [2]    CC74 ch0 abs          [Learn] │
│ custom.3 Transpose   global.transpose(-24..12) CC5 ch0 abs  t(trig)  [Learn] │
│ [+ Add Custom Slot]                                                       │
└─────────────────────────────────────────────────────────────────────────┘
```

Address picker for custom slots:

1. Pick a prefix: `global`, `node`, or `setlist`.
2. For `global` / `setlist`: dropdown of everything in `ParamRegistry` (label + range + unit shown).
3. For `node`: pick effect type from `EffectRegistry::GetAllTypes()` → pick parameter from that type's `EffectTypeInfo::parameters` (plus the reserved bypass target). The address is `node.<effectType>.<paramId>`. Resolution is automatic against matching nodes in the graph at apply time, so there is no node-instance picker. An optional **node selector** field (text input, placeholder "first") lets the user enter a future selector expression (e.g. `2`, `last`, `label:Lead`); v1 accepts and stores it but only implements the default (first). `node.<effectType>.bypassed` is a special target and applies to all matching nodes of that effect type.

New UI ↔ Engine messages (added to `core/src/dispatcher/MessageDispatchState.cpp` and `docs/user-interface.md`):

| Direction | Type | Payload |
|---|---|---|
| UI → Engine | `getAutomation` | `{}` |
| Engine → UI | `automation` | `{slots:[{slotId,label,address,nodeSelector?,isDefault,presetId?,value,midiMap?,keyMap?}], registry:[{address,label,unit,min,max,isStepped,isTrigger}], maxCustomSlots:16, maxPresetSlots:16}` |
| UI → Engine | `setAutomationSlot` | `{slotId, label?, address?, nodeSelector?, midiMap?, keyMap?, presetId?}` (create/update custom; defaults allow `label`/`midiMap`/`keyMap` only; `presetId` on a new slot makes it a per-preset MIDI mapping, which takes `label`/`address`/`midiMap`) |
| UI → Engine | `removeAutomationSlot` | `{slotId}` (custom only) |
| UI → Engine | `setAutomationValue` | `{slotId, value:0..1, source?}` (source: `"ui"` (default) or `"keyboard"`) |
| UI → Engine | `armMidiLearn` | `{slotId}` |
| UI → Engine | `cancelMidiLearn` | `{}` |
| Engine → UI | `midiLearnCapture` | `{slotId, eventType, channel, controller}` |
| Engine → UI | `automationValue` | `{slotId, value, source}` (real-time from DAW/MIDI/keyboard) |

State broadcast: include a compact `automationState` array in the existing `state` message so the UI always reflects current slot values without a separate request. The `registry` array is sent once on `getAutomation` and on registry changes (which only happen on app startup, so effectively once per session).

## 10. Implementation Phases

| Phase | Scope | Touch points |
|---|---|---|
| **P1 — ParamRegistry + slot model** | `ParamRegistry`, `ParamRegistryEntry`, `AutomationSlot`, slot table, `automation.json` load/save, `ApplyAutomationLocked`. Register all existing `MultiPresetMixer` global controls as `global.*` entries + the three `setlist.*` entries. Defaults populated from `kDefaultSlots`. | New `core/src/automation/{ParamRegistry,AutomationSlotTable}.{h,cpp}`; `PluginController` ownership; `SerializeState`/`DeserializeState` extension. |
| **P2 — Apply routing** | `MultiPresetMixer::SetNodeParam(nodeId, paramId, value, alreadyLocked)` (refactor of existing per-node code). Single `node.*` registry handler that resolves the effect type to the first matching enabled node in topological order. Setlist cursor + bank up/down + trim setters wired to existing mixer methods. Add `bankSize`/`cursorIndex` to setlists JSON. | `MultiPresetMixer.{h,cpp}`; `PluginController.cpp` setlist handling. No DSP changes. |
| **P3 — JUCE parameter exposure** | `AutomationSlotParameter` per slot; drain DAW parameter changes into `ApplyAutomationFromDAW` in `processBlock`; real program API driven by setlist. `acceptsMidi=true`. | `juce/source/PluginProcessorAdapter.{h,cpp}`. |
| **P4 — MIDI plumbing** | Drain `MidiBuffer` in `processBlock`; `PluginController::HandleMidi`; CC absolute/relative/toggle/pickup; MIDI learn capture. | `PluginProcessorAdapter.cpp`; new `core/src/automation/MidiDispatcher.{h,cpp}`; UI message plumbing in `MessageDispatchState.cpp`. |
| **P5 — Keyboard handling** | Document-level `keydown` listener with text-entry suppression; `KeyboardMap` array on slots; key-to-value/trigger resolution; `setAutomationValue` with `source:"keyboard"`. | New `core/ui/ts/automationKeyboard.ts`; integration in `automationPanel.ts`. |
| **P6 — UI panel** | Automation & MIDI Mapping view, address picker (global/setlist/node), MIDI/keyboard learn buttons, value sync. | New `core/ui/ts/automationPanel.ts`; `state.ts`; `messages.ts`; HTML template. |
| **P7 — Docs & tests** | Update `docs/user-interface.md` (message table), `docs/data-models.md` (`automation.json`), `docs/architecture-overview.md` (automation layer). Unit tests for registry resolution, node.* lazy binding, MIDI decode, keyboard focus suppression, source reconciliation, trigger edge detection. | docs + `core/tests/`. |

## 11. Test Plan

- `ParamRegistry` round-trip: every registered address resolves to its entry; unregistered address returns null.
- `node.*` address: effect type resolves via `EffectRegistry::Resolve`; first enabled node of that type in topological order is driven; if none, apply is a no-op and value is retained.
- `node.*` address on a missing effect type (no node of that type in graph): apply is a no-op, value retained; adding a node of that type resumes writes.
- `node.*` address where the only matching node is disabled/bypassed: apply is a no-op until `enabled=true` (existing bypass semantics preserved).
- `node.*` address with multiple matching nodes and no `nodeSelector`: only the first in topological order is driven.
- `node.*.bypassed` target with multiple matching nodes: bypass state is applied to all matching nodes of the selected type.
- `node.*` address with `nodeSelector` set to an unrecognized value: falls back to first, slot flagged as "selector not supported".
- `node.*` address with `nodeSelector="2"` (future grammar, not yet implemented): v1 falls back to first; once implemented, selects the 2nd matching node and is a no-op if fewer than 2 exist.
- `global.*` address: apply calls the bound setter with the correct native-range value.
- `setlist.preset` stepped quantization: normalized 0..1 maps to indices 0..(N-1) with rounding.
- `setlist.bankUp`/`bankDown` trigger edge detection: fires only on the 0→0.5+ rising edge, not on held value; clamps at setlist boundary.
- DAW write + MIDI write in the same block: last writer wins, UI receives the resolved value.
- DAW program API: `getNumPrograms` tracks setlist length; `setCurrentProgram` loads the preset and broadcasts `presetLoaded`.
- MIDI learn: arming a slot captures the next event and writes `MidiControlMap`; cancel leaves the slot untouched.
- Keyboard `setValue` mapping: pressing `"1"` sends `setAutomationValue` with the correct normalized value; engine applies it as a UI-source write.
- Keyboard `trigger` mapping: pressing the mapped key fires the trigger slot once per keydown (not on auto-repeat / `event.repeat`).
- Keyboard focus suppression: when `document.activeElement` is a text input/textarea/contenteditable, the keydown listener does not intercept.
- Keyboard focus passthrough: when the window has focus but no input control is focused, keys are intercepted and `preventDefault` is called for matched mappings.
- `automation.json` persistence: file is written to the platform UI-storage path (`ResolveUiStoragePath` → `settings/ui/automation.json`) and round-trips with `SaveUiStorageJson`/`LoadUiStorageJson`.
- Unmapped slot (address cleared): apply is a no-op, value retained, no errors.
- Backward compatibility: loading a state blob from a build without automation leaves the slot table empty and all defaults at their current trim/0 values — no audio regression.
- Future-global-proof: adding a `global.transpose` registry entry and a custom slot pointing at it works with no slot-model code change (verified by a test that registers a fresh address at runtime and applies it).

## See Also

- [Architecture Overview](architecture-overview.md) — layering this slots into
- [Signal Chain](signal-chain.md) — `node.*` target resolution against the graph
- [FX Library](fx-library.md) — `EffectTypeInfo::parameters` (node-target picker source of truth)
- [User Interface](user-interface.md) — new message types and the Automation panel
- [Data Models](data-models.md) — `automation.json` schema location
