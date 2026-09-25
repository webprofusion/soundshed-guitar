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
| `presetLoaded` | `{preset, sceneId, activePresetIds, activePresetDirty?, created?, parameters?}` | The active preset as the engine now holds it: after a load, a scene switch or a scene edit (`selectScene`, `addScene`, `renameScene`, `removeScene` all answer with it), a new preset, a setlist step or a program change. `activePresetDirty` is the engine's unsaved-changes flag (see `presetDirtyChanged`); a footswitch scene switch and a setlist step leave it out, and neither changes it. `created: true` marks the answer to `newPreset`: the UI files the unsaved preset in its library |
| `presetDirtyChanged` | `{dirty}` | The active preset's unsaved-changes flag changed. The engine compares its working copy (the scenes; not the live state of a hosted plugin) with the preset as it was when it became the active one (a load of another preset, any load by id, `newPreset`) or was last saved, re-checking at most every 0.4 s while a UI is ready, so an edit put back clears it. A scene switch is not an edit. The engine owns this flag: the web UI may flag an edit at once, but only this message, or a different preset arriving, clears it |
| `presetFavorites` | `{favorites: [presetId]}` | The favourite presets; the reply to `getPresetFavorites` and `setPresetFavorite` |
| `presetRatings` | `{ratings: {presetId: 1-5}}` | Preset star ratings; the reply to `getPresetRatings` and `setPresetRating` |
| `presetRecents` | `{presetIds}` | The recently played presets, newest first, four at most. The engine records a preset when it becomes the active one (a new id goes first; one already listed keeps its place) and keeps the list in the UI settings as `presetRecents`, where the web UI used to keep it; sent on every change and in reply to `getPresetRecents` |
| `outputMutedChanged` | `{muted}` | The output mute changed (`setOutputMuted`) |
| `appSettingChanged` | `{key, value}` | The engine changed one app setting itself, answering an edit command (`setResourceFavorite` changes `resources.favorites`). `value` is the setting's whole new value; the UI records it without sending it back |
| `setlistCursorChanged` | `{activeSetlistId, cursorIndex, presetId?}` | The active setlist or its cursor moved: a slot step, a bank change, or `selectSetlist` (cursor 0, no `presetId`) |
| `presetSaved` | `{preset, sceneId}` | Preset saved to disk confirmation |
| `presetList` | `{presets: [{id, name, category, source}]}` | Factory/user presets from disk: the reply to `getPresetList`, and to `deletePreset`. These are the presets `loadPreset {presetId}` can load. The UI keeps its folder, tag and search filter, and a new preset it has not saved yet, when the list arrives |
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
| `presetArchivesInstalled` | `{requestId, entryId, presetIds, resourceCount}` | Answer to `installPresetArchives`: the new presets' ids and how many models and IRs it added (not counting ones the library already had) |
| `presetArchivesInstallFailed` | `{requestId, entryId, message, detail}` | `installPresetArchives` wrote nothing: a download that is not a preset archive, or Tone3000 models the library lacks (named in `detail`). The engine's own `error` message carries the same text |
| `installedPresetArchiveDeleted` | `{id, presetIds}` | Answer to `deleteInstalledPresetArchive`: the presets it removed |
| `resourceDeleteFailed` | `{message, detail, resourceType?, id?, presetName?, blendName?}` | A resource delete was refused. `detail` names what still uses it: `Used by preset: …`, or for a model only a blend plays, `Used by blend: …` |
| `resourceUsageInfo` | `{resourceType, id, inUse, presetName, blendName}` | Answer to `queryResourceUsage`. `inUse` counts a model a blend plays, since presets store only a blend node's `blendId` |
| `blendExportSaved` | `{path}` | A blend `.namz` archive was written |
| `blendExportFailed` | `{message}` | A blend archive export failed or was cancelled |
| `globalChain` | `{config}` | Global signal chain configuration |
| `effectCatalog` | `{catalog: [{type, name, category, requiresResource, parameters, ...}]}` | Available effect types. Each parameter is `{key, name, min, max, default, unit, group?, advanced?, step?, labels?, taper?}`; the optional fields are left out at their defaults. `taper: "log"` means knobs, custom-layout sliders and automation move by ratio across `min..max` (see fx-library.md, Parameter tapers); absent means linear |
| `effectResponse` | `{requestId, effectType, supported, frequencies?, magnitudesDb?}` | Reply to `getEffectResponse`: the effect's small-signal magnitude response in dB (to 0.01 dB) at log-spaced frequencies from 20 Hz to 20 kHz, both ends included. `supported` is false, and the arrays absent, for an effect with no fixed response (anything that varies in time or with level). |
| `effectPresets` | `{byEffectType: {[effectType]: [{id, name, parameters, resources?, config?}]}}` | Every user effect preset, keyed by canonical effect type. Sent on `getEffectPresets` and after each save or delete. A hosted plugin's state is scrubbed from `config` (`pluginStateBase64Length` stands in for it), as it is from graphs. Entries saved before resources and config were captured carry `parameters` only |
| `simpleCabIrMatch` | `{requestId, resourceId?, params?, rmsErrorDb?, error?}` | Reply to `matchSimpleCabToIr`: the Simple Cabinet settings whose response shape is closest to the IR's, and the shape difference left over. Output, Auto Level and Speaker Drive are not in `params`, so applying it keeps the node's own. `error` instead when the library entry's file is missing or cannot be read. |
| `dspPerformance` | `{stats: {totalProcessingTimeUs, realTimeUs, dspLoadPercent, totalLatencySamples, nodeProcessingTimesUs, nodeLatencySamples}, sampleRate, blockSize}` | DSP timing at 5 Hz, for the performance panel and the per-node readouts. Both node maps are keyed `<scope>::<nodeId>` — `pre::`, `post::`, or the preset id — since a bare node id only identifies a node within one executor. A node missing from `nodeProcessingTimesUs` did not run last block; the UI blanks it rather than showing a zero. |
| `sldRoster` | `{seq, nodes: [[scope, presetId, nodeId, nodeType, hasAnalyzer]], spectrogramRange, barkRange}` | Signal diagnostics roster: everything about the node set that does not change frame to frame. Sent only when the node set changes, and after `getSignalDiagnostics`. |
| `sld` | `{seq, r, i, o, d}` | Signal level frame at 20 Hz. `r`/`i`/`o` are raw input, processed input and output; `d` holds one tuple per roster node, flattened in roster order. Every tuple is `[peakDbfs, rmsDbfs, clipCount, clipped, channelCount]`, the levels rounded to 0.1 dB; `headroomDb` is derived UI-side. `channelCount` (0 = the node did not run this block, 1 mono, 2 stereo) rides here rather than in the roster: it tracks the signal, so keeping it in the roster re-sent that whole message several times a second. Frames whose `seq` does not match the held roster are dropped. |
| `sldA` | `{seq, id, t, l, s, b}` | Analyzer telemetry for one node — levels `l`, spectrogram bins `s` and bark bands `b` in whole dBFS. Sent separately from `sld` because it is an order of magnitude larger than a level tuple. |
| `sldS` | `{scope, presetId?, id, r, s}` | Spectrum of one node's input, for the EQ curve backdrop, ~30 Hz while a `setSpectrumWatch` is held and the UI is visible. `s` is 128 whole-dB bins log-spaced across `r = [minHz, maxHz, floorDb, ceilingDb]` (20 Hz-20 kHz, -96-0 dB), both ends included, tilted +3 dB/octave about 1 kHz so pink noise reads flat. Nothing is sent for a node that is not there; the UI clears a spectrum that stops arriving. |
| `spatialPosition` | `{nodes: [{scope, presetId?, nodeId, azimuth, elevation, distance, itdUs, ildDb, rateHz, moving}]}` | Live source position for every 3D Spatial node, ~20 Hz. Purely cosmetic: it keeps the spatial panner's puck in sync with what is being heard, and the widget falls back to the anchor position if it never arrives. Only sent while at least one such node exists. |
| `riffLibraryState` | `{library: {path, riffs}, capture}` | Reply to `getRiffLibrary`: the riff library as it is on disk. The UI applies `library`; capture state reaches it through the live capture messages |
| `toneSharingPackDeleteFailed` | `{message}` | An installed pack's archive could not be deleted. The pack is already gone from the installed list; this only says its files were left behind |
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
| `audioDeviceState` | `{available, error, state}` | Standalone: the whole audio/MIDI device setup, sent after every `audioDevice` request and every device manager change. `error` is the failed request's reason, else `""`. `available: false` (and no `state`) means a plugin format, where the host owns the devices. See [Audio Device](#audio-device) |
| `audioDeviceLevels` | `{input, xruns}` | Standalone: raw device input level in dB (floor −100) and the driver's dropout count (−1 if it cannot count), at 15 Hz while a `watchLevels` lease is held |

### UI → Engine Messages

| Type | Payload | Description |
|------|---------|-------------|
| `uiReady` | `{}` | WebView loaded and ready |
| `requestState` | `{}` | Request full state sync |
| `setParameter` | `{name, value}` | Set one global FX value by flat name; alias for `setGlobalChainParam` |
| `loadPreset` | `{presetId, sceneId?}` or `{preset, presetId?, sceneId?}` | Load a preset, on `sceneId` when it has a scene of that id (else its first). By id alone the engine reads the stored preset (user store, factory file or factory archive; an unknown id is refused with an `error`) and a load discards unsaved edits, even of the preset already playing. With a body, the body is loaded: for a preset the engine does not store (shared, tone-sharing, a new unsaved one), and for chain undo putting back an earlier graph of the preset being edited, which keeps its unsaved state. Answered by `presetLoaded` |
| `newPreset` | `{}` | Make a new, unsaved preset from the default template (input, bypassed gate and Neural FX, Neural Amp, bypassed IR cab, output), with a new `user-` id, named "New Preset" in category User, and load it; answered by `presetLoaded` with `created: true` |
| `selectScene` | `{sceneId}` | Switch the active preset's scene, as a footswitch does (the outgoing scene's hosted-plugin state is banked first); answered by `presetLoaded`, an unknown scene by an `error`. When the preset shares a multi-preset mix, only its own slot is rebuilt and the other presets keep playing |
| `addScene` | `{fromSceneId?, title?}` | Add a scene to the active preset: a copy of `fromSceneId` (the playing scene when omitted), id `scene-N` with N counting up from the scene count plus one until the id is free, titled `title` or "Scene <count+1>". It becomes the active scene; answered by `presetLoaded` |
| `renameScene` | `{sceneId, title}` | Rename a scene; the title is trimmed, and an empty one becomes "Scene". Answered by `presetLoaded` |
| `removeScene` | `{sceneId}` | Remove a scene. The last one is refused with an `error`. If it was playing, the scene now in its place (else the new last one) becomes active and is applied, rebuilding only the preset's own slot when it shares a multi-preset mix, as `selectScene` does. Answered by `presetLoaded` |
| `setPresetFavorite` | `{presetId, favorite}` | Mark or unmark one preset as a favourite, leaving the rest of the list alone; answered by `presetFavorites` |
| `setPresetRating` | `{presetId, rating}` | Rate one preset 1-5 (clamped); `0` clears its rating. Answered by `presetRatings` |
| `getPresetRecents` | `{}` | Request `presetRecents` |
| `selectSetlist` | `{setlistId}` | Make a setlist the active one with its cursor on the first slot, as a bank select does, and store it; nothing is loaded. An unknown or already-active setlist is ignored. Answered by `setlistCursorChanged`. Edits to the setlists themselves still go whole with `setSetlists` |
| `setResourceFavorite` | `{resourceId, favorite}` | Add or remove one id in the `resources.favorites` app setting; answered by `appSettingChanged` |
| `setOutputMuted` | `{muted}` | Mute or unmute the output. The mixer applies it after the output gain, so an output level change or a chain rebuild leaves it alone. Not saved: it is reported as `outputMuted` in `state`, and answered by `outputMutedChanged` |
| `uiSettingsChanged` | `{settings}` or `{patch}` | The UI settings blob (zoom, window bounds, signal-path height and wrap, `presetRecents`), saved with the app settings. `settings` replaces the blob, keeping the engine's `native` key (Soundshed Guitar Nano's part) when the new blob has none; the web UI only ever sends back the `native` it was given. `patch` is a JSON merge patch (a `null` removes a key), which is how Nano writes its own part. Because the engine keeps `presetRecents` there itself, a UI sending the whole blob must carry the list the engine last reported |
| `uiViewStateChanged` | `{viewState}` or `{patch}` | Which panels and tabs are showing, saved with the host state. Replaced or merge-patched as `uiSettingsChanged` is, with the same `native` rule |
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
| `setMixGain` | `{gainDb}` | The Multi-Rig's own level in dB, applied to the summed preset mix ahead of the global post-chain and output stage. Independent of `output.gain`; reported back as `mixer.mixGainDb`, saved with a Multi-Rig, and reset to 0 dB when a single preset is loaded |
| `saveCompositePreset` | `{name, description?, tags?, id?}` | Save the current mixer (slots, levels, mix gain) as a Multi-Rig preset; with `id`, update that one in place |
| `loadCompositePreset` | `{id}` | Replace the mixer with a saved Multi-Rig's slots, levels and mix gain. The instance's output gain is left alone, as with any preset load |
| `getCompositePresetList` | `{}` | Request `compositePresetList` |
| `removeCompositePreset` | `{id}` | Delete a saved Multi-Rig preset |
| `setInputMode` | `{mode}` | Set input mode (mono/stereo) |
| `setAmpCabState` | `{...}` | Set amp/cab enable state |
| `setMetronome` | `{bpm?, enabled?, volumeDb?, pan?, clickType?, clickConfig?, beatPattern?, timeSigNum?, timeSigDen?, grouping?, subdivision?}` | Update metronome settings. The engine normalises before storing: a grouping that does not add up to the bar is dropped, and a meter change re-seeds `beatPattern` unless the same message carries one. |
| `tuner` | `{action}` | Start/stop/configure tuner |
| `runSignalPathTest` | `{}` | Run signal path diagnostic |
| `previewDemoAudio` | `{clipId, repeat?}` or `{audio, region?}` | Play audio into the input as a demo. By `clipId` the engine reads the clip from `ui/demo/clips.json` itself (the ids the full `state` lists as `demoClips`; an unknown one is refused with an `error`), and with `repeat` it loops the whole clip in place until stopped, so no `previewComplete` comes between passes. `audio` is the older form: the file's bytes as base64 `{id, title, data, contentType}` |
| `renderDemoAudio` | `{clipId? , takeId?, audio?, title?, suggestedName?, renderSampleRate?}` | Render a demo clip (`clipId`, read by the engine), a riff take (`takeId`) or sent audio (`audio`, the older form) to a WAV file using the current preset. `renderSampleRate` accepts `44100`, `48000`, `88200`, `96000`, `176400`, or `192000`; omit or pass `0` for the current device rate. The save-dialog filename appends the resolved rounded kHz rate before `.wav`. |
| `stopDemoAudio` | `{}` | Stop demo audio playback |
| `setSpectrumWatch` | `{scope, nodeId, presetId?}` or `{}` | Start, move or renew the spectrum tap on one node's input (`scope` is `pre`, `post` or `preset`; a preset node without `presetId` is looked up in the active preset). The watch lapses 5 s after the last renewal, so the UI re-sends it every 2 s while an EQ curve is on screen; `{}` stops it. See `core/ui/ts/eqSpectrum.ts`. |
| `getEffectResponse` | `{requestId, effectType, params, points?}` | Ask for an effect's response curve with these parameters (`points` 2-1024, default 160); answered by `effectResponse`. Built from a fresh instance at 48 kHz rather than the running node, so it needs no DSP lock and works for a node in no running graph. See `core/ui/ts/effectResponse.ts`. |
| `exportEffectAsIr` | `{requestId, effectType, params, name?}` | Render the effect with these parameters as a 48 kHz 16-bit IR (mono unless its two sides differ) into the IR library. Answered by `resourceImported` or `resourceImportFailed` carrying the `requestId`. A name the library already has gets a number added, so an earlier export is never overwritten. |
| `matchSimpleCabToIr` | `{requestId, resourceId}` | Find the Simple Cabinet settings closest in shape to library IR `resourceId`; answered by `simpleCabIrMatch`. |
| `getEffectPresets` | `{}` | Request `effectPresets` |
| `saveEffectPreset` | `{effectType, name, nodeId?, parameters?}` | Save a user preset for an effect type (a name already saved is overwritten, keeping its id). With `nodeId`, the engine snapshots its own copy of that node in the chain being edited: parameters (not the injected NAM calibration), every resource slot, and config bar transient keys, with a hosted plugin's live state read from the running plugin. Without it, or when the node is not found, `parameters` is saved alone. Answered by `effectPresets` |
| `applyEffectPreset` | `{nodeId, effectType, presetId}` | Load user preset `presetId` of `effectType` into a node of that type. Parameters are applied in the order the effect declares them, leaving out keys it no longer declares. When the entry carries resources or config that differ from the node's, they replace them and the chain is rebuilt, as choosing a model does; otherwise the parameters are set on the running chain with no rebuild. The node comes back in `state` |
| `deleteEffectPreset` | `{effectType, presetId}` | Delete a user effect preset; answered by `effectPresets` |
| `previewCapturedRiff` | `{startRatio, endRatio, repeat}` | Play the captured riff take. The whole take goes over once; the markers travel as a region and the engine loops it in place, so repeating costs no further messages. |
| `setRiffPreviewRegion` | `{startRatio, endRatio, repeat}` | Retune the region/repeat of the preview already playing — for a marker dragged mid-playback. Debounced by the UI, since each one rebuilds the wrap crossfade behind the DSP lock. |
| `importRemoteResource` | `{requestId?, provider, resourceType, resourceId, name?, description?, category?, subfolder?, fileName?, metadata?, tags?, data}` | Import a downloaded resource (base64 `data`) into the library under `resources/content/<provider>/<subfolder>`; answered by `resourceImported` or `resourceImportFailed` carrying the `requestId`, and other instances are told through shared sync. The engine hashes the bytes itself; a `hash` in the payload is ignored. A file already at the name is replaced only when it is this resource's own; another resource's file is left alone and this one is written beside it with a hash suffix |
| `installPresetArchives` | `{requestId?, entry: {id, title?, source?, packId?}, folder?, archives: [{title?, data}]}` | Install tones downloaded from tone sharing: each `data` is a base64 zip in the web UI's archive format (`preset.json` or `presets.json` plus `resources/<file>`). Models and IRs go into the library (one it already holds, by content hash, is reused), blends get new ids, and the presets are saved under new user ids without replacing the running preset; a one-preset archive's preset takes the archive's `title`. With `folder`, the presets also get a top-level folder of that name (`tone-sharing::<packId>`). The install is recorded under `entry.id` in the `toneSharing.installedPacks` app setting, in the web UI's schema plus the `blends` it added, and announced with `appSettingChanged`, `presetList` and, with a folder, `presetFolders`. Every archive is read and every Tone3000 reference resolved against the library before anything is written. Answered by `presetArchivesInstalled` or `presetArchivesInstallFailed` |
| `deleteInstalledPresetArchive` | `{id}` | Take an install out again: its presets (and them out of folders, favourites and ratings, dropping a folder they leave empty), the blends it added that no preset uses, and the models and IRs it added that no remaining preset or blend uses, then its `toneSharing.installedPacks` entry. Answered by `installedPresetArchiveDeleted`, with the new `presetList`, `presetFolders`, `presetFavorites`, `presetRatings` and `appSettingChanged` |
| `previewRemoteResource` | `{resourceType, tempResourceId, nodeId, resourceIndex, isZip, data}` | Play downloaded bytes on a node slot from a temp file without importing them. With `isZip`, the first `.nam`/`.json` (NAM) or `.wav`/`.ir` (IR) entry in the zip's order is used, as the UI's zip import does. A preview on the slot already previewing replaces it and keeps the slot's original for `cancelPreviewResource` to restore; one on another slot restores that slot first |
| `cancelPreviewResource` | `{nodeId, resourceIndex, restoreOriginal?}` | End the preview and delete its temp file. `restoreOriginal` (default true) puts the slot's original resource back; `false` is for a caller that sets the slot itself straight after, as selecting a resource does |
| `updateSignalPathNodeConfig` | `{nodeId, key, value, persist?, capture?, presetId?}` | Set one node config value on the running node and, with `persist` (default), in the working preset. A blend node's per-node mode is `blendModeOverride` (`snap`, `interpolate`, or `""` to follow its blend) |
| `saveBlendDefinition` | `{blend}` | Add or replace a blend definition (see [data-models.md](data-models.md#blenddefinition)). Every running mixer slot with a node playing it is rebuilt, so the edit is heard. A saved edit of a factory blend is stored as the user's own copy |
| `deleteBlendDefinition` | `{blendId}` | Delete a blend. Refused with an `error` for a factory blend, a blend from an open preset archive, or one a preset plays (working copy, mixer slots, user or factory-archive presets). Deleting the user's copy of a factory blend brings the factory one back |
| `saveBlendArchive` | `{data, fileName}` | Save a base64 `.namz` blend archive through a save dialog; answered with `blendExportSaved` or `blendExportFailed` |
| `setSetting` | `{key, value}` | Persist and apply an app setting |
| `setUserInputCalibrationTrainingActive` | `{active}` | Temporarily bypass the active calibration profile while training |
| `setGlobalChainParam` | `{param, value}` | Set global chain parameter |
| `getGlobalChain` | `{}` | Request global chain state |
| `getEffectCatalog` | `{}` | Request effect catalog |
| `getPresetList` | `{}` | Request preset list from disk |
| `audioDevice` | `{action, ...}` | Standalone: one audio/MIDI device request, answered with `audioDeviceState`. Actions: `getState {rescan?}`, `setDeviceType {deviceType}`, `setDevice {kind: "input"\|"output"\|"linked", name}` (`""` is no device), `setInputChannels {group}` / `setOutputChannels {group}` (group index, `-1` for none), `setSampleRate {sampleRate}`, `setBufferSize {bufferSize}`, `setInputMuted {muted}`, `playTestTone`, `showControlPanel`, `resetDevice`, `requestInputPermission`, `setMidiInputEnabled {identifier, enabled}`, `setMidiOutput {identifier}` (`""` is none), `watchLevels {enabled}` (a lease that lapses 5 s after the last renewal, so the UI renews it every 2 s while the controls are on screen) |
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

The engine also answers a few requests the UI itself never sends:

| Type | Payload | Description |
|------|---------|-------------|
| `getPerformanceStats` | `{}` | Publish a `dspPerformance` frame now rather than on the next tick. A pull for tests and scripted debugging; the UI takes the pushed feed |
| `getSignalDiagnostics` | `{}` | Publish the next signal-level frame with its `sldRoster`, for a client that has no roster yet. Tests and scripted debugging only |
| `splitSignalPathEdge` | `{edge: {from, to, fromPort, toPort}}` | Insert a splitter/mixer pair on one edge of the edited graph, making two parallel lanes. The engine supports it; the UI has no gesture for it yet, though it can show and collapse (`collapseSignalPathSplit`) a split a preset already has |
| `setMasterGain` | `{gain}` | Set the mixer's linear master multiplier directly. Kept for older UIs and scripted use; the UI mutes with `setOutputMuted`. Level changes go through `setGlobalChainParam` with path `output.gain`, since the engine derives the multiplier from that setting and re-derives it on every chain rebuild, which also undoes a mute made this way |

Retired on 19 September 2026, and now ignored: `setAutoLevel` (mixer-wide auto-level), `openAudioPreferences` (JUCE's audio dialog; use `audioDevice`), `setLimiterEnabled` (use the `audio.dsp.outputLimiterEnabled` app setting), `setGlobalChain` (use `setGlobalChainParam`), `setNodeEnabled`/`setNodeParam` (use `updateSignalPathNodeBypass`/`updateSignalPathNodeParam`), `setTunerEnabled`/`setTunerReference` (use `tuner`), `removePreset` (use `removeActivePreset`), `removeLocalLibraryResource` (use `deleteLibraryResource`, which also checks the resource is unused) and `importToneSharingPack` (packs are imported in the UI).

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

Fields for what the engine owns rather than the UI:

| Field | Scope | Meaning |
|-------|-------|---------|
| `activePresetDirty` | Both, with `preset` | The active preset's unsaved-changes flag, as `presetDirtyChanged` reports it |
| `outputMuted` | Both | The output mute (`setOutputMuted`) |
| `demoClips` | Full | `[{id, title}]`, the demo clips from `ui/demo/clips.json`, for `previewDemoAudio {clipId}` and `renderDemoAudio {clipId}` |
| `uiSettings.presetRecents` | Full | The recently played presets the engine records (`presetRecents`) |

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
  `uiVisibility`, which also switches the DSP's signal diagnostics off. The page cannot
  report its own teardown, so the JUCE editor (`SoundshedEditorBase`, for Nano too) sends `uiVisibility {visible:false}` to the
  controller itself when it is destroyed, and `{visible:true}` when a new one opens.

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

### Edits the engine makes

Some edits used to be made by the web UI to its own copy of a preset or document, which it then
sent back whole. So that Soundshed Guitar and Soundshed Guitar Nano cannot make the same edit two
different ways, the engine now makes them, asked by name (docs/plans/native-ui.md, "Engine-owned
edits"): the scene commands, `newPreset`, `loadPreset` by id, `setPresetFavorite`,
`setPresetRating`, `selectSetlist`, `setResourceFavorite` and `setOutputMuted`. The engine also
records the recently played presets and tracks unsaved changes itself. The web UI may still draw
the result before the answer arrives (a scene tab, a favourite star), but the engine's answer is
what stands. The older whole-document forms (`setPresetFavorites`, `setPresetRatings`,
`setSetlists`, a `loadPreset` body) still work, and the web UI uses them for real bulk edits and for
presets the engine does not store.

## UI Views

| View | Purpose |
|------|---------|
| **Main** | Amp panel, global controls, level meters |
| **Preset Browser** | Local preset management, search, load/save |
| **Community Browser** | Remote preset search and download |
| **Signal Chain Editor** | Visual node-based effect chain |
| **Resource Browser** | NAM model and IR selection |
| **Settings** | Audio preferences, storage, theme |

## Settings → Audio & MIDI

The Settings panel's own tab strip (`core/ui/ts/settings/tabs.ts`, markup in
`ui-components/panels/settings-panel.html`): General, **Audio & MIDI**, DSP Performance,
Features, Library, Help. Audio & MIDI holds the two device sections below and nothing else;
the engine-side settings that used to sit beside them — Preset Switching, Advanced DSP Level
Targets — stayed in General under its *DSP* heading. Each tab is one partial under
`ui-components/panels/`, and its id is `equipment-tab-<tabId>`; a new one needs the button,
the partial's include, and the tab id in `resolveEquipmentTabId()`.

### Audio Device

The standalone app's audio and MIDI device controls (`core/ui/ts/settings/audioDevice.ts`,
engine side `juce/source/StandaloneAudioSettings.cpp`). They replace JUCE's Audio/MIDI
Settings dialog: that is a window JUCE draws itself, which on Android cannot share the screen
with the WebView, and which elsewhere looks nothing like the app. The approach follows the
tone3000 plugin's `StandaloneAudioSettings`: the engine owns every device decision and sends
the whole setup back as one snapshot, and the UI keeps no device state of its own.

The section shows only when `environment.audioDeviceSettings` is true (the standalone app).
In a DAW the host owns the devices.

**Same semantics as JUCE's dialog**, so a setup made in one reads back unchanged in the other:
driver type; input and output device, or one "Device" for drivers such as ASIO that open both
sides together; the active channels, picked in groups as wide as the plugin's main bus (stereo
pairs), at most one group per side; sample rate and buffer size from the device's own lists
(the shown value is what the device runs at); the driver's control panel, a device reset and
the test tone; MIDI inputs and the MIDI output; and the holder's feedback-loop input mute.
The MIDI inputs are one full-width row per port (names are long), so the list has a
*MIDI Inputs* heading of its own above it; with no port it still shows, over the word
**None**, so the section never looks as if inputs were left out.
A list the device offers only one entry for is shown disabled, with a line saying who sets it
(an ASIO driver's buffer size, Windows shared mode's sample rate).

**What it does that the dialog did not:**
- *Input mute, remembered per device pair.* JUCE keeps the mute only on desktop and starts a
  phone muted on every launch. Here a pair seen for the first time is muted only if it looks
  like a built-in microphone playing into speakers (device names containing "microphone" and
  "speaker", on every platform); a mute set by hand sticks for that pair. On the first run
  with this in place, a desktop keeps the mute its JUCE dialog last saved.
- *Saves straight away.* JUCE writes the device setup on a clean exit, which a phone rarely
  gives an app; every change here is written to the settings file at once.
- *Android record permission.* If it was refused, an alert offers to ask again, and the input
  side is reopened once it is granted — including when it is granted from the system settings
  while the app keeps running.
- *Latency and dropouts.* The driver's reported input and output latency, and its dropout
  count while the section is on screen.

Errors from a request appear in an alert at the top of the section, with a Reset Device
button, and stay until the next request.

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
- **The live spectrum is the component's too.** Given a `spectrumSource`, the
  panel draws the source node's *input* behind the curve while the curve is on
  screen (an `IntersectionObserver` on the canvas), so the user sees where the
  energy is before the EQ touches it. The Global EQ passes its post-chain node;
  the Practice Tool passes none, having no graph node to tap.

### EQ spectrum backdrop (`core/ui/ts/eqSpectrum.ts`, `core/ui/ts/eqPlot.ts`)

Every EQ curve — the Global EQ, and a Parametric or Graphic EQ in the chain —
shows a live spectrum of what is arriving at that EQ, on the same log frequency
axis as its handles (`eqCurveFreqToX` is the one mapping for both).

- **One watch for the whole UI.** The engine taps one node at a time.
  `subscribeEqSpectrum` keeps a stack: the newest subscriber is the one served,
  a displaced one is told it has nothing, and it gets the feed back when the
  newer one goes, which is what the Global EQ modal over a node's panel needs.
- **Only while visible, and never stuck on.** `EqSpectrumWatcher` subscribes
  while its canvas is on screen. The watch is a 5 s lease renewed every 2 s, a
  dropped subscription is released after 250 ms (so a panel rebuild does not
  restart the tap), and a spectrum that stops arriving is cleared after 500 ms.
- **Where it comes from.** `SpectrumTap` (`core/src/dsp/SpectrumTap.h`) copies
  the node's input into a ring on the audio thread and nothing more; the FFT
  (~6 Hz bins, whatever the host block size), log binning and attack/release
  smoothing run on the message thread in `TelemetryPublisher`. It is separate
  from the Signal Analyzer's spectrogram, which resolves no finer than the block
  size. A node inside a composite being edited has no spectrum.


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

## Effect presentation (`core/ui/data/effect-presentation.json`)

How each effect and category looks is one data file, shared with Soundshed Guitar Nano so the
two UIs cannot drift: the FX library's categories (order, name, colour), the icon for each
category and effect, the chain node's colour class, the gear categories shown as amps, the FX
library category a blend is listed under, and the effect view's background gradient and stock
artwork. Effects are keyed by their `EffectGuids` constant, with the guid alongside.

- **Web UI:** `node tools/gen-effect-presentation.mjs` writes it as typed constants to
  `core/ui/ts/generated/effectPresentation.ts` (never edit that by hand). `fxSelector.ts`
  (`CATEGORY_METADATA`, the blend categories), `iconAssets.ts`, `signalPath/nodeTypes.ts` (node
  classes, `getNodeCategory`) and `signalPath/visualization.ts` build their tables from it. The
  file's images are relative to `core/ui`; the web UI prefixes them with `../`, and turns each
  `visualBackground` pair into `linear-gradient(145deg, <first> 0%, <second> 100%)`.
- **Nano:** `core/src/uiclient/EffectPresentation.cpp` reads the file at run time.
- **Checks:** `npm run check:presentation` (part of `npm run verify`, and in CI) runs
  `tools/check-effect-presentation.mjs`. It fails when an effect's name or guid is not in
  `core/src/dsp/EffectGuids.h`, a category the file refers to is not defined, a colour is not
  `#rgb`, `#rrggbb`, `rgb()` or `rgba()`, an icon or image file is missing, or the generated
  module is out of date. An icon the file names must also be an `IconKey` in `iconAssets.ts`,
  or the typecheck fails.

To change an effect's look, edit the JSON, then run the generator.

## Signal Chain Editor Notes

- To create parallel paths, add the **Splitter** effect from the Utility category. The join **Mixer** node is inserted automatically and is not user-addable.

### Wrapping the chain (`core/ui/ts/signalPath/chainRow.ts`, `css/signal-path/wrap.css`)

The toggle in the chain bar's top-right corner (`#signal-path-wrap-btn`) wraps the chain onto
more lines instead of scrolling it sideways. The choice is `uiSettings.signalPathWrap`, so it
is the web UI's own; Soundshed Guitar Nano's chain page has `nativeUi.chainWrap`.

- **Segments.** The main row is built from `.signal-chain-segment`s: the input, then each
  connector with what it leads into. A splitter's segment also carries its parallel block and
  join mixer, because the branch routes reach under both. Unwrapped, a segment is
  `display: contents`, so the row lays out as the flat list it always was; wrapped, the row
  breaks only between segments. A line never ends on a connector, and each later line starts
  with the connector (and "+") leading into its first node.
- **Return routes.** After layout, `updateSignalChainWrapRoutes` groups the segments into the
  lines the browser made and draws an SVG path from each line's end, round under it, to the
  next line's start. It also narrows the row to its widest line, which keeps every line break,
  so the container centres the block. A `ResizeObserver` on the rendered chain redraws the
  routes when the bar's width or a segment's size changes; it is re-armed only for a newly
  rendered chain, because observing always reports once.
- **Height.** The bar grows to show every line, up to half the window, and scrolls beyond
  that. On the compact chain stage the chain has the whole stage and scrolls within it.
- **Dragging between lines.** Moving a node to another line is a vertical drag, which is also
  the bypass flick. A drag released on a different line with no drop target does nothing
  (`releasedOnAnotherLine` in `signalPathDropTargets.ts`); a flick that ends on its own line,
  between lines or off the chain still toggles bypass.
- **What still scrolls sideways.** A single segment wider than the bar, in practice a
  parallel block on a phone, cannot be split.

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
