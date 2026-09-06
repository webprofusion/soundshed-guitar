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
| `state` | Full or preset-scoped state object | Complete sync on startup/major changes; preset/scene switches send a preset-scoped subset (see below) |
| `presetLoaded` | `{preset, sceneId, activePresetIds, parameters}` | Preset load notification |
| `presetSaved` | `{preset, sceneId}` | Preset saved to disk confirmation |
| `presetList` | `{presets: [{id, name, category, source}]}` | Factory/user presets from disk |
| `error` | `{message, detail}` | Error notification |
| `signalPathTestResult` | `{frequency, duration, elapsed, ...}` | Signal test completed |
| `previewStarted` | `{id, title}` | Demo audio playback started |
| `previewComplete` | `{id, title}` | Demo audio playback finished |
| `previewStopped` | `{id?, title?}` | Demo audio playback stopped by user |
| `demoAudioRenderSaved` | `{path, sampleRate}` | Rendered demo audio written to disk |
| `demoAudioRenderFailed` | `{message}` | Demo audio render/save failed |
| `tunerUpdate` | `{note, cents, frequency, ...}` | Tuner pitch detection update |
| `tunerStarted` | `{}` | Tuner activated |
| `tunerStopped` | `{}` | Tuner deactivated |
| `modelLoaded` | `{path}` | NAM model loaded |
| `irLoaded` | `{path}` | IR cab loaded |
| `hostedPluginResourceLoadFailed` | `{nodeId, resourceType, resourceId?, filePath?, resourceIndex?, message}` | Hosted plugin failed to load; UI shows inline error and clears loading indicator |
| `hostedPluginResourceLoadCompleted` | `{nodeId, resourceType, resourceId?, resourceIndex?}` | Hosted plugin resource selection finished loading; UI clears loading indicator |
| `nodeResourceBrowseCancelled` | `{nodeId, resourceType, resourceIndex?, exposedResourceId?}` | Node resource browse dialog dismissed without a selection |
| `resourceImported` | `{...}` | Remote resource imported |
| `resourceImportFailed` | `{message}` | Remote resource import failed |
| `globalChain` | `{config}` | Global signal chain configuration |
| `effectCatalog` | `{effects: [...]}` | Available effect types |
| `dspPerformance` | `{stats: {totalProcessingTimeUs, realTimeUs, dspLoadPercent, totalLatencySamples, nodeProcessingTimesUs, nodeLatencySamples}, sampleRate, blockSize}` | DSP timing at 5 Hz, for the performance panel and the per-node readouts. Both node maps are keyed `<scope>::<nodeId>` — `pre::`, `post::`, or the preset id — since a bare node id only identifies a node within one executor. A node missing from `nodeProcessingTimesUs` did not run last block; the UI blanks it rather than showing a zero. |
| `sldRoster` | `{seq, nodes: [[scope, presetId, nodeId, nodeType, hasAnalyzer]], spectrogramRange, barkRange}` | Signal diagnostics roster: everything about the node set that does not change frame to frame. Sent only when the node set changes, and on `getSignalDiagnostics`. |
| `sld` | `{seq, r, i, o, d}` | Signal level frame at 20 Hz. `r`/`i`/`o` are raw input, processed input and output; `d` holds one tuple per roster node, flattened in roster order. Every tuple is `[peakDbfs, rmsDbfs, clipCount, clipped, channelCount]`, the levels rounded to 0.1 dB; `headroomDb` is derived UI-side. `channelCount` (0 = the node did not run this block, 1 mono, 2 stereo) rides here rather than in the roster: it tracks the signal, so keeping it in the roster re-sent that whole message several times a second. Frames whose `seq` does not match the held roster are dropped. |
| `sldA` | `{seq, id, t, l, s, b}` | Analyzer telemetry for one node — levels `l`, spectrogram bins `s` and bark bands `b` in whole dBFS. Sent separately from `sld` because it is an order of magnitude larger than a level tuple. |
| `spatialPosition` | `{nodes: [{scope, presetId?, nodeId, azimuth, elevation, distance, itdUs, ildDb, rateHz, moving}]}` | Live source position for every 3D Spatial node, ~20 Hz. Purely cosmetic: it keeps the spatial panner's puck in sync with what is being heard, and the widget falls back to the anchor position if it never arrives. Only sent while at least one such node exists. |
| `metronomeState` | `{bpm, enabled, volumeDb, pan, clickType, clickTypes, beatPattern, timeSigNum, timeSigDen, grouping, subdivision, subdivisions}` | Metronome state. `beatPattern` is one character per beat — `H` accent, `M` medium, `L` normal, `S` silent — always exactly `timeSigNum` long. `grouping` is `"2+2+3"` for an odd meter, empty otherwise. `clickTypes` and `subdivisions` are the lists the pickers are built from. |
| `metronomeBeat` | `{beatIndex, beatsPerBar, level}` | One per beat while the click runs and the UI is visible, for the beat display. `level` is `accent`/`medium`/`normal`/`silent`. Subdivision ticks are not sent. |
| `layoutSaved` | `{...}` | Effect layout saved |
| `layoutLibraryLoaded` | `{layoutLibrary}` | Layout library loaded |
| `compositeLibrary` | `{...}` | Composite effect library |
| `compositeDefinitionAdded` | `{...}` | Composite effect added |
| `compositeDefinitionRemoved` | `{...}` | Composite effect removed |
| `compositeEditState` | `{...}` | Composite edit mode state |
| `compositeEditModeExited` | `{}` | Exited composite edit mode |
| `compositePresetList` | `{compositePresets: [{id, name, description, tags, createdAt, modifiedAt, slots: [{slotId, presetId, mix, pan, mute, solo}], mixGainDb}]}` | Saved Multi-Rig presets; sent on request and after every save or delete. Older files' `masterGain`/`limiterEnabled` are ignored |
| `compositePresetSaved` | `{id, name}` | Multi-Rig saved. The UI treats this id as the loaded Multi-Rig (toolbar offers Update/Delete) until the mixer's membership changes |
| `compositePresetLoaded` | `{id, name}` | Multi-Rig applied to the mixer; a full `state` follows |
| `practiceToolFileLoaded` | `{path, title, durationSec, waveformPeaks}` | Practice Tool: backing-track file decoded and ready |
| `practiceToolTransportState` | `{state, positionSec}` | Practice Tool: playback state/position, pushed periodically while loaded |
| `practiceToolPlaybackEnded` | `{}` | Practice Tool: playback reached the end of the file (non-looping) |

### UI → Engine Messages

| Type | Payload | Description |
|------|---------|-------------|
| `uiReady` | `{}` | WebView loaded and ready |
| `requestState` | `{}` | Request full state sync |
| `setParameter` | `{name, value}` | Set one global FX value by flat name; alias for `setGlobalChainParam` |
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
| `setMasterGain` | `{gain}` | Set the mixer's linear master multiplier directly. Only the output mute uses this (0 to mute, the previous value to restore); level changes go through `setGlobalChainParam` with path `output.gain`, since the engine derives the multiplier from that setting and re-derives it on every chain rebuild |
| `setMixGain` | `{gainDb}` | The Multi-Rig's own level in dB, applied to the summed preset mix ahead of the global post-chain and output stage. Independent of `output.gain`; reported back as `mixer.mixGainDb`, saved with a Multi-Rig, and reset to 0 dB when a single preset is loaded |
| `setLimiterEnabled` | `{enabled}` | Enable/disable the output limiter. Still accepted; the UI no longer sends it since the Mix tab's limiter switch was removed |
| `saveCompositePreset` | `{name, description?, tags?, id?}` | Save the current mixer (slots, levels, mix gain) as a Multi-Rig preset; with `id`, update that one in place |
| `loadCompositePreset` | `{id}` | Replace the mixer with a saved Multi-Rig's slots, levels and mix gain. The instance's output gain is left alone, as with any preset load |
| `getCompositePresetList` | `{}` | Request `compositePresetList` |
| `removeCompositePreset` | `{id}` | Delete a saved Multi-Rig preset |
| `setInputMode` | `{mode}` | Set input mode (mono/stereo) |
| `setAmpCabState` | `{...}` | Set amp/cab enable state |
| `setAutoLevel` | `{...}` | Legacy compatibility message; controller forces mixer-wide auto-level back off |
| `setMetronome` | `{bpm?, enabled?, volumeDb?, pan?, clickType?, clickConfig?, beatPattern?, timeSigNum?, timeSigDen?, grouping?, subdivision?}` | Update metronome settings. The engine normalises before storing: a grouping that does not add up to the bar is dropped, and a meter change re-seeds `beatPattern` unless the same message carries one. |
| `tuner` | `{action}` | Start/stop/configure tuner |
| `runSignalPathTest` | `{}` | Run signal path diagnostic |
| `previewDemoAudio` | `{audio}` | Preview demo audio clip |
| `renderDemoAudio` | `{audio? , takeId?, title?, suggestedName?, renderSampleRate?}` | Render selected demo audio to a WAV file using the current preset. `renderSampleRate` accepts `44100`, `48000`, `88200`, `96000`, `176400`, or `192000`; omit or pass `0` for the current device rate. The save-dialog filename appends the resolved rounded kHz rate before `.wav`. |
| `stopDemoAudio` | `{}` | Stop demo audio playback |
| `previewCapturedRiff` | `{startRatio, endRatio, repeat}` | Play the captured riff take. The whole take goes over once; the markers travel as a region and the engine loops it in place, so repeating costs no further messages. |
| `setRiffPreviewRegion` | `{startRatio, endRatio, repeat}` | Retune the region/repeat of the preview already playing — for a marker dragged mid-playback. Debounced by the UI, since each one rebuilds the wrap crossfade behind the DSP lock. |
| `importRemoteResource` | `{...}` | Import resource from remote |
| `setSetting` | `{key, value}` | Persist and apply an app setting |
| `setUserInputCalibrationTrainingActive` | `{active}` | Temporarily bypass the active calibration profile while training |
| `setGlobalChainParam` | `{param, value}` | Set global chain parameter |
| `getGlobalChain` | `{}` | Request global chain state |
| `getEffectCatalog` | `{}` | Request effect catalog |
| `getPresetList` | `{}` | Request preset list from disk |
| `openAudioPreferences` | `{}` | Open audio device settings |
| `browsePracticeToolFile` | `{}` | Practice Tool: open native file browser for a backing track |
| `loadPracticeToolFile` | `{path}` | Practice Tool: load a backing track by native path |
| `loadPracticeToolFileData` | `{fileName, data}` | Practice Tool: load a backing track from base64 bytes — used for a drag-and-drop, where WebView2 never exposes the real file path |
| `setPracticeToolTransport` | `{action}` | Practice Tool: `"play"`, `"pause"`, or `"stop"` |
| `seekPracticeToolFile` | `{seconds}` | Practice Tool: seek to a position |
| `setPracticeToolSpeed` | `{ratio}` | Practice Tool: time-stretch ratio, clamped `[0.25, 2.0]` |
| `setPracticeToolPitch` | `{semitones}` | Practice Tool: pitch shift, clamped `[-12, 12]` semitones |
| `setPracticeToolGain` | `{gain}` | Practice Tool: linear output gain |
| `setPracticeToolBalance` | `{balance}` | Practice Tool: stereo balance, `-1` (full left) to `+1` (full right) |
| `setPracticeToolLoopRegion` | `{startSec, endSec}` or `{}` | Practice Tool: set (or, with bounds omitted, clear) the active loop region. Sent only when the UI activates/deactivates a loop — the engine has no concept of the loop library itself |
| `setPracticeToolLooping` | `{enabled}` | Practice Tool: enable/disable looping of the active region |
| `setPracticeToolEq` | `{enabled?, params?}` | Practice Tool: backing-track EQ. Both fields optional and applied independently — the toggle alone, one band's `{lowGain, lowFreq, lowQ}` mid-drag, or the whole curve on a project recall. `params` keys are `ParametricEQEffect`'s own (`lowGain`/`lowFreq`/`lowQ`, `lowMid*`, `highMid*`, `high*`); unknown keys are ignored and every value is clamped by the effect |

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

### Broadcast scope

`state` comes in two scopes (`PluginController::StateScope`):

- **Full** — everything above. Sent on startup, on an explicit `requestState`/`uiReady`, and
  whenever library or settings state changes. On a real library this is ~510 KB, ~90% of it
  `resourceLibrary`, which also costs one filesystem stat per entry to build. It is followed
  by `compositeLibrary` and `effectCatalog` (~68 KB together).
- **Preset-scoped** — sent when a preset or scene switch is the only thing that changed
  (~10 KB, no supplementary messages). Carries `preset`, `activePresetId`, `activeSceneId`,
  `activePresetIds`, `mixer`, `globalSignalChain` and `presetArchiveSession`; omits the
  resource/riff/blend/custom-effect libraries, app settings, UI settings, UI view state,
  metronome, environment and automation, none of which a preset switch can change.

Rules for anyone touching this:

- Every section the UI reads is behind a presence check, so omitting a key is a no-op there.
  Three keys are **not** safe to omit and are always sent: `activePresetId` (read
  unconditionally), `globalSignalChain` (its absence triggers a `getGlobalChain` round trip)
  and `presetArchiveSession` (its absence clears the UI's archive-session state).
- A full request queued in the same idle window wins over a preset-scoped one.
- The periodic telemetry feeds (`sld` at 20 Hz, `dspPerformance`) only
  drive on-screen meters and are suppressed while the UI reports itself hidden via
  `uiVisibility`.

## JavaScript Bridge

### Sending Messages (UI → Engine)
```typescript
window.NAMBridge.postMessage({
  type: "setGlobalChainParam",
  path: "gate.threshold",
  value: -52.0,
});
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
UI changes a global FX value:
1. User adjusts control
2. UI updates local state immediately (optimistic)
3. UI sends setGlobalChainParam message (debounced 50ms)
4. Engine writes it into the global chain config — the single source of truth
5. Engine includes update in next state broadcast

UI changes a node parameter:
1. UI sends updateSignalPathNodeParam with {nodeId, paramKey, value}

Engine changes a value (automation):
1. DAW writes an automation slot value
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

The knob widget itself is `core/ui/ts/knob.ts` (`GenericKnob`): drag to change,
double-click to reset, double-click the value to type an exact one. It lives
apart from `controls.ts` so any module can use one without importing that file
and joining an import cycle with it; `controls.ts` re-exports it for the modules
that already import it from there.

### EQ panel (`core/ui/ts/eqPanel.ts`)

**One EQ UI, however many EQs the app grows.** `EqPanel` is the whole four-band
parametric control surface — the band knobs, the draggable curve, the enable
toggle, an optional reset — and it is what both the Global EQ modal and the
Practice Tool's Backing Track EQ render. Adding an EQ to a future feature means
writing a binding, not another panel.

- **The binding is the only thing a feature supplies.** An `EqPanelBinding` says
  where the values live (`readParams`) and how a change reaches the engine
  (`writeParams(changed, commit)`), plus the same pair for the enabled flag.
  The Global EQ binds to the post-chain `global_eq` node and sends
  `setGlobalChainParam`; the Practice Tool binds to `uiState.practiceTool.eq`
  and sends `setPracticeToolEq`, coalescing in-progress drags (`commit: false`)
  and flushing at the end of a gesture. Neither owns any control code.
- **The bands come from the topology, not from markup.** Rows are generated from
  `EQ_BAND_KEYS` / `EQ_BAND_RANGES` / `EQ_FREQ_DEFAULTS` in `eqCurve.ts`, which
  already mirror `ParametricEQEffect`, so a band added there appears in every
  panel. Host markup is one empty div. Knob ids are namespaced by `idPrefix` so
  two panels can be open at once without colliding.
- **Curve and knobs stay in step.** Dragging a band handle updates its knobs and
  vice versa; a `syncing` guard stops a render echoing the whole curve straight
  back to the engine. The curve is built lazily, because a canvas inside a
  `display: none` modal measures zero and would draw nothing.
- **Dimming is the component's.** It toggles `is-eq-enabled` on its bands host
  whenever the flag changes, so a panel outside an `.eq-section` gets the
  disabled affordance without the host arranging for it.

### Waveform range selection (`core/ui/ts/waveform/`)

**One two-handle range gesture, shared by every waveform editor.** The Practice
Tool's loop editor and the Riff Capture crop editor present the same interaction —
two handles dragged across a canvas — and used to implement it twice. The copies
drifted, and each had learned a lesson the other had not: the Practice Tool had a
handle hit radius and a click/drag threshold, the Riff editor had Home/End keys,
and dragging one handle past the other was broken in both, differently.

- **Three modules: model, gesture, picture.** `range.ts` is headless — no DOM
  beyond a pointer-to-ratio helper, no bridge, no state — so it unit-tests
  directly. `rangeSelect.ts` owns which handle is being steered, when a press
  becomes a drag, and when a gesture is finished. `render.ts` draws the canvas.
  Hosts keep the range and say what it means; they track no drag state and make
  no canvas calls of their own.
- **One themed palette, two emphases.** Every colour resolves from CSS custom
  properties (`--waveform-*`), the same way `eqCurve.ts` does, so the three themes
  restyle these canvases without touching TypeScript. Callers name meanings — an
  active range, a candidate range, a recording in progress — never colours.
  Defaults live in `variables.css` and are the dark values, so dark needs no
  override; light and classic override in `css/waveform.css`, alongside the rest
  of the component (`themes/light.css` is already a thousand lines and should not
  keep growing). The palette is resolved once per theme, not once per draw:
  `getComputedStyle` forces a style recalc and the Practice Tool repaints every
  animation frame while a track plays. Both editors repaint on the `themeChanged`
  event, because a canvas does not restyle itself. The one deliberate divergence is what a range does to
  what it covers: a loop region *tints what it includes*, a crop range *darkens
  what it will discard*. Those say different things, so both are kept — and they
  sit on opposite sides of the trace to match, a shade going down before it so the
  excluded peaks stay legible, a tint over it because it is colouring what it
  covers. Stereo lanes are just `lanes.length`, and the Riff editor's recording
  overlay is `traceLimitRatio` + `shadeAfterRatio` + a coloured playhead rather
  than a second code path.
- **Crossing swaps and retargets.** `clampRatioRange` reports a `swapped` flag when
  the bounds arrive inverted, and the controller flips the steered handle in
  response. Without the flip a caller keeps writing the bound it is no longer
  holding, so every subsequent event re-crosses — the Practice Tool's region used
  to shrink to one pointer-step wide and slide along, and the Riff editor's
  trailing marker was dragged onto the leading one, destroying it.
- **Steps are seconds, not ratios.** Both editors used to nudge by a fraction of
  the material, so the same arrow key moved 10ms on a short riff and half a second
  on a long backing track. Hosts now declare `nudgeStepSec` and supply duration.
- **The host decides what the gesture means.** `onCreate`/`onSeek` are optional:
  omit `onSeek` and a click does nothing (the Riff editor has no transport to seek).
  `onCommit` fires once at the end of a gesture — including after a burst of
  keyboard repeats collapses — which is where a debounced send is flushed and
  state persisted.
- **Pointer events, with capture.** Same reasoning as `pointerDrag.ts`: touch and
  pen work, and a drag survives the pointer leaving the canvas or the window. The
  shared `.waveform-range` class (`css/waveform.css`) carries the cursor rules,
  including the `is-resizing` affordance that only appears once a press is held
  past `CURSOR_HOLD_MS`.

## Signal Chain Editor Notes

- To create parallel paths, add the **Splitter** effect from the Utility category. The join **Mixer** node is inserted automatically and is not user-addable.

### Chain undo/redo and A/B (`core/ui/ts/signalPath/history.ts`, `historyModel.ts`)

Undo/redo over the active preset's signal chain — nodes, wiring, parameters,
bypass and per-node config — plus an A/B pair of chains in the footer
(`#footer-chain-history`). Not to be confused with the preset **navigation**
history in `presets.ts`, which steps between presets and restores nothing
unsaved.

- **What is recorded.** Every chain edit ends up on `uiState.activePresetDraft`
  (or the focused mixer slot). Rather than instrument each of the ~20 senders
  that can change it, the module listens for `presetDirtyChanged` — raised by
  `setPresetDirty()`, which all of them call — and, after a 320 ms debounce,
  compares the live graph's signature with the top of the stack. An unchanged
  graph records nothing, so a broad trigger is harmless and cannot miss a path.
  The debounce also lets a topology edit's engine round trip land first, and
  coalesces a knob drag into one step.
- **How a restore is applied.** Reloading the whole preset rebuilds the DSP
  graph and every NAM model, which is audible. So a restore whose topology
  matches the live chain is replayed as targeted
  `updateSignalPathNodeParam` / `...Bypass` / `...Config` messages instead;
  only a structural difference — added/removed/reordered/replaced nodes,
  changed edges or gains, a swapped resource — falls back to a full
  `loadPreset`. `diffNodeStates()` decides, returning `null` for anything the
  targeted messages cannot express.
- **A and B.** Two independent stacks, one live at a time, both seeded from the
  preset as loaded. Switching applies the other slot's current chain and leaves
  each cursor untouched, so A/B is a comparison rather than an undo step. The
  ⇄ button forks the live chain into the other slot as an undoable step there.
  Loading a different preset or scene re-seeds both and makes A live.
- **Dirty flag.** A full restore round-trips through `loadPreset`, whose
  `presetLoaded` echo overwrites `uiState.activePresetSnapshot` — the app's
  record of the preset as last saved. History puts the real one back afterwards
  and recomputes the flag, so stepping back through edits does not silently
  look like a save and drop the discard-changes prompt.
- **Keys.** Ctrl/Cmd+Z undoes, Ctrl/Cmd+Shift+Z and Ctrl/Cmd+Y redo. Suppressed
  while a text field has focus, while any `.modal` is open (the Layout Designer
  has its own undo on the same chord), and in composite edit mode.

### Spatial panner (`core/ui/ts/spatialPanner.ts`)

The **3D Spatial** effect gets a bespoke widget in its parameter panel, mounted the same
way the EQ curve is (see `updateSpatialVisualization` in `signalPath.ts`).

- **Top-down radar** — azimuth and distance. The listener is at the centre facing up the
  screen; distance rings are logarithmic so the near field, where the cues change
  fastest, is actually draggable. The source puck shrinks with distance and shifts
  colour when it passes behind.
- **Elevation arc** — height, linked back to ear level by a dashed drop line so the two
  views read as one object rather than two unrelated controls.
- **Motion** — while the motion engine is running, the dashed ring is the anchor you
  dragged and the filled puck is what you are actually hearing, driven by the
  `spatialPosition` message. A fading trail shows the trajectory.
- **Honesty** — with `listenMode = Speakers` the elevation pane is dimmed and the rear
  half of the radar is shaded, because the DSP is no longer delivering those cues. The
  header hint switches from "Best on headphones" to say so.
- **Interaction** — pointer and touch drag, Shift for fine adjustment, double-click to
  reset just the axis you clicked, and full keyboard control: arrows pan and tilt,
  Alt+Up/Down changes distance, Home re-centres. The canvas is focusable with a live
  `aria-label` describing the position in words.
- Redraws are coalesced through a single `requestAnimationFrame`; there is no free-running
  animation loop.

### Effect layout selection (`core/ui/ts/layoutPreferences.ts`, `layoutPicker.ts`)

Every effect renders either the **standard** auto-generated controls or a **custom
layout** from the layout library. A layout button (`.node-layout-switch-btn`) in the
effect shell's meta rail opens the layout picker popover; it is the single entry point
for both choosing and designing layouts, so it stays visible whenever the `EffectLayout`
feature flag is on — even before the effect has any layouts — and otherwise only when
the effect has layouts to switch between. (The separate gear button that used to open
the designer was removed once the picker covered it.)

- **Master switch** — a *Use Effect Layouts* checkbox sits at the top of the popover,
  above the tabs, backed by `ui.effectLayoutsEnabled` (absent = on, so existing installs
  are unaffected). Turned off, every effect renders the standard controls regardless of
  rules or library defaults — `resolveLayoutSelection()` short-circuits to
  `{ layoutId: STANDARD_LAYOUT_ID, source: "disabled" }` and `getCustomLayout()` /
  `hasCustomLayout()` return nothing, which also drops the layout thumbnails from the
  chain nodes and the FX browser — and the popover collapses to just the toggle and an
  explanation. Saved rules are deliberately *not* cleared, so turning it back on restores
  every previous choice. The toggle applies immediately; there is nothing to Apply.
- **Picker** — two radio options, "Standard controls" and "Custom layout". Because an
  effect type can accumulate many layouts, the custom ones sit behind a `<details>`
  dropdown rather than one radio each: the trigger shows the selected layout (thumbnail,
  name, control count, Factory badge) and the expanded list shows all layouts available
  for the node's lookup keys (`effectType::blendId` first, then `effectType`). Picking an
  item writes its id into the custom radio's `value`, which is what Apply reads. The
  popover is appended to `<body>` with fixed positioning because `.default-effect-shell`
  clips its overflow.
- **Rules** — a choice is saved as a preference rule scoped to one of:
  *every use of this effect* (`effectType`), *amps/FX matching a keyword*
  (`keyword`, matched case-insensitively against the node's display name, loaded
  resource/model names and effect name — the picker suggests keywords parsed from
  that same text), or *one preset* (`preset`, pinned to `uiState.activePresetId`).
  Rules are persisted in app settings under `ui.effectLayoutPreferences`.
- **Tabs** — the popover has a **Layout** tab (the options and the "remember this for"
  scope) and a **Rules** tab listing the saved rules for this effect, each with its own
  delete button; there is no bulk clear. The tab bar carries the rule count. Switching
  tabs only toggles `hidden` on the panels, so a pending layout selection survives it,
  and the active tab is held in `openLayoutPicker`'s scope so deleting a rule (which
  re-renders) leaves the user on the Rules tab. *Apply* is hidden outside the Layout
  tab, since it commits that tab's selection.
- **Resolution order** — master switch → preset rule → keyword rule (longest matching
  keyword wins) → effect-type rule → layout library default → standard controls. With no rules
  saved the behaviour is identical to the library default, so existing installs are
  unaffected. `STANDARD_LAYOUT_ID` (`__standard__`) is a valid rule target, which is
  how "always use the standard controls for this amp" is expressed.
- **Design actions** — the footer offers *New layout…*, which opens the Layout Designer
  on a fresh auto-generated layout for that effect type (and blend, where applicable).
  A pencil button sits next to the dropdown trigger (edits the selected layout) and next
  to every item in the expanded list, opening the designer on that specific layout via
  `findLayoutById`; on a Factory layout it opens the editable fork the designer creates,
  since factory layouts are read-only. All of these close the popover first — the
  designer is a modal that would otherwise sit under it — and all are driven by the
  picker's optional `onDesignLayout(layoutId | null)` callback, which `signalPath.ts`
  only supplies while the `EffectLayout` feature flag is on.
- The signal-path node avatar uses the same resolver, so the thumbnail on the chain
  bar always matches what the parameter panel will render.

### Layout Designer name (`core/ui/ts/layoutDesigner.ts`)

The designer toolbar starts with a **Name** field bound to `EffectLayout.name` — the
title shown in the layout picker and the layout library list. It is committed on
blur/Enter as a single undo step, and again on save so an uncommitted edit is not lost.
Leaving it blank falls back to the effect display name (plus blend name for per-blend
layouts), which is also the name pre-filled for a new layout; forking a factory layout
appends " (copy)" so the two are distinguishable in the picker.

### Neural Amp 3D view (`core/ui/ts/amp3d/`) — DISABLED

> This view is switched off. `CHAIN_3D_VIEW_ENABLED` in `core/ui/ts/signalPath.ts` is
> `false`, so the toggle button is never rendered and the dynamic
> `import("./amp3d/index.js")` never runs — effect visualisation is served entirely by
> the standard controls and custom layouts described above. The code and the notes
> below are retained so the experiment can be revived by flipping that flag.

Neural Amp nodes (`kAmpNam`, `kAmpNamOptimized`, `kAmpNamBlend`) can swap the generic knob
grid for a photoreal 3D amp head. A toggle button (`.node-amp3d-toggle-btn`) sits in the
effect shell's meta rail next to the bypass switch; the preference is per user and stored
in app settings under `ui.neuralAmp3dView.enabled`.

- **Models** — glTF 2.0 components in `core/ui/assets/models/` (`amp-head`, `amp-knob`,
  `amp-switch`, `amp-led`, `amp-jack`, `amp-cabinet`), generated by the committed script
  `core/ui/scripts/generate-amp-models.js` (`npm run build:models`). The glTF files carry
  geometry plus named material slots only (`extras.materialSlot`); textures are generated
  procedurally at runtime from canvas so the silkscreen labels follow the node's real
  parameter list and the repo stays small. The one exception is the tolex covering of the
  head and cabinet, which uses the photographed swatch
  `core/ui/assets/models/amp-tolex-black.jpeg` (its normal map is derived from the image at
  load time, and the theme tint is applied as a light multiplier). If the image cannot be
  loaded or read back, the procedural tolex is used instead.
- **three.js** — vendored locally into `dist/vendor/three/` by `scripts/copy-vendor.js` and
  resolved through the import map in `index.template.html`. `signalPath.ts` loads the view
  with `await import("./amp3d/index.js")`, so nothing three-related is fetched until the
  user actually turns the 3D view on. `amp3d/ampSupport.ts` is deliberately three-free so
  the WebGL capability check and the stored preference can be imported statically.
- **Cabinet** — a 4x12 is stacked under the head when `nodeUsesFullRigNamCategory(node)` is
  true (i.e. the loaded capture is a full rig).
- **Layout** — when the 3D view is on, `signalPath.ts` puts `amp3d-immersive` on `<body>` and
  the render claims the whole area between the effect shell header and the window footer (the
  class is what threads the height down through the tab panel, visualization panel and effect
  shell, all of which otherwise size to their content). The model chooser and the leftover
  HTML parameter controls share a single translucent `.amp3d-dock` that floats over the
  bottom of the render, chooser on top, so the floor or the base of the cabinet stays visible
  behind them. The camera reserves part of that band (`measureBottomInset()` in `ampView.ts`,
  re-measured whenever the dock reflows) and adds fixed headroom above the model so the top of
  the head is never clipped.
- **Themes** — `ampTheme.ts` holds a small studio lighting rig (exposure, backdrop, key /
  fill / rim lights, environment intensity, grille backlight, LED colour, floor) per app
  theme: `dark`, `light` and `classic` ("Vintage"). Changing the theme re-renders the
  panel, which rebuilds the scene with the new preset.
- **Interaction** — drag a knob vertically to change its parameter (Shift for fine, wheel to
  step, double-click to restore the default); click the power switch to bypass or enable the
  node, which also kills the power LED, the grille backlight and the model display. Dragging
  anywhere else orbits within a limited arc; the wheel zooms; double-clicking the background
  resets the camera.
- **Amp internals** — when the node is active, `ampValves.ts` fills the thin slab between the
  grille backlight plane and the perforated grille face with a row of slowly breathing valve
  heaters (always warm, whatever the theme), a few small circuit indicators and drifting
  ember/dust particles in the theme's glow colour, plus one short-range point light so the
  cavity actually receives the glow. It is only ever seen through the grille perforations, so
  it reads as "something is alive in there" rather than as an overlay. Bypassing the node
  hides the whole group.
- **Fallbacks** — parameters that cannot be a physical knob (toggles, enums, advanced params
  and any beyond the panel's knob capacity) are still rendered as standard HTML controls in
  the floating dock, so no control is ever lost. If WebGL is unavailable the toggle is not
  offered at all, and if the models fail to load the viewport shows an inline error telling
  the user to switch back. The animated internals are decorative: if they cannot be built the
  amp still renders without them.
- Rendering is on demand (a `requestAnimationFrame` per state change). While the amp is
  active, on screen and the user has not set `prefers-reduced-motion: reduce`, the view keeps
  a self-scheduling loop capped at 30fps to drive the internals; it stops as soon as the node
  is bypassed. Reduced motion holds the deterministic `t = 0` pose. The view instance is
  reused across parameter-panel re-renders rather than being rebuilt, and it is disposed when
  the params panel closes, so nothing animates off screen.

## Jam Panel Notes

### Practice Tool (`core/ui/ts/practiceTool.ts`, `core/ui/ts/practiceTool/`)

A fourth Jam-panel section (alongside backing-track search, Scales, and the Riff
Library) that loads a local audio file — a WAV, AIFF, or MP3 backing track — and
mixes it directly into the native audio engine post-chain, independent of the
guitar signal path, with its own tempo (speed) and pitch controls and a set of
named loop regions for drilling difficult passages.

`practiceTool.ts` is the facade — waveform, loops, transport, faders. Behind it,
`practiceTool/projects.ts` owns all persistence (the per-file loop store and
saved projects) plus the seams the panel can only *request* through,
`practiceTool/projectsPanel.ts` is the project bar's controls,
`practiceTool/eq.ts` / `eqSend.ts` / `eqModal.ts` are the backing-track EQ's
state, its engine sends and the binding that points the shared `EqPanel` at it,
`practiceTool/trackImport.ts` is the drop zone and the reset confirmation the
Browse button shares with it, and
`practiceTool/types.ts` holds the state shapes (re-exported from `ts/types.ts`).
None of those import the facade back. `eq.ts` additionally takes no bridge or
DOM import at all, because `state.ts` seeds the default EQ from it and
eq → bridge → state would otherwise close a cycle.

- **Engine has no loop library.** The engine only ever knows the *currently-active*
  loop's bounds and an on/off flag (`setPracticeToolLoopRegion`/`setPracticeToolLooping`).
  The full named-loop list — add, rename, delete, and the section-name templates — is
  100% client-side state, persisted via `setAppSetting` under
  `practiceTool.loops`, keyed by a fingerprint of the loaded file (`path` +
  `durationSec`, the only stable identifiers `practiceToolFileLoaded` provides) so
  loops reappear when the same file is reopened. No message round-trips through the
  engine for loop CRUD.
- **List, not overlay bands.** Because loops can legitimately overlap (a short lick
  nested inside a longer solo region), the saved-loop list is the source of truth
  rather than always-visible waveform bands. The waveform only ever shows one
  editable start/end handle pair at a time — whichever loop is selected in the list
  (or, with none selected, the in-progress drag-selection for a new loop). The
  handle gesture itself is the shared one in `ts/waveform/rangeSelect.ts`, the same
  controller the riff-take trim editor in `riffLibrary.ts` drives.
- **Selecting = activating.** Clicking a loop row seeks to its start, sends
  `setPracticeToolLoopRegion`, and shows its handles on the waveform for fine-tuning;
  dragging a handle live-updates the loop's bounds locally and re-sends the region
  (debounced) if it is the active loop. Clicking the already-active loop's row
  deactivates it (`setPracticeToolLoopRegion` with bounds omitted).
- **Naming.** A new loop (dragged on the waveform then "+ Add Loop", or "+ New Loop")
  is added to the list immediately — auto-named, auto-selected — and opens for
  name/start/end editing inline in its own row, committed on blur/Tab/Enter. There is
  no separate naming dialog. Common song-section templates (`LOOP_NAME_TEMPLATES` —
  Intro, Verse, Pre-Chorus, Chorus, Bridge, Solo, Outro, Turnaround, Breakdown) are
  offered as `<datalist>` suggestions on every name field, so the same control serves
  both naming and renaming. A template auto-suffixes a number against existing loop
  names on the same track (`suggestLoopTemplateName`: "Verse" → "Verse 1", pick it
  again → "Verse 2"), so a whole song structure can be laid down in a few clicks.
  Deleting is likewise dialog-free: the loop goes immediately and an inline
  "Deleted *X*. [Undo]" banner keeps it reversible for `DELETE_UNDO_WINDOW_MS`.
- **Backing-track EQ.** The panel's "EQ" button (next to play/stop) opens its own
  modal — the panel already carries a waveform, a loop list and four faders, and
  a curve editor does not fit alongside them. Everything inside it is the shared
  `EqPanel` component (see below), so it is the same control surface as the
  Global EQ; all this feature adds is the binding. The DSP is likewise the same
  four-band `ParametricEQEffect` the signal path uses, but a separate instance
  owned by `PracticeToolService`
  and applied in `RenderPostChain()` to the popped backing-track frames only —
  the guitar signal never passes through it. Like gain and balance it is applied
  at mix time rather than baked into the render-ahead ring, so it needs no flush
  and a drag is heard immediately; the setters take `mDSPMutex`, which the audio
  callback already `try_lock`s. UI state (`practiceTool/eq.ts`) is keyed by the
  effect's own parameter names end to end, so nothing translates between the
  curve, the saved project and the `setPracticeToolEq` message. Loading a new
  file resets it to flat and off, exactly as it resets the faders. The panel
  button lights only when the EQ is on *and* has real gain on some band, so a
  switched-on-but-flat EQ reads as the no-op it is.
- **Projects.** The bar on the panel's title row saves the whole practice session
  under a name — the loaded track, its loop list, which loop was active, all four
  fader settings, the EQ curve, and (with the "Preset" box ticked) the preset that
  was selected at save time. Also client-side, stored via `setAppSetting` under
  `practiceTool.projects` (`practiceTool/projects.ts`), so nothing round-trips
  through the engine except the file load and the settings sends a recall replays.
  Saving under an existing name offers to overwrite it rather than accumulating
  duplicates. Recall is the mirror of save: if the project's track is already
  loaded its settings are applied straight away, otherwise `loadPracticeToolFile`
  is sent and the project is parked until `practiceToolFileLoaded` answers for
  that path (`consumePendingProjectRecall`) — which is also what lets a recall
  override the fader reset a fresh file load otherwise performs. A track that was
  *dragged* in has no real path to reopen (WebView2 never exposes one), so those
  projects say so and ask for the file to be opened again first. A project's
  preset is reloaded through `applyPresetFromLibrary`, handed in by `main.ts`
  because the Practice Tool cannot import the preset library directly without
  closing a cycle.
- **Transport.** Play/pause/stop plus four faders — Volume, Balance, Speed (25%–200%)
  and Pitch (±12 semitones) — each sending its own bridge message per change
  (`setPracticeToolGain`/`setPracticeToolBalance`/`setPracticeToolSpeed`/
  `setPracticeToolPitch`). The faders share one normalized slider domain so every
  control's default sits dead center regardless of how asymmetric its real range is;
  each has an adjoining text field for a typed exact value, and double-clicking a
  slider resets it. Speed/pitch sends are debounced during a drag (each one flushes
  the engine's render-ahead ring) and flushed on release. Looping follows loop
  selection rather than a separate toggle. Playback state itself is authoritative
  from the engine (`practiceToolTransportState`), not assumed optimistically in the UI.
- Gated behind `Features.PracticeTool` (`features.practiceTool.enabled`,
  default on), included in `JAM_PANEL_FEATURE_IDS` so the Jam panel experience as a
  whole still shows if only this section is enabled.

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
