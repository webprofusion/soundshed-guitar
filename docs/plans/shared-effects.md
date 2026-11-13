# Shared effects: Custom Effects and composites in Tone Sharing

Goal: people can publish WASM Custom Effects and composite effects (small signal-chain snippets
that act as one effect) to Tone Sharing. Others install them, use them in presets, and share
presets that use them. Each effect works with its standard controls, and can ship one or more
custom layouts. An effect can also draw parts of its own UI (an oscilloscope, a reverb's room
shape, a spectrogram) in places the layout chooses.

This plan lists what exists, what is missing, and the order to build it in. It was written on
2026-10-30 against HEAD 29e300d9 plus the uncommitted layout image picker work. The server-side
detail (API, upload checks, moderation) is in the private repo:
`soundshed-guitar-private/docs/plans/2026-10-30-shared-effects-server.md`.

## Where things stand

| Area | What exists | What blocks sharing |
| --- | --- | --- |
| **Custom Effects** (`WasmEffect`) | ABI 2 params by id (5e9795b8); a sandbox with epoch deadlines, a store limiter, a 256 KiB stack and a 32 MiB module cap; a hosting gate (`features.customEffects.enabled`); `CustomEffectLibrary` entries with name, category, thumbnail and `descriptorSummary` | ABI 2 phases 2-4 are open: resources are still positional, the UI has its own descriptor parser, and every sample and the authoring doc are ABI 1. A module's identity is a local library id, and `LibraryResource.hash` is 64-bit FNV-1a, not SHA-256. Library lookups fall back loosely by id suffix and hash, wasm included. A missing or failed module passes dry with no sign in the UI. The guest has no output apart from audio. |
| **Composites** (`CompositeEffectDefinition`) | An inner graph, `exposedParams`, `exposedResources`, three factory definitions, a user library in `<settings>/composites/user/<id>.json`, inner-graph editing in the main signal path | No export or import. Ids are local (`composite-<time>-<rand>`) and go into file paths unsanitised (`CompositeEffectLibrary::SaveDefinition`). There are no revisions; a save overwrites, and running instances keep the old definition. A preset whose composite is missing gets a silent passthrough. The editor's New, Clone and metadata edits never reach the engine: `HandleEnterCompositeEditModeRequest` needs an id the library already has, and `HandleExitCompositeEditModeRequest` ignores `metadata`. There is no editor UI for `exposedResources`. Nothing stops a composite containing itself. `GetParam` returns defaults. The `layout` field round-trips but nothing renders it. |
| **Custom layouts** | Knobs, sliders, toggles and resource dropdowns; text; image overlays; backgrounds; scaling; filmstrip knobs; `requires` and version stamping; a zip export (`*.sgfxlayout.zip`); Nano Q draws them read-only | Layouts are keyed by `effectType` (+ `blendId`), so **every Custom Effect shares the `wasm_host` layouts**. The designer lists the registry's params, not the module's, so it can't bind a module's own params. Import is UI-only, into whatever layout the designer has open, and the archive's `effectType` is thrown away. The engine accepts any JSON as a layout, with no schema check or size cap. Preference rules can hide a newly saved layout (review §3). No element shows live data. The value readout is unescaped, and for a module its text comes from the descriptor. Nano Q can't draw SVG, and every factory category image is SVG. |
| **Live data to the UI** | `TelemetryPublisher` feeds at fixed rates; leased watches; `SpectrumTap` and `LoudnessTap` rings; `BandMeter` atomics; everything as JSON through `evaluateJavascript` | Each feed is bespoke. Spectrum and loudness watches are singletons, so the most recent watcher wins. The visual panels (EQ curve, multiband, analyzer and so on) are fixed per type and sit beside a custom layout, never inside it. Nano Q parses `sldS` but not band meters. |
| **Tone Sharing** | Preset items and packs; two archives per preset (public, with Tone3000 models by reference, and private); install in the engine (`installPresetArchives`); preview staging; email sign-in; per-account feature gates (`user_features`); `items.type` already allows `layout` and `composite` | The client treats every item as a preset archive and the browse feed has no type filter. Archives strip modules (`IsShareableResourceType` is nam/ir only) and carry no composite definitions or layouts. The server's payload check allows only `.json`, `.wav` and `.nam`. There are no item versions, no update model and no payload hashes or signatures. Publishing approves at once. Nano Q can browse and install but not publish. |

## Decisions to make first

**S1. Freeze the ABI before anything is shared, and decide block processing (ABI plan D1) now.**
A shared module is a promise to keep running. Visual streams (below) make per-sample processing
worse: a guest would have to count samples to know when to publish. Measure D1 as that plan says
(a gain module and the phaser through the standalone effect harness at 48 kHz / 64). If it's
taken, ABI 2 ships with `audiofx_process(frames)` and nothing is shared on the per-sample form.

**S2. One package format for both kinds.** A Custom Effect and a composite are both "an effect
you drop in a chain, with optional layouts". They share the format, the item type, install and
update. A composite can depend on Custom Effects, and on other composites, by package id.

**S3. The host draws; the effect supplies data.** Effects don't run UI code. A layout places
host-drawn **visual elements** (scope, spectrum, spectrogram, meter, curve, shape and so on).
Each element reads a **visual stream**: either one the host derives from the node's audio, or
one the guest publishes as numbers. This rules out:

- a second "view" module running in the UI, which would need `'wasm-unsafe-eval'` in the CSP,
  a wasm runtime in Nano Q's UI and a second sandbox;
- author HTML or JS in an iframe, which Nano Q can't run and which needs CSP holes.

It works the same in the web UI, Nano Q, iOS and Android, and the only thing the guest can send
the UI is clamped floats.

**S4. Revisions are immutable, and presets pin them.** Changing a published effect changes how
every preset using it sounds. Each publish makes a new revision with a content hash. A preset
node records the package id and revision. Updating an installed effect is a user action, and
presets keep the revision they were made with until the user moves them on. See D3.

**S5. Code from Tone Sharing is treated as code.** Installing a Custom Effect, or anything that
contains one, shows a consent prompt that says so and names the publisher. The app installs only
modules whose SHA-256 is pinned in a package manifest signed by the server. The signature means
"this is the module that passed review", not "this module is safe"; the sandbox stays the
safety. This lifts the nam/ir rule at its one place, `IsShareableResourceType`, as its header
comment intends. `plugin` stays blocked.

## The effect package

A zip (`*.sgfxeffect`) with `effect.json` at the root:

```json
{
  "formatVersion": 1,
  "kind": "custom-effect | composite",
  "packageId": "fx_8c1e…",            // global; the Tone Sharing item id once published
  "revision": "r_3f9a…",               // immutable; content hash of everything below
  "name": "…", "category": "…", "description": "…",
  "author": { "handle": "…", "displayName": "…" },
  "licence": "…",
  "minAppVersion": "1.7.0",
  "requires": ["custom-effect-abi-2", "visual-streams"],
  "modules": [{ "id": "main", "file": "modules/main.wasm", "sha256": "…" }],
  "composite": { /* CompositeEffectDefinition; inner wasm nodes reference modules[].id */ },
  "dependencies": [{ "packageId": "fx_…", "revision": "r_…" }],
  "resources": [/* NAM/IR exactly as preset archives carry them: embedded, or Tone3000 by reference */],
  "layouts": [{ "file": "layouts/<n>/layout.json", "default": true }],
  "thumbnail": "thumbnail.webp",
  "effectPresets": [/* optional per-effect presets: params and resources */]
}
```

and a detached `effect.sig`: the server's Ed25519 signature over `effect.json`. The manifest pins
every file by SHA-256, so the one signature covers the module, layouts and images.

- Layout files keep the existing `layout.json` + `images/` shape, so the layout import and
  compatibility code is reused. Images in a shared package are raster only (PNG, JPEG, WebP).
  Publishing turns any SVG into a PNG at the layout's largest drawn size, since Nano Q can't draw
  SVG and SVG content is only magic-checked today.
- A composite's inner NAM and IR resources travel exactly as a preset archive's do, Tone3000
  terms included. Inner Custom Effects are either in `modules` or listed as dependencies.
- A preset that uses shared effects lists them in its archive as
  `effects: [{packageId, revision, itemId}]`, the way Tone3000 models are listed. Install fetches
  missing ones from the server rather than embedding code in preset archives. A local file export
  may embed packages, behind the same consent.

## Work

Sizes are rough: **S** is a day or less, **M** a few days, **L** a week or more.

### Phase 0 — foundations (blocks everything)

1. **Finish Custom Effect ABI 2** (`docs/plans/custom-effect-abi-v2.md`), with D1 decided (S1):
   resources by id, the one C++ descriptor parser with the TS parser deleted, docs, the phaser
   sample, and the ten generator modules ported. **L**
2. **Content identity for modules.** Real SHA-256 for `wasm` resources; resolve `wasm` refs by
   id or hash only, never through the loose suffix/originalId fallback in
   `ResourceLibrary.cpp`; store modules content-addressed (`wasm:sha256:<hex>`) so two packages
   carrying the same module share one file. **M**
3. **Missing and failed effects are visible.** A node whose composite definition or module is
   missing, whose hosting is off, or whose guest trapped says so on its card and panel. It names
   the effect, and an install link appears when the node carries a `packageId`. This covers DAW
   projects opened on another machine too. Today all of these pass audio silently. The text is
   escaped. Also fix the `SetError` string assignment on the audio thread. **M**
4. **Make the composite editor work end to end.**
   - Enter edit mode with a new or cloned definition, and save the metadata and `exposedParams`
     the UI sends.
   - Add an `exposedResources` editor.
   - Sanitise ids before they become file names.
   - Refuse nesting cycles.
   - Make `GetParam` return live values.
   - Rebuild running instances on save.
   - Split the overloaded `"composites"` shared-sync key from the Multi-Rig one.
   - Wire `createCompositeFromNodes` to "Make composite from selection".
   - Bring `docs/composite-effects.md` in line with the code.

   **L**
5. **Revisions** (S4): composites and Custom Effect entries get `packageId` and `revision`
   fields. Nodes record `config.packageId` and `config.revision`. The library keeps every
   revision a saved preset still uses (they are small), and installs never overwrite one. **M**

### Phase 1 — layouts that belong to one effect

6. **Key layouts per Custom Effect.** Generalise the lookup key's `blendId` part to a variant:
   `wasm_host::ce:<customEffectId>` for Custom Effects, `composite:<id>` as now. Then
   update `layoutLookupKey`, `PluginControllerLayouts.cpp`, `layoutPreferences.ts`,
   `uiclient/EffectLayouts.*` and the FX browser thumbnails. A `wasm_host` layout with no
   variant stays the fallback. **M**
7. **Designer binds a module's own params and resources.** Build the designer's param list from
   `getNodeEffectInfo(node)`, as the runtime panel does, rather than from the registry. **S**
8. **Drop the composite's dead `layout` field.** On load, move the factory composites' layouts
   into the layout library under `composite:<id>`; packages carry layouts as files. **S**
9. **Engine-side layout install.**
   - A `installLayoutFiles` path that takes `layout.json` + images for a given lookup key.
   - Image ids renamed (the rules in `layoutImport.ts`).
   - A schema check and caps on the layout JSON: total bytes, element and image counts.
   - The package's default layout becomes the default for its key unless the user has a rule
     for that key.

   This is the same code the UI import should call, per the drift policy. **M**
10. **Fix what a shared layout would trip over:** escape the value readout; review §3 (rules
    shadowing a newly installed layout, dangling rules winning); a warning when a layout
    `requires` features this app lacks. **M**

### Phase 2 — visual elements fed by the host

This phase needs no ABI change, and built-in effects benefit from it too: their EQ curves and band
meters can be placed inside a layout.

11. **A generic per-node visual feed.**
    - `setVisualWatch {scope, presetId, nodeId, streams[]}` with the existing 5 s lease, and many
      watches at once.
    - One `sldV` message per frame carrying every watched stream: `{id, seq, s: {streamId: data}}`.
    - Arrays are quantised to int16 and base64'd so a 512-point scope at 30 Hz stays small.
    - The singleton spectrum and loudness watches become streams on this feed, so two elements
      that both want a spectrum stop fighting over it.

    **L**
12. **Host-derived streams per node:** `in.scope` and `out.scope` (a triggered or free-running
    window from a new `ScopeTap`); `in.spectrum` and `out.spectrum`; `out.spectrogram` (one column
    per frame); `in.level` and `out.level`; `response` (`GetFrequencyResponse`); `bands`
    (`ReadBandMeters`). Taps attach only while watched, as `SpectrumTap` does. Audio-thread cost
    is a copy into a ring. **M**
13. **Layout visual elements.**
    - Kinds: `scope`, `spectrum`, `spectrogram`, `meter`, `led`, `readout`, `curve`, `xy`,
      `shape`.
    - Each has a frame, a stream binding, a range and the colour, line and grid style fields
      the controls already use.
    - Drawn on a canvas in the web UI, through the telemetry flush that's already coalesced at
      30 fps, and with Elements in Nano Q via shared parsing in `uiclient`.
    - Stamped into `requires` as `visual-elements`, so older apps skip them.
    - Edit-mode placeholders in the designer, which also needs a stream picker.

    **L**
14. **Place built-in visuals in layouts:** the EQ curve, multiband band panel and analyzer become
    elements bound to `response` / `bands` / spectrum streams, so a layout can put them inside
    itself rather than above it. **M**

### Phase 3 — visual streams from the guest

15. **ABI addition** (additive to ABI 2; a module that imports it states
    `requires: visual-streams`, and older hosts refuse it at link time, as they should):
    - descriptor `visual.<id>.kind=series|scalar|xy|shape`, `.length`, `.min`, `.max`, `.title`;
    - imports `visual_handle(idPtr, idLen) -> i32` and `visual_publish(handle, ptr, count) -> i32`.

    The host copies at most the declared length of f32s from guest memory into a preallocated
    triple buffer per stream, ignores repeat publishes within a block, and drops NaN and infinity.
    Nothing allocates on the audio thread, and the guest never sends text. A `shape` is a list
    of polylines in 0..1 coordinates: this is what a room shape, a filter curve or an LFO
    path uses, published by the guest when its params change. **M**
16. **Guest streams on the feed.** Guest streams appear on a node as `guest.<id>` beside the
    host-derived ones. The dual-mono twin instance's streams are ignored. **S**
17. **Composites expose visuals.** `exposedVisuals: [{id, nodeId, stream}]` maps an inner node's
    stream (host-derived or guest) to the composite, the way `exposedParams` maps params. **S**
18. **Samples:** a scope-and-meter module and a room-shape reverb module, both ABI 2 with
    layouts, in `tools/wasm-samples/`. Add the visual ABI to the authoring doc and to the AI
    designer's prompt template. **M**

### Phase 4 — package, install and local sharing

19. **Export a package:** from a Custom Effect entry or a composite, the user picks its layouts.
    - Validation runs before writing: ABI 2; the descriptor parses; a one-second offline render of
      noise through the effect produces no trap, no NaN and nothing over a CPU ceiling; no SVG
      left; within size caps.
    - The manifest is written with SHA-256s. Unsigned packages are for local files only.

    **M**
20. **`installEffectPackage` in the engine:**
    - Parses everything before writing anything, as `installPresetArchives` does.
    - Checks the hashes, and the signature when one is required.
    - Asks for consent when there is code.
    - Writes modules content-addressed, creates the `CustomEffectLibrary` entry or composite
      definition with `origin: toneSharing`, `packageId` and `revision`, installs the layouts
      (step 9) and resources, and records the install.
    - Uninstall checks whether any preset uses the effect first.

    Installing with Custom Effects turned off is allowed, but the effect stays disabled, and the
    prompt says so. **L**
21. **Preset archives carry effect dependencies.**
    - Export lists `effects[]`.
    - Install fetches missing packages first, as it fetches Tone3000 models now, and asks for one
      consent covering all of them.
    - `IsShareableResourceType` allows `wasm` only when it comes from a verified package.
    - The TS archive builders filter the way the C++ exporter does.
    - Preview stages the effect packages it needs in the temp folder after the same consent, and
      never installs them.

    **L**

### Phase 5 — Tone Sharing

22. **Server:** an `effect` item type, an `effect_package` upload kind and its validator, SHA-256
    on every asset, signing at approval, revisions per item, pre-publication review for anything
    containing code, a `publishEffects` per-account gate, size caps and a report endpoint. Details
    are in the private plan. **L**
23. **App publish:** "Publish to Tone Sharing" on Custom Effect and composite library entries and
    in the composite editor. It reuses the publish consent, thumbnails and pack support, and says
    when a submission waits for review. An installed effect can't be republished as the user's
    own; a composite that uses someone else's effect lists it as a dependency. **M**
24. **App browse and install:** type tabs (Tones, Effects, Layouts); an effect detail page that
    shows the thumbnail, layout screenshots, publisher, licence, "contains code" and
    dependencies; install routed by item type; an Installed list with "update available" per
    revision. Updates never happen silently. **L**
25. **Nano Q:**
    - Parse guest descriptors and the `customEffectLibrary` (it ignores both today).
    - Draw visual elements and band meters.
    - Browse and install effects through `uiclient::ToneService`.
    - Pass the consent prompt through to its own dialog.

    Publishing stays in the web UI. **L**

### Optional, cheap once the above exists

- **Layout-only items** for built-in effects, using the existing `layout` item type and the
  install path from step 9.
- **Effect presets inside packages** (`effectPresets`), installed into the per-effect presets.

## Order and dependencies

```
Phase 0 (1-5) ──► Phase 1 (6-10) ──► Phase 4 (19-21) ──► Phase 5 (22-25)
                       │                    ▲
                       └► Phase 2 (11-14) ──► Phase 3 (15-18)
```

Phases 2 and 3 can run alongside 4, but the ABI addition (15) must be settled before the first
public effect is published, for the same reason as S1. Server work (22) can start once the
package format (Phase 4 step 19) is fixed.

## Open decisions

- **D1.** Block processing in ABI 2. The measurement is above (S1).
- **D2.** One `effect` item type covering both kinds (recommended), or keep the schema's existing
  `composite` type and add a second type for Custom Effects.
- **D3.** Revision pinning. Recommended: pin, and keep every revision a preset uses. The
  alternative is that the latest revision wins: simpler, but a publisher's update changes other
  people's saved tones.
- **D4.** Review before publication for code items (recommended), versus the current
  publish-then-moderate for presets.
- **D5.** Who may publish code at first: accounts granted `publishEffects` (recommended), or
  everyone.
- **D6.** Whether a licence is required on effect items, and from which list.
- **D7.** Signing-key handling: an Ed25519 key held as a Worker secret, the public key in the app,
  and how a rotation is shipped.
- **D8.** Whether locally exported (unsigned) packages can be installed at all, or only with
  Custom Effects' advanced setting on and a stronger warning.
