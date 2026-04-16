# User Interface

## Key Files
- `core/ui/ts/messages.ts` — Message handlers and state application
- `core/ui/ts/state.ts` — UI state management
- `core/ui/ts/main.ts` — Application entry point
- `core/src/PluginController.cpp` — Engine-side state and message handling
- `core/src/UiBridge.h` — Native bridge interface

## Overview

The UI is a web-based single-page application (SPA) hosted in a native WebView. Communication with the plugin uses a bidirectional JSON message protocol. The UI maintains local state synchronized with the engine via events.

## Architecture

```
┌──────────────────────────────────────┐
│          Web UI (TypeScript)         │
│  ┌────────────────────────────────┐  │
│  │     View Components            │  │
│  ├────────────────────────────────┤  │
│  │     State Management           │  │
│  ├────────────────────────────────┤  │
│  │     Message Handler            │  │
│  └────────────────────────────────┘  │
└───────────────┬──────────────────────┘
                │ JSON Messages
┌───────────────▼──────────────────────┐
│          WebUI Bridge (C++)          │
│  Serialize/deserialize, dispatch     │
└───────────────┬──────────────────────┘
                │
┌───────────────▼──────────────────────┐
│       Plugin Controller (C++)        │
└──────────────────────────────────────┘
```

### WebView Host
- **Windows**: WebView2 (Chromium-based)
- **macOS**: WKWebView (WebKit-based)
- Sandboxed execution, communication only through message bridge

## Message Protocol

### Message Format
```json
{
  "type": "messageType",
  "payload": { ... },
  "timestamp": 1704801234567
}
```

### Engine → UI Messages

| Type | Payload | Description |
|------|---------|-------------|
| `state` | Full state object | Complete sync on startup/major changes |
| `presetLoaded` | `{preset, sceneId, activePresetIds, parameters}` | Preset load notification |
| `presetSaved` | `{preset, sceneId}` | Preset saved to disk confirmation |
| `presetList` | `{presets: [{id, name, category, source}]}` | Factory/user presets from disk |
| `error` | `{message, detail}` | Error notification |
| `signalPathTestResult` | `{frequency, duration, elapsed, ...}` | Signal test completed |
| `previewStarted` | `{id, title}` | Demo audio playback started |
| `previewComplete` | `{id, title}` | Demo audio playback finished |
| `previewStopped` | `{id?, title?}` | Demo audio playback stopped by user |
| `tunerUpdate` | `{note, cents, frequency, ...}` | Tuner pitch detection update |
| `tunerStarted` | `{}` | Tuner activated |
| `tunerStopped` | `{}` | Tuner deactivated |
| `modelLoaded` | `{path}` | NAM model loaded |
| `irLoaded` | `{path}` | IR cab loaded |
| `resourceImported` | `{...}` | Remote resource imported |
| `resourceImportFailed` | `{message}` | Remote resource import failed |
| `globalChain` | `{config}` | Global signal chain configuration |
| `effectCatalog` | `{effects: [...]}` | Available effect types |
| `dspPerformance` | `{...}` | DSP performance statistics |
| `signalLevelDiagnostics` | `{rawInput, input, output, nodes}` | Signal level diagnostics and per-node meters |
| `metronomeState` | `{bpm, enabled, ...}` | Metronome state |
| `layoutSaved` | `{...}` | Effect layout saved |
| `layoutLibraryLoaded` | `{layoutLibrary}` | Layout library loaded |
| `compositeLibrary` | `{...}` | Composite effect library |
| `compositeDefinitionAdded` | `{...}` | Composite effect added |
| `compositeDefinitionRemoved` | `{...}` | Composite effect removed |
| `compositeEditState` | `{...}` | Composite edit mode state |
| `compositeEditModeExited` | `{}` | Exited composite edit mode |

### UI → Engine Messages

| Type | Payload | Description |
|------|---------|-------------|
| `uiReady` | `{}` | WebView loaded and ready |
| `requestState` | `{}` | Request full state sync |
| `setParameter` | `{name, value}` | Update parameter value |
| `loadPreset` | `{preset, sceneId?}` | Load preset with full object and optionally select a scene |
| `savePreset` | `{name, category, description}` | Save current state as preset to disk |
| `loadModel` | `{filePath}` | Load NAM model by path |
| `loadIR` | `{filePath}` | Load IR cab by path |
| `browseModel` | `{}` | Open model file browser |
| `browseIR` | `{}` | Open IR file browser |
| `addSignalPathNode` | `{node, afterNodeId}` | Add effect to graph |
| `deleteSignalPathNode` | `{nodeId}` | Remove effect from graph |
| `replaceSignalPathNode` | `{nodeId, newNode}` | Replace effect in graph |
| `reorderSignalPathNode` | `{nodeId, newIndex}` | Reorder effect in graph |
| `updateSignalPathNodeParam` | `{nodeId, paramId, value}` | Update effect parameter |
| `updateSignalPathNodeBypass` | `{nodeId, bypassed}` | Bypass/enable effect |
| `updateNodeResource` | `{nodeId, resource}` | Change node resource |
| `browseNodeResource` | `{nodeId}` | Browse for node resource |
| `addActivePreset` | `{presetId}` | Add preset to multi-mixer |
| `removeActivePreset` | `{presetId}` | Remove preset from mixer |
| `setPresetMix` | `{presetId, mix}` | Set mixer preset level |
| `setPresetPan` | `{presetId, pan}` | Set mixer preset pan |
| `setPresetMute` | `{presetId, mute}` | Mute mixer preset |
| `setPresetSolo` | `{presetId, solo}` | Solo mixer preset |
| `setMasterGain` | `{gain}` | Set master output gain |
| `setLimiterEnabled` | `{enabled}` | Enable/disable limiter |
| `setInputMode` | `{mode}` | Set input mode (mono/stereo) |
| `setAmpCabState` | `{...}` | Set amp/cab enable state |
| `setAutoLevel` | `{...}` | Legacy compatibility message; controller forces mixer-wide auto-level back off |
| `setMetronome` | `{bpm?, enabled?, ...}` | Update metronome settings |
| `tuner` | `{action}` | Start/stop/configure tuner |
| `runSignalPathTest` | `{}` | Run signal path diagnostic |
| `previewDemoAudio` | `{audio}` | Preview demo audio clip |
| `stopDemoAudio` | `{}` | Stop demo audio playback |
| `importRemoteResource` | `{...}` | Import resource from remote |
| `setSetting` | `{key, value}` | Persist and apply an app setting |
| `setUserInputCalibrationTrainingActive` | `{active}` | Temporarily bypass the active calibration profile while training |
| `setGlobalChainParam` | `{param, value}` | Set global chain parameter |
| `getGlobalChain` | `{}` | Request global chain state |
| `getEffectCatalog` | `{}` | Request effect catalog |
| `getPresetList` | `{}` | Request preset list from disk |
| `openAudioPreferences` | `{}` | Open audio device settings |

## State Object

Sent via `state` message on startup and major changes:

```json
{
  "parameters": {
    "input_trim": 0.0,
    "output_trim": -3.0,
    "amp1_drive": 0.65
  },
  "currentPreset": {
    "id": "preset-123",
    "name": "My Crunch Tone",
    "modified": true
  },
  "presets": [
    {"id": "preset-1", "name": "Clean", "category": "Clean"}
  ],
  "library": {
    "nam": [{"id": "plexi-bright", "name": "Plexi Bright", "category": "Marshall"}],
    "ir": [{"id": "4x12-sm57", "name": "4x12 SM57", "category": "Marshall"}]
  },
  "signalGraph": {
    "nodes": [...],
    "edges": [...]
  }
}
```

## JavaScript Bridge

### Sending Messages (UI → Engine)
```typescript
window.NAMBridge.postMessage("setParameter", { id: "amp1_drive", value: 0.72 });
```

### Receiving Messages (Engine → UI)
```typescript
// Called by native code
window.IPlugReceiveData = function(jsonString) {
    const message = JSON.parse(jsonString);
    handleMessage(message);
};
```

## Synchronization

### Startup Sequence
1. WebView loads UI application
2. UI sends `requestState` message
3. Engine sends `state` message with full snapshot
4. UI renders initial state

### Parameter Updates
```
UI changes parameter:
1. User adjusts control
2. UI updates local state immediately (optimistic)
3. UI sends setParameter message (debounced 50ms)
4. Engine updates parameter
5. Engine includes update in next state broadcast

Engine changes parameter (automation):
1. DAW writes automation value
2. Engine includes in state broadcast
3. UI updates display
```

### Conflict Resolution
Engine value is authoritative. If UI receives a state broadcast with a different value than it sent, it adopts the engine value.

### Scene Editing

Presets can expose multiple named scenes. The UI edits one scene at a time in the signal-path bar,
while the engine keeps the full preset definition synchronized. Existing single-graph presets are
treated as a one-scene preset automatically.

## UI Views

| View | Purpose |
|------|---------|
| **Main** | Amp panel, global controls, level meters |
| **Preset Browser** | Local preset management, search, load/save |
| **Community Browser** | Remote preset search and download |
| **Signal Chain Editor** | Visual node-based effect chain |
| **Resource Browser** | NAM model and IR selection |
| **Settings** | Audio preferences, storage, theme |

## Settings → Audio

### User Input Calibration

The live product uses named user input calibration profiles instead of the older NAM interface calibration reference model.

**Behavior**
- A profile stores one fixed gain value in dB.
- The active profile applies that gain once at the mixer input before the pre-chain and preset graphs.
- While calibration training is active, the live calibration gain is bypassed temporarily so the capture reflects the raw input.

### Advanced DSP Level Targets

Two advanced settings affect runtime level behavior immediately:

- **Nominal Operating Level**: shared loudness target used by NAM output normalization when resource-owned normalization data is unavailable.
- **Output Protection Ceiling**: final ceiling used by mixer output protection.

**Defaults**
- Nominal operating level: **-18 dBFS**
- Output protection ceiling: **-1 dBFS**

## Parameter Controls

| Control | Usage |
|---------|-------|
| Knob | Continuous parameters (gain, drive) |
| Slider | Linear parameters (trim, mix) |
| Toggle | On/off states (bypass) |
| Dropdown | Selection (effect type, category) |
| Button | Actions (load, save, browse) |

## Signal Chain Editor Notes

- To create parallel paths, add the **Splitter** effect from the Utility category. The join **Mixer** node is inserted automatically and is not user-addable.

## Performance Targets

| Metric | Target |
|--------|--------|
| Initial Load | < 500ms |
| View Switch | < 100ms |
| Parameter Response | < 50ms |
| Frame Rate | 60fps |

## Error Handling

| Error Type | Presentation |
|------------|--------------|
| Validation | Inline message near control |
| Operation Failure | Toast notification |
| Connection Error | Status indicator |
| Critical Error | Modal dialog |

## Accessibility

- Tab order for all controls
- ARIA labels on interactive elements
- Keyboard navigation (arrows for lists, Enter/Space for activation)
- Sufficient color contrast, scalable text

## See Also
- [Theme System](theme-system.md) — CSS theming
- [Architecture Overview](architecture-overview.md) — System layers
- [Signal Chain](signal-chain.md) — Graph modification messages
