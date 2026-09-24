# Soundshed Guitar Nano: a native, WebView-free UI for small displays

Status: **proposed, 2026-09-23. Nothing built.** Packaging, scope, Android, platforms and
the drift policy were decided the same day (see [Decisions](#decisions)). Only the scope
switches are still open.

**Soundshed Guitar Nano** is a second product. It ships its own plugin and standalone builds
and its own installers, and its front end is written in JUCE Components with no WebView. It
is optimised for small displays and carries only what playing needs. It runs on the same
engine, profile and data as Soundshed Guitar, so a user can move between the two freely.

## Goal

1. **A distinct product.** Soundshed Guitar Nano has its own name, plugin ids, bundle id,
   installer and standalone app. It installs alongside Soundshed Guitar and does not replace
   it.
2. **No WebView.** It links and starts no WebView2, WKWebView or Android WebView. That
   covers devices and hosts where a WebView is missing, heavy or unreliable: phones, small
   Linux boards with 800×480 touch screens, and low-power laptops.
3. **Small displays.** Designed for 800×480, and still usable at 640×360 in landscape and
   360×640 in portrait. Touch is a first-class input.
4. **Fully compatible.** Same profile folder, `soundshed.db`, app settings, presets, scenes,
   setlists, favourites, resource library and Tone3000 imports. Anything saved in one
   product is found and used unchanged by the other. On Android there is only one app:
   Nano replaces the WebView app and keeps its data (see
   [C1](#what-fully-compatible-means)).
5. **The core of playing.** The signal chain editor, the selected effect's controls and
   visualisation, local and Tone3000 resource browsing, tuner, metronome and demo audio. It
   leaves out the JAM tab, the TONES tab, riff capture and the authoring tools.

## What "fully compatible" means

These are the rules the implementation must keep. The compatibility tests in
[Testing](#testing-and-tooling) check them.

| # | Rule | Why it needs stating |
|---|---|---|
| C1 | **One profile.** Both products resolve the same settings folder (`"Soundshed Guitar"`, hard-coded at `juce/source/PluginProcessorAdapter.cpp:940`; keep it that way in Soundshed Guitar Nano), the same `soundshed.db`, the same `resources/content/` tree and the same riff library path. This holds on Windows, macOS and Linux, where both products install side by side. **On Android there is only one app.** The profile lives in each app's private storage (`PluginProcessorAdapter.cpp:239-248`), so two apps could never share it. Nano therefore takes over the WebView app's `applicationId`, and an upgrade keeps the existing profile. On macOS, sharing relies on neither product being sandboxed, which rules out a Mac App Store build of either. | A second product name must not create a second profile. |
| C2 | **Same app-setting keys.** For anything Soundshed Guitar already stores, Soundshed Guitar Nano reads and writes the same key through `setSetting`: `theme`, `resources.favorites`, `tone3000.*`, `demoAudio.*`, `audio.*`, `features.*` and so on. Settings only the native UI has go under `nativeUi.*`. | Otherwise, for example, a favourite model set in one product would not show in the other. |
| C3 | **The engine writes persisted documents.** Presets, scenes and favourites are changed through engine commands (see [Engine work, part 2](#2-engine-owned-edits)), not composed by each UI. Wherever the native UI has to send a whole document (setlists, preset folders), it round-trips the JSON it received and keeps fields it does not understand. | Today the TypeScript UI builds preset documents itself: scenes, drafts, new presets and the save payload. A second implementation of that would drift. |
| C4 | **UI blobs are merged, not replaced.** `uiSettings` and `uiViewState` are single blobs, and `uiSettingsChanged` replaces the whole thing (`core/src/dispatcher/MessageDispatchSettings.cpp:72-93`). The native UI keeps its own view state under a `native` sub-key and changes only that key and the shared ones (`presetRecents`), using a merge patch. | In the standalones these blobs are app-wide, so each product would otherwise wipe the other's zoom, bounds and chain height. |
| C5 | **Shared sync is honoured.** The native UI reacts to `sharedSyncUpdated` the same way the web UI does: it reloads the preset list, setlists, favourites and resource library. | Another plugin instance, or the other product, may change them. |
| C6 | **Tone3000 imports are identical.** Same `resourceId` (`tone3000:<modelId>[:<entry>]`), name, subfolder and metadata shape as `core/ui/ts/tone3000Shared.ts:205-249`. | So a model imported in either product shows as "imported" in the other, and is not downloaded twice. |
| C7 | **Feature flags are respected.** `features.tone3000.enabled` and the others gate the same features. | A user who turned something off expects it off everywhere. |
| C8 | **The DAW state blob format is unchanged.** The native editor adds only its own merged view state (C4). DAW projects do **not** move between the products, because the plugin ids differ; a rig moves between them as a preset. | Worth telling users up front. |

## Scope

### In scope

"Engine today" says whether the engine already supports the feature over the message
protocol, or whether the logic currently lives in the TypeScript UI.

| Area | Soundshed Guitar Nano includes | Engine today | Work needed |
|---|---|---|---|
| **Preset navigation** | Previous/next, name, favourite star, unsaved dot, stepping through a setlist | Supported, but a load is two round trips (`getPresetById`, then `loadPreset` carrying the whole preset; `presets/load.ts:134-188`) | Load by id (engine work, part 2). Port the list-order and filter logic |
| **Preset browser** | Folders, All / Favourites / Recents, search, tag filter; load, save, save as, rename, new, delete | Folders, favourites and ratings are engine documents. Recents are in `uiSettings`. New presets are built in TypeScript (`presets/saveModal.ts:131`) | `newPreset` and recents move to the engine (part 2). Port the filter (`presets/filter.ts`) |
| **Scenes** | Scene strip: select, add, rename, remove | The engine can select by index (`PluginControllerAutomation.cpp:493`). The UI switches by re-sending the whole preset (`signalPath/mixer.ts:148`) | Scene commands in the engine (part 2) |
| **Setlists** | Choose a setlist, step through it, pads view for live use | `setSetlistCursor`; the engine loads the slot itself | Setlist editing is deferred to phase 6 |
| **Signal chain editor** | Select, bypass, reorder, add (FX picker), remove, replace; parallel lanes shown and collapsible; clip LEDs; missing-resource badge | All topology messages exist | Port the lane layout (`signalPath.ts:719-935`), drop rules and chain rules |
| **Selected effect: controls** | Header with bypass; knob grid (landscape) or slider list (portrait) from catalog metadata; groups; Main/Advanced; per-effect presets | Catalog carries `group`, `advanced`, `step`, `labels` and `taper`. `ParamTaper.h` is C++ | Port value formatting (`paramControls.ts:264-283`) and the pitch-shift range rules |
| **Selected effect: visualisation** | Per-node peak meter and DSP chip; EQ curve with draggable handles over a live spectrum; cab response; NAM/IR model card; spatial radar; input analyzer | `sld`, `sldS`, `dspPerformance`, `spatialPosition` and `getEffectResponse` exist. EQ curves are TypeScript maths fixed at 44.1 kHz (`eqPlot.ts:281`). Only Simple Cab implements `GetFrequencyResponse` | EQ responses move into the engine (part 5). Presentation tables are shared (part 6) |
| **Resource browsing: local** | Library and Folders tabs, search, favourites, architecture badge, audition-then-keep or cancel, previous/next from the effect header | Library, folder scan, metadata, usage, preview and apply are all engine-side | Port dedup, filter, sort and stepping (`resourceDedup.ts`, `resourceBrowser.ts:976-1367`) |
| **Resource browsing: Tone3000** | Search, gear/sort/architecture filters, tone → models, audition, keep, thumbnails | **All in TypeScript**: HTTP, session, zip extraction; bytes reach the engine as base64. The engine has no HTTP client | New `IHttpClient` and `Tone3000Service` (part 3) |
| **Tuner** | Full-screen tuner, live/mute toggle, reference pitch | `tuner` messages | Port smoothing (6-sample average, ±3 cents in tune) |
| **Metronome** | BPM, tap tempo, meter, beat pattern grid, subdivision, click type, volume | `setMetronome`, `metronomeBeat`. `MetronomeSupport.h` is C++ | Port tap tempo |
| **Demo audio** | Clip picker (built-in clips, plus favourite riffs recorded in Soundshed Guitar, playback only), play/stop, repeat, render a clip through the current rig to WAV | The UI reads each WAV and sends it as base64 (`demoAudio.ts:259`); repeat is done in TypeScript (`captureHandlers.ts:151`); riffs play with `previewRiffTake` | Engine plays clips by id and handles repeat (part 4) |
| **Global controls** | Input gain, gate, transpose, global EQ, doubler, output gain, output mute, input mode and channel, calibration profile | `setGlobalChainParam`, `setInputMode`. Mute is inferred in TypeScript from `masterGain` (`controls.ts:99-170`) | Engine-owned mute (part 2) |
| **Meters** | Input and output meters with clip and peak hold | `sld` at 20 Hz | Port roster decoding |
| **Audio and MIDI device** (standalone) | Device type, in/out devices, channels, rate, buffer, input level, test tone, MIDI inputs, Android permission | `audioDevice` messages (`StandaloneAudioSettings`) | A native page driven by the same messages |
| **Settings (subset)** | Theme, UI scale, Tone3000 key and mode, NAM quality, output limiter and ceiling, preset-switch tails | `setSetting` | — |
| **Safety and live use** | Undo/redo and A/B for chain edits; MIDI learn by long-press; keyboard mappings; errors as toasts; confirm dialogs | Undo and A/B are TypeScript-only (`signalPath/historyModel.ts`). Keyboard mappings are run by the UI (`automationPanel.ts:727-792`) | Port both into the client library |
| **Awareness only** | Multi-Rig: a chip showing *n* rigs, the focused one editable, "Reduce to one". Hosted plugin nodes: "Open plugin" on desktop | Supported | — |

### Additions to the original list

Beyond the chain, visualisation, resources, tuner, metronome and demo audio, the list above adds:

- **Preset management and scenes.** Without them, nothing is saved.
- **Setlist stepping.** The main way to change sound mid-gig.
- **Global input/output controls with meters, and output mute.**
- **Audio and MIDI device settings.** The standalone is unusable without them, and Android needs the input-permission flow.
- **Undo/redo and A/B.** On a touch screen, a mistaken tap is common.
- **MIDI learn and keyboard mappings.** Mappings are stored and run by the engine, except keyboard mappings, which the UI runs.
- **Shared-sync handling, errors, confirm dialogs and missing-resource warnings.**

**Recording** in Soundshed Guitar Nano means the plugin running in a DAW, where the host
records. The standalone has no recorder of its own, the same as Soundshed Guitar.

### Out of scope

On desktop, all of the following stay available in Soundshed Guitar, which uses the same
profile. **On Android they leave the app entirely**, because Nano replaces the WebView app
there.

- **Riff capture and the riff library.** Includes the capture dialog, trim, save and the
  library page. Favourite riffs still play in the demo list.
- **JAM tab:** YouTube backing tracks, Scales, Practice Tool.
- **TONES tab:** tone sharing, packs, publishing, AI tone search, account.
- **Authoring tools:** custom effect designer, layout designer, blend editor, composite editor, automation mapping editor (MIDI learn is kept), preset pack import/export, library cleanup.
- **Diagnostics:** DSP performance page, signal diagnostics table, feature-flag page.
- **Multi-Rig editing:** building and saving Multi-Rigs.

## Target displays and interaction rules

| Envelope | Logical px | Examples |
|---|---|---|
| Design reference | 800×480 landscape | Raspberry Pi 7" touch screen, most 5–7" SBC panels |
| Floor, landscape | 640×360 | Phone in landscape (~780–900 × 360–410 dp), DAW plugin window at the current 640×400 minimum |
| Floor, portrait | 360×640 | Phone in portrait |
| Comfortable | 1280×800 and up | Laptop; layout stretches, nothing new appears |

The rules come from Soundshed Guitar's compact mode (`core/ui/ts/compactMode.ts`,
`compactStage.ts`, `css/compact/`), which already solved this layout problem:

- **Height is the binding constraint.** Choose the layout from both dimensions and the UI
  scale, never from width alone.
- **Landscape uses a rail; portrait uses stacked bars.**
- **Nothing used mid-song goes behind a menu:** preset step, scene select, tuner, bypass of
  the selected effect, tap tempo.
- **Touch targets** are at least 44 logical px on touch devices and 28 px with a mouse.
- **One sheet at a time.** Browsers, tuner, metronome and settings open as full-height sheets on
  small screens and as side sheets on large ones.
- **Global UI scale** from 80% to 150% (`nativeUi.scale`), the counterpart of the web UI's
  zoom setting.
- **Wrapping chain page** (`nativeUi.chainWrap`, Settings → Appearance): the chain page wraps
  onto more lines to fit its width instead of scrolling sideways, breaking only between a
  connector's segments as the web UI does (`uiSettings.signalPathWrap` there, a choice of its
  own). The mini strip stays one row.

### Landscape, 800×480 (Play, effect selected)

```
┌─────┬─────────────────────────────────────────────────────────────────────┐
│ ▶   │ ◀  ★ Clean Machine •        ▶ │ [A] [B] [C] + │ IN ▮▮▮▯ OUT ▮▮▯ │ ♪ ⋯ │ 44
│Play │─────────────────────────────────────────────────────────────────────│
│     │ Gate ─ TS9 ─ [■ NAM ■] ─ IR ─ Dly ─ Rev                          +  │ 52
│ ☰   │─────────────────────────────────────────────────────────────────────│
│Rigs │ NAM Amp · Dumble ODS   A2 ⏻ │   Input      Output       Mix         │
│     │ ◀ Dumble ODS Clean (T3K)  ▶ │    ◯           ◯           ◯          │
│ ⚙   │ ┌─────────────────────────┐ │  +3.0 dB    −2.0 dB      100%        │ ~300
│Set  │ │ model card / EQ curve / │ │                                      │
│     │ │ spectrum / cab response │ │   Main │ Advanced     Presets ▾      │
│     │ └─────────────────────────┘ │                                      │
├─────┴─────────────────────────────────────────────────────────────────────┤
│ Demo: Riff 1 ▾  ▶ ⟲ │ Tuner │ ♩ 120 ▶ TAP │  ↶  ↷   A│B                   │ 40
└───────────────────────────────────────────────────────────────────────────┘
```

Soundshed Guitar's compact mode alternates the chain and the effect detail. Soundshed
Guitar Nano keeps a **52 px mini strip** above the detail instead. It shows icons, the
selection and bypass LEDs, and tapping a node switches the effect. The full-stage chain
view is used for structural edits: reorder, add, split. Landscape has width to spare, so
the visual and the controls sit side by side.

### Portrait, 360×640 and taller

```
┌──────────────────────────────┐
│ ◀  ★ Clean Machine •      ▶  │ 48
│ [A] [B] [C] +    IN▮▮ OUT▮▮  │ 36
│ Gate TS9 [■NAM■] IR Dly  →   │ 52  (scrolls)
├──────────────────────────────┤
│ NAM Amp · Dumble ODS    A2 ⏻ │
│ ◀ Dumble ODS Clean        ▶  │
│ ┌──────────────────────────┐ │
│ │ visual                   │ │ ~160
│ └──────────────────────────┘ │
│ Input   ──────●─────  +3.0dB │  slider list
│ Output  ────●───────  −2.0dB │  (knobs waste width here)
│ Mix     ───────────●   100%  │
├──────────────────────────────┤
│ Demo ▶ │ Tuner │ ♩120 ▶ │ ⋯  │ 44
│    Play      Rigs      Set   │ 56
└──────────────────────────────┘
```

### Control behaviour

- **Knob or slider:**
  - Relative vertical drag, one full sweep per 200 px, through the parameter's taper.
  - Double-tap resets to the default.
  - Long-press opens a menu: type a value, reset, MIDI learn, MIDI learn for this preset.
  - The mouse wheel works on desktop.
- **Enum parameters** (for example the drive pedals' Model) are segmented buttons when they
  fit, and a picker otherwise.
- **Effect header ◀ ▶** steps through the filtered resource list without opening the browser.
  This is the quickest way to audition amps while playing.

## Architecture

```
 Soundshed Guitar (WebView)              Soundshed Guitar Nano (no WebView)
 ┌──────────────────────┐                ┌──────────────────────────────────────────┐
 │ WebEditor            │                │ NativeEditor (JUCE Components)           │
 │  WebBrowserComponent │                │  shell, sheets, visuals, LookAndFeel     │
 │  core/ui (TypeScript)│                │        │ typed state, commands           │
 └─────────┬────────────┘                │  ui client lib (JUCE-free C++)           │
           │                             │   stores · chain layout · filters ·      │
           │                             │   history · leases · Tone3000 thumbnails │
           │                             └─────────┬────────────────────────────────┘
           │   JSON strings, message thread        │  same protocol, in process
 ┌─────────▼───────────────────────────────────────▼────────────────────────────────┐
 │ SoundshedEditorBase: 60 Hz OnIdle, uiVisibility, window size, CLAP DPI, deep link │
 │ PluginProcessorAdapter: createEditor(), transport slot, HTTP client, device page  │
 ├───────────────────────────────────────────────────────────────────────────────────┤
 │ PluginController + MessageDispatcher + services (+ Tone3000Service)               │
 │ soundshed.db · app settings · resource library                                    │
 └───────────────────────────────────────────────────────────────────────────────────┘
   Both products build this bottom half from the same sources; each links one editor.
```

### Decisions this plan makes

1. **One protocol, two clients.** The native UI talks to `PluginController` through the same
   JSON messages, in process, on the message thread. It does not call controller internals.
   - The protocol is already the compatibility contract, and `tools/check-protocol.mjs`
     already polices it.
   - The controller stays the only writer of state.
   - The engine behaves identically under either UI, and the threading is already solved.
   - The cost is encoding and parsing JSON. Everything is small except the full `state`
     message, which is about 510 KB. Phase 0 measures its cost on the slowest target.
   - If that turns out too slow, the in-process transport can hand over the
     `nlohmann::json` object without `dump()` and `parse()`, and the protocol stays the same.
2. **Persisted edits move into the engine.** This is the main way C3 is met, and it
   benefits Soundshed Guitar too: scene switches stop re-sending whole presets, and a load
   stops needing two round trips.
3. **Shared capabilities land with both UIs on them.** When the engine gains a capability
   both UIs need (a command, a service, a data file), the web UI moves onto it in the same
   change, and its TypeScript version is retired. No capability exists in two
   implementations for longer than one change. This is what keeps drift minimal.
4. **Pure core helpers are called directly.** `ParamTaper.h`, `BiquadDesign.h`,
   `MetronomeSupport.h` and `FiniteCheck.h` are header code with no state. The TypeScript UI
   has to mirror them; the native UI links them and cannot drift.
5. **Presentation data is shared.** Icons, category colours, stock images and category
   remaps are hard-coded TypeScript tables today. They move to one data file that both UIs
   read. Theme tokens are generated from the web UI's CSS variables.
6. **The client logic lives in a JUCE-free library** (`core/src/uiclient/`, built as its own
   static library `SoundshedUiClient`, not part of the engine library). It is unit-tested in
   ctest against a real headless `PluginController`, the way
   `PresetManagementWorkflowTests.cpp` and `MessageThreadTestHost.h` already do. Only views
   live in `juce/`.

### Where the code goes

```
core/src/uiclient/                   JUCE-free, ctest-covered; files under the 800-line budget
  UiClient.{h,cpp}                   send/receive, requestIds, uiReady/visibility,
                                     leases (spectrum watch, device levels) renewed every 2 s
  stores/                            Preset, Chain, Catalog, Resource, Telemetry, Tuner,
                                     Metronome, Settings, Tone3000, Device
  logic/                             ChainLayout, ChainDropRules, ResourceIndex (dedup/filter/
                                     sort/step), PresetFilter, ParamFormat, ChainHistory
                                     (undo, A/B), TapTempo, TunerModel, KeyboardAutomation,
                                     TelemetryDecoder, NodeLabels
core/src/net/IHttpClient.h           async GET/POST, headers, cancel, size cap
core/src/controller/Tone3000Service.{h,cpp}   answers its own messages
core/protocol/effect-presentation.json        icons, colours, images, category remap
juce/source/editor/SoundshedEditorBase.{h,cpp}  shared by both editors
juce/source/editor/WebEditor.{h,cpp}            today's PluginEditor, renamed; Soundshed Guitar only
juce/source/nativeui/                NativeEditor, Shell, TopBar, NodeStrip, ChainView,
                                     EffectView, KnobGrid, SliderList, visuals/, sheets/,
                                     widgets/, theme/ThemeTokens.generated.h
juce/source/JuceHttpClient.{h,cpp}   IHttpClient on juce::URL
tools/gen-native-theme.mjs           CSS variables → ThemeTokens.generated.h (CI drift check)
tools/agent-ui-debug/native/         live-driving tool for the native editor
```

### Packaging (decided)

| | Soundshed Guitar | Soundshed Guitar Nano |
|---|---|---|
| CMake target | `SoundshedGuitar` | `SoundshedGuitarNano` |
| `PRODUCT_NAME` | Soundshed Guitar | Soundshed Guitar Nano |
| Plugin code / bundle id / CLAP id | `SND1` / `com.soundshed.guitar` | New, e.g. `SGNA` / `com.soundshed.guitar.nano` |
| Formats | Standalone, VST3, AU, AAX, CLAP, LV2 (Linux) | Same set |
| Editor | `WebEditor` | `NativeEditor` |
| WebView | `NEEDS_WEB_BROWSER`, `NEEDS_WEBVIEW2`, `JUCE_WEB_BROWSER=1` | All off; web editor sources not compiled |
| `JUCE_FORCE_DEBUG` | On on desktop (for WebView2 DevTools) | Off |
| Shipped `resources/ui/` | Whole UI | Only the data the engine needs: `presets/`, `assets/`, `metronome/`, `demo/`, `images/icons/` |
| Installer | As today | Own installer. No WebView2 runtime step. Never touches the shared profile on uninstall |
| `soundshed://` handler | Registers it | Does not register it |
| Android | Retired once Nano ships | **Replaces the WebView app:** same `applicationId` (`com.soundshed.guitar`), so an upgrade keeps the profile |
| iOS | Not planned | Later; see [iOS later](#ios-later) |

## Engine and host work

This is the prerequisite work. Most of it also benefits Soundshed Guitar.

### 1. Editor seams and the second product (phase 1)

- **Extract `SoundshedEditorBase` from `PluginEditor`.** It covers everything that is not
  WebView-specific:
  - The **60 Hz timer that drives `PluginController::OnIdle()`**
    (`PluginEditor.cpp:646, 663-665`). Nothing else ticks it while an editor is open, and
    the telemetry rate dividers assume 60 Hz.
  - Window-size reporting (`PluginEditor.cpp:827-849`).
  - The CLAP-on-Windows DPI workaround (`PluginEditor.cpp:856-908`).
  - A deep-link hook, so `Main.cpp:457-470` stops using `dynamic_cast<PluginEditor*>`.
  - Sending `uiVisibility` **both ways**. The web editor never sends `false`, so DSP
    metering stays on after it closes (`SignalGraphExecutor.cpp:844-857`).
- **`createEditor`** (`PluginProcessorAdapter.cpp:521-533`) picks the editor from a compile
  define, `SOUNDSHED_NATIVE_UI`. The headless LV2 editor at lines 107-132 is the precedent.
  There is no runtime switch.
- **Transport:** keep the single callback slot, typed as a small interface. There is one
  editor per instance, so no fan-out is needed.
- **CMake:** turn the `juce_add_plugin` call and the `SharedCode` setup (`juce/CMakeLists.txt`
  around 328-463) into a function and call it once per product, with the values in the
  packaging table. JUCE modules compile into each consuming target, so each product can have
  its own defines. The WebView2 NuGet setup and the web editor sources go only into
  Soundshed Guitar.
- **Hard-coded names to check:**
  - The user data folder (`PluginProcessorAdapter.cpp:940`) **stays "Soundshed Guitar"**,
    because that is what C1 requires.
  - The startup log folder (`PluginEditor.cpp:303`).
  - The macOS sandbox-migration bundle id (`PluginProcessorAdapter.cpp:78-80`).
  - `PRODUCT_NAME_WITHOUT_VERSION` (`juce/CMakeLists.txt:440`).
- **Resource root:** `UiBridge::IsValidResourceRoot` requires `ui/index.html`
  (`UiBridge.cpp:61-72`). Check for the data the engine actually needs instead:
  `ui/presets/factory`, `ui/assets/layouts`, `ui/assets/composites`, metronome clicks and
  `ui/demo`.
- **Standalone window:**
  - Fix the clamp at `Main.cpp:369-370`. It forces the restored size to at least 1024×768
    after clamping to the display, so on a smaller screen the window ends up bigger than
    the screen. This also affects Soundshed Guitar, and a fix is already in progress in a
    separate session.
  - Soundshed Guitar Nano keeps its own `window-state-nano.json`.
  - Add a full-screen/kiosk option for SBCs.
- **One standalone at a time.** JUCE's single-instance check keys on the app name, so the two
  standalones could run together. A shared inter-process lock prevents that: they would
  contend for the audio device, and app-wide `uiSettings` would go to whichever wrote last.
- **Installers:** a Soundshed Guitar Nano variant of `juce/packaging/installer.iss`, plus
  macOS and Linux packages. Neither product's uninstaller may remove the shared profile.
- **Android: Nano replaces the WebView app.**
  - **Build.** The Gradle build is tied to the target name `SoundshedGuitar_Standalone`
    (`android/CMakeLists.txt:75` and the targets list in `build.gradle.kts`). Make the
    target a Gradle property: the WebView app stays the default until phase 6, and the
    release then switches to `SoundshedGuitarNano_Standalone` for good.
  - **Side-by-side testing.** Development builds of Nano use `applicationIdSuffix ".nano"`,
    so they install next to the WebView app with their own throwaway profile. The release
    build drops the suffix.
  - **Upgrade in place.** This needs the same signing key and a higher `versionCode`. Today
    release builds are signed with the local debug key (`build.gradle.kts` around line 100)
    and `versionCode` is 1. The app isn't distributed yet, so there are no store users to
    migrate. A tester's device keeps its data only if the new build is signed with the same
    machine's debug key.
  - **Assets.** Staging shrinks to what the engine needs (`presets`, `assets`, `metronome`,
    `demo`, `data`, `images`); the `dist`, `css` and `ui-components` staging goes.
    `SoundshedApp` still unpacks them to `<dataDir>/resources`.
  - **Code to retire at the switch:**
    - the patched `JuceWebViewClasses.java` and its upstream-hash pin task in
      `build.gradle.kts`;
    - the CSS inset plumbing in `SafeAreaInsets.java`;
    - the WebView sections of `docs/android-build.md`.
  - **Safe area.** Nano needs `juce::Display::safeAreaInsets`, which is untested with this
    peer setup (spike in phase 0).
  - **Orientation.** The manifest locks landscape (`AndroidManifest.xml:39`). Lift the lock
    once the portrait layout lands. `configChanges` already covers rotation, so JUCE only
    has to relayout.
  - **Launcher label.** "Soundshed Guitar" or "Soundshed Guitar Nano"; decide at release.
- **Linux:** HTTPS needs `JUCE_USE_CURL=1` (currently `0`, `juce/CMakeLists.txt:412`).

### 2. Engine-owned edits (phase 2)

New or extended messages, each backward compatible, each documented in
`docs/user-interface.md`, and each added to `core/protocol/ui-messages.json`.

| Message | Replaces | Notes |
|---|---|---|
| `selectScene {sceneId}` | Re-sending the whole preset as `loadPreset` | Reuses the `SelectSceneByIndexDirect` logic (banks hosted-plugin state, then applies). Also helps gapless switching |
| `addScene {fromSceneId?, title?}`, `renameScene {sceneId, title}`, `removeScene {sceneId}` | `presetScenes.ts` changing a UI draft | The engine changes its working copy and reports it with `presetLoaded`, as scene switches already do |
| `loadPreset {presetId, sceneId?}` with no preset body | `getPresetById` followed by `loadPreset {preset}` | The engine resolves the id from the store, factory or user presets |
| `newPreset {templateId?}` | `presets/saveModal.ts:131` | — |
| `savePreset` with no `preset` | — | **Already supported:** it saves the working copy (`PluginControllerPresets.cpp:223-262`). Add a test that the result matches what the TypeScript path writes, including `attachments` and tags |
| `activePresetDirty` in `state` / `presetLoaded` | The UI comparing against a snapshot | Needs an engine-side "last saved" snapshot |
| `setPresetFavorite {presetId, favorite}`, `setPresetRating {presetId, rating}` | Replacing the whole favourites/ratings document | A patch avoids lost updates between instances |
| Engine-kept recents (updated on load; `getPresetRecents`) | `uiSettings.presetRecents` | Migrates the existing list |
| `setResourceFavorite {resourceId, favorite}` | Rewriting the whole `resources.favorites` array | — |
| `setOutputMuted {muted}` plus `outputMuted` in `state` | `setMasterGain(0)` and inferring from gain ≤ 1e-4 (`controls.ts:99-170`) | — |
| `uiSettingsChanged {patch}`, `uiViewStateChanged {patch}` | Replacing the whole blob | C4 |

The web UI moves onto each command in the change that adds it, and the TypeScript code it
replaces is deleted then (architecture decision 3). The old message forms keep working for
backward compatibility.

### 3. HTTP and Tone3000 (phase 4)

- **`IHttpClient`** (`core/src/net/`):
  - Async requests on a worker, with cancel, a size cap and progress.
  - Injected through `IPluginHost`, implemented with `juce::URL` in `juce/source/`.
  - A fake implementation for ctest.
- **`Tone3000Service`** answers its own messages, the way `TunerService` does. Its behaviour
  is ported from `tone3000.ts` and `tone3000Api.ts`:
  - Messages: `tone3000Search`, `tone3000Favorites`, `tone3000Models`, `tone3000Import`,
    `tone3000Preview` and `tone3000SessionState`. Each reply carries `requestId`.
  - Session: `POST /auth/session` with `tone3000.apiKey`, refreshed 60 s before expiry
    (5 s minimum), with one retry after a 401.
  - Proxy mode rewrites URLs to `api-guitar.soundshed.com`.
  - Gear, sort and architecture mappings, and lenient pagination parsing.
  - Models are fetched **across all pages**. The TypeScript client stops at the first 100.
- **Downloads happen in the engine.** Zip archives are unpacked with miniz, which is already
  linked. `ExtractFirstResourceFromZip` is a stub today (`PluginControllerResources.cpp:1839-1847`).
- **Imports reuse the existing import code** and fix its gaps:
  - it computes the hash;
  - it calls `TouchSharedSyncState({"resourceLibrary"})`;
  - it keeps the C6 identity.
- **Preview** uses the existing temp-file flow.
- **Thumbnails** are fetched by the client library through `IHttpClient` into
  `<settings>/cache/tone3000-images/`, not sent over JSON.
- **Soundshed Guitar's web UI moves onto the service in the same phase.** The browser tab,
  the Settings browser, the details view, header stepping and shared-preset reference
  resolution all switch to it. That retires the TypeScript HTTP, session and JSZip code, and
  the base64 transfer over the bridge. The import code then exists once, so C6 holds by
  construction. The TypeScript UI keeps only presentation: lists, filters and thumbnails,
  with `<img>` loading thumbnails straight from their URLs as it does today.
- The zip, hash, shared-sync, second-preview and paging fixes are already being worked on in
  a separate session. Rebase this work onto them.

### 4. Demo clips (phase 2)

- A manifest, `core/ui/demo/clips.json` (id, title, file), replaces the list in
  `core/ui/ts/state.ts:10-35`. Both UIs read it.
- `previewDemoAudio {clipId, repeat}` and `renderDemoAudio {clipId}` let `DemoPreviewService`
  read the file itself. It also handles repeat.
- Favourite riffs keep playing through the existing `previewRiffTake`.

### 5. EQ responses (phase 5)

- `GetFrequencyResponse` on the parametric and graphic EQs, built on `BiquadDesign.h`
  responses. The native EQ view then uses `getEffectResponse`, with requests coalesced as
  `effectResponse.ts` already does.
- The web UI's EQ curves move onto `getEffectResponse` in the same phase, and the 44.1 kHz
  maths in `eqPlot.ts:288-385` is deleted. Check that dragging a handle still feels
  immediate with the engine round trip. If it doesn't, draw a provisional curve until the
  reply arrives, rather than keeping the maths.

### 6. Shared presentation data and theme (phase 3)

- **`core/protocol/effect-presentation.json`** holds the tables that are TypeScript-only today:
  - icons by effect type and category (`iconAssets.ts:60-132`);
  - node colour classes and the category remap (`nodeTypes.ts:18-34, 156-166`);
  - stock images and background gradients (`visualization.ts:16-46`).
  The TypeScript UI loads the file too, and its hard-coded tables are deleted in the same
  change.
- **Icons** are the existing SVGs, rendered with `juce::Drawable`. Phase 0 checks that JUCE's
  SVG subset handles them.
- **Theme:** `tools/gen-native-theme.mjs` turns `css/variables.css` and
  `css/themes/{light,dark,classic}.css` into `ThemeTokens.generated.h`, and CI fails on drift.
  The theme choice is the shared `theme` setting.
- **Font:** the web UI's font is embedded as `BinaryData`.

## TypeScript logic to port into the client library

| Source (TypeScript) | What | Destination | Parity check |
|---|---|---|---|
| `resourceDedup.ts:117-205` | Dedup by hash or path, preference order | `ResourceIndex` | Shared vectors (see Testing) |
| `resourceBrowser.ts:976-1367`, `resourceBrowser/helpers.ts` | Filters, facets, sort, search | `ResourceIndex` | Shared vectors |
| `resourceBrowser.ts:2011-2100` | Previous/next lists per context | `ResourceIndex` | Shared vectors |
| `signalPath.ts:719-935` | Split and join detection, lane layout | `ChainLayout` | Fixtures from real presets |
| `signalPathDropTargets.ts`, `signalPath/chainRules.ts` | Drop rules, "cab in path" | `ChainDropRules` | Shared vectors |
| `presets/filter.ts`, `presets/controls.ts:28-60` | Library filter and wrap-around stepping | `PresetFilter` | Shared vectors |
| `signalPath/historyModel.ts`, `history.ts` | Undo/redo (320 ms settle, 100 deep, per preset and scene), A/B | `ChainHistory` | Unit tests |
| `paramControls.ts:264-283`, `layoutRenderer.ts:654-669` | Value formatting | `ParamFormat` | Shared vectors |
| `signalPath/paramsPanel/pitchShiftRange.ts` | Semitone range and snap | `ParamFormat` | Unit tests |
| `tuner.ts` | Smoothing, in-tune window, neighbouring notes | `TunerModel` | Unit tests |
| `metronome.ts:435` | Tap tempo (at least 3 taps, 2.5 s reset, keeps 8) | `TapTempo` | Unit tests |
| `automationPanel.ts:727-792` | Running keyboard mappings | `KeyboardAutomation` | Unit tests |
| `messages/telemetry.ts`, `signalPath/telemetry.ts` | `sld` roster decode, DSP chip smoothing | `TelemetryDecoder` | Unit tests |
| `signalPath/nodeLabels.ts:99-275` | Display names, architecture badge, "Calibrated" | `NodeLabels` | Shared vectors |
| `presetV2.ts:163-283` | WASM effect parameter parsing from `node.config` | **Engine:** include it in node info | — |
| `presetScenes.ts`, new preset, recents, mute, demo repeat | Persisted edits | **Engine** (parts 2 and 4) | Compatibility tests |

## Testing and tooling

- **Client library in ctest.** Each store and logic unit runs against a real headless
  `PluginController` (`MessageThreadTestHost.h`'s pumped host), with no JUCE involved.
- **Compatibility tests** (C1–C8). These are the ones that matter most:
  - Record Soundshed Guitar's message traces with `tools/agent-ui-debug` for these
    operations: save, save as, new, rename, scene add/rename/remove/switch, preset favourite
    and rating, recents, setlist step, resource favourite, Tone3000 import.
  - Store the traces under `core/tests/fixtures/ui-traces/`.
  - Replay each trace, and the native client's version of the same operation, into fresh
    headless controllers.
  - Compare the resulting `soundshed.db` documents field by field.
- **Shared vectors.** Inputs and expected outputs for each ported pure function live in
  `core/protocol/fixtures/`. The vitest suite and ctest both run them, so a change on one
  side fails on the other.
- **`check-protocol` covers the native client.** Today it scans only `core/ui/ts`. Extend it
  to `core/src/uiclient/` and `juce/source/nativeui/`, so a native send that nothing routes
  fails CI, as a web one already does.
- **Render snapshots.** Draw key screens to images with `createComponentSnapshot` at 640×360,
  800×480, 360×740 and 1280×800, in all three themes. They catch layouts that overflow at
  the floor sizes.
- **Live driving tool** (`tools/agent-ui-debug/native/`). The native counterpart of the CDP
  workflow in `CLAUDE.md`, compiled into debug and dev builds only:
  - a localhost JSON port that dumps the component tree (ids, bounds, state);
  - clicks and drags by component id;
  - screenshots;
  - an answer to `captureDebugSnapshot` with the native UI's own state
    (`PluginControllerDiagnostics.cpp:46-63` expects the page to answer).
- **Device performance.** Measure message-thread frame time on the slowest target with
  meters, a spectrum and the chain all animating, and confirm the audio deadline is untouched.
- **Existing gates stay green:** `npm run verify`, `smoke-test.mjs` for Soundshed Guitar, the
  C++ size check, pluginval and clap-validator on both products' plugins
  (`tools/validate-plugins.mjs` needs to learn the second product).
- **Manual cross-product checklist** at the end of each phase: do it in one product, then
  check it in the other, with both installed on the same machine.

## Phases

Sizes are rough estimates for one engineer who knows JUCE.

| Phase | Contents | Done when | Size |
|---|---|---|---|
| **0. Spikes and decisions** | The open decisions. On the slowest target device: JUCE software renderer vs OpenGL; parse cost of the full `state`; SVG icons through `Drawable`; Android safe area with the current peer | Results and decisions recorded in this document | S, ~1 wk |
| **1. Seams and second product** | Editor base and factory; `SoundshedGuitarNano` target building every format on Windows, plus an Android build of Nano (the `.nano` suffix, next to the WebView app); client library with preset, catalog and state stores plus tests; resource-root check; single-instance lock; check-protocol covering native; live driving tool v1 | The Soundshed Guitar Nano standalone boots on the shared profile, shows the preset, and previous/next works. Both products install side by side. Soundshed Guitar is unchanged (`smoke-test.mjs` passes) | M, ~2 wk |
| **2. Engine-owned edits** | Part 2 commands; demo clip manifest and engine playback; Soundshed Guitar's web UI moved onto them; compatibility tests | Compatibility tests pass; web verify and smoke test pass | M, ~2 wk |
| **3. Play MVP** | Shell (rail and stack), theme, top bar, scenes, mini strip and chain view, FX picker, effect controls, meters, controls sheet, tuner, metronome, demo audio, preset browser and save, setlist stepping, device settings, settings subset, toasts and dialogs, shared sync, Multi-Rig chip, presentation data file (web UI moved onto it) | A setlist can be played end to end on an 800×480 touch screen and on an Android phone, and every edit shows up in Soundshed Guitar | L, ~5 wk |
| **4. Resource browsing** | Library and Folders tabs, audition and cancel, resource favourites, stepping from the header; `IHttpClient`, `Tone3000Service` and the Tone3000 tab; the web UI's Tone3000 code moved onto the service | A Tone3000 model found, auditioned and kept in Soundshed Guitar Nano shows as imported in Soundshed Guitar, and the reverse. No Tone3000 HTTP is left in TypeScript | L, ~5 wk |
| **5. Visualisations** | Engine EQ responses, with the web UI's EQ curves moved onto them; EQ curve with handles and spectrum; cab response, IR match and export; NAM/IR model card; spatial radar; analyzer; blend ticks. Custom image layouts are optional here (M) | Every in-scope effect type has a visual at all four snapshot sizes | M, ~3 wk |
| **6. Parity, packaging and hardening** | Undo/redo and A/B; MIDI learn by long-press; keyboard mappings; hosted-plugin "Open"; accessibility (value interfaces on knobs, keyboard focus); setlist editing; update check through `IHttpClient`; installers (Windows, macOS, Linux with curl); the Android release switched to Nano and the WebView-only Android code retired; docs (`user-interface.md`, `agent-quickstart.md`, `android-build.md`, the change checklist) | Release candidate | M, ~3 wk |

The total is roughly 19–21 engineer-weeks, including moving the web UI onto each shared
capability as it lands. Phases 2, 4 and 5 each improve Soundshed Guitar on their own, and
can ship before Soundshed Guitar Nano does.

## Risks

| Risk | Mitigation |
|---|---|
| **Two UIs drift.** Every new feature needs building twice, or has to be declared Soundshed Guitar-only | Shared capabilities land with both UIs on them (architecture decision 3); engine-owned edits; shared presentation and theme data; check-protocol covering native. Add two lines to the change checklist in `.github/copilot-instructions.md`: "Soundshed Guitar Nano: yes / no / later" and "Shared capability: both UIs moved onto it, TypeScript copy deleted" |
| **Behaviour hidden in TypeScript is missed** | Found so far: demo repeat, output mute, keyboard mappings, recents, the save-echo guard, the debug-snapshot answer. The porting table and the compatibility tests cover these; expect more |
| **Android users lose everything outside Nano's scope**: tone sharing, Practice Tool, riff capture and library, JAM, the authoring tools | Their data is kept by the in-place upgrade, and every preset still plays, because the engine is the same. Say so in the release notes. Bring back later only what users ask for |
| **Android upgrade in place fails** (wrong signing key, or `versionCode` not raised) | Set up a real release keystore before the first public Android release; add a CI check that `versionCode` rises |
| **JUCE drawing is too slow on SBCs and cheap phones** | Measure in phase 0. Use an OpenGL context where it helps, repaint only what changed, draw meters at 20–30 Hz, and pause the spectrum when it is off screen |
| **Android peer and insets** | JUCE adds its peer through `WindowManager.addView`, so the activity never sees the view. Spike in phase 0; the fallback is fixed margins from `Display::userArea`. Also handle the soft keyboard in search boxes and the Back button |
| **The two standalones run at once** | Shared single-instance lock |
| **Last writer wins on `uiSettings`** | Merge patches (C4) |
| **Users expect DAW projects to move between the products** | They can't (different plugin ids). Say so in the installer and docs, and point to presets as the way to move a rig |
| **The 510 KB `state` is slow to parse on slow devices** | Measure in phase 0; hand over the JSON object in process if needed |
| **C++ UI iteration is slower than TypeScript** | Keep views thin and put logic in the ctest-covered client library; use render snapshots for fast visual feedback |

## iOS later

iOS is a later target, not part of this plan. It is not scheduled, but these choices keep
it cheap:

- **Views stay touch-first and platform-neutral.** No desktop-only interactions without a
  touch equivalent (hover, right-click), and no platform APIs outside `juce/source`.
- **Networking is `IHttpClient` on `juce::URL`**, which works on iOS unchanged.
- **The profile path stays behind `FileSystem`.** On iOS the standalone app and an AUv3
  extension can share data only through an App Group container. That is the one place iOS
  will need new host code, the same way Android sets a root override today
  (`PluginProcessorAdapter.cpp:239-248`).
- **The engine already builds without the desktop-only pieces** (WASM effects, plugin
  hosting, ASIO), because Android needs the same.

## Decisions

**Decided 2026-09-23:**
- **Packaging:** a distinct product, Soundshed Guitar Nano.
- **Scope:** no riff capture.
- **Android:** Nano replaces the WebView app under the same `applicationId`.
- **Platforms:** Windows, macOS, Linux (including an 800×480 SBC) and Android in landscape
  and portrait. iOS comes later.
- **Drift:** the web UI moves onto each shared engine capability as it is built, in the same
  change.

**Still open:**

1. **Scope switches:**
   - Multi-Rig: awareness only (recommended) or full editing.
   - Setlist editing: phase 6 (recommended) or phase 3.
   - Custom image layouts: optional in phase 5.
   - Input-calibration training: later.

## Appendix: messages the native UI uses

About 65 of the 148 UI→engine types, plus the new ones in parts 2–4.

- **Lifecycle and settings:** `uiReady`, `requestState`, `uiVisibility`, `uiSettingsChanged`,
  `uiViewStateChanged`, `getAppInfo`, `openUrl`, `setSetting`, `setTheme`, `getTheme`.
- **Presets:** `getPresetList`, `getPresetById`, `loadPreset`, `savePreset`, `deletePreset`,
  `get/setPresetFolders`, `get/setPresetFavorites`, `get/setPresetRatings`, `getSetlists`,
  `setSetlists`, `setSetlistCursor`.
- **Chain:** `addSignalPathNode`, `deleteSignalPathNode`, `replaceSignalPathNode`,
  `reorderSignalPathNode`, `collapseSignalPathSplit`, `updateSignalPathNodeBypass`,
  `updateSignalPathNodeParam`, `updateSignalPathNodeConfig`, `updateNodeResource`,
  `browseNodeResource`, `getEffectCatalog`, `getEffectPresets`, `saveEffectPreset`,
  `deleteEffectPreset`.
- **Visuals:** `getEffectResponse`, `setSpectrumWatch`, `setSignalDiagnosticsEnabled`,
  `matchSimpleCabToIr`, `exportEffectAsIr`.
- **Resources:** `listResourceFolder`, `browseResourceFolder`, `saveLocalLibraryResource`,
  `updateLibraryResource`, `queryResourceUsage`, `previewRemoteResource`,
  `cancelPreviewResource`.
- **Global:** `setGlobalChainParam`, `getGlobalChain`, `setMasterGain`, `setInputMode`.
- **Tuner, metronome, demo:** `tuner`, `setMetronome`, `previewDemoAudio`, `stopDemoAudio`,
  `renderDemoAudio`, and `getRiffLibrary` / `previewRiffTake` for favourite riffs.
- **Device and MIDI:** `audioDevice`, `runSignalPathTest`, `armMidiLearn`, `cancelMidiLearn`,
  `getAutomation`, `setAutomationValue`.
- **Multi-Rig awareness:** `removeActivePreset`, `focusMixerPreset`.

### Found while planning

These came from reading the code and affect Soundshed Guitar today. Fix sessions were
started on 2026-09-23 for the first three items.

- **Tone3000 imports:**
  - they skip the hash and the shared-sync touch;
  - previewing a zipped model fails, because `ExtractFirstResourceFromZip` is a stub;
  - a second preview may record the deleted temp file as the "original";
  - only the first page of 100 models is fetched per tone.
- **Standalone window:** the restored size is forced to at least 1024×768 after clamping to
  the display (`Main.cpp:369-370`).
- **Tuner:** the mute button only logs (`core/ui/ts/tuner.ts:257-266`); the Live toggle is
  what actually silences.
- **Web editor:** it never sends `uiVisibility: false` when it closes, so DSP metering keeps
  running.
