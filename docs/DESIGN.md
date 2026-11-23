# Design System Documentation

## Overview

Soundshed Guitar uses a token-driven, multi-theme design system built on CSS custom properties. Three themes are available:
- **Light** (default for desktop/bright environments)
- **Dark** (primary theme for low-light sessions)
- **Classic** (1970s studio gear aesthetic)

---

## Color Tokens & Architecture

### Token Hierarchy

**`:root` (Light Theme Defaults)**
- Base values used by light theme and theme-agnostic controls
- Light backgrounds (#f9fafb, #ffffff) with dark text (#111318)
- Theme files (light.css, dark.css, classic.css) provide full overrides for their respective themes

**Theme Overrides**
- `.theme-light` — Light theme (bright background, dark text, clean minimalist)
- `.theme-dark` — Dark theme (dark background, light text, high contrast)
- `.theme-classic` — Classic theme (warm brown background, cream text, vintage studio aesthetic)
- `.theme-studio` — Studio theme, applied together with `.theme-light`: the light theme drawn as
  current studio hardware (moulded plastic case and panels, 3D keys, soft shadows, rendered knobs, an LCD)
- `.theme-studio-2003` — Studio 2003 theme, applied together with `.theme-light`: the light theme drawn
  as used studio hardware (a worn steel case, brushed aluminium, rendered knobs and screws, an LCD)
- `.theme-stage` — Stage theme, applied together with `.theme-dark`: flat neutral charcoal, near-square
  corners, amber for what is selected or on, heavy grotesque type, and the effect panel floating over a
  spot-lit wooden stage
- `.theme-high-gain` — Eleven theme (its id is still `high-gain`), applied together with `.theme-dark`:
  the dark theme drawn as a high-gain amp head (black diamond plate and tolex, rendered black knobs,
  red only for what is lit, heavy condensed type, and the effect panel in front of a black grille with
  valves glowing behind it)
- `.theme-studio-1973` — Studio 1973 theme, applied together with `.theme-dark`: the dark theme drawn
  as a 1970s control room (walnut and teak veneer, cream plastic keys, turned aluminium knobs, a padded
  leather armrest, amber lamps behind black glass, and the effect panel in a walnut sleeve in front of an
  out-of-focus control room)

### Semantic Color Roles

#### Primary Accent (`--color-accent`)
- **Light:** #b5521e (warm terracotta)
- **Dark:** #ff8850 (bright orange)
- **Classic:** #f0a010 (amber, like Fender chicken-head knobs)
- Used for: active states, primary actions, highlights, key UI signals

#### Text Colors
- **Primary:** Main body text (requires 4.5:1 contrast on backgrounds)
- **Secondary:** Supporting labels, descriptions (requires 4.5:1 contrast)
- **Tertiary:** Subtle hierarchy (3:1 minimum contrast)
- **Muted:** Placeholder, disabled, or de-emphasized text (4.5:1 on light, 4:1 on dark)

**Contrast Commitments:**
- Body text (primary/secondary): **≥4.5:1** (WCAG AA)
- Muted/helper text: **≥4.5:1** (polished level; standard minimum is 3:1)
- Large text (18px+ or bold 14px+): **≥3:1** (WCAG AA)

#### Background Colors
- **Surface:** Primary content area (white in light, dark in dark)
- **Panel:** Sidebar/toolbar background (slightly offset from surface)
- **Input:** Form control backgrounds
- **Elevated:** Modals, popovers (lightest in light theme, brightest in dark)

#### Status Colors
- **Success:** #68b868 (green) — positive actions, loaded state
- **Info:** #6888a8 (blue) — informational messages
- **Warning:** #d63031 (red) — warnings and destructive actions
- **Error:** #c86868 (muted red) — error states

### Border & Divider Tokens

Three levels of border hierarchy:
- **Light:** Subtle separation (#e5e7eb in light, #2c2c38 in dark)
- **Medium:** Standard borders (#d1d5db in light)
- **Strong:** Strong separation or focused containers

---

## Typography

- **Font Family:** System font stack (inherited, no display fonts in product UI)
- **Scale:** Fixed sizes (not fluid clamp-based, as users view at consistent DPI)
  - Headings: Semantic sizes with appropriate weight
  - Body: 13px (standard), 12px (compact)
  - Monospace: System monospace for data/code (`--font-mono`)

### Line Length & Readability
- **Body text:** 65–75ch (prevents eye strain on long reads)
- **Data/tables:** 120ch+ permitted for density
- **Headers:** text-wrap: balance for even line breaks
- **Prose:** text-wrap: pretty to reduce orphans

---

## Spacing & Layout

### Spacing Scale
- Gaps follow a consistent scale (8px, 16px, 24px, etc.)
- No arbitrary gap values like 13px or 7px

### Responsive Grid
- For grids without explicit breakpoints: `repeat(auto-fit, minmax(280px, 1fr))`
- Sidebar collapse on mobile is structural, not fluid typography

### Z-Index Scale
Global layering (theme-independent):
- `--z-dropdown: 120` — Dropdowns and popovers
- `--z-popover: 900` — Popover overlays
- `--z-modal: 1000` — Modal dialogs
- `--z-modal-priority: 1100` — Priority modals

---

## Interactive State Patterns

Every interactive element should support these states:

1. **Default:** Resting state
2. **Hover:** Subtle feedback (color, background, shadow)
3. **Focus:** Keyboard focus (outline or focus-ring)
4. **Active:** Pressed/activated state (transform or color shift)
5. **Disabled:** Non-interactive appearance (opacity 0.5 minimum)
6. **Loading:** Async operation feedback (spinner or skeleton)
7. **Error:** Validation failure state (error color, message)
8. **Success:** Successful completion state (success color, icon)

### Focus Indicator
- **Appearance:** 2px solid outline
- **Color:** Theme's accent color or high-contrast variant
- **Offset:** 2px from element edge
- **Never remove:** Always provide keyboard focus visibility

---

## Themes: Technical Implementation

### Light Theme (`.theme-light`)
- Backgrounds: Off-white (#f3f4f6 to #ffffff)
- Text: Dark (#111318 to #6a7280)
- Accent: Warm terracotta (#b5521e)
- Aesthetic: Clean, minimal, high contrast for readability
- Use case: Bright environments, default desktop

### Dark Theme (`.theme-dark`)
- Backgrounds: Dark gray to near-black (#0e0e13 to #18181e)
- Text: Off-white to cream (#ebebf4 to #9fa0b8)
- Accent: Bright orange (#ff8850)
- Aesthetic: High contrast, easy on eyes in low light
- Use case: Long sessions, evening/night use, reduced eye strain

### Classic Theme (`.theme-classic`)
- Backgrounds: Warm brown tolex (#2a1f15 to #4a403a)
- Text: Cream/silk-screen (#f5f1e8)
- Accent: Vintage amber (#f0a010)
- Aesthetic: Nostalgic studio gear, flat surfaces, no gradients
- Use case: Users who value vintage aesthetics and analog feel

---

## Component Vocabulary

### Buttons
- **Primary (`.btn-primary`):** Accent gradient, white text, shadow. CTA actions.
- **Secondary (`.btn-secondary`):** Neutral gradient, dark/light text. Standard actions.
- **Ghost (`.btn-ghost`):** Transparent, text only. Inline or toolbar actions.
- **Danger (`.btn-danger`):** Red background, white text. Destructive actions.

All buttons:
- Padding: 8px 16px
- Border radius: 6px
- Font weight: 600 (bold)
- Transition: 0.2s ease on color, background, border, shadow

### Form Controls
- **Inputs:** Solid background (white/input token), dark text, bordered
- **Placeholders:** Muted text color, 4.5:1 contrast minimum
- **Validation:** Red borders for errors, green for success
- **Labels:** Always paired; required indicators consistent (e.g., `*` or "(required)")

### Modals
- **Backdrop:** Dark overlay (`--overlay-darker`)
- **Surface:** Modal token background with subtle gradient (dark theme)
- **Close button:** Ghost style, clear on hover
- **Focus trap:** Keyboard nav contained within modal

---

## Motion & Transitions

- **Duration:** 150–250ms for most transitions (users are in flow, don't wait for choreography)
- **Easing:** Exponential curves (ease-out-quart, ease-out-quint, ease-out-expo)
- **No bounce/elastic:** Feels dated
- **Reduced motion:** Every animation must have a `@media (prefers-reduced-motion: reduce)` alternative (typically instant or crossfade)
- **Reveal animations:** Must enhance already-visible default; never gate visibility on transitions

---

## Accessibility (WCAG 2.1 AA Baseline)

### Contrast
- Body text: 4.5:1 minimum
- Muted/helper: 4.5:1 (polished standard)
- Large text (18px+ or bold 14px+): 3:1 minimum
- Non-text contrast: 3:1 minimum (borders, icons, UI components)

### Focus & Keyboard
- Focus indicators always visible (2px outline, 2px offset)
- All interactive elements keyboard-accessible
- Tab order logical and intuitive
- No keyboard traps

### Color
- Never rely on color alone to convey state or information
- Use icons, text, or patterns alongside color
- Support all three themes without content loss

### Images & Icons
- All meaningful images have descriptive alt text
- Icons paired with text labels when semantically important
- SVGs have proper ARIA labels or `<title>` elements

---

## Common Patterns

### Browser/Library Panels
- Unified "browser" aesthetic across preset, effect, and resource browsers
- Semi-transparent surfaces for layering depth
- Hover states with accent tint
- Selection feedback with accent background

### Signal Path Nodes
- Node coloring: input (green), output (blue), neutral (gray)
- Selected state: border + glow with accent color
- Hover: subtly brightened
- Drag feedback: distinct visual change (highlight or shadow)

### Effects Visualization
- Background: controlled via `--effect-visual-bg`
- Icon overlay: faded (opacity 0.08–0.12)
- Placeholder copy: uses muted text color
- Theme support: dark gradient in dark theme, flat light in light theme

---

## Maintenance & Updating

### Adding a New Color Token
1. Define in `:root` (light theme default)
2. Override in each theme file (.theme-light, .theme-dark, .theme-classic)
3. Document the semantic role (e.g., "Status color for warnings")
4. Test contrast in all themes (DevTools, WebAIM Contrast Checker)
5. Update this file with the new token and its use cases

### Testing Contrast
- Use [WebAIM Contrast Checker](https://webaim.org/resources/contrastchecker/)
- DevTools: Right-click element → Inspect → Accessibility panel
- Never ship with contrast < 4.5:1 for body text or muted labels

### Theme Switching
- Implemented via `.theme-{name}` class on `<body>` or root element
- All token overrides are CSS custom properties (no global re-rendering needed)
- Persistence: handled in `theme-switcher.ts` and stored in localStorage

---

## Related Files

- **CSS:** `core/ui/css/` — All styles, organized by component
  - `variables.css` — Master token definitions
  - `themes/` — Per-theme overrides
  - `components.css` — Canonical button, tab, form styles
  - `base.css` — Global reset and defaults
- **TypeScript:** `core/ui/ts/theme-switcher.ts` — Theme selection logic
- **HTML:** `core/ui/index.html` — Theme class binding and structure

---

## Next Steps for Polish

- [ ] Verify all interactive components have full state coverage
- [ ] Audit spacing consistency across all surfaces
- [ ] Standardize focus-visible behavior (currently inconsistent in legacy areas)
- [ ] Simplify Settings/Library UI to reduce cognitive overload (distill pass)
- [ ] Review "vintage" surface styling (currently over-layered with texture + blur + gradient)
