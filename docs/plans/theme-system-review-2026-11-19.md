# Theme system review (2026-11-19)

The question: how do we keep the cost of each theme low, and how do we get our themes into
Nano Q? This reviews `docs/theme-system.md` as built, against the seven themes on main
(light, dark, classic/Vintage, stage, high-gain, studio, studio-2003).

## Where it stands

| | |
|---|---|
| Theme CSS | 4,553 lines in 9 sheets under `css/themes/`, on top of 338 in `variables.css` (211 `:root` tokens) |
| Palettes | 3 (light, dark, classic). The other four are variants: `theme-light` or `theme-dark` plus their own class |
| Token overrides per theme | 79–121 `:root` tokens restated, plus 8–33 tokens of the theme's own |
| Component rules per theme | 27 (dark) to 108 (light) component selectors restyled; 179 distinct ones across all themes |
| Restyled by 5+ themes | 31 selectors, e.g. `.nav-tab`, `.preset-action-btn`, `.default-effect-shell*`, `.fx-flyout*`, `.control-bar`, `select` |
| Literal colours | 500 of the 798 colour declarations in theme component rules are literals; 79 `!important` |
| Component sheets | 773 literal colours in 44 sheets (worst: `params-panel.css` 97, `jam.css` 54, `nodes.css` 51, `resource-slots.css` 49, `effect-presets.css` 48, `tone-sharing.css` 43) |
| Theme rules outside `themes/` | 25, nearly all `.theme-light`. Component sheets are mostly theme-agnostic, which is good |
| Nano Q | `tools/gen-native-theme.mjs` exports 46 tokens for light, dark and classic only. Studio themes show as light, Stage and High Gain as dark |
| Nano Q literals | 93 `Theme::Pick(darkLiteral, lightToken)` calls and about 270 `el::rgba(...)` literals in `nanoq/src/ui` |

## What costs the maintenance

**1. Themes restyle components when they should set tokens.** `.nav-tab` is the clearest case:
six themes each restate three colours (rest, hover, active) with `!important`, because there
is no `--nav-tab-*` token. The same goes for the preset bar, effect shell, FX flyout, control
bar, inputs, toasts and toolbar buttons. Each new theme copies about 30 of these blocks, and
a layout change to any of those components has to be checked in seven places.

**2. Dark's colours are partly hard-coded in component sheets.** The doc says so already:
Stage and High Gain restate the dark blue-greys (effect panel legends, resource keys, the
chain's add keys) as neutral, and *"a new hard-coded colour in a dark-only rule needs
checking here too"*. That rule is easy to forget. The 773 literals are where those colours
live. Some are theme-neutral shadows and white/black overlays (`rgba(0,0,0,.35)`,
`rgba(255,255,255,.06)`), which are fine. The tinted ones are not (`#e0e0f0`, `#4a4a5a`,
`rgba(16,18,24,.98)`, `#6ea8ff`).

**3. The theme list is kept in about nine places.** `ThemeName`, `THEME_OPTIONS` and
`THEME_BODY_CLASSES` in `theme-switcher.ts`; the `<link>`s in `index.template.html`;
`gen-native-theme.mjs`'s `THEMES`; `Theme::Set` in `nanoq/src/ui/widgets/Theme.cpp`;
`NormalizeTheme` in `UiClientPresetHandlers.cpp`; the `theme` handler in
`UiClientLibraryHandlers.cpp`; the tests; and the thumbnails. The three C++ copies already
disagree in form (one maps studio to light inline, one in a helper, and `Theme::Set` just
dark-defaults it).

**What already works:** the base-plus-variant body classes (Stage = dark + stage, and so on),
the `--node-*` tokens (shape once in `nodes.css`, colours per theme), `components.css` as the
canonical button/tab/icon-button styles, and the Blender-generated assets. Keep all of these.
The `--node-*` approach is the one to extend.

## What blocks Nano Q

1. **The generator only reads the three palettes, and only the first `selector { }` block in
   each file.** It can't produce Stage or Studio, because those are a base block plus an
   overlay block. It takes the first stop of a gradient and the first argument of a
   `color-mix()`, which is close enough for a surface but wrong for anything mixed toward the
   accent.
2. **Nano Q's dark look is mostly literals, not tokens.** The 93 `Pick()` calls carry a
   colour *"measured from the dark web UI"*. Those are the same colours CSS hard-codes in
   component sheets, which is why the generator can't export them. Until they are tokens,
   every new palette in Nano Q falls back to dark's blue-greys.
3. **Vintage is already wrong in Nano Q** for that reason. `IsLight()` is false for classic,
   so every `Pick()` returns the dark literal, and Vintage's warm browns turn blue-grey on
   chain rows, the effect page, popovers and the DSP status card.
4. **`--check` is not wired into anything** (no workflow, CMake or npm script calls it), so
   the header only stays in sync by hand.
5. **Decoration doesn't port.** Textures, the rendered knobs, 3D keys and backdrops are CSS
   images and box-shadows. Nano Q draws with Elements and loads no images. Fonts would port:
   Nano Q already chooses a font in one place (`Font()` in `Theme.cpp`, Inter today).

## What should change

In order. Each step pays for itself on the web side, and each one gets Nano Q closer.

### R1. One theme manifest

`core/ui/themes.json` (or a `.ts` module the build reads) is the one list:

```json
{ "id": "high-gain", "label": "High Gain", "base": "dark", "scheme": "dark",
  "classes": ["theme-dark", "theme-high-gain"],
  "sheets": ["themes/high-gain.css"], "fonts": { "ui": "Oswald", "title": "Grenze Gotisch" },
  "native": true }
```

`theme-switcher.ts` builds `THEME_OPTIONS` and `THEME_BODY_CLASSES` from it. The UI build
writes the `<link>`s. `gen-native-theme.mjs` emits a name → tokens table plus
`NormalizeTheme()` into the generated header, so the three hand-written C++ mappings and
`Theme::Set`'s if-chain go away. `scheme` replaces `IsLight()`'s name test. After this, adding
a theme means editing the manifest and writing its sheets.

### R2. Separate a theme's palette from its skin

Split each theme into two layers:

- **Palette:** token values only, in a `.theme-x { … }` block. This is what Nano Q gets.
- **Skin (optional):** images, textures, fonts, extra shadows, shape changes such as
  Stage's square radii. Web only, in `x-skin.css`.

The studio themes already split this way (`studio.css` for tokens and surfaces,
`studio-controls.css` for hardware). Make it the rule for every theme. Dark, light and classic
then become palette-only, and a theme's skin sheet should hold almost no colour that isn't a
`var()`.

### R3. Make tokens of what themes restyle

Use two token tiers, as `--node-*` already does:

- **Primitives (about 30 per theme):** surface scale, ink scale, border scale, accent (+ rgb,
  hover, on-accent), semantic colours, shadow colour. A new palette sets only these.
- **Component tokens:** defaults in `:root`, derived from primitives with `var()` and
  `color-mix()`, such as `--nav-tab-ink`, `--nav-tab-ink-hover`, `--nav-tab-active`,
  `--preset-bar-bg`, `--effect-shell-*`, `--flyout-*`, `--control-bar-*`, `--field-*` and
  `--toast-*`. A theme overrides one only where its look departs from the derived default.

Migration order:

1. The 31 selectors restyled by 5+ themes. Each becomes a handful of tokens, and its 5–7
   theme blocks shrink to token lines or disappear, along with their `!important`s.
2. Dark's tinted literals that Stage and High Gain restate. Once those are tokens, the
   "check the dark-only rule in Stage and High Gain too" caveat leaves the doc.
3. The rest of the component-sheet literals, worst sheets first. Leave neutral black/white
   overlays and shadows as they are.

Add a ratchet so the debt can't grow: a lint/test that counts literal tinted colours per
component sheet (and `.theme-*` selectors outside `themes/`) and fails if a count goes up.
The repo's eslint already has custom rules; a small stylelint or node script does the same
for CSS.

Expected result: a palette-only theme is about 30–60 token lines. Skinned themes keep their
skin sheets but lose most of their colour restatements.

### R4. Generator v2

- Read every theme from the manifest and resolve the layering (`:root`, then base class,
  then variant class, every block in each file, not only the first).
- Evaluate `color-mix()` properly (srgb/oklab mixing is a few lines) instead of taking the
  first argument.
- Widen `TOKENS` to the primitives plus the component tokens Nano Q draws (R3), and emit
  `scheme` and the font names.
- Run `--check` in the UI test script and in CI.

### R5. Nano Q reads the tokens

- Replace each `Pick(darkLiteral, lightToken)` with the component token that R3 creates for
  that part. This fixes Vintage straight away, and every palette after it comes for free.
  Count `Pick(` and the `el::rgba(` literals down the same way as the CSS ratchet.
- `IsLight()` reads the manifest's `scheme`, not the name.
- Fonts: take `fonts.ui` from the tokens. Bundling Archivo and Oswald (both OFL, already in
  `css/fonts/`) gives Stage and High Gain their type in Nano Q at low cost.
- Decoration stays out for now. If it's wanted later, the cheapest piece is the knob: one PNG
  per theme under the pointer, as the web themes already draw it. Textures and backdrops
  aren't worth it on the Pi or Android targets.

With R1–R5, all seven themes show in Nano Q with their own palette and type, and the
web-only parts are confined to the skin sheets.

## Not recommended

- **Moving to a CSS-in-TS or design-token tool chain.** Plain custom properties plus the
  generator are enough. The problem is missing tokens, not the tooling.
- **Making Nano Q draw the skins.** It doubles the cost of every skinned theme for the
  smallest gain. The palette and type carry most of a theme's identity.
- **Reworking the base-plus-variant classes.** They work. R1 only stops the class lists from
  being written by hand.

## Effort

| Step | Size | Notes |
|---|---|---|
| R1 manifest | small | one file + switcher/template/generator readers; removes 3 C++ copies |
| R2 palette/skin split | small–medium | mostly moving rules between files; light.css (108 component rules) is the biggest |
| R3 tokens, first two passes | medium | 31 selectors + dark's tinted literals; pixel-compare every theme with the thumbnail capture script before and after |
| R3 ratchet | small | |
| R4 generator v2 | small–medium | |
| R5 Nano Q | medium | 93 `Pick()` sites + literals; side-by-side check per the Nano Q comparison recipe |

For R3, `tools/theme-thumbnails/capture.mjs` is the natural regression check: capture all
seven before a pass and after, and diff them. Any visible change is a bug in the migration,
since the aim is the same pixels with fewer rules.
