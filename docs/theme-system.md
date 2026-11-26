# Theme System

## Key Files
- `core/ui/ts/themes.ts` — **the list of themes**: each one's name, label, base palette, light or
  dark, body classes, stylesheets and type faces. Everything below reads it
- `core/ui/ts/theme-switcher.ts` — puts the chosen theme's classes on the body and stores the choice
- `core/ui/scripts/assemble-html.js` — writes each theme's stylesheets into `index.html`
- `tools/gen-native-theme.mjs` → `core/src/uiclient/ThemeTokens.generated.h` — the themes, their
  colours and type faces for the native UI (Nano Q). `tools/theme-css.mjs` resolves each theme's
  custom properties as the browser does (the cascade in link order, `:root` values resolved at
  `:root`, a variant over its base palette, component tokens' fallbacks, `color-mix()`);
  `npm run check:native-theme` fails when the header is out of date
- `tools/capture-native-controls.mjs` → `core/src/uiclient/ThemeControls.generated.h` — each
  theme's effect-editor controls (knob, switch, tabs, group box, labels, plot card) as a headless
  browser computes them from the CSS, drawn by `nanoq/src/ui/widgets/ControlSkin.h`;
  `npm run check:native-controls` fails when the header is out of date
- `core/ui/css/variables.css` — Base/default tokens (`:root`) shared by every theme
- `core/ui/css/themes/<theme>.css` and `<theme>-skin.css` — every theme is two sheets: its
  **palette** (nothing but its colour tokens, on its class; the native UI reads these too) and its
  **skin** (everything else in its look: component rules, pictures, textures, type, shape). See
  "Palette and skin" below
- `core/ui/css/themes/light.css` — `.theme-light` token overrides
- `core/ui/css/themes/dark.css` — `.theme-dark` token overrides (primary theme)
- `core/ui/css/themes/classic.css` — `.theme-classic` (vintage) token overrides
- `core/ui/css/themes/studio.css` — `.theme-studio`, the light theme drawn as current hardware
- `core/ui/css/themes/studio-2003.css` — `.theme-studio-2003`, the light theme drawn as used hardware
- `core/ui/images/themes/studio/`, `studio-2003/` — each one's rendered knob and textures (and 2003's screws)
- `tools/studio-theme-assets/studio.py`, `studio_2003.py` — the Blender scripts that render them
- `core/ui/css/themes/stage.css` — `.theme-stage`, the dark theme as a rig on a stage
- `core/ui/images/themes/stage/stage.jpg` — its backdrop, rendered by `tools/stage-theme-assets/stage.py`
- `core/ui/css/themes/high-gain.css` — `.theme-high-gain`, the dark theme as a high-gain amp head
- `core/ui/images/themes/high-gain/` — its knob, diamond plate, tolex and grille backdrop, rendered by
  `tools/high-gain-theme-assets/high_gain.py`
- `core/ui/css/themes/studio-1973.css` — `.theme-studio-1973`, the dark theme as a wood-panelled 1970s control room
- `core/ui/images/themes/studio-1973/` — its walnut, teak, leather and stitching (`tools/studio-1973-theme-assets/textures.py`)
  and its knob and control-room backdrop (`tools/studio-1973-theme-assets/studio_1973.py`)
- `core/ui/css/custom-background.css`, `core/ui/ts/visualizerBackground.ts` — the player's own
  effect-panel background, drawn over whichever theme is on
- `core/ui/ts/themeFlyout.ts`, `core/ui/css/theme-flyout.css` — the footer's Theme button and the
  flyout of zoom and theme pictures
- `core/ui/images/themes/thumbnails/` — those pictures, captured by `tools/theme-thumbnails/capture.mjs`
- `core/ui/css/components.css` — Canonical button/tab/icon-button classes
- `scripts/convert-css-variables.ps1` — Batch conversion script for CSS files

## Overview

GuitarFX uses a CSS variable-based theme system with eight active themes (light, dark, vintage/classic, stage, eleven (id high-gain), studio, studio 2003, studio 1973). Theme preference persists via backend settings and is reflected in UI state.

## Available Themes

| Theme | Class | Description |
|-------|-------|-------------|
| Light | `.theme-light` | High brightness, clean, modern |
| Dark | `.theme-dark` | Low brightness, reduced eye strain |
| Vintage | `.theme-classic` | Warm vintage browns/ambers |
| Stage | `.theme-dark.theme-stage` | Flat charcoal with one amber accent; the effect stands on a spot-lit wooden stage |
| Eleven (id `high-gain`) | `.theme-dark.theme-high-gain` | A modern high-gain amp head: black plate and tolex, black knobs in red arcs, valves glowing behind a grille |
| Studio | `.theme-light.theme-studio` | Light and skeuomorphic: current studio hardware, all moulded plastic, chunky 3D keys, soft light |
| Studio 2003 | `.theme-light.theme-studio-2003` | Light and skeuomorphic: used studio hardware, a worn steel case, brushed rails and screws |
| Studio 1973 | `.theme-dark.theme-studio-1973` | Dark and skeuomorphic: a 1970s control room, walnut and teak veneer, aluminium knobs and keys, a leather armrest, amber lamps |

Legacy persisted values (`default`, `gritty`) are normalized to `dark` (`LEGACY_THEME_NAMES` in `themes.ts`), in the web UI and the native one alike.

## Palette and skin

A theme's `<theme>.css` holds only blocks on the theme's own class (`.theme-stage { … }`) that
declare only custom properties: its palette. `<theme>-skin.css` holds the rest, and loads right
after it (`ts/themes.ts` lists both; `index.html` links them in that order). The split matters
because the palette is all that crosses to the native UI (`tools/gen-native-theme.mjs` reads it)
and all a new palette-only theme needs; a skin is web-only decoration. `tests/themes.test.ts`
fails if a palette sheet holds anything else. When a skin rule only sets a colour, use a
component token instead (next section).

## Component tokens

Tokens come in two tiers. The global ones (`--bg-primary`, `--text-primary`,
`--color-accent`, ...) are defined in `css/variables.css` and every palette. Component tokens
name one part of one component, and a component sheet reads them with its own base look as the
fallback, so `:root` never has to define them:

```css
/* navigation.css */
.nav-tab { color: var(--nav-tab-ink, var(--text-muted)); }

/* themes/stage.css: only the themes that differ set it */
.theme-stage { --nav-tab-ink: #9c9c9c; }
```

A theme that looks like the base sets nothing; a variant inherits its base palette's value. The
`--node-*` tokens of the signal chain work the same way. Today's component tokens, by sheet:

| Sheet | Tokens |
|---|---|
| `navigation.css` | `--nav-tab-ink`, `--nav-tab-ink-hover`, `--nav-tab-active-ink`, `--nav-tab-active-line`, `--icon-bar-rule` |
| `signal-path/params-panel.css` | `--effect-panel-bg`, `--effect-shell-title-ink`, `--effect-shell-subtitle-ink`, `--effect-shell-value-ink`, `--effect-shell-warning-bg`, `--effect-shell-warning-ink` |
| `signal-path/resource-slots.css` | `--eq-visualizer-bg`, `--eq-visualizer-border`, `--eq-visualizer-header-ink`, `--eq-canvas-bg`, `--toolbar-btn-bg`, `--toolbar-btn-border`, `--param-group-title-ink` |
| `preset-library/toolbar.css` | `--preset-display-bg`, `--preset-display-border`, `--preset-name-bg`, `--preset-name-bg-hover`, `--preset-name-ink-hover`, `--preset-favorite-bg`, `--preset-status-bg` |
| `fx-library.css` | `--fx-panel-bg`, `--fx-header-bg`, `--fx-header-rule` |
| `signal-path/nodes.css` | `--add-trigger-bg`, `--add-trigger-border`, `--add-trigger-ink` |
| `signal-path/parallel.css` | `--parallel-bg`, `--parallel-border` |
| `signal-path/resource-picker.css` | `--resource-key-bg` |
| `base.css` | `--toast-bg`, `--toast-ink`, `--toast-border` |
| `amp.css` | `--effect-overlay-ink`, `--effect-placeholder-ink` |

What stays in a skin: pictures, gradients and textures (the studio plates, the high-gain tolex),
fonts and shapes, and colours a theme sets on a more specific selector than the component's own
rule (turning those into tokens would change which rule wins elsewhere, so each needs a look).

**The ratchet.** `npm run check:theme-ratchet` (part of `npm run verify` and the UI checks
workflow) pins three counts per sheet in `scripts/theme-ratchet-baseline.json`, and fails if one
goes up: colour literals in a component sheet outside a `var(--token, fallback)`, selectors
naming a `.theme-*` class outside `css/themes/`, and declarations in each skin sheet. When a
count goes down, re-pin with `npm run check:theme-ratchet -- --update`.

## In the native UI (Nano Q)

Nano Q draws every theme in its own colours from `ThemeTokens.generated.h`, through
`nanoq/src/ui/widgets/Theme.h`. To draw a part as the web UI does, add the custom property it
uses to `TOKENS` in `tools/gen-native-theme.mjs`, regenerate, and read it with an accessor.
Two stop-gaps remain for parts no token carries yet:

- `DarkOr(dark, token)`: a colour measured from the Dark web UI on Dark, and the theme's own
  token on every other theme. Each of these is a part to give a token, then a plain accessor.
- `Pick(dark, light)`: a pair of shades by light or dark (a plot's backdrop, the shading over a
  picture, the fretboard's wood), for colours no theme changes.

The effect editor's controls carry over whole, skin rules included. `tools/capture-native-controls.mjs`
lays the effect panel's markup out in a headless Chrome or Edge with every stylesheet the page
links, reads the computed styles under each theme, and writes `ThemeControls.generated.h`: the
knob (face, ring band, picture, pointer, tick), the switch on and off, the tabs, a group's box, a
control's label and value, the panel, the artwork panel, and the plot card (`.eq-visualizer`) the
EQ curve, a cabinet's and a pickup's response and the IR alignment sit in. Nano Q draws them in
`nanoq/src/ui/widgets/ControlSkin.h` (`Theme::Controls()`), the knob pictures from the UI's
assets. So a theme's knob or switch changes in its CSS only; rerun the tool after. Its `--check`
(`npm run check:native-controls`, in `verify` and CI) reads the styles again and fails when the
header no longer matches, so a CSS edit that moves no control passes untouched.

The same capture reads the surfaces' backgrounds whole (`ControlSkin::backdrops`): the bars
(`.icon-bar`, `.control-bar`, the chain's, `.footer-bar`), the page (`.main-content`), the play
page behind the effect (`#panel-visualizer`: Stage's stage, Eleven's grille), the effect's
shell, and a dialog and its header. Each is its colour and its layers, every picture placed,
sized and tiled as CSS has it (the film grain, plate, tolex, casing, screws) and every linear and
radial gradient. Nano Q lays them over its own colour for each surface (`Dressed`,
`DrawBackdrop`); the effect's shell draws its colour and layers in place of the flattened fill.
A layer the native side cannot draw stops the capture, so a new kind is noticed. A repeating
linear gradient straight across or down (a dial's scale ticks, an LCD's pixel grid) repeats its
stops in Nano Q too, its hard stops as solid bands.

Keys, the chain's modules and the displays carry over the same way, for a theme that restyles
them. The capture reads a dialog's keys (`.btn`, `.btn.active` latched, `.btn-primary`), the
preset toolbar's (`.preset-action-btn`), the arrows beside the preset (`.preset-nav-btn`), two
modules (`.signal-node`, one chosen), the preset's display (`.preset-selector` and its name's
lettering and glow) and the footer's tempo (`.bpm-display`): each as a box, a background (in
`backdrops`), the side wall its hard box-shadow bands make (how far a key stands proud) and its
ink (`ControlSkin::keys`, `nodes`, `presetDisplay`, `tempo`). One is `drawn` only where it
computes to something other than its base theme's (the theme its first class names), so the base
themes keep Nano Q's own look. Nano Q's `DrawKey` stands the face on its wall, pushes it down when
pressed, and lightens it on hover; `Button`, `IconButton` (Boxed, Soft, Primary, Accent, Chip,
Slate, and `Look::Tempo` for the tempo), `PresetPill` and `NodeCard` draw with it. A module keeps
its category's colour on its border unless the theme's border is the same for every category
(`fixedBorder`: Stage's modules, Studio 1973's chosen one), and its name takes the theme's
lettering. So Studio's and Studio 2003's LCD, Eleven's red readout and Studio 1973's amber dial
and cream keys look in Nano Q as they do on the web.

Type carries over too. The capture reads the page's face and weight (`body`) and a title's
(`title`, with `titleScale` for `font-size-adjust`), and Nano Q's `Font()` draws in that face:
Regular at the page's weight (Oswald and Archivo are set at 500), Medium and SemiBold a step and
two above, `Weight::Title` in the title face and weight (Oswald 700, Archivo 800). The faces are static
cuts in `core/nativeui/fonts`, made from the web UI's own variable fonts by
`node tools/gen-nano-fonts.mjs` (its `FAMILIES` table names the weights each theme draws); the
theme fonts are kept only as WOFF2, which `tools/nano-fonts/woff2.mjs` unpacks first. Every face
is listed with Inter after it, for any character a Latin subset lacks. A theme that adds a face
needs its cuts made, then an entry in `kThemeFaces` (`nanoq/src/ui/widgets/Theme.cpp`); until
then Nano Q draws it in Inter.

What stays web-only: textures on surfaces Nano Q has no counterpart for (the FX selector panel),
the stage's tint (`.effect-visualization::before`), repeating gradients at an angle, and the
player's own effect-panel background.

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

### Choosing a theme
The footer's **Theme** button (`#footer-theme-btn`) opens the theme flyout (`ts/themeFlyout.ts`,
`css/theme-flyout.css`): the UI zoom at the top, then a tile per theme with a picture of the app
wearing it. A tile applies its theme at once and the flyout stays open, so themes can be tried
one after another; a click outside, Escape or the button closes it. At compact density the button
is in the footer's OPTIONS sheet, which stays open while the flyout is used. Nano Q has the same flyout (`nanoq/src/ui/shell/ThemeFlyout.cpp`), with the same pictures and zoom stops; its zoom is kept in `uiSettings.native.zoom`.

The pictures are `core/ui/images/themes/thumbnails/<theme>.jpg`: 320px squares of the app's top-left
corner (its 560px square at 1280x800 and 100% zoom), captured from the running Standalone by
`tools/theme-thumbnails/capture.mjs` (usage in its header). Capture them with a factory preset
whose names are our own, as they ship. A theme with no picture yet gets an empty frame.

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

1. Create `css/themes/yourtheme.css` with the theme's token overrides (for a variant, the
   overrides on top of its base palette, under its own class):
   ```css
   .theme-yourtheme {
     --color-accent: #your-accent;
     --bg-primary: #your-bg;
     --text-primary: #your-text;
     /* ...only the tokens this theme overrides; the rest inherit from :root */
   }
   ```

2. Add an entry to `ts/themes.ts`: its `id`, `label`, the palette it builds on (`base`, itself
   for a new palette), `scheme` (light or dark), its body `classes` (the base's first), its
   `palette` and `skin` sheets, and any `fonts`. The pickers, the body classes, the `<link>`s in
   `index.html` (written at the template's `<!--#theme-palettes-->` / `<!--#theme-variants-->`
   markers) and the native theme list all follow from it. A name earlier builds stored goes in
   `LEGACY_THEME_NAMES`.

3. Capture its flyout picture: `node tools/theme-thumbnails/capture.mjs <ws>` redoes them all
   (see "Choosing a theme").

4. Run `npm run build`, `node tools/gen-native-theme.mjs` and
   `node tools/capture-native-controls.mjs`. The native UI (Nano Q) then lists the theme and
   draws it in its own colours (a variant's palette over its base's) and its own knobs,
   switches, cards and textures. Any palette or control change needs the headers regenerated
   too; `npm run verify` says so. A new font also needs `node tools/gen-nano-fonts.mjs` (add it
   to `FAMILIES` there) and an entry in `kThemeFaces` in `nanoq/src/ui/widgets/Theme.cpp`.

> Base/default token values live in `css/variables.css` (`:root`). A theme sets component
> tokens in its palette where it differs; what tokens cannot carry (pictures, gradients,
> per-theme scrollbar and input treatments) goes in its skin sheet, so `base.css` stays
> theme-agnostic.

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
(linked after light's and `grain.css`) only restyle what makes it hardware. A new
light-only rule therefore reaches both; check it there too. The two classes never appear
together, and each theme's `--studio-*` tokens are defined on its own class, so the two sets
of sheets do not see each other.

- `studio.css` / `studio-2003.css` hold the tokens; `studio-skin.css` / `studio-2003-skin.css`
  draw the surfaces, then the knobs, switches, keys, LCD and meters.
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
`stage.css` and `stage-skin.css` (linked after dark's and `grain.css`) only restyle what makes it Stage.

- Neutral charcoal with no blue in it: near-black bars, panels a step lighter, hairline edges,
  and the radius scale (`--radius-sm` … `--radius-2xl`) cut to 2–6px, so corners are nearly square.
- One amber (`--stage-amber`), kept to choices: the selected chain block is filled with it
  (its legend goes dark, `--stage-on-amber`), a latched key is outlined in it, the primary key
  and switches that are on are lit with it, and knobs draw their value arc in it.
- Flat chain blocks: a grey cap with a hairline edge; the category colour stays on the icon.
- Type is the grotesque Archivo (a Latin-subset variable woff2 in `css/fonts/`, declared in
  `stage-skin.css`): weight 500 throughout, 700 for legends, block names and values, 800 for tabs
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
  legends, resource keys, the chain's add keys); `stage-skin.css` restates those neutral. A new
  hard-coded colour in a dark-only rule needs checking here too.

## Eleven: the Dark Theme as a High-Gain Amp Head

Eleven (first named High Gain, which is still its id and file names) is built on dark the same way as Stage (`theme-dark` plus `theme-high-gain` on the
body; its `high-gain.css` palette and `high-gain-skin.css` load after dark's). It takes the look shared by the amp sims made
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
- Type is heavy and condensed, like a gig poster: Oswald for the whole UI, at weight 500,
  bolder for tabs and labels, and 700 for titles (the effect panel's name, dialog and flyout
  headings). It is a Latin-subset variable woff2 file in `css/fonts/` (the CSP allows only
  local fonts), declared in `high-gain-skin.css` and used only by this theme.
- The images are generated: `blender -b --factory-startup -P tools/high-gain-theme-assets/high_gain.py
  -- [knob] [textures] [grille]` (`grille preview <file>` for a quick, small one). The knob uses
  `studio_2003.py`'s scene helpers; the plate and tolex are numpy height fields shaded in the
  script and made to tile.
- Like Stage, it restates the dark theme's hard-coded blue-greys (effect panel legends,
  resource keys, the chain's add keys) neutral; a new hard-coded colour in a dark-only rule
  needs checking here too.

## Studio 1973: the Dark Theme as a 1970s Control Room

Studio 1973 is the oldest of the studio themes, but it is built on dark, like Stage and
Eleven (`theme-dark` plus `theme-studio-1973` on the body; `studio-1973.css` and
`studio-1973-skin.css` load after dark's), because walnut is dark. It is deliberately over
the top: as much wood as the UI will hold, drawn to look real.

- Wood is the case; what you touch is cream plastic or metal. The room is figured walnut
  veneer (`walnut.jpg`): the top bar, the page backgrounds and the chain's tray, with a paler
  teak fascia (`teak.jpg`) under the top bar. The units are a finer, straighter quarter-sawn
  walnut (`walnut-fine.jpg`, and `walnut-fine-v.jpg` with the grain upright), as a real
  cabinet would be: the dialog, flyout and picker headers (the metronome's, tuner's and EQ's
  own header bars included) and the side cheeks either side of the effect panel, drawn by
  its `::before`/`::after` outside its box so they take no room (left out at compact
  density). Each veneer is drawn at half its pixel size under a glaze: a gradient that
  darkens it for the cream ink and adds the lacquer's sheen.
- Keys and the chain's modules are cream plastic caps (`--s73-ivory`, lighter at the top, a
  slight dish lit from above) with near-black legends, standing on a plastic side wall and
  travelling down when pressed. Nothing is outlined: the rim (`--s73-ivory-rim`) is a shade
  under the face, the wall is the same plastic in shadow, and the only dark is a soft contact
  shadow on the wood (a hard dark band would read as a drawn outline, in Nano Q too). The
  effect panel is a black-faced unit with polished trim, and knobs are
  `knob.png`: solid turned aluminium with a spun face, polished chamfer and fine knurling,
  framed as Studio's (1.3 skirt diameters across), inside the base knob's amber arc.
- Amber is light, not paint (`--s73-amber`, `--s73-amber-hot`): the selected module's lamp,
  latched keys, switches that are on, the value arc and values, and the preset name and
  tempo, which glow through black glass like a receiver's tuning dial, scale ticks and all.
  Burnt orange (`--s73-orange`) is the one paint: the primary key, and the thin orange,
  amber and mustard stripe along the bottom of the top bar.
- The footer is a padded saddle-leather armrest (`leather.jpg`, a rolled gradient over it)
  stitched along its top edge (`stitch.png`, repeated).
- The play page shows `room.jpg`: a 1970s control room out of focus, wood panelling, a
  wooden console with its meters glowing, kept dim with the light low in the frame. The
  stage's tint covers all of it, as in Eleven.
- The input and output meters are moving-coil VU meters (`.vu-gauge`, which the base sheet
  hides and this skin shows in place of the ladder). `ts/vuMeter.ts` drives them with RMS
  (0 VU at -18 dBFS, the stop at +3 VU) through VU ballistics, about 300 ms to settle, and
  writes one custom property, `--vu-needle`, an angle. A red peak dot, `--vu-peak`, rides
  the red dBFS scale printed just under the VU arc, on the same sweep with 0 dBFS at the stop
  (so -3 dBFS sits under 0 VU, where the red zone starts): it jumps to a new peak at once and
  falls back 20 dB in 1.7 s, as a peak programme meter does. The needle is a background (a
  thin conic wedge cut short by a radial mask) and the dot a radial gradient placed with
  `sin()` and `cos()`, so both repaint their own box and never relayer. The face
  (`vu-face.png`) is drawn as SVG by `tools/studio-1973-theme-assets/vu-face.mjs` and
  rasterised at 4x by headless Chrome or Edge. Its geometry (a 66 x 48 face, the pivot 52px
  down, a 41px needle, ±42°, the peak scale's line 31.5px from the pivot) is repeated in the
  skin and in Nano Q's `MeterBar`.
- Type: Fraunces (`css/fonts`, OFL), softness all the way up and the wonky letters on, for
  names and legends: the nav, the control legends, the preset name, the chain's modules, the
  effect's title, tabs, groups and knob legends, and dialog titles. Values, lists and body
  text stay Inter. Nano Q draws the effect panel's title, tabs, groups and legends in it
  (static cuts at 600, 700 and 800; their STAT table is dropped, or DirectWrite files them
  under a family named after the pinned axes). Its top bar and chain keep Inter, as they draw
  in the body face.
- Nano Q draws the walnut cheeks too: the capture reads the panel's `::before` and `::after`
  (`cheekWidth`, `cheekRadius`, `backdrops.cheekLeft`/`cheekRight`), and the effect page
  sets them beside the panel when the stage has room. The black face is the shell's
  background (the panel only frames it) so the capture sees it. The stripe's three bands are
  gradient layers sized and placed along the bar's foot, which the capture and Nano Q's
  `DrawLinear` now carry for every linear gradient (auto is still the whole box).
- The images are generated. Textures: `blender -b --factory-startup -P
  tools/studio-1973-theme-assets/textures.py -- [walnut] [walnut-fine] [teak] [leather]` (or plain Python
  with `--preview <dir>` for PNG previews): numpy fields filtered in the frequency domain, so
  they tile. Knob and backdrop: `blender -b --factory-startup -P
  tools/studio-1973-theme-assets/studio_1973.py -- [knob] [room]` (`room preview <file>` for
  a quick one), which uses `studio_2003.py`'s scene helpers. The VU face: `node
  tools/studio-1973-theme-assets/vu-face.mjs`.

## The Player's Own Effect-Panel Background

Settings → User Interface → Effect Panel Background lets the player put their own picture behind the play
page's effect panel (`#panel-visualizer`), in place of whatever the theme draws there: Stage's
stage, Eleven's grille, or the plain panel of the others. It applies under every theme.

- **Storage.** Choose Image… sends `browseUiBackgroundImage`; the engine
  (`core/src/controller/UiBackgroundService.h`) opens a file dialog, copies the PNG, JPEG, GIF or
  WebP (at most 32 MB) to `<profile>/data/v1/ui-backgrounds/<hash>.<ext>` (the folder's only file:
  picking another removes the one before) and answers `uiBackgroundImageSelected` with
  `ui-backgrounds/<file>`, which the editor's resource route serves. The UI keeps that URL in the
  app setting `ui.visualizerBackground.image`, so every instance shows it, as the theme is. Remove
  clears the setting and sends `clearUiBackgroundImage`.
- **Fade** (`ui.visualizerBackground.fade`, 0-90 %, default 30) veils the picture toward the
  theme's ground — black on the dark themes, white on `.theme-light` and the studio themes — so the
  text over it keeps its contrast.
- **Drawing.** `visualizerBackground.ts` sets `.has-custom-visualizer-bg` on the body with the
  picture in `--custom-visualizer-bg` and the fade in `--custom-visualizer-bg-fade`.
  `custom-background.css` loads after every theme sheet and matches their
  `body.theme-x #panel-visualizer` weight, so it wins by coming later. It also clears the effect
  frame's base tint and faint icon watermark, which would otherwise sit over the picture. The
  theme's card for the effect's controls still floats over it. Only a URL of the engine's own form
  ever reaches the CSS, since the setting is shared.
- The native UI (Nano Q) does not show it.

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
