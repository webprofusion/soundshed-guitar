# Theme System

## Key Files
- `core/ui/ts/theme-switcher.ts` — Core theme management logic
- `core/ui/css/variables.css` — Base/default tokens (`:root`) shared by every theme
- `core/ui/css/themes/light.css` — `.theme-light` token overrides
- `core/ui/css/themes/dark.css` — `.theme-dark` token overrides (primary theme)
- `core/ui/css/themes/classic.css` — `.theme-classic` (vintage) token overrides
- `core/ui/css/themes/studio.css`, `studio-controls.css` — `.theme-studio`, the light theme drawn as current hardware
- `core/ui/css/themes/studio-2003.css`, `studio-2003-controls.css` — `.theme-studio-2003`, the light theme drawn as used hardware
- `core/ui/images/themes/studio/`, `studio-2003/` — each one's rendered knob and textures (and 2003's screws)
- `tools/studio-theme-assets/studio.py`, `studio_2003.py` — the Blender scripts that render them
- `core/ui/css/themes/stage.css` — `.theme-stage`, the dark theme as a rig on a stage
- `core/ui/images/themes/stage/stage.jpg` — its backdrop, rendered by `tools/stage-theme-assets/stage.py`
- `core/ui/css/themes/high-gain.css` — `.theme-high-gain`, the dark theme as a high-gain amp head
- `core/ui/images/themes/high-gain/` — its knob, diamond plate, tolex and grille backdrop, rendered by
  `tools/high-gain-theme-assets/high_gain.py`
- `core/ui/css/components.css` — Canonical button/tab/icon-button classes
- `scripts/convert-css-variables.ps1` — Batch conversion script for CSS files

## Overview

GuitarFX uses a CSS variable-based theme system with seven active themes (light, dark, vintage/classic, stage, high gain, studio, studio 2003). Theme preference persists via backend settings and is reflected in UI state.

## Available Themes

| Theme | Class | Description |
|-------|-------|-------------|
| Light | `.theme-light` | High brightness, clean, modern |
| Dark | `.theme-dark` | Low brightness, reduced eye strain |
| Vintage | `.theme-classic` | Warm vintage browns/ambers |
| Stage | `.theme-dark.theme-stage` | Flat charcoal with one amber accent; the effect stands on a spot-lit wooden stage |
| High Gain | `.theme-dark.theme-high-gain` | A modern high-gain amp head: black plate and tolex, black knobs in red arcs, valves glowing behind a grille |
| Studio | `.theme-light.theme-studio` | Light and skeuomorphic: current studio hardware, all moulded plastic, chunky 3D keys, soft light |
| Studio 2003 | `.theme-light.theme-studio-2003` | Light and skeuomorphic: used studio hardware, a worn steel case, brushed rails and screws |

Legacy persisted values (`default`, `gritty`) are normalized to `dark` in `theme-switcher.ts` for backward compatibility.

## Usage

### Switching Themes (TypeScript)
```typescript
import { themeSwitcher } from './theme-switcher.js';

// Set specific theme
themeSwitcher.setTheme('dark');

// Cycle to next theme
themeSwitcher.cycleTheme();

// Get current theme
const theme = themeSwitcher.getCurrentTheme(); // 'light' | 'dark' | 'classic' | 'stage' | 'high-gain' | 'studio' | 'studio-2003'
```

### UI Control
Click the 🎨 icon in the icon bar to cycle through themes.

## CSS Variables

Theme tokens are defined in `variables.css` (`:root`) and selectively overridden by theme files:

```css
:root {
  --color-primary: #e07848;
  --bg-primary: #f0f2f5;
  --text-primary: #1c1c24;
}

.theme-dark {
  --color-accent: #ff9d5c;
  --bg-primary: #1a1a20;
  --text-primary: #e0e0e8;
}
```

### Variable Categories

| Category | Examples | Usage |
|----------|----------|-------|
| Accent / semantic | `--color-accent`, `--color-success`, `--status-error-text` | Primary actions and semantic states |
| Backgrounds | `--bg-primary`, `--bg-surface`, `--bg-panel`, `--bg-browser-shell` | App and panel surfaces |
| Text | `--text-primary`, `--text-secondary`, `--text-dark-primary` | Typography and readable contrast |
| Borders | `--border-light`, `--border-medium`, `--border-dark-medium` | Separators and control strokes |
| Controls | `--control-bg`, `--knob-ring-color`, `--toggle-active` | Knobs, toggles, and interactive elements |
| Overlays / shadows | `--overlay-light`, `--overlay-darker`, `--shadow-md` | Depth and transient visual layers |
| Shared component tokens | `--accent-primary`, `--button-bg`, `--button-border-hover` | Canonical button/tab/icon-button styling |

`--color-accent-rgb` is the primitive accent channel token. `--accent-primary-rgb`
is the semantic alias consumed by existing component CSS (for selection outlines,
drag-over states, and overlay mixes). Theme files should override
`--color-accent-rgb` alongside `--color-accent`.

### Hardening Notes

- `variables.css` includes **legacy alias tokens** (for example `--accent`, `--border-color`, `--input-bg`) so older component CSS keeps rendering safely during refactors.
- Dynamic hook tokens used by specific components (for example `--knob-pct`, `--mapped-angle`, `--icon-url`) have default values to avoid invalid-property rendering when runtime state is absent.
- When introducing a new token in component CSS, add a `:root` default first, then override per theme only where needed.

### Theme rules must not change functional layering

Structural/functional properties — `z-index`, and any property that **creates a stacking
context** (`backdrop-filter`, `filter`, `opacity` < 1, `transform`, `mix-blend-mode`,
`will-change`, `isolation: isolate`) — must be applied consistently across all themes, not
in a single theme block. Adding such a property to an element in only one theme silently
changes how descendant `z-index` resolves and can hide popovers/menus that work in other
themes.

Concrete example (fixed): `.theme-classic .control-bar` adds `backdrop-filter`, which turns
the control bar into a stacking context. The input levelling popover lives inside the bar,
so in the classic theme its `z-index` was trapped within that context and rendered behind
the main content — while light/dark (no `backdrop-filter` on the bar) were unaffected. The
fix is structural and theme-independent: opening the popover toggles a `has-open-calibration`
class on `.control-bar` that raises it to `var(--z-popover)` (the same mechanism the preset
popover uses via `has-open-popover`), defined once in `navigation.css`. If a theme needs a
stacking-context-creating effect on a container that hosts overlay UI, pair it with that lift
in base CSS rather than relying on the descendant's own `z-index`.

Use the `--z-*` tokens (`--z-dropdown`, `--z-popover`, `--z-modal`, `--z-modal-priority`) for
global overlay layers; keep local sibling stacking (`z-index: 1/2/3`) as small literals.

### Component CSS
Components use variables instead of hardcoded colors:

```css
.button {
  background: var(--color-primary);
  color: var(--text-on-primary);
  border: 1px solid var(--border-dark);
}
```

## Canonical UI Components

Core interactive controls have a single source of truth in
`core/ui/css/components.css`. Reuse these classes instead of creating new
per-feature button/tab styles — they are fully theme-driven, so every theme
stays cohesive automatically. A semantic hook class (used by TypeScript) may be
kept alongside the canonical class, e.g. `class="midi-tab-btn tab-btn tab-btn-underline"`.

| Component | Base class | Modifiers |
|-----------|-----------|-----------|
| Button | `.btn` | `.btn-primary`, `.btn-secondary`, `.btn-danger`, `.btn-ghost`, `.btn-small`/`.btn-sm`, `.btn-large`, `.btn-block` |
| Icon button | `.icon-btn` | `.icon-btn-accent`, `.icon-btn-sm`, state: `.active`/`.is-active` |
| Tab | `.tab-btn` | `.tab-btn-underline`, `.tab-btn-vertical`, state: `.active`/`.is-active`/`[aria-selected="true"]` |

Component-specific CSS files should only add layout/positioning tweaks (width,
alignment, placement) on top of these classes. Button hover surfaces are driven
by `--button-bg-hover` / `--button-border-hover` tokens in `variables.css`.

## Adding a New Theme

1. Create `css/themes/yourtheme.css` with the theme's token overrides:
   ```css
   .theme-yourtheme {
     --color-accent: #your-accent;
     --bg-primary: #your-bg;
     --text-primary: #your-text;
     /* ...only the tokens this theme overrides; the rest inherit from :root */
   }
   ```

2. Link it in `index.template.html` alongside the other `css/themes/*.css` files.

3. Update `theme-switcher.ts`:
   - add the theme to `ThemeName`, to `THEME_OPTIONS` (which fills the theme pickers and the
     cycle order) and to `THEME_BODY_CLASSES`
   - remove/adjust any relevant legacy mapping in `LEGACY_THEME_MAP`

4. The native UI (`core/src/uiclient`, Nano Q) has its own palettes and shows dark for a name it
   does not know. If the new theme is a variant of one it has, map it there (`NormalizeTheme`
   in `UiClientPresetHandlers.cpp` and the `theme` handler in `UiClientLibraryHandlers.cpp`),
   as both studio themes map to light. Stage and High Gain need no mapping: the native UI shows them as dark.

> Base/default token values live in `css/variables.css` (`:root`). Theme files
> also own theme-specific component styling selectors (for example
> `.theme-classic .footer-*` and per-theme scrollbar/input treatments), so
> `base.css` stays theme-agnostic.

## Signal Chain Nodes

Every theme draws a chain node as a piece of equipment: a block a few pixels proud of the
strip, with a lit top edge, a side wall under its face, a soft shadow, and its category
colour as a thin line all round its edge (full colour, with a soft halo, when selected).
The shape lives once, in `css/signal-path/nodes.css`; a theme only sets its colours:

| Token | What it is | Fallback |
|-------|------------|----------|
| `--node-face` | the face | `--bg-card` |
| `--node-sheen` | a highlight across the top of the face | faint white |
| `--node-edge-light` | the lit top edge | faint white |
| `--node-wall` | the side wall | the face in shade |
| `--node-wall-edge` | where the wall meets the strip | dark line |
| `--node-shadow` | the soft shadow | black |
| `--node-shadow-x` | sideways shadow offset, for a theme lit from one side | `0px` |

Set them on the theme class (`.theme-dark { --node-face: … }`), not on `.signal-node`.

## Themes Built on Another: Studio and Studio 2003

Both studio themes are the light theme drawn as hardware, each as a different unit:

- **Studio** is a current one, all moulded plastic with no metal: a warm light-grey case,
  paler raised panels, chunky keys with a visible side wall that travel down when pressed,
  knobs with a light cap on a dark grey skirt, soft shadows from one large soft box overhead,
  and one accent colour (orange) kept to value rings, on states and latched keys. The primary
  key is graphite. Raised parts share `--studio-raised` / `--studio-raised-high`: a lit top
  edge, a few pixels of side wall (`--studio-wall`), then a soft shadow. `grain.png` is the
  moulding texture as a transparent overlay, so one image serves every plastic colour.
- **Studio 2003** is a used one: a worn satin-steel case (scratched, rubbed, a light
  patina), finely brushed aluminium rails at the top and bottom, panel screws, wear and a
  spun-aluminium knob, under a harder light. Its case and rail images are made at twice the
  size they are drawn, so the grain stays finer than a pixel.

Both keep the same grey-green LCD for the preset name and tempo, and LED ladders for the meters.

They are variants of light rather than palettes of their own, so `THEME_BODY_CLASSES` puts
**both** `theme-light` and the theme's class (`theme-studio` or `theme-studio-2003`) on the
body. Every `.theme-light` rule across the stylesheets keeps matching, and the studio sheets
(linked after `light.css` and `grain.css`) only restyle what makes it hardware. A new
light-only rule therefore reaches both; check it there too. The two classes never appear
together, and each theme's `--studio-*` tokens are defined on its own class, so the two sets
of sheets do not see each other.

- `studio.css` / `studio-2003.css` hold the tokens and the surfaces; the `-controls.css` sheet
  beside each holds the knobs, switches, keys, LCD and meters.
- Each theme assumes one light: Studio's is straight above and a touch to the left (shadows
  fall down, wide and soft); Studio 2003's is above and to the left (shadows down and right,
  tighter). The rendered images use the same light, so keep new shadows consistent with it.
- The knob is a still image (`knob.png`, shadow baked in, 1.3 collar diameters across) under
  the existing `.knob-indicator`, which the theme redraws as a pointer sized in percentages of
  the knob. Only the pointer turns, so the highlights stay where the light is. A custom
  layout's own knob art (an image, or the Pedal, Amp and Minimal styles) is left alone.
- The images are generated, not hand-made (Blender 5.x):
  `blender -b --factory-startup -P tools/studio-theme-assets/studio.py -- [knob] [textures]` and
  `blender -b --factory-startup -P tools/studio-theme-assets/studio_2003.py -- [knob] [screws] [textures]`.
  `studio.py` reuses `studio_2003.py`'s scene helpers. The knobs and screws are path traced;
  the textures are seeded numpy noise made to tile. The app's resource server knows `png` and
  `jpg`, so keep to those formats.

## Stage: the Dark Theme on a Stage

Stage does on the dark side what the studio themes do on the light side: `THEME_BODY_CLASSES`
gives the body `theme-dark` and `theme-stage`, so every `.theme-dark` rule still applies and
`stage.css` (linked after the studio sheets) only restyles what makes it Stage.

- Neutral charcoal with no blue in it: near-black bars, panels a step lighter, hairline edges,
  and the radius scale (`--radius-sm` … `--radius-2xl`) cut to 2–6px, so corners are nearly square.
- One amber (`--stage-amber`), kept to choices: the selected chain block is filled with it
  (its legend goes dark, `--stage-on-amber`), a latched key is outlined in it, the primary key
  and switches that are on are lit with it, and knobs draw their value arc in it.
- Flat chain blocks: a grey cap with a hairline edge; the category colour stays on the icon.
- Type is the grotesque Archivo (a Latin-subset variable woff2 in `css/fonts/`, declared in
  `stage.css`): weight 500 throughout, 700 for legends, block names and values, 800 for tabs
  and titles. It is about Inter's width, so labels that fit in dark still fit.
- The play page (`#panel-visualizer`) shows `images/themes/stage/stage.jpg`, a dim room with a
  slatted wall and a plank floor under one warm spotlight, anchored to the bottom so the pool of
  light falls under the effect, with the edges darkened further in CSS. The effect panel floats
  over it as a near-opaque charcoal card. Other pages keep the plain charcoal.
- The backdrop is rendered, not drawn:
  `blender -b --factory-startup -P tools/stage-theme-assets/stage.py` (Cycles, about a minute;
  `-- preview <file>` makes a quick small one to judge framing and light). Keep it dim: text
  around the panel has to read over any part of it.
- The dark theme hard-codes a few blue-grey colours in component sheets (the effect panel's
  legends, resource keys, the chain's add keys); `stage.css` restates those neutral. A new
  hard-coded colour in a dark-only rule needs checking here too.

## High Gain: the Dark Theme as a High-Gain Amp Head

High Gain is built on dark the same way as Stage (`theme-dark` plus `theme-high-gain` on the
body; `high-gain.css` loads after `stage.css`). It takes the look shared by the amp sims made
for metalcore, deathcore and djent: everything black, one hot light source, metal hardware.

- Black with a little cold in it and no blue. The top bar and the footer are black diamond
  plate (`plate.jpg`), the preset bar and the chain's strip black pebble tolex (`tolex.jpg`),
  each drawn at half its pixel size and with a gradient over it; hardware has a bright
  machined top edge (`--hg-chrome-edge`) and a shadow under it, so the light is above.
- Red is light, not paint (`--hg-red` for fills, `--hg-red-hot` on black): the selected chain
  block keeps its black face and glows red at its edge and from below, a latched key's legend
  lights red, the primary key and switches that are on are lit red, knobs draw their value arc
  dark red to bright red, and the preset name and tempo are red LED type on a black display.
  Nothing else is red. Meters keep their green/yellow/red LEDs behind a black window.
- Knobs: `knob.png` (a gunmetal cap on a black knurled body, 1.15 skirt diameters across, its
  shadow baked in, a faint red rim from below) sits inside the base knob's conic arc, inset
  10.5%; only the white pointer line turns. Everything is in percentages, so any size works.
- The play page shows `grille.jpg`: a black hex-perforated grille, eight power valves glowing
  red-orange behind it out of focus, low in the frame so the glow rises under the effect
  panel. The panel floats over it as a near-opaque black card. The stage's tint
  (`.effect-visualization::before`) is stretched over the whole grille; at its usual 400px
  floor it stopped halfway down and left a seam.
- Type is heavy and condensed, like a gig poster: Oswald (weight 500, bolder for tabs and
  labels) for the whole UI, and the blackletter Grenze Gotisch only for titles (the effect
  panel's name, dialog and flyout headings), scaled by `font-size-adjust` so each keeps its
  own size. Both are Latin-subset variable woff2 files in `css/fonts/` (the CSP allows only
  local fonts), declared in `high-gain.css` and used only by this theme.
- The images are generated: `blender -b --factory-startup -P tools/high-gain-theme-assets/high_gain.py
  -- [knob] [textures] [grille]` (`grille preview <file>` for a quick, small one). The knob uses
  `studio_2003.py`'s scene helpers; the plate and tolex are numpy height fields shaded in the
  script and made to tile.
- Like Stage, it restates the dark theme's hard-coded blue-greys (effect panel legends,
  resource keys, the chain's add keys) neutral; a new hard-coded colour in a dark-only rule
  needs checking here too.

## Converting Existing CSS

Run the conversion script to replace hardcoded colors with variables:

```powershell
.\scripts\convert-css-variables.ps1
```

### Manual Conversion Patterns

```css
/* Direct color → variable */
color: #5a5a68;          →  color: var(--text-secondary);

/* Gradient → solid variable */
background: linear-gradient(180deg, #d0d4dc 0%, #b8bcc8 100%);
                         →  background: var(--bg-secondary);

/* RGBA → overlay variable */
background: rgba(255, 255, 255, 0.3);
                         →  background: var(--overlay-light);
```

## Building & Testing

```powershell
# Build TypeScript
cd core/ui
npm run build

# Build JUCE standalone app
cmake --build juce/builds --config Debug --target SoundshedGuitar_Standalone

# Test: launch app, click 🎨 icon to cycle themes
```

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Theme not switching | Check `variables.css` loads first in `index.html` |
| Some colors unchanged | Search for hardcoded values and convert to tokens; if using a legacy alias token, migrate toward canonical token names over time |
| New component needs theming | Use existing variables; add new ones to all theme blocks |
| Popover/menu hidden in one theme only | A theme-only stacking-context property (e.g. `backdrop-filter`) on an ancestor is trapping the overlay's `z-index`. Apply such properties across all themes and lift the container with a `--z-popover` class on open (see "Theme rules must not change functional layering"). |

## See Also
- [User Interface](user-interface.md) — UI architecture
