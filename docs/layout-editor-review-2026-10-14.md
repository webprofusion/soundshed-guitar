# Custom Layout Editor Review — 2026-10-14

Scope: the Layout Designer (`core/ui/ts/layoutDesigner.ts`, `core/ui/ts/layoutDesigner/`), the
runtime renderer and picker (`layoutRenderer.ts`, `layoutPicker.ts`, `layoutPreferences.ts`,
`signalPath/paramsPanel/`), the layout manager, and the engine side
(`core/src/controller/PluginControllerLayouts.cpp`, `controller/internal/LayoutSupport.cpp`).
Line numbers are against HEAD `c7cda254`.

Method: code reading, with every finding below checked against the source. Nothing was
reproduced in the running app, so the scenarios are traced through code rather than observed.

**Status (later on 2026-10-14):** fixed in the working tree, not yet committed:

| Items | Fix |
|---|---|
| 1.1 | Engine checks each image name and content |
| 1.2 | Designer escapes its text and filters CSS |
| 2.1–2.7 | All the data-loss items |

Also fixed: the *Use Default Controls* resync from §4.

The tests that cover them all fail on HEAD:
- `core/tests/LayoutStorageWorkflowTests.cpp`. On HEAD it wrote `..\..\..\..\escape.png` to
  `data/v1/` of its sandbox.
- `core/ui/tests/layoutImport.test.ts`, `layoutStoreSave.test.ts` and
  `layoutDesignerSafety.test.ts`.

§3–§8 are still open.

## Summary

The editor covers the basics: positioned knobs, sliders and toggles, resource selectors,
text labels, rectangle overlays, a bypass hotspot, two background layers, a "backdrop only"
mode, undo, zoom, a preview, and zip export/import. Most problems sit where the parts meet:

- **Importing a layout archive can write any file the user can write.** Fix this first.
- **Edits are easy to lose.** Nothing guards against closing with unsaved changes. Background
  layers collide. Reset turns into a duplicate. Deleting one layout can take another's images.
- **A saved layout often doesn't appear**, because a preference rule decides what the node
  shows and nothing updates the rule.
- **Undo can't be trusted.** Clicks add empty steps, and several deletes add none.
- **Layout controls lag the standard ones.** Toggles, resource selectors, label positions,
  overlays after a bypass, and per-parameter special cases all behave differently.

## 1. Security

### 1.1 Layout import writes files anywhere (high)

- `PluginControllerLayouts.cpp:430` does `imagesDir / util::PathFromUtf8(fileName)`. That
  `fileName` comes straight from the archive's `images[]` manifest, sent by `layoutStore.ts:446-452`.
- JSZip 3.10 only collapses `..` segments separated by `/`. A manifest name of
  `..\..\..\..\..\..\Microsoft\Windows\Start Menu\Programs\Startup\x.bat` (with a zip entry
  of that literal name) passes the `zip.file()` lookup. Windows then treats the backslashes as
  separators.
- A drive-absolute name such as `C:/Users/Public/x.dll` replaces the base path entirely,
  because `operator/` with an absolute right-hand side discards the left.
- Nothing checks the extension, size or content.
- The write happens as soon as the file is picked, before the user saves or cancels.

**Fix:** in C++, run `fileName` through `util::SanitizeFilename` (as `ResolveLayoutDir`
already does for `layoutId`), or reject any name with a parent or root path. Allow only image
extensions, cap the decoded size, and assert `destPath.parent_path() == imagesDir`. In the UI,
also check manifest names against `^[\w.-]+$`.

### 1.2 Imported strings reach the designer's DOM unescaped (medium)

- **Markup injection:** `layoutDesigner.ts:979-980` interpolates the image `fileName` and
  `type` into `<option>` text. Both come from an imported manifest, so a name containing
  `</select><iframe src=…>` breaks out of the select.
  - The CSP blocks script, but allows `frame-src https:` and `img-src http: https:`.
  - The same pattern appears at `layoutDesigner.ts:1048`, `1052` and `1068` (control
    `labelOverride`), `elementProperties.ts:95` (binding label) and `layoutDesigner.ts:1431`
    (resource display name).
  - `no-unescaped-html-attribute` checks attributes only, so the lint can't catch these.
- **CSS sink:** `layoutDesigner.ts:849` assigns a gradient value straight to
  `style.background`, so an imported `url(https://tracker/…)` is fetched when the canvas
  renders. The runtime renderer already filters the same value through `safeCssValue`; the
  designer should too.

## 2. Work that gets lost

| # | Issue | Where | Confidence |
|---|---|---|---|
| 2.1 | **No unsaved-changes guard.** Several ways out discard every edit without asking: Cancel, ✕, Escape with nothing selected, and a mousedown on the backdrop (just outside the dialog). Escape on the *Delete layout?* confirm also closes the designer, because both are `document` keydown listeners and neither stops propagation (`dialogs.ts:112`). | `layoutDesigner.ts:254-258`, `444`, `1844-1849` | high |
| 2.2 | **Background layer index collision.** With two layers, delete layer 1 and add a new one: the new layer gets `layerIndex = backgrounds.length` = 1, the remaining layer's index. An added *image* replaces that layer (`findIndex` by `layerIndex`). An added *colour* duplicates the index, and *Remove Background* then deletes both. | `layoutDesigner.ts:1267`, `1312`, `1350`; `canvasProperties.ts:360` | high |
| 2.3 | **Reset then Save creates a duplicate.** `createDefaultLayout` returns a layout with no `layoutId`, `name` or `blendId`. Save posts an empty id, so the engine mints a GUID while `persistLayoutLocally` mints a different `layout-…` id. The original layout stays alongside the new one. Images browsed after Reset go to `layouts/content/images/`, which `BuildLayoutImages` never reads, so they vanish on restart. | `layoutDesigner.ts:543`; `layoutStore.ts:76`; `PluginControllerLayouts.cpp:349-351`, `418-419` | high |
| 2.4 | **Deleting one layout can take another's images.** *Use Existing* lists every layout's images, but they are copied into the layout's own folder only on its first save (`referencedImageIds` is sent only when `isNewLayout`). Layout B borrows an image from A; deleting A runs `remove_all` on A's folder; B loses its background and exports without it. A factory fork never gets its images copied either, because the copy search walks only user folders. | `layoutStore.ts:165`; `PluginControllerLayouts.cpp:150-160`, `50-52` | high |
| 2.5 | **Upper-case extensions and SVG disappear after a restart.** The extension checks (`.png`/`.jpg`/`.jpeg`) are case-sensitive, so `IMG_0042.JPG` shows during the session (Blink sniffs it) but `BuildLayoutImages` skips it on reload. The JUCE picker offers `*.svg`, which is labelled `image/png` and never renders. | `PluginControllerLayouts.cpp:665`, `377-383`, `832-835`; `juce/source/PluginProcessorAdapter.cpp:717` | high |
| 2.6 | **Failures are reported as success.** `SaveLayoutToFile` returns `void` and only logs a failed `Store().Put`. The index is still updated and `layoutSaved` still sent, so the user gets "Layout saved" twice (`layoutStore.ts:187` and `layoutHandlers.ts:41`) for a layout that disappears on the next load. Delete removes the document before the folder, then returns early if `remove_all` fails, leaving the index pointing at nothing. Meanwhile the UI has already removed the layout optimistically. | `PluginControllerLayouts.cpp:44-58`, `879-893` | high |
| 2.7 | **Import keeps the archive's layout id.** Importing a layout whose id already exists overwrites that layout's images immediately and its document on save. The shipped Chaos Engine layout carries a user-style id (`layout-1778489132806-5d12ni`), so importing an export of it makes a user copy that `layoutManager.ts:186-188` can't delete (it finds the factory entry first). An imported blend layout also keeps its `blendId` and lands under a key the current effect doesn't use. | `layoutStore.ts:456-464` | high |

## 3. A saved layout doesn't show up

- **Rules shadow the layout just saved.** `resolveLayoutSelection` returns the first matching
  rule and reads the library default only when no rule exists (`layoutPreferences.ts:258-291`).
  Saving only moves the library default. `layoutDesigner.onSave` has no subscriber (`notifySaved`
  is dead), and `signalPath.ts:69` only re-renders.
  - After clicking the split toggle's *Standard* half, *New layout…* → Save still shows the
    standard controls.
  - Editing a factory layout that a rule points at forks a new id. The rule still points at
    the factory layout, so the user's edit looks lost.
  - **Fix:** subscribe to `onSave` in `openLayoutDesignerForNode` and re-point the deciding
    rule (`resolveLayoutSelection(ctx).rule`) to the saved id. Also migrate rules when a factory
    layout is forked.
- **A rule pointing at a deleted layout still wins**, so the node falls to the standard controls
  instead of the remaining library default. Delete never prunes rules.
  `layoutPreferences.test.ts:159-162` is titled "ignores rules pointing at a layout that no
  longer exists", yet it asserts `null` while `tweed` is the fixture's default. The test pins
  the bug. **Fix:** skip rules whose target is neither `STANDARD_LAYOUT_ID` nor an available
  entry, prune rules on delete, and correct the test.
- **The picker's Apply defaults to the broadest scope.** *Every use of this effect* is
  pre-checked (`layoutPicker.ts:300-302`). If a preset or keyword rule chose the current layout,
  the new effect-type rule loses to it. The panel doesn't change, yet the toast says it did.
  `selectStandardControls` already handles this case; Apply should preselect the deciding
  rule's scope, or warn.
- **The pencil on a factory layout opens an unrelated user layout.** `layoutDesigner.ts:381-391`
  redirects to the user's default layout for the key, or to any user layout, rather than to a
  fork of *this* factory layout. Once a user has made their own layout for an effect, they can
  never fork the factory one. `docs/user-interface.md:804` says the pencil opens a fork.
  **Fix:** record `forkedFrom` on the fork and redirect only to that, or always fork.
- **Every save makes that layout the library default** (`PluginControllerLayouts.cpp:273`).
  Renaming an alternate layout makes it the default for every node without a rule, and nothing
  can make the factory layout the default again. Set the default only on first save or when
  asked, and add a `setDefaultLayout` message.
- **Two keys can hold one layout id.** Editing a base layout from a blend node finds it under
  the base key, but `layoutDesigner.ts:406-408` stamps `blendId`, so the same id is saved under
  `type::blend` as well. Delete cleans only the key in its payload. `LoadLayoutLibrary` also
  trusts a `defaultLayoutId` without checking it exists, and a dead default blocks the factory
  fallback (`PluginControllerLayouts.cpp:512-516`, `616`).

## 4. Undo

| Issue | Where |
|---|---|
| **Every mousedown on an element pushes an undo step**, even a plain click to select. The undo count grows with no-op steps, and selecting anything after an undo throws away the redo stack. Push lazily on the first real movement, or on mouseup only if the position changed. | `layoutDesigner.ts:1649` |
| **The sidebar's delete buttons add no undo step.** *Remove from Layout*, *Delete Label*, *Delete Rectangle*, *Remove Background* and *Clear* (knob image) mutate the layout on `click`; the once-per-session undo hook listens only for `input`/`change`. Ctrl+Z then reverts the previous action as well. The Delete *key* does push a step. | `elementProperties.ts:234`, `245`, `413`, `609`; `canvasProperties.ts:358` |
| **Inline label edits (double-click) are not undoable.** `label.text` is assigned directly. | `layoutDesigner.ts:1151-1158` |
| **Undo, redo, import and reset don't resync the *Use Default Controls* checkbox** or the dimmed *Add Control* button. After undoing that toggle, the checkbox still shows the old state. | `layoutDesigner.ts:583-605` |
| Every edit in one selection session collapses into a single step: change X, then the font, then the colour, and one Ctrl+Z reverts all three. This is a deliberate design choice, but a coarse one. Each `bind*Handlers` call also adds two `once` listeners to the persistent sidebar element, and they pile up until the next input. | `layoutDesigner.ts:619-624`; `elementProperties.ts:163` |

## 5. Layout controls vs the standard controls

| Issue | Where | Confidence |
|---|---|---|
| **Layout toggles:** the readout never updates, because the handler looks for `.node-param-group` but layout controls sit in `.custom-layout-control`. A toggle also always sends 0/1, yet the designer lets *Toggle* bind to any parameter, so a 20–20000 Hz parameter receives 0 or 1. | `paramControls.ts:156-165`; `layoutRenderer.ts:431` | high |
| **One layout resource control hides every other resource selector.** The designer offers only `__resource__:primary:0`, but the panel drops the whole `resourceSelector`. The IR Cabinet loses IR B, the blend amp loses models 2..N, and a blended node loses the Blend info and *Edit Blend*. The layout's *Browse* button does nothing for anything but nam/ir, and plugin-host layouts can't open the plugin editor. | `panel.ts:358-361`, `437`; `layoutDesigner.ts:1549-1557` | high |
| **Label position Left/Right hides the label** in both the designer canvas and the runtime, which render only `top` and `bottom`. Without a label the knob's aria-label falls back to `nodeId_paramKey`. `valuePosition` is in the model but has no UI. | `layoutRenderer.ts:446`, `639`; `layoutDesigner.ts:1047`, `1067`; `elementProperties.ts:135-136` | high |
| **The bypass hotspot covers the controls under it.** It sits at `z-index: 4` over the controls (2) and labels (3); `layoutTypes.ts:158` documents overlays as *below* controls. The factory Chaos Engine layout has one. | `layoutRenderer.ts:173-175` | medium |
| **"When enabled" / "When bypassed" overlays go stale after a bypass.** Visibility is baked into inline styles at render, and a bypass doesn't re-render the panel for the same node. An LED overlay stays lit. Drive it from CSS off `.is-bypassed` instead. | `layoutRenderer.ts:138-146`; `bypass.ts:14-44`; `signalPath.ts:394` | high |
| **Backdrop mode clips the default controls.** The comment says min-height, but the code sets a fixed `height` with `overflow: hidden` on both wrappers, so the Advanced tab, a blend's extra parameters, or a parameter added later is cut off. | `layoutRenderer.ts:261-263`, `289`, `297-298` | medium |
| **Per-parameter special cases are skipped.** Pitch Shift Semitones ignores Range Min/Max and Snap. Picker enums (Pickup Match's ~250 models) become a 250-step knob. The designer offers no *Dropdown* type for parameters, and enum parameters default to a knob. Factor the panel's per-parameter resolution (range, step, taper, picker) into one helper both paths call. | `layoutRenderer.ts:400-416`; `panel.ts:136-161`; `elementProperties.ts:39-45` | medium |
| **The picker popover ignores UI zoom.** It mixes zoomed rects with unzoomed `offsetWidth`, and it isn't marked `data-control-bar-popover`. Copy `enumPicker.ts:274-288`. | `layoutPicker.ts:399-441` | high |
| **Sliders lag behind knobs:** no double-click reset (`data-default` is emitted but unused), no fine adjust, and readout formatting differs (pan shows raw numbers, dB shows 1 dp). Sliders, toggles and the hotspot `div` have no accessible name or role. | `layoutRenderer.ts` | medium |
| **The value readout is inserted unescaped.** For `wasm_host`, enum labels come from a preset's node-config descriptor. The standard panel's `${label}` and `${group}` have the same exposure. | `layoutRenderer.ts:644`; `panel.ts:167`, `204`, `255` | low |

## 6. Performance

- **Every parameter update re-renders the selected panel**, including updates for other nodes
  (`signalPathHandlers.ts:134`). With a layout, each rebuild re-inlines every background as a
  full base64 data URL, once per knob for custom knob faces. When automation or an expression
  pedal moves *any* parameter, a knob being dragged is torn down mid-gesture. **Fix:** return
  early for other nodes, update the changed control in place, and cache `blob:` URLs per image id.
- **`BuildLayoutImages` sends everything in one message on the message thread.** It reads and
  base64-encodes every image of every layout, factory ones included, with no cache or size cap.
  It runs again in full after every save or delete (`layoutHandlers.ts:27-29`). Re-picking an
  image leaves the old `stem_<timestamp>` file behind, and it keeps being shipped.
  - The `layout-images/` URL fallback (`layoutDesigner/images.ts:18`) is dead, because nothing
    in `core/src` serves that path. Serving images through the WebView resource provider would
    remove the base64 traffic entirely.
- **The image request can get stuck.** If `BuildLayoutImages` throws (`directory_iterator`
  outside a try), no reply comes back, `layoutImages.ts:21`'s `pending` never clears, and
  layout images stay missing for the rest of the session. A forced refresh during an in-flight
  request is also dropped.

## 7. Feature improvements

In rough order of value:

1. **Dirty indicator and close confirmation.** Compare against a snapshot taken at open, show
   a dot in the title, and confirm on Cancel, Escape and backdrop clicks (or drop the backdrop
   close).
2. **Resizable controls.** `LayoutControl.size` exists and the renderer honours it, but the
   designer has no W/H fields or handles except on rectangles. Skeuomorphic layouts need knob
   diameter and slider length. Add vertical sliders and a *Dropdown* type for enums.
3. **Selection tools:** multi-select (shift-click or marquee), align and distribute, duplicate
   (Ctrl+D; only text labels copy/paste today), and smart guides. Add layer reordering: the
   `canvasProperties.ts` header promises reordering, but there is none.
4. **Parameter coverage:** flag `paramKey`s that no longer exist when a layout opens or is
   imported. Warn when an import's `effectType` differs (`layoutStore.ts:461` silently
   overrides it). At runtime, offer an "N more controls" disclosure for parameters the layout
   doesn't place; today they are reachable only through the *Standard* half.
5. **Safer import:** give every import a fresh id, validate the shape, and defer the image
   writes until Save (and see 1.1).
6. **Phones:** `layoutOverlay.ts:51-58` scales a 1024 px canvas down to about 0.33× with no
   floor, giving 16 px knobs. Below about 0.6× in compact density, fall back to the standard
   controls or allow horizontal scroll.
7. **Live colour preview:** use `input` rather than `change` on the label, fill and border
   colour pickers.
8. **Make *Add Control* do nothing in backdrop mode.** Today it is only dimmed, and it still
   adds controls that never render.
9. **More faithful thumbnails:** the thumbnail ignores gradients (they come out blank `#1c1c1c`),
   size modes and offsets, and is drawn from images that may not have loaded yet.
10. **Consistent snapping:** arrow keys nudge by 1 px, but the X/Y fields snap to 8. Drag
    clamping assumes every control is 60 px wide, so a 180 px resource dropdown can overhang
    the right edge.
11. **Keyword rules:** match on word boundaries. `includes` makes "amp" match every effect name.
12. **Index storage:** move the layout index into the SQLite store (`ItemType::kEffectLayout`
    exists but is unused) or rebuild it from the documents. On a parse error, move the bad file
    aside rather than write an empty index over it, and add a shared-sync domain so other
    editors pick up layout changes.

## 8. Tests and docs

- **Tests.** The only UI test is `layoutPreferences.test.ts`, and C++ has none for
  save/delete/fork/import. Worth adding:
  - traversal `fileName` rejection
  - upper-case extension reload
  - delete while another layout borrows the image
  - a dangling default and a dangling rule
  - the rule re-point after save
  - background layer add after delete
  - undo after a sidebar delete
  - layout toggle binding
  - overlay visibility after a bypass
  - picker positioning under zoom
- **Migration test mismatch.** `StorageMigrationTests.cpp:81-82` writes `{"id": …}`, but real
  documents carry `layoutId`, so `ImportLayouts` passes only through its folder-name fallback.
- **Stale docs.**
  - `docs/features.md:636-639` describes `layouts/content/<id>.layout.json` and
    `layouts/images/`. Documents are now in SQLite, and images are in `layouts/content/<id>/images/`.
  - `features.md:611` links a `settings-storage-v1.md` that doesn't exist.
  - `docs/user-interface.md:112-113` lists 2 of the 10 layout messages.

## Suggested order

1. **Path traversal (1.1)**, then escape the designer's text interpolations (1.2).
2. **Data loss:** the unsaved-changes guard (2.1), the layer index (2.2), Reset keeping its
   identity (2.3), and save/delete failure reporting (2.6).
3. **Re-point rules on save** and skip dangling rules (§3). This is the "I saved it and nothing
   changed" report waiting to happen.
4. **Undo fixes (§4).** They are small and mechanical.
5. **Runtime parity:** toggles, resource selectors, left/right labels, the hotspot z-order and
   overlay visibility (§5).
6. **Image storage:** always copy referenced images, extension case, and serving images instead
   of inlining them (2.4, 2.5, §6).
