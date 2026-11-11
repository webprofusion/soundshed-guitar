# Compact UI review: web and Nano Q, iPad size and smaller

Status: **reviewed 2026-11-11 at `9869b6db` plus the working tree; the blocking faults (step 1 of
"Suggested order") fixed the same day, uncommitted. See "Fixed so far".** Everything else is
open. One build fix was needed to run Nano Q (see "How it was reviewed").

## Fixed so far (2026-11-11, uncommitted)

| Item | What changed | Checked |
| --- | --- | --- |
| C1 Scale Guide on small landscape screens | Compact rules in `css/compact/panels.css`: the panel scrolls as one, the controls take two rows, the neck fits the width on the rail and is no taller than the stage leaves | 844x390 shows the whole neck; 667x375 shows about 100 px of it and scrolls to the rest; Arpeggios at 667x375 shows only the top of the neck before scrolling |
| C2 Practice Tool overlap | `.practice-tool-body` no longer shrinks below its content (`css/practice-tool.css`). The overlap was there at full density too (the body was 0 px high at 1024x768) | No overlap at 1024x768 (both densities), 800x480, 844x390, 768x1024, 1366x1024 |
| C3 Preset library off the bottom | Full density: the popover stops short of the bottom, and the tree and list give up height so the Setlists bar stays in view (`css/preset-library/library.css`) | 1024x768, 1180x820, 1366x1024; compact sizes measure the same as before |
| C4 Looper strip clipped | Full density below 1240 px wide: the modes and Layers go to a second row (`css/looper.css`). The strip is then 99 px tall, so the editor above it scrolls sooner | Nothing past the right edge from 981 to 1400 px wide |
| C7 Nano Q Global EQ at 800x480 | The band cards size to the dialog (four across down to about 770 px wide, two rows of two below), and the plot gives up height on a short window (`effects/EqDialog.cpp`, `ParamKnob::CellWidth`). The window no longer grows | 800x480 shows everything without scrolling; 640x400 scrolls two rows of two; 960x600 as before |
| C9 Nano Q dialogs that scroll (found with C7) | A dialog too tall for the window gave its scrolling body half the height and left the rest empty (`dialogs/Dialogs.cpp`). This was the blank body under the EQ curve and the Save As body cut through a field | Global EQ and Save As at 800x480. The on-screen keyboard over Save As was not rechecked |
| C8 Nano Q preset menu at 1024 wide | The wide header drops the NEW and PUBLISH labels when its middle is too narrow for them, as the web UI does, and the top bar is looked at again on a resize (`shell/WideFrame.cpp`, `shell/Shell.cpp`) | The menu opens at 1024x768; the labels return at 1180 |
| E Nano Q on iOS: one-finger scrolling | Drag scrolling is on for iOS as it is for Linux (`widgets/DragScroll.cpp`) | **Not built or run for iOS** (no Mac here). The same path was already in use on the kiosk |
| E Nano Q on iOS: typing | The on-screen keyboard is on for iOS (`input/OnScreenKeyboard.cpp`); `SOUNDSHED_NANOQ_KEYBOARD=0` turns it off | **Not built or run for iOS** |

`npm run verify` passes for the web changes. Nano Q was rebuilt and driven on Windows only.

The question: the compact UI is meant for iPad screens and smaller, should stay consistent with
the full UI where it can, and should lose clutter gracefully as the window shrinks. Where does it
still feel clunky?

## Summary

1. **An iPad does not get the compact UI.** Both iOS apps are landscape-only, so an iPad is
   1024x768 to 1366x1024, and the rule (compact at or below 980 wide or 620 high) gives every one
   of them the full shell. The full shell has no touch sizing: every `(pointer: coarse)` rule is
   scoped to compact. Pinning compact at that size gives the phone layout stretched over a tablet,
   with about 70% of the stage empty. Tablets sit in a gap between the two layouts.
2. **Compact removes things instead of tucking them away.** The looper loses Save, Clear, Redo and
   its modes; the preset back/forward trail is gone; Copy A to B disappears below 680 px. None has
   another route.
3. **Some content cannot be reached at small sizes.** The Scale Guide neck on a landscape phone,
   the Global EQ bands in Nano Q at 800x480, the preset menu in Nano Q's wide frame at 1024 wide,
   and a few overlaps and clipped strips.
4. **Touch targets are 16 to 32 px in both UIs**, and labels are 8 to 10 px. The web UI's own floor
   is 32 px; Nano Q has no touch mode at all. The bars have room for larger hit areas at no cost.
5. **Nano Q on an iPad cannot be scrolled with one finger and has no way to type** without a
   hardware keyboard (read in code, not tried on a device).
6. **Short screens show too little of each list.** The preset library shows one preset at
   667x375.

## How it was reviewed

- **Web UI:** Release Standalone built from the working tree, driven over CDP with viewport and
  touch emulation (`mobile: true`, so `(pointer: coarse)` matches), on a throwaway profile
  (`APPDATA` pointed at a temp folder). 365 captures across 1024x768, 1180x820 and 1366x1024 (auto,
  which is full density), the first two pinned compact, 844x390, 667x375, 800x480, 768x1024 and
  390x844. Each capture has a probe: overflow, targets under 40 px, text under 11 px, truncation.
- **Nano Q:** Release standalone on a throwaway profile, driven with DevDriver. 163 captures at
  960x600, 800x480, 1024x600, 640x400 (compact), 1024x768 and 1180x820 (wide) and 1024x768 pinned
  compact.
- **Code:** three read-only audits (web shell, web panels, Nano Q frame), with the main claims
  rechecked by hand.
- **Not done:** nothing ran on a real iPad, phone or Pi. iOS findings for Nano Q come from
  `nanoq/ios/elements_host/base_view.mm`. Sizes marked "measured" come from the probe or the
  captures; "read" means from code.
- **Build fix:** Nano Q did not compile with MSVC 19.52 (the VS 18 Insiders toolset):
  two range-for loops over a braced list with a structured binding captured in a lambda, in
  `nanoq/src/ui/shell/ShellFrame.cpp`. Rewritten as plain arrays. Uncommitted.
- Captures and manifests are in `D:\t\compact-review\shots` (not in the repo).

## Where each size lands

| Size | What it is | Web | Nano Q |
| --- | --- | --- | --- |
| 1366x1024 | iPad Pro 12.9 | Full. Chrome 212 px, editor has room | Wide |
| 1180x820 | iPad Air, Pro 11 | Full. Looper strip clipped 19 px at the right | Wide. Stage 606 px high |
| 1024x768 | iPad 9.7, mini | Full. Chrome 212 px, chain 148, editor 408. Looper strip clipped 107 px; preset library runs 47 px off the bottom | Wide. Chrome 214 px; the editor gets about 265 px and scrolls; preset menu button unreachable |
| 1024x768 pinned compact | | Rail, staged. Stage 964x685; the amp editor uses 196 px of it | Rail, staged. Stage 964x696; about 400 px empty |
| 1024x600, 960x600 | Short windows, Nano Q default | Rail, staged | Rail, staged. Stage 528 px high |
| 800x480 | Pi touch panel | Rail, staged. Stage 740x397 | Rail, staged. Stage 740x408 |
| 844x390, 667x375 | Phones, landscape | Rail, staged. Stage 308 and 293 px high | Below Nano Q's 640x400 minimum |
| 768x1024 | Portrait or a snapped window | Stack, split. Chain 254 px, editor 196 px, 455 px empty below | Rail, staged (no stack or split) |

## Findings

### A. The layout ladder has a missing rung

The full shell at 1024x768 spends 212 px on chrome and has mouse-sized controls. The compact
shell at the same size spends 83 px on chrome and then shows either the chain or the effect, each
alone on a 685 px stage it cannot fill. Neither is the tablet layout.

What exists already is close. `data-compact-split` shows the chain over the effect, as the full
shell does, but only for a stacked (portrait) window of 600x800 or more.

- **A1. Let the rail layout split too.** When the stage is tall enough (about 600 px of viewport
  height), show the chain over the effect in the compact shell. That gives 1024x768 a layout that
  reads like the full UI with the compact shell's smaller chrome, and keeps the staged
  Chain/Effect turn-taking for phones and 800x480. Nano Q needs the same; it has the rail only.
- **A2. In split, draw the editor as the full shell does.** Today split shows the phone editor
  (34 px knobs, no equipment picture, left-aligned) with hundreds of empty pixels under it. With
  room to spare it should use the full shell's editor: the picture, the larger knobs, centred.
- **A3. Size for touch by pointer, not by density.** Move the `(pointer: coarse)` rules out from
  under `html[data-density="compact"]` (`css/compact/shell.css:566`, `:722`, `:734`,
  `effect-panel.css:390`, `panels.css:80`, `:298`, `rail.css:265`). Then a tablet on the full
  shell gets them too. Nano Q needs a touch setting or platform flag that does the same.
- **A4. Choose what an iPad gets by default.** Recommended: on a touch device, Auto picks the
  compact shell with split up to about 1200x850 (iPad 9.7, 10.2, mini, Air, Pro 11) and the full
  shell above that. A mouse-driven window keeps today's thresholds. If the full shell is kept for
  iPads instead, A3 and the three overflow bugs in C become the priority.
- **A5.** The shell flips completely between 620 and 621 px of height at any width. Once split
  looks like the full layout, that change stops being jarring.

### B. Functions that disappear at compact density

| | Web | Nano Q | Suggested |
| --- | --- | --- | --- |
| Looper Save, Loops, project name | Hidden (`css/looper.css:507-510`) | Loops and Layers kept; Save dropped (`performance/LooperStrip.cpp:220-234`) | A "..." on the strip opening the looper's Options, with Save in it |
| Looper Redo, Clear, modes (Press/Armed, Free/Bars, Click) | Hidden (`css/looper.css:348-354`) | Dropped; the comment says the modes are "in Options too", but Options is wide-only | Same sheet |
| Preset back/forward | Hidden (`css/compact/shell.css:233`). The comment says there is a shortcut and a footer control; there is neither | Only in the wide frame (`shell/WideFrame.cpp:216-218`) | Two entries in the preset "..." menu |
| Copy A to B | Hidden at 680 px wide or less, by width (`css/footer.css:558`) | Kept | Drop the rule: the bar has room |
| Source badge (Factory/User) | Hidden | Hidden | Keep a small badge: it explains a disabled Save |
| Tap tempo | In Options | Only inside the Metronome dialog | Fine in Options; add it there in Nano Q |

### C. Content that cannot be reached, or is broken

Web:

- **C1. Scale Guide on a landscape phone.** The controls fill the stage and the neck starts under
  the footer. The panel overflows by 148 px at 667x375, 133 px at 844x390 and 43 px at 800x480,
  and no ancestor scrolls. The Scale Guide has no compact rules; the neck is `min-width: 680px`.
- **C2. Practice Tool at 1024x768 compact.** The Balance slider and the ones after it draw
  underneath the Links section.
- **C3. Full shell, 1024x768: preset library** runs 47 px off the bottom; the Setlists bar is cut.
- **C4. Full shell, 1180 wide or less: looper strip** is clipped at the right (107 px at 1024),
  with no wrap or overflow. Open at 1024x768 it also cuts the editor's knob values.
- **C5. Multi-Rig list** overflows its popover by 49 px (844x390) and 83 px (667x375).
- **C6. Metronome** is taller than a phone screen; the whole card scrolls, so its close button
  scrolls away and the beat dots start below the fold. The Global EQ shows only its curve first.

Nano Q:

- **C7. Global EQ at 800x480.** Opening it widened the window to 854 px, where it stayed after
  closing. The body under the curve is blank and does not scroll, so the bands cannot be reached.
  Switching to compact at 1024x768 widened the view to 1036 the same way. On a fixed panel that is
  clipping. Cause not investigated.
- **C8. Wide frame at 1024 wide: the preset "..." button sits under the EQ cell** and does not
  open. It works at 1180. Export, import, delete and the rest of that menu are out of reach on a
  1024 iPad.
- **C9. Save As at 800x480.** The scrolling body is cut through the Preset Name field with a blank
  band above the buttons. With the on-screen keyboard up, the keyboard covers Cancel and Save and
  half the name field.
- **C10. 640x400:** the Controls strip runs off the right edge (mute is cut).
- **C11. Model browser at 800x480:** the search field is squeezed to "Searc".
- **C12. Wide frame at 1024x768:** the FX Library extends over the footer.
- **C13 (read, not seen).** The stage height sums ignore the looper strip (46 px) and the mixer
  tabs (26 px) (`chain/ChainStrip.cpp:657`, `effects/EffectPage.cpp:343`), so content may clip
  when both are showing.

### D. Touch sizes and text

Measured in the web UI with a coarse pointer; read from code for Nano Q. Nothing here is 44 px.

| Control | Web compact | Nano Q compact |
| --- | --- | --- |
| Preset bar buttons | 28-32 | 28 |
| Footer buttons | 32 | 26 |
| A / B | 24x27 | 24x21 |
| CONTROLS, OPTIONS | 76x28, 64x32 | 76x28, 64x26 |
| Node bypass and remove | 16x16, no larger hit area | 16, hit area 24 |
| Scene chip, its x, add | x 22, add 26 | chip 18 high, add 20x18 |
| Floating + | 38 | 24 |
| Section chips | 29 high | 30 high |
| Controls sheet | mute, gate and status buttons about 30x26 with their hit areas | switches 22x12, drop-downs 70x16 |

- The web UI's stated floor is 32 px (`css/compact/shell.css:18`, `:559-581`). Nano Q has nothing
  that enlarges a hit area.
- **There is room.** The compact web footer at 844 wide holds seven controls in 784 px. Hit areas
  can fill the bar's height and be at least 44 px wide without changing how anything looks.
- The node corner buttons are the hardest: 16 px, on the card the user is trying to tap. On touch,
  give them a 32 px hit area, or rely on the editor's ON switch and a Remove in the node menu.
- **Text:** rail labels 9 px, CONTROLS 9, OPTIONS 10, node type and the A2 badge 8, knob names 9,
  values 10, the ON/OFF switch text 7, Tones card buttons 9. Nano Q is the same family (rail 9,
  badges 8, knob names 10). Suggested floor: 11 px for anything read in order to operate a control,
  10 for badges.

### E. Nano Q touch input (read in code; iPad not tried)

- **One finger does not scroll on an iPad.** The pan recogniser needs two touches
  (`nanoq/ios/elements_host/base_view.mm:323-324`) and drag scrolling is on for Linux only
  (`widgets/DragScroll.cpp:18`). Turn drag scrolling on for iOS.
- **No typing on an iPad without a hardware keyboard.** The on-screen keyboard is armed only by
  `SOUNDSHED_KIOSK` or `SOUNDSHED_NANOQ_KEYBOARD` (`input/OnScreenKeyboard.cpp:104-112`), and the
  view does not take UIKit text input. Search, Save As and rename are dead. Arm it on iOS.
- **Right-click-only actions.** The node menu (Move earlier/later, Replace, Turn off), MIDI Learn
  and scene rename. Long press exists only on iOS, so a Pi panel or a Windows touch screen has no
  way in, and Settings still says "Right-click a knob for MIDI Learn". Double tap to replace
  cannot work in compact: the first tap moves to the Effect page.
- **An unseen bypass corner.** The power corner of a card takes a tap even while it is not drawn
  (`chain/NodeCard.h:591`), so a tap near that corner bypasses the effect. The web UI fixed the
  same thing for touch (`css/signal-path/nodes.css:636-640`).
- The Options popover closes after every action, including starting the metronome.

### F. Lists and dialogs on short screens

- **Preset library at 667x375 shows one preset.** The tag row, three folder rows, the folder
  tools and the Setlists bar take the rest. On a short screen: the list first, folders behind a
  drop-down, the folder tools behind a "...", and Setlists as a third tab beside Presets and
  Multi-Rig.
- **Model browser:** about 2.5 rows at 667x375 (web), 4 at 800x480 (Nano Q); the filters take two
  rows.
- **Options sheet (web):** four rows of icons with no labels, and it repeats Pads and Tuner, which
  are already on the bar. Nano Q keeps Record, Loop, tempo and the metronome on the bar, which the
  web footer has room for. Follow Nano Q, and leave Options the rarely used items with labels.
- **Add effect** covers the preset bar and the chain it adds to, but reads well. At 768x1024
  (split) it opens the FX Library instead, in three narrow columns with names cut to "Neur...".
- Fine as they are: Tones on a phone (web), Settings, the Controls sheet.

### G. Consistency

| | Full | Web compact | Nano Q compact |
| --- | --- | --- | --- |
| Navigation | PLAY, TONES, JAM, gear | Rail: Chain, Effect, Tones, Jam, gear | Rail: Chain, Effect, Setlist, Tones, Jam, gear |
| Layouts | | Rail, stack, split | Rail only |
| Footer | Everything | Pads, Tuner, undo/redo, A/B, copy, OPTIONS | Adds Record, Loop, tempo, metronome play, demo (760 and up) |
| Editor width | Centred with the picture | Amp full width, gate a centred 636 px column | Amp and EQ full width, reverb a narrow card at the top centre |
| Section navigation | Side lists | Chips in Tones, Jam, Settings; side lists in FX library and folders; Scale Guide has its own underlined tabs under Jam's chips | Chips |
| Close | | x at 30 px (dialogs), x at 28 px (library), a "Close" button (FX library) | One dialog chrome |

Pick one rule for the editor (centred, one maximum width) and one close control, and give Nano Q
the same footer and rail as the web UI, or the other way round where Nano Q's is better (the
footer is).

### H. Smaller points

- Group titles repeat their only knob ("SWELL" over "SWELL"), and "OTHER" is not a name.
- Nano Q EQ labels run together ("LOW-MID GAIN LOW-MID FREQ"); drop the band prefix inside a
  titled group. The Global EQ dialog lays its bands out differently from the node's EQ.
- Nano Q empty states are one small line on a blank page; Pads says "No setlist selected" twice.
- Nano Q Save As fields are about 22 px tall beside a 32 px drop-down.
- The full web footer cuts the demo clip's name ("Guitar Riff (") at 1024 wide.
- Adding a preset to a setlist is drag-only in the web UI.
- From the code audit, not rechecked: the pre-paint script in `index.template.html` ignores UI
  zoom and a pinned density, so the first frame can be the wrong shell; several web dialogs have
  no Escape handler, which matters with an iPad keyboard.

## Suggested order

1. Blocking faults: C1, C7, C8, C2, C3, C4, and E's scrolling and typing on iPad.
2. Put back what compact drops: B (looper sheet, preset trail, Copy A to B).
3. The ladder: A1 to A4, web first, then Nano Q.
4. One touch pass over both UIs: D, plus E's hidden corner and right-click-only actions.
5. Short-screen lists and the consistency table: F and G.

## What already works well

The rail on a landscape phone; the effect page at 844x390; the web add-effect chooser; Tones at
compact size; the Controls sheet; the web UI's rules for hover-only reveals on touch; Nano Q's
compact footer.
