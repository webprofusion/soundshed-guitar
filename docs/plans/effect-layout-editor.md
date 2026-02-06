# Effect Layout Editor – Feature Plan

## Overview

The Effect Layout Editor allows users to create custom visual layouts for effect parameter panels within the signal path. Users can position controls (knobs, toggles, sliders), add background images with layering, and place text labels — all via a drag-and-drop modal designer. Layouts are saved per effect type and rendered at runtime in place of the default parameter grid.

## Current State

### Working

- Designer modal opens/closes, loads existing layouts or creates defaults from param definitions
- Drag-and-drop positioning for controls and text labels with 8px grid snap
- Background images: browse via native file dialog, copied to `layouts/images/`, base64-encoded for WebView
- Background properties: size mode (cover/contain/stretch/tile/custom), scale slider, offset X/Y, opacity
- Color/gradient background option (solid color or CSS gradient via Color BG button)
- Control properties: type, position, label override, label position, hide label, label color, knob style preset, show value, custom knob image
- Text label properties: text, position, font family, font size, weight, color, alignment
- Re-add deleted controls from available parameters list (Add Control button)
- Knob style presets: pedal, amp, minimal with distinct CSS visuals
- Slider control rendering in designer (placeholder) and runtime (actual range input)
- Save to `[AppData]/SoundshedGuitar/layouts/{effectType}.layout.json`; updates UI state and refreshes signal path view
- Export as `.sgfxlayout.zip` (JSZip in UI creates zip with `layout.json` + images; C++ writes binary blob)
- Import from `.sgfxlayout.zip` (UI extracts with JSZip, registers images, loads layout)
- Undo/redo stack (Ctrl+Z/Ctrl+Y, toolbar buttons, up to 50 states, nudge debouncing)
- Zoom controls (toolbar +/−/reset buttons, keyboard +/−/0, 25%–300%, CSS transform-based)
- Keyboard shortcuts: Delete, arrow nudge (Shift for grid-step), G (grid toggle), P (preview), Escape, Ctrl+Z/Y (undo/redo), +/−/0 (zoom)
- Runtime renderer applies custom layouts in the signal path panel
- C++ backend handles save, export, import image save, browse image, and loads layout library on startup

### Key Files

| Area | File |
|------|------|
| Types | `src/resources/ui/ts/layoutTypes.ts` |
| Designer modal | `src/resources/ui/ts/layoutDesigner.ts` |
| Runtime renderer | `src/resources/ui/ts/layoutRenderer.ts` |
| Designer CSS | `src/resources/ui/css/layout-designer.css` |
| HTML (modal) | `src/resources/ui/index.html` (layout-designer-modal) |
| Message handlers | `src/resources/ui/ts/messages.ts` |
| Signal path integration | `src/resources/ui/ts/signalPath.ts` |
| C++ handlers | `src/src/GuitarFXPlugin.cpp` (HandleSaveEffectLayout, HandleExportEffectLayout, HandleBrowseLayoutImage, HandleSaveLayoutImage, LoadLayoutLibrary) |

## Known Bugs

All previously identified bugs have been resolved:

- ~~B1 – Knob positions shift in rendered layout vs designer~~ — Fixed: runtime container now exactly matches designer dimensions with explicit margin/padding reset
- ~~B2 – `stretch` background size not mapped correctly~~ — Fixed: emits `100% 100%` instead of `stretch`
- ~~B3 – Delete key does not remove selected background~~ — Fixed: `deleteSelectedElement()` now handles `type: "background"`

## Completed TODO Items

1. ✅ Fix `stretch` bg size → `100% 100%`
2. ✅ Fix Delete key for backgrounds
3. ✅ Fix knob position mismatch (B1)
4. ✅ Add re-add control button
5. ✅ Add color/gradient background option
6. ✅ Knob style CSS presets (pedal/amp/minimal)
7. ✅ Slider control rendering (designer + runtime)
8. ✅ Export as zip with images (JSZip in UI)
9. ✅ Import layout from zip
10. ✅ Undo/redo stack (50-level, debounced nudges, keyboard shortcuts)
11. ✅ Zoom controls (25%–300%, toolbar + keyboard, transform-based with scroll support)

## Data Flow

```
User clicks "Customize Layout" button on effect node
  → signalPath.ts: bindCustomizeLayoutButton()
  → layoutDesigner.open(effectType, existingLayout?)
  → Designer modal shown with canvas, controls, sidebar

User edits layout → changes stored in LayoutDesigner.layout (in-memory EffectLayout)

User clicks "Save"
  → postMessage({ type: "saveEffectLayout", effectType, layout })
  → C++: HandleSaveEffectLayoutRequest → SaveLayoutToFile
  → C++ sends { type: "layoutSaved", effectType, layout } back
  → messages.ts: updates uiState.layoutLibrary, calls renderActivePreset()
  → signalPath.ts: getCustomLayout() finds layout → renderCustomLayout() generates HTML

User clicks "Add Background"
  → postMessage({ type: "browseLayoutImage", purpose: "background", layerIndex })
  → C++: HandleBrowseLayoutImageRequest → native file dialog → copy + base64 encode
  → C++ sends { type: "layoutImageSelected", imageId, dataUrl, ... }
  → messages.ts: adds to uiState.layoutLibrary.images
  → layoutDesigner.handleImageSelected() updates layout and re-renders

On app startup
  → C++: LoadLayoutLibrary() scans layouts/ dir, base64-encodes images
  → Sends { type: "layoutLibraryLoaded", layoutLibrary }
  → messages.ts: stores in uiState.layoutLibrary
```

## Storage

- **Layouts:** `[AppData]/SoundshedGuitar/layouts/{effectType}.layout.json`
- **Images:** `[AppData]/SoundshedGuitar/layouts/images/{imageId}.{ext}`
- **Export format:** `.sgfxlayout.zip` containing `layout.json` + `images/` folder (created by JSZip in UI)
