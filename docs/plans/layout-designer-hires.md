# Layout Designer: high-resolution art and scale-to-fit

Status: plan, nothing built. Written 2026-10-23 against `da542757`.

Goal: custom effect layouts can use very large, high-resolution images for
backgrounds, knobs and other art, and the whole layout scales to whatever space
the panel has, up as well as down, staying sharp on high-DPI screens.

**Priority constraint: every layout made before this work keeps loading and
rendering exactly as it does now.** That covers user layouts, the factory
layout, exported `.sgfxlayout.zip` files and the preference rules that point at
layouts. See [Compatibility](#compatibility-with-existing-layouts) before
changing anything below. Every item in this plan is written to that rule.

## Where things stand

| Area | Today | Problem for high-res art |
|---|---|---|
| Image transport | `BuildLayoutImages()` (`PluginControllerLayouts.cpp:641`) reads **every** image of every user and factory layout, base64-encodes it and sends the lot in one `layoutImagesLoaded` message. Browse (`HandleBrowseLayoutImageRequest`) copies the file, then reads it back and sends it as a data URL too. | A 12 MB PNG becomes a 16 MB string, gets copied through `json::dump`, WebView2 `postMessage`, `JSON.parse` and `uiState`. Ten such images means hundreds of MB of strings in the UI process before anything is drawn. |
| Runtime render | `renderBackgrounds()` (`layoutRenderer.ts:372`) writes the data URL into an inline `style` string, which goes through `escapeHtml` and `innerHTML` on **every** panel render. The custom knob face does the same. | Each re-render (bypass toggle, panel redraw) re-serialises and re-parses the multi-MB string. |
| Size cap | `kMaxLayoutImageBytes` = 16 MB (`LayoutSupport.h:50`). | Rules out large lossless art. The cap exists because bytes cross the bridge. |
| `layout-images/` URL | Both resolvers fall back to `layout-images/<fileName>`, and `layoutImageCssUrl` already allows it. | No route serves it: `PluginEditor::getResource` only maps into `resources/ui/`, so the fallback 404s. It is a half-built version of the fix. |
| Canvas size | `LAYOUT_DIMENSION_LIMITS` max 1024 × 768. `sanitizeLayout` clamps to it on import. | A layout authored at 2048 × 1024 is silently clamped, leaving controls off-canvas. |
| Runtime scaling | `applyCustomLayoutScaling` (`layoutOverlay.ts:25`) only scales **down** and only by **width**. Wider panel = native size, centred. | It can't fill a big panel, and it ignores height, so a tall layout overflows a short panel. |
| Knobs | `.knob` is a fixed 38 × 38 px (`controls.css`). `control.size` only sets the wrapper width. The designer has no size field or resize handles for controls. A custom knob image is one picture, rotated, clipped to a circle with `inset: 4px`. | A 512 px knob render is drawn at 30 px. Rotating a lit render spins its highlights and shadow. Drop shadows are clipped. |
| Control text | Label 10 px, value 9 px, label `max-width: 80px`, all hard-coded in the renderer and CSS. | On a 2048-wide design scaled to fit, labels become unreadable. |
| Export | `layoutStore.ts` builds the zip from `img.dataUrl`. Import sends each image back as base64 (`HandleSaveLayoutImageRequest`). | Depends on every image being held in memory as a data URL. |

Things that are already fine: the UI CSP allows `'self'` images and fonts, so a
same-origin image route needs no CSP change. Same-origin images don't taint
the thumbnail canvas. The native Nano UI does not draw custom layouts, so this
is WebView-only work.

## Compatibility with existing layouts

### What exists today

| Data | Where | Shape |
|---|---|---|
| User layouts | The store (`storage::ItemType::kLayout`), indexed by `layouts/indexes/effect-layouts.json` | `version: 1`, the `EffectLayout` fields in `layoutTypes.ts`, often with an embedded `thumbnailDataUrl` |
| User images | `layouts/content/<layoutId>/images/<imageId>.<ext>` (file stem = image id) | Any of PNG, JPEG, GIF, WebP, SVG |
| Legacy images | `layouts/content/images/`, from before every layout had an id | Same naming |
| Borrowed images | A layout may reference an image id that lives in **another** layout's folder or the legacy folder. Ids are global, not per layout. | Resolved by id across all folders |
| Factory layouts | `ui/assets/layouts/<folder>/layout.json` (`chaos-engine` today) | The archive shape below. Image file names differ from ids, so the manifest maps them. Uses `useDefaultControls` and a `size: "custom"` background |
| Exported archives | `.sgfxlayout.zip` | `layout.json` = `{ formatVersion: 1, createdAt, layout, images: [{ imageId, fileName, type }] }` plus `images/<fileName>` |
| Preference rules and per-instance references | Point at a `layoutId` | Ids must never change |
| `control.size` | Already in saved layouts | The designer draws it as width and height. The runtime applies only the width, to the control's wrapper |

What older apps do with newer data. This matters for sharing an archive with
someone on 1.6.0, and for a user who downgrades:

- 1.6.0's importer runs `sanitizeLayout`. That clamps `dimensions` to
  1024 × 768, snaps controls and overlays to the 8 px grid, and keeps unknown
  fields. It never checks `version` or `formatVersion`.
- It reads only `images/<fileName>` entries named in the manifest, and turns
  each into a data URL.
- Its folder scans (`directory_iterator`) are not recursive.

UI↔engine messages (`layoutImagesLoaded`, `layoutImageSelected` and so on) are
not a compatibility surface: the UI and the engine always ship in the same build.
They can change freely. Only stored data and archives need compatibility.

### Rules

1. **Read old, never rewrite on load.** Migration is in memory only:
   normalise on read (as `normalizeBackgroundLayers` already does), and
   write back only when the user saves an edit. Never rename, re-encode,
   move or delete an existing image file or layout id.
2. **Absent means today.** Every new field is optional, and its absence gives
   exactly the current rendering:
   - no `scaling` gives fit-width, no upscale, centred
   - no knob diameter gives 38 px
   - no text sizes gives 10 px labels, 9 px values and the 80 px label cap
   - no `gridSize` gives 8 px snapping in `sanitizeLayout`
   - no `knobImage` gives today's `knobStyle` / `knobImageId` rotate
3. **Never reinterpret an existing field.** In particular `control.size` keeps
   its current meaning (wrapper width at runtime). Knob diameter, slider length
   and so on go in **new** fields. Applying `size` to the knob itself would
   resize controls in layouts that already set it.
4. **Same id, same image.** Ids are shared by every layout, and every reader
   resolves one with `CollectLayoutImageSources` (`LayoutSupport.h`). Settled
   in phase 0:
   - When folders clash on an id, the later one wins, in this order: the legacy
     shared folder, then each indexed layout's folder in index order, then the
     factory layouts.
   - That is the order the UI has always **shown**. Before phase 0, a save's
     copy step let a user layout's copy win over the factory's instead, so it
     could copy a different file from the one on screen. It now copies the
     shown file.
   - Layout folders the index doesn't list (a layout never saved, or one a
     delete left behind) are never shown, but the save-copy step still reads
     them, at the lowest priority, so they only supply ids found nowhere else.
   - Pinned by `TestSharedImageIdsResolveAsTheLibraryShowsThem` in
     `core/tests/LayoutStorageWorkflowTests.cpp`. The new image route uses the
     same function.
5. **New files go where old code doesn't look.** Renditions go in one cache
   folder, `layouts/renditions/`, outside every layout's own folder (built in
   phase 3; the first draft put them in each layout's `images/renditions/`).
   No build, old or new, lists that folder as layout images; factory images
   can have copies too; and losing a copy only costs making it again. Copies
   are optional: a missing copy falls back to the original. Old layouts get
   copies lazily, the first time a large image is drawn, not by a bulk
   migration. Only images longer than 2048 px get copies at all, so every
   smaller image is still drawn from its own file, pixel for pixel.
6. **Data URLs keep working.** Anything carrying a `dataUrl` (old library
   entries, old `imageAssets`) still renders. The renderer keeps accepting
   `data:image/` URLs alongside the new route.
7. **`version` means the oldest schema that can render the layout.** A layout
   saved by the new app stays `version: 1` unless it uses a v2 feature. Then
   it is `2`, plus a `requires: ["filmstrip", "upscale", ...]` list, so a later
   app can say what it can't show rather than guessing.
8. **Degrade gracefully in older apps**, since they can't be changed:
   - Filmstrip knobs: write `knobImage` only, and leave `knobStyle` as a
     built-in style. 1.6.0 then shows a standard knob rather than a whole
     filmstrip squashed into a circle.
   - Layered knobs: also write the rotating cap as `knobImageId` with
     `knobStyle: "custom"`, so 1.6.0 shows the cap rotating.
   - Canvases above 1024 × 768: older importers clamp them. On export, the
     designer warns that the layout needs the new version.
9. **Archives stay `formatVersion: 1`-readable.**
   - Keep `layout.json` and the `images/<fileName>` entries old importers read.
     For an image over 16 MB or 4096 px, put a down-sized copy at
     `images/<fileName>` and the original at `originals/<fileName>`, with an
     `original` field on its manifest entry. New importers prefer the
     original. Old ones get a working image.
   - Never export renditions. The importer regenerates them.
   - New importers keep accepting every older archive, including ones whose
     layouts carry `imageAssets` with embedded data URLs.

### Proving it

Built in phase 0:

- **UI golden render test**, `core/ui/tests/layoutCompat.test.ts`. Fixtures are
  in `core/ui/tests/fixtures/layout-compat/`, and snapshots in
  `core/ui/tests/__snapshots__/layout-compat/`.
  - Each fixture is rendered enabled and bypassed, through
    `renderCustomLayout` or `renderCustomLayoutBackdrop` as the params panel
    would.
  - Snapshots are in a canonical form: attributes sorted, each style's
    declarations kept in order, whitespace collapsed. Layout image URLs are
    written as the image id they resolve, so serving images by route leaves
    them unchanged. Reformatting a template doesn't change a snapshot.
    Changing what a layout draws does.
  - Fixtures:
    - the factory `chaos-engine` layout (read from `assets/`)
    - a 1.6.0-format archive, rendered as stored and after
      `prepareLayoutImport`, with the import result recorded
    - controls with `control.size`, every label position, hidden labels and
      values, each knob style, and a parameter the effect no longer has
    - six background layers, covering every size mode, offsets, opacity and a
      missing image
    - a layout whose images exist only in `imageAssets` data URLs
    - a backdrop with offset, uneven scale, light theme and default text
      colours
    - model and IR resource selectors, including a slot the effect lacks
  - The same file pins `applyCustomLayoutScaling`: native size and centred at
    or above the design width, never larger, and a uniform shrink by width
    below it.
  - The 1.6.0 archive is built to the shape 1.6.0's exporter writes
    (`git show 1.6.0:core/ui/ts/layoutDesigner/layoutStore.ts`), using only
    fields 1.6.0 had. It is not a file exported by a real 1.6.0 build.
- **Engine image precedence**, `TestSharedImageIdsResolveAsTheLibraryShowsThem`.
  It covers legacy, indexed, unindexed and factory folders clashing on one id,
  what the library lists, and what a save copies. Borrowed and legacy images
  were already covered by `TestBorrowedImagesSurviveDeletingTheirLayout` and
  `TestLoaderListsLegacyAndUpperCaseImages`.

Behaviours the snapshots pin that look accidental but must be kept:

- `size: "tile"` writes `background-size: tile`, which is invalid CSS. The
  browser drops it and tiles at natural size, which is the intended look.
  Phase 3 must keep that look, not "fix" it into something else.
- Background layers with `layerIndex` 2 and up get that z-index, so layers 4
  and 5 paint above the controls (z 2) and labels (z 3). The designer only makes
  layers 0 and 1, but stored or imported layouts can hold more.

- **Live baseline**, `tools/agent-ui-debug/layout-compat-baseline.mjs`, with
  57 screenshots and a manifest in `docs/plans/layout-designer-hires-baseline/`.
  They were taken 2026-10-23 on `0a0f7435`, in WebView2 155.
  - Each fixture is drawn by the running app's own renderer, `GenericKnob`
    and `applyCustomLayoutScaling`, inside the params panel's shell
    wrappers, in a temporary overlay. Nothing touches the profile's layouts.
  - Shots: panel widths 360, 520, 760 and 1200 at 1×, and 360 and 1200 at
    2×. Bypassed versions where overlays change. Each background layer of
    `background-image-modes` is also shot on its own, because the cover layer
    hides the others in the full stack.
  - Images are drawn by the script (labelled grids, pointer knobs, an SVG
    logo); the factory layout uses its real image. So the baseline covers
    rendering, CSS and scaling, not how the engine finds files on disk.
  - Repeatability: two full re-runs matched exactly (0 of 57 differ). Each
    shot is clipped to the layout's own box, moved onto whole pixels. A shot
    is kept only once two screenshots in a row agree, and `--compare` retakes
    a mismatch twice before reporting it. Without these, the shell's 1 px
    highlight, a fractional clip edge and raster tile seams each varied by a
    few levels between runs.
  - Run it against an app copy launched with:
    `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9333
    --disable-features=CalculateNativeWinOcclusion
    --disable-backgrounding-occluded-windows --disable-renderer-backgrounding
    --disable-background-timer-throttling"`.
    Without those switches, a covered or minimised window stops painting, and
    the run hangs or shoots stale frames. The script refuses to run while the
    page reports itself hidden.
  - To check a later phase:
    `node tools/agent-ui-debug/layout-compat-baseline.mjs --port 9333 --out <scratch dir> --compare docs/plans/layout-designer-hires-baseline`.
    Exit code 1 and a `.diff.png` per changed shot mean something an old
    layout draws has changed.

Still to do (each later phase):

- **Round trip with real builds**: export from the new app, import into a 1.6.0
  build. It must load, with the degradations in rule 8 and nothing worse.
  Import a real 1.6.0 export into the new app: it must be unchanged. A real
  1.6.0 export should also replace or join the hand-built archive fixture.

## Changes needed

### 1. Serve layout images by URL, not by message (prerequisite)

This is the change that makes large images workable at all. Everything else
builds on it.

- Add a route to `PluginEditor::getResource`, keyed by **image id**:
  `layout-images/id/<imageId>`. The editor asks the controller to resolve it
  with `CollectLayoutImageSources` (compatibility rule 4), the same lookup the
  library uses. It must not look in the requesting layout's folder first,
  which would differ from what users have always seen when ids clash. Old
  layouts that borrow images resolve to the same files as today. The lookup
  only ever returns files inside a layout's `images/` folder or a factory
  manifest's. Cache it, since it lists folders.
- `layoutImagesLoaded` and `layoutImageSelected` carry metadata only:
  `{ imageId, layoutId, fileName, url, width, height, bytes }`. Read width and
  height from the file header (PNG IHDR, JPEG SOF, WebP VP8/VP8L/VP8X, GIF
  LSD, SVG `viewBox`), with no decode. The designer needs them for "fit canvas
  to image", filmstrip frame maths and oversize warnings.
- `getLayoutImageUrl` in both `layoutRenderer.ts` and `layoutDesigner/images.ts`
  returns the URL. Fold the two copies into one module while doing it.
  `LayoutImageRef.dataUrl` stays optional for old imports only.
- Export: either fetch each URL as a blob in the UI and add it to the zip, or
  have the engine write the zip from disk. The engine option is better because
  bytes never cross the bridge. Import: the engine already unpacks and
  validates. Make it write image files straight from the archive rather than
  having the UI send base64 back through `HandleSaveLayoutImageRequest`.
- Once nothing crosses the bridge, raise `kMaxLayoutImageBytes` for originals
  (64 MB, say). The real limit becomes decoded pixels, covered in §3.
- Things to check: which thread JUCE calls the resource provider on for each
  backend, since a 50 MB synchronous read on the message thread would stall it.
  Whether WebView2 caches custom-scheme responses, or re-requests on each
  `innerHTML` render. If it re-requests, add an in-process LRU of recently
  served bytes, or switch to item 4 below.

**Built (phase 1).**

- **Route.** `PluginEditor::getResource` serves `layout-images/id/<id>` through
  `PluginController::ResolveLayoutImageResource`.
  - The id is percent-encoded by `LayoutImageUrl`. `LayoutImageIdFromResourcePath`
    decodes it and refuses a separator, `.`, `..` or a bad escape.
  - Resolution uses `CollectLayoutImageSources`, with unindexed folders at the
    lowest priority, so an image just picked for an unsaved layout shows at
    once. Indexed ids resolve exactly as the library lists them.
  - The id-to-file map is cached behind a mutex. It's rebuilt after
    `LoadLayoutLibrary` (which every save, delete and import ends in), after
    an image is written, and once on a miss.
- **Messages.** The message shapes are unchanged, but entries carry `url`,
  `width`, `height` and `bytes` instead of `dataUrl`. `ReadLayoutImageSize`
  reads PNG, GIF, JPEG (walking markers past a large EXIF block), all three
  WebP kinds and SVG (attributes, else `viewBox`). The URL carries
  `?v=<mtime>`, so a file replaced under the same id is fetched again.
- **UI.**
  - `resolveLayoutImageUrl` in `layoutImages.ts` is the one lookup for the
    renderer and the designer. A data URL still wins where there is one: an
    image imported this session, or an older entry.
  - Export fetches each image's URL into the zip. The archive format is
    unchanged.
  - The thumbnail draws from the same URL.
  - The dead `layout-images/<fileName>` fallback is gone. It was never served,
    so nothing that drew before draws differently.
- **Limit.** `kMaxLayoutImageBytes` and the UI's `MAX_LAYOUT_IMAGE_BYTES` are
  now 64 MB. Import still sends each image once as base64; a 20 MB one is
  covered by `TestLargeImportedImagesAreStored`.
- **Compatibility.** `layoutCompat.test.ts` renders every fixture a second
  time with its images listed by URL only, as the engine now sends them, and
  requires the same snapshot. They match.

### 2. Logical canvas and scale-to-fit

- Treat `dimensions` as a logical design size, separate from image pixel size.
  Raise `LAYOUT_DIMENSION_LIMITS` to around 4096 × 4096 in the designer **and**
  in `sanitizeLayout`. Raising a maximum changes nothing for layouts already
  inside the old limits. Keep the minimums and the rounding as they are.
- Add a `scaling` block to `EffectLayout`:
  ```ts
  scaling?: {
    mode: "fit-width" | "fit" | "fixed";   // default "fit-width" = today's behaviour
    minScale?: number;                      // e.g. 0.25
    maxScale?: number;                      // default 1 = today's behaviour (no upscale)
    align?: "start" | "center";
  };
  ```
  In `applyCustomLayoutScaling`, `fit` uses
  `min(outerW / designW, availableH / designH)`, clamped to min/max. Allow scale
  above 1 when `maxScale > 1`. Available height comes from the params panel's
  own box, with a `ResizeObserver` on that too, not only the outer width.
- Keep `transform: scale()` on the container. Chromium re-rasterises
  transformed content at the final scale, so text and SVG stay sharp. **Never**
  add `will-change: transform` there: that rasterises once at 1× and stretches
  the bitmap, which goes blurry when scaled up.
- Pointer maths: knob drags and the designer already divide by zoom. Check the
  runtime knob drag under a scale above 1, and that body-level popovers still
  divide by `getUiZoom` (see the body-level popover note).
- Grid: keep 8 px as the default, but make it a per-layout setting, and let Alt
  turn snapping off while dragging. On a 2048-wide design, 8 px steps are fine.
  On a 400-wide one, people want 4 px or 2 px.

**Built (phase 2).**

- **Scaling maths.** `layoutScaling.ts` holds `computeLayoutScale`, the pure
  scale and placement rule. With no `scaling` block it reproduces the
  original rule exactly. `applyCustomLayoutScaling` uses it. Fit mode
  measures down to the bottom of the nearest scrolling ancestor (in the app,
  `.visualization-panel`) and watches it for resizes.
- **Two things the live check found**, both handled only for layouts with a
  `scaling` block:
  - The panel and its custom-layout section are `fit-content`, so a layout
    never had room to grow. The renderer marks a layout whose scaling lets it
    grow (`maxScale > 1`, not fixed) with `data-layout-grows`, and
    `params-panel.css` widens the panel and section to the stage width with
    `:has()` only then.
  - `.custom-layout-container` pins `margin: 0 !important`, so the original
    "margin: auto" centring was always a no-op. Centring at native size comes
    from the `fit-content` section. A scaled layout that should sit in the
    middle is moved there by the transform (`translateX(…) scale(…)`).
- **Text.** `controlLabelFontSize`, `controlValueFontSize`,
  `controlFontFamily` and `controlLabelMaxWidth` on the layout, plus
  `style.labelFontSize` and `style.valueFontSize` per control. Unset, the
  renderer writes the original style strings byte for byte.
- **Grid and canvas.** `gridSize` sets the designer's snap step, the drawn
  grid, the Shift-arrow nudge and the paste offset. Alt drags without
  snapping, and `sanitizeLayout` snaps imports to the same grid. The canvas
  limit is 4096 × 4096 in the designer, its inputs and `sanitizeLayout`.
- **Version stamping.** `layoutRequirements.ts` stamps `version: 2` and
  `requires` (`large-canvas`, `scaling`, `text-style`) on save only when one
  is used, and stamps `version: 1` with no list otherwise.
- **Designer.** A "Fit to Panel" group, a grid size and the text defaults on
  the canvas panel, and label and value sizes on each control. An emptied
  field removes the setting. The image picker lists each image's pixel size.
- **Checks.**
  - The compatibility snapshots are unchanged, and the live baseline shows
    0 of 57 shots changed.
  - In the real params panel, a 400 × 280 user layout given `maxScale: 2`
    filled the 1,585 px stage at 2×, centred. Given fit mode, it drew at
    1.66× within the panel's 640 px height. With no block, it was unchanged.

### 3. Resolution-matched renditions

A 6000 × 4000 background on a 600 px panel decodes to about 96 MB of RGBA and is
resampled on every paint. On the S21 that is the difference between working and
not working.

- On import or browse, generate downscaled copies of the original: longest edge
  1024, 2048 and 4096, only where smaller than the source. Store them as
  `images/renditions/<imageId>@1024.webp` and so on: a subfolder, never next to
  the original (compatibility rule 5). The original is never modified, and is
  kept for re-export and future re-renditions. Skip SVG. Existing images get
  renditions lazily, the first time they are drawn.
- Where to generate them: the core has no image codec and JUCE's is only on the
  editor side. The UI can do it everywhere it runs, with
  `createImageBitmap(blob, { resizeWidth, resizeQuality: "high" })` then
  `OffscreenCanvas.convertToBlob({ type: "image/webp", quality: 0.92 })`.
  That's one bounded transfer per rendition at import time, which is acceptable.
  PNG output for art with hard alpha edges, if WebP shows fringing.
- At render time, pick the smallest rendition at least
  `designPx × effectiveScale × devicePixelRatio` for the element's drawn size,
  and swap it on scale changes (debounced). Backgrounds and knob faces both go
  through one `resolveLayoutImage(imageId, drawnCssPx)` helper.
- Preload before showing: when a panel opens, `new Image()` plus `decode()` the
  chosen renditions, then swap the panel in. That avoids the blank-then-pop of a
  large CSS background decoding on first paint.
- Designer warnings: flag an image larger than about 2× its largest drawn size
  at `maxScale`, which only wastes memory. Flag a filmstrip taller than 16384 px,
  which goes over GPU texture limits.

**Built (phase 3).**

- **Which images get copies.** Only images longer than 2048 px on their
  longest edge (`RENDITION_MIN_SOURCE_EDGE`). They get copies at 1024, 2048
  and 4096 px, each only when smaller than the original. Every smaller image
  is drawn from its own file, as before. All 20 images in the real profile
  are ≤ 1774 px, so none of them changes.
- **Making them.** The WebView makes the copies, since the engine has no image
  codecs (`layoutRenditionMaker.ts`): `createImageBitmap` with
  `resizeQuality: "high"`, then WebP at quality 0.92, once per file version
  per session, the first time a large image is drawn.
- **Storing and serving them.** The engine's
  `HandleSaveLayoutImageRenditionRequest` stores a copy in
  `layouts/renditions/` only if all of these hold:
  - its version matches the source file's current write time
  - its edge is one of the kept sizes, and smaller than the source
  - it is WebP, and its header declares exactly that edge
  
  Saving a copy removes copies of older versions at that size. The image list
  carries `version` (a string, past 2^53) and `renditions`. The route
  `layout-images/rendition/<id>/<edge>` serves a copy only while it matches
  the current file.
- **Choosing one.** The renderer passes how wide each image is drawn at the
  layout's largest: background size mode × `maxScale`, or the 30 px knob
  face. `resolveLayoutImageUrl` then picks the smallest copy with a pixel for
  every device pixel at the window's `devicePixelRatio`.
- **Live check** with a 6000 × 4000 test image:
  - The 1024, 2048 and 4096 copies were made and stored in 2.3 s, and served
    as WebP at their listed sizes.
  - The 1024 copy decodes in 17 ms against 77 ms for the original.
  - Against the browser's own downscale of the original, the mean difference
    is 5.1 levels in 255, with no pixel more than 24 apart.
  - A 600 × 400 cover background drew from the 2048 copy at the window's
    1.75 DPR.
- **Baseline: the WebView2 runtime updated from 155 to 156** between the
  phase 2 and phase 3 checks.
  - On 156, one shot (`resource-selector--w360--1x`) differs from the
    committed 155 baseline, by 6 pixels on the anti-aliasing of the app's own
    folder icon.
  - The phase 0 build shot on 156 matches phase 3 on that shot, so the
    runtime caused it, not the code.
  - Comparing two separate launches on 156 also showed a sub-pixel shift in
    one scaled background image (`backdrop-offset-scale--w1200`). Its markup
    snapshot is identical and phase 3 matched the committed baseline there,
    so that is run-to-run variance too.
  - Compare against the committed baseline. When a shot differs, check it
    against the phase 0 build on the same runtime before calling it a
    regression.
- **Not done.** Preloading before a panel swap. The designer still draws
  originals: it's the authoring view, one layout at a time. There are no
  oversize warnings yet.

### 4. Controls that suit high-res art

- **Control size.** Add **new** fields (e.g. `style.knobDiameter`,
  `style.sliderLength`, `style.toggleSize`) rather than reusing `control.size`,
  which old layouts already set with a different meaning (compatibility
  rule 3). Absent means today's 38 px knob and 80 px slider. Add fields and
  corner handles to the designer, which overlays already have and controls
  don't. Scale the indicator, ring width and tick from the diameter rather than
  fixed px.
- **Filmstrip knobs**, the standard plugin-skin format:
  ```ts
  knobImage?: {
    imageId: string;
    mode: "rotate" | "filmstrip";
    frames?: number;                 // filmstrip
    columns?: number;                // sprite sheet, to stay under texture limits
    minAngle?: number; maxAngle?: number;  // rotate; default -135..135
  };
  ```
  `knob.ts` already sets `--knob-pct`. A filmstrip picks
  `frame = round(pct × (frames − 1))` and sets `background-position`. That is a
  repaint of one small layer, cheaper than a rotate.
- **Layered knobs**: a static base (skirt, shadow), a rotating cap, an optional
  static glare on top. The lighting stays put while the cap turns. This is the
  `rotate` mode with up to three image ids.
- Make the circle clip and `inset: 4px` optional for image knobs, so shadows
  and non-round knobs work. They stay **on** by default, so existing custom
  knobs are drawn exactly as now.
- **Image toggles and switches**: two frames, or a two-frame strip, for
  footswitches, toggles and LEDs. **Image sliders**: separate track and thumb
  images.
- **Per-layout text defaults**: `controlLabelFontSize`, `controlValueFontSize`
  and font family, with per-control overrides, the same pattern as the existing
  `controlLabelColor`. Replace the hard-coded 10 px / 9 px and the
  `max-width: 80px` cap with the control's own width.

**Built (phase 4).**

- **Sizes.** `style.knobDiameter`, `sliderLength`, `toggleWidth` and
  `toggleHeight`. A styled knob at another diameter is zoomed from its 38 px
  design, so its ring and pointer keep their proportions. An image knob is
  drawn at the diameter directly.
- **Image knobs.** `style.knobImage` (`layoutKnobImages.ts`) with `mode`
  either `"rotate"` or `"filmstrip"`.
  - Filmstrip: `frames`, and `columns` for a sheet.
  - Rotate: `minAngle` and `maxAngle`, and `clip` (default on, as custom faces
    always were).
  - Either mode: a still `baseImageId` and `glareImageId`.
  - `knob.ts` moves a filmstrip to the frame for the value, or turns a face
    over its range. A legacy custom face still turns −135° to 135°.
  - The layout's own `knobImage` (canvas panel, "Knobs") is the default for
    every knob without one: the default controls of a `useDefaultControls`
    layout, drawn at 38 px (`applyKnobImageToKnobs` rewrites the params
    panel's knob markup), and each positioned knob whose `knobStyle` is
    `"default"` or unset (`withLayoutKnobImage`). Pedal, Amp, Minimal or a
    custom face keeps the knob's own look. It counts as `knob-image`, and
    older apps, with no fallback written, draw standard knobs.
- **Image toggles and sliders.** A toggle gets two pictures swapped by
  `:checked`. A slider's track and thumb come through custom properties
  holding absolute URLs, since a relative `url()` in a custom property
  resolves against the stylesheet.
- **Image elements** are rectangle overlays with `style.imageId` and
  `imageFit`, so they inherit the overlay's drag, resize and bypass
  visibility in the designer. The first image a rectangle gets clears its
  fill and border, so older apps, which ignore the image, draw nothing there.
- **Fallbacks for older apps,** written on save (`stampLayoutRequirements`):
  - A turning face is also written as `knobStyle: "custom"` plus
    `knobImageId`, so 1.6.0 shows the cap turning.
  - A filmstrip drops any custom knob style, so 1.6.0 shows a standard knob
    rather than a squashed strip.
  - New `requires` entries: `control-size`, `knob-image`, `image-toggle`,
    `image-slider`, `image-overlay` and (later) `control-shadow`.
- **Image references.** Every new reference is listed by `layoutImageIds`
  (export), renamed by `renameLayoutImageIds` (import), and collected by the
  engine's `CollectLayoutImageIds` (the copy on save; pinned by
  `TestImageControlReferencesAreCopiedOnSave`). A layout keeps all its images
  when the layout they came from is deleted.
- **Designer.** An "Image & Size" group per control (knob, toggle or slider
  fields) and an Image group on rectangles, in
  `layoutDesigner/controlImageProperties.ts`.
- **Live check.**
  - The filmstrip stepped frames with fixed lighting, at 0, half and full
    travel.
  - The layered knob turned its cap under a still tick ring and glare.
  - The toggle swapped LEDs, and the slider and logo overlay drew.
  - A real 150 px mouse drag moved the filmstrip to its last frame.
  - Old layouts are unchanged: snapshots identical, and the live baseline
    differs only by the known runtime icon shot.
- **Not done.** The designer's edit-mode placeholders don't draw image knobs,
  toggles or sliders; preview mode does, through the real renderer. There are
  no oversize warnings for a filmstrip taller than 16384 px.

## Further additions to make it more useful

In rough order of value:

1. **Image elements.** Positioned, sized images (logo, screws, panel graphics,
   LED glows) with z-order and the same `visibilityMode` the rectangle overlays
   have (always / enabled / bypassed). That gives a proper bypass LED and lit
   footswitch, and lifts the two-background-layer limit for decoration.
2. **Asset panel.** Every image the layout uses, with pixel size, file size,
   decoded memory and which elements use it. Replace an image while keeping its
   references, remove unused images, and see the oversize warnings from §3.
3. **Preview at size.** Designer toolbar presets for panel widths and heights,
   1× / 2× DPR and the compact layout, showing the effective scale and which
   rendition would load. That's what users actually see, rather than only
   zoom.
4. **Arrangement tools.** Multi-select, align and distribute, arrow-key nudge
   (Shift for a grid step), snap to other elements' edges and centres, rulers
   with draggable guides, lock and hide per element, and a layers list.
5. **Custom fonts.** WOFF2 files in the layout package, served by the same
   route and registered with `FontFace`. The CSP's `font-src 'self'` already
   allows it.
6. **Size variants**, maybe later. An optional compact variant (a second set of
   control positions, or a second layout) chosen when the panel is below a width
   the layout sets. That beats scaling a wide pedal down to illegibility in the
   compact layout.
7. **Schema version 2**, as compatibility rule 7 describes. `version: 2` and
   a `requires` list only when a layout uses a v2 feature. v1 is read as is,
   with no stored migration. Archives follow rule 9.

**Built (phase 5).** Each item is in its own module, since
`layoutDesigner.ts` is pinned at its size, and hooks into the property
panels:

- **Images in this Layout** (`layoutDesigner/layoutAssets.ts`), on the canvas
  panel.
  - Each image the layout draws on, with a thumbnail, pixel size, file size,
    memory when decoded, smaller copies, and every use ("Background 1",
    "Drive: filmstrip", "Rectangle image"). Uses come from
    `visitLayoutImageRefs`, which export, import and the engine also follow.
  - Warnings for a decode over 64 MB, a filmstrip taller than 16,384 px, and
    an image missing from the library.
  - Replace swaps an image everywhere the layout uses it, in one undo step.
- **Image strips** (`layoutKnobImages.ts`). Strips as knob renderers export
  them, one image of every frame:
  - A filmstrip knob counts its own frames and columns when its picture is a
    strip of square frames, in a column or a row (`squareFrameStrip`).
  - A toggle can take `toggleImage.strip` (`{imageId, frames, columns?,
    aspect?}`): off is the first frame, on the last, at the frame's shape.
  - A slider can take `sliderImage.strip`: the frame for its value, the range
    input over it unseen; a tall frame runs upwards (`writing-mode`). One
    document `input` listener moves the frame for drags, the wheel and keys.
  - Switch and slider frames are seldom square, so their count is set by
    hand (Strip Frames); the picture gives the layout and frame shape.
- **The layout's switch** (`layout.switchImage`, `layoutDesigner/switchProperties.ts`).
  As the layout's knob image is for knobs: an off and an on picture, or a
  strip, which wins, with an optional width and height (default 36 wide at
  the off picture's or a frame's shape). Every toggle without images of its
  own draws it: positioned toggles (`withLayoutSwitchImage`) and the default
  controls', whose standard switch `applySwitchImageToToggles` replaces, the
  input kept. Adds the `switch-image` requirement; older apps draw standard
  toggles. Export, import and the copy on save include its images.
- **The default controls' area** (`layout.defaultControlsArea`,
  `layoutControlsArea.ts`). A box in canvas px, its top left at
  `defaultControlsOffset`, that a `useDefaultControls` layout's controls stay
  inside, so they no longer run past the background's edges. They wrap at its
  width (the wrapper is `width / scaleX` wide), and while they are taller
  than it they shrink, Scale X and Y kept in proportion, to the largest fit
  at which they are not: an eight-step search measuring the rewrapped height,
  run once they are in the document and again on any resize
  (`fitDefaultControlsAreas`, from `applyCustomLayoutScaling`). Unset, they
  wrap at the canvas width and may run past its foot, as before. The canvas
  panel sets Width and Height under Default Controls — Position (either alone
  takes the other out to the canvas edge; Fit to Canvas mirrors the left and
  top margins), and the designer outlines the box. It counts as
  `default-controls-style`; older apps wrap at the canvas width. Nano Q does
  not read it yet.
- **Image licence and credit** (`layoutImageCredits.ts`). Each user image can
  record the licence it was shared under (CC0 and the CC 4.0 licences, own
  work, other) and a credit line, in Images in this Layout. The engine keeps
  them in `layouts/indexes/image-credits.json` (`setLayoutImageCredit`), lists
  them with the image, and an exported layout's manifest carries them back
  in on import. Nothing shows them in the params panel yet.
- **Generated control shadows** (`layoutShadows.ts`,
  `layoutDesigner/shadowProperties.ts`). A layout's `controlShadow` is one
  light for the panel: `angle` (where it comes from, 315 = top left),
  `elevation` (15-80 degrees), `softness` and `opacity`; unset casts nothing,
  as before, and it adds the `control-shadow` requirement (older apps just
  draw no shadows).
  - Every knob and toggle, positioned or default, is taken to stand as tall
    as it is wide (the smaller side) and casts its footprint away from the
    light by height / tan(elevation): a capsule for a knob, a swept pill for a
    toggle. Sliders cast none. `style.castShadow: false` turns one off.
  - The shadow is one still SVG, sized in percent of the control, inside the
    knob or toggle element at z-index -1; its parent gets
    `has-control-shadow` (isolation), so it stays above the background. The
    knob is never wrapped: the panel finds its label and value through its
    parent. Turning a knob does not redraw it.
  - It is the footprint, not the silhouette: a pointer cap casts a round
    shadow.
- **Image pickers and categories** (`layoutDesigner/imagePicker.ts`,
  `layoutImageCategories.ts`). Every image list in the designer opens as a
  dropdown of thumbnails over its hidden `<select>`, which stays the value the
  bindings read.
  - Tabs filter it: All, Knob, Background, Switch and Misc (`general`). It
    opens on the chosen image's category, else the row's (a toggle's Switch),
    else All. Your images come above the factory ones.
  - An image is filed when it is added (Browse files it under the knob or
    background it was picked for, an import under its archive's `type`), and
    dragging one onto a tab refiles it (`setLayoutImageCategory`). The engine
    keeps categories in `layouts/indexes/image-categories.json`, since image
    ids are shared by every layout. An image never filed takes its category
    from where the library's layouts use it.
  - Browsing for a picture the library already holds reuses that image
    (`FindIdenticalLayoutImage`) instead of copying it under a new id.
  - Factory images ship in `ui/assets/layout-images/<category>/`, made by
    `tools/gen-layout-images.mjs`: seven knob images (four faces, a scale
    ring, a glare and a 21-frame LED ring filmstrip), five backgrounds and
    four switch pairs. Their folder decides their category; they cannot be
    refiled.
- **Preview at panel size** (`layoutDesigner/sizePreview.ts`). The layout as
  the params panel draws it, by the real renderer and
  `applyCustomLayoutScaling`, in a Narrow 360 × 480, Laptop 760 × 520,
  Desktop 1200 × 700 or Large 1800 × 1000 panel. It reports the scale reached
  and the image copies loaded. Closing it gives the shared scaling observers
  back to the live panel.
- **Align to Canvas** (`layoutDesigner/alignTools.ts`). Left, centre, right,
  top, middle or bottom for the selected control, label or rectangle, using
  its drawn size on the designer canvas (label and value included).
- **Live check** on the real designer, opened on a copy of the profile's
  "Deluxe" layout and closed without saving:
  - the images list showed its 3 images with sizes and uses
  - the preview reported 66% at Laptop and 27% at Narrow
  - Align Centre put the Input Gain control's centre within 1 px of the
    1024 px canvas's centre
- **Not done**, each a follow-up:
  - multi-select with align and distribute across several elements (needs
    the designer's selection model, in the pinned `layoutDesigner.ts`)
  - snapping to other elements, and rulers with guides
  - a lock and hide toggle per element, and a layers list
  - custom fonts (the engine would need to serve and package font files the
    way it does images)
  - compact size variants
  - drawing image controls in the designer's edit-mode placeholders

## Suggested order

| Phase | Scope | Unblocks |
|---|---|---|
| 0 | **Done** 2026-10-23: compatibility fixtures, golden render snapshots, scaling pins and the live screenshot baseline, captured on today's code. Image-id precedence settled in one shared lookup (rule 4). | A baseline every later phase is checked against |
| 1 | **Done** 2026-10-23. Live check: the real library's 20 images all served with the listed type, bytes and size; the list is one 4.3 KB message instead of about 25 MB of base64; 0 of 57 baseline shots differ. §1: image route, metadata-only messages, file-based export and import, intrinsic size, higher byte cap | Large images load at all, and panel renders stop copying MBs |
| 2 | **Done** 2026-10-23. Old layouts unchanged (snapshots and 0 of 57 live shots). A layout that opts in grows to fill the real panel, or fits its height. §2: limits, `scaling` modes with upscale and height fit, grid setting, per-layout text sizes | Layouts fill the space they have |
| 3 | **Done** 2026-10-23. Only images over 2048 px get copies; a 6000 × 4000 image's copies were made, kept and drawn live. §3: renditions, DPR-aware selection, preload | Memory and paint cost bounded, Android viable |
| 4 | **Done** 2026-10-23. Image knobs, toggles, sliders and overlays drawn and dragged live; older apps get fallbacks. §4: control sizing and handles, filmstrip and layered knobs, image toggles and sliders, image elements | Real skeuomorphic skins |
| 5 | **Done in part** 2026-10-23: images-in-layout panel, preview at panel size, align to canvas. Multi-select, guides, fonts and variants remain. Asset panel, preview at size, arrangement tools, fonts | Editor ergonomics |

Phases 1–3 touch `PluginControllerLayouts.cpp`, `LayoutSupport`,
`PluginEditor.cpp`, `layoutRenderer.ts`, `layoutOverlay.ts`, `layoutTypes.ts`
and `layoutDesigner/*`. Phase 4 also touches `knob.ts`, `controls.css` and
`custom-layouts.css`.

Test notes: unit-test the route's path validation alongside the existing
`IsSafeLayoutImageFileName` tests, and the header-size reader on each format.
Measure before and after with the UI render-cost probe: CDP heap, plus process
CPU while toggling bypass on a layout with two 4K backgrounds. Check scale
above 1 knob drags and popovers live over CDP.
