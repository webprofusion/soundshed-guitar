# Nano Q against the web UI: second gap review

Status: **in progress, 2026-11-27.** Reviewed at `793f2617`. Follows
[nano-q-ui-review-2026-10-29.md](nano-q-ui-review-2026-10-29.md), whose 21 items are all done, and
[nano-q-parity.md](nano-q-parity.md). Anything those two list as done, left out on purpose, or
acceptable is not repeated here unless users still see it.

This pass goes further than the last one. Every effect editor was compared in both UIs, and so was
every surface outside the editor. The pass also looked closely at alignment, sizes, styling, order,
accessibility and the visualisations for each effect. The web UI is the reference throughout.

How to read an item:
- Each item gives the web reference, the Nano Q reference, the change, and a size: **S** (hours),
  **M** (a day or two) or **L** (several days).
- Paths: web paths are under `core/ui/`, Nano Q paths under `nanoq/src/ui/`, and shared client
  paths under `core/src/uiclient/`.
- **Live** means the item was seen side by side in the running apps. **Code** means it was found
  by reading both sides and has not been seen live.

## Outcome

Worked through in batches of related items, one commit per batch on main.

| Item | Commit | What changed, and what was left out |
| --- | --- | --- |
| 1 | `818da269` | Backdrop layouts lost their controls: Elements culled them against port bounds that the scale transform never moved (`LayoutView.cpp`, `LocalPort`). `defaultControlsArea` is parsed, and the standard controls are fitted into it, the tab row with them. Checked live on Chaos Engine and Verbal |
| 2 | `818da269` | Practice Tool projects keep every field Nano Q does not edit (`tab`, `youtube`, anything newer), YouTube-only projects are read (and say they open in the web UI), and saving over a project keeps them while it follows the same track. `JamPracticeToolTests` covers it |
| 3 | `818da269` | Use Multiple CPU Cores defaults to on in the standalone app only |
| 4 | `818da269` | Root cause: the engine sends scenes without a working graph, and the client kept showing the base graph. `ClientState::UseActiveSceneGraph` takes the active scene's graph on `state`, `presetLoaded` and `presetSaved`, as the web's `selectPresetScene` does. `UiClientTests` covers it; checked live in Solo |
| 5 | `818da269` | The plugin's device text, the Channels hint (the web's wording), the Multi-Rig empty text (+ Mixer), the product name (Soundshed Guitar as its native editor) and About's Interface line and Soundshed Guitar note |
| 6 | `818da269` | Nominal Level slider removed |
| 7 | `818da269` | Folder files show `metadata.sampleRate`, falling back to `sample_rate` |
| 8 | none | Not a bug: an unsaved new preset has no library entry, so Save already opens the Save dialog, named as the preset and filed in the open folder |
| 9 | `818da269` | A confirm or prompt opened over a dialog or popover stacks on it (`ShowOverlay`, `DismissOverlay`). Cancel, Esc and a tap outside go back to it; the confirming button closes both, as before. Checked live: New folder from the preset chooser |
| 10 | `c896be31` | Signal Analyzer and Graphic EQ size as every other editor (content width, at least half the stage); the analyzer's tiles wrap to it, and the graphic EQ asks for its bands in one row |
| 11 | `cc2aa398` | The cards follow the web's container widths: over 700 px the blend stands between them, over 520 px the cards share a line with the blend in a row under them, narrower everything stacks. Each card's knobs wrap as the card's width leaves room (Level, Pan, a rule, Radial, Proximity). The fade was there; it shows (0.4) |
| 12 | `cc2aa398` | An empty slot is a dashed accent drop zone: "+ Choose an IR" (or a model), the file a drop takes, and an accent Browse where the prompt still fits beside it; the clear goes. IR B reads "+ Add a second IR", quietly |
| 13 | `c896be31` | The gain thumb is a round accent dot. The number boxes were already there (underlined, typed in place); left as they are |
| 14 | `c896be31` | Title 11 px; subtitle 8 px capitals at 0.12 em (`Text::Tracking`, `FillTrackedText`) |
| 15 | none | Already there: values bold over smaller labels, and the "Updated" line. The "Waiting" capture was the demo audio not playing |
| 16 | batch 2d | A composite or Custom Effect draws one slot per exposed resource, with its own name: the type's slots, then a Custom Effect's `resource.<id>.*` declarations from its descriptor (`uiclient/ExposedResources`). A composite's slots are numbered in declaration order, as the web's. `UiCommands` names the slot (`exposedResourceId`) on every set, clear, file and browse, so each pick lands in its own slot. A module or data slot opens a menu of the library's files of its kind and "Browse for file..." (when the slot allows it). Custom layouts place them by id (`__resource__:<id>:<i>`). `UiClientTests` covers the slot list. Checked live: Supercharged Neural Amp's Amp Model and Cab IR keep their own picks; Custom Effect's Module menu. Left out: artwork from an exposed resource, and a module that declares files (no sample does) |
| 17 | `1644187b` | The Mixer's Input Channels: a group per connected input port, named for the effect feeding it, with Level (-60..12 dB), Pan and Delay (0-60 ms) and a green/red Mute; "No inputs connected…" with none. The page is rebuilt when what feeds a port changes |
| 19 | `cc2aa398` | The header's peak rail between the titles and the controls, green to red over -48..0 dBFS, faded with no reading, glowing while clipped; a tap shows or hides the DSP status. Hidden below the 900 px stage, as the web's. Not seen with signal: the test profile had no input device |
| 20 | `c896be31` | A blend-unit param (Simple Cab's Mic Blend) is an A-B slider reading A, B or the share of B. The IR cards keep their knob, as the web's do |
| 21 | `1644187b` | The enum picker as the web's: a trigger with the choice's picture, name and kind, and a flyout with "Search N choices", the groups as tiles with counts (narrowed by the search), subgroup headings, and a picture and detail line on each choice. ClientState carries labelInfo's icon and detail. The pickup pictures are drawn natively from the web's set. Checked live on Pickup Match |
| 22 | `c896be31` | With no main params every param shows with no tabs; each node opens on Main |
| 23 | `c896be31` | DSP status floats in the controls' corner; the IR Cabinet's Full Rig note; a custom file's path on the slot's label line; Plugin Host: inline load error, paths, Remove (confirm, file kept), the custom-file row; splitter hint as the web's; Presets chip whenever the type has params, Splitter and Mixer too; art gap 16 and a 52 x 34 thumbnail when the stage is too narrow for the column. Left out: a spinner beside "Loading plugin..." (the text says it). "Back to one lane" stays |
| 25 | `c896be31` | Web fix: Pitch Tracking EQ is no longer an EQ node, so it gets no curve |

## How it was compared

- **Web UI:** the Release Soundshed Guitar standalone, driven over CDP (port 9333), with the
  viewport emulated at Nano Q's size: 1472 x 877 for the full layout, 960 x 600 for compact.
- **Nano Q:** the Release `nanoq_standalone` at HEAD, driven with the DevDriver (`click`, `key`,
  `shot` and the `cmd` verbs).
- **Same data in both:** both apps used one throwaway profile, so they read the same presets,
  library and settings. The preset was Black Album (Full Rig), with scenes Rhythm and Solo.
- **Effect sweep:** a chain of one slot was set to each of the 49 effect types in turn, then
  captured in both apps. Nano Q's `cmd replace` indexes the catalog in GUID order, so the two
  apps' types were matched by GUID.
- **Surfaces:** 13 surfaces were captured in both apps: settings, preset chooser, + menu, theme,
  tuner, pads, MIDI, the EQ dialog, the gate flyout, input mode, the New menu, demo actions and
  the metronome.
- **Code review:** four read-only reviews were run in parallel (chain and shell, effect editors,
  pages and dialogs, accessibility). Where a review and a capture disagreed, the capture was
  trusted.
- **Evidence:** the screenshots are in `D:\t\rev` and are not in the repo:
  - `web\` and `nq\` hold the per-effect captures.
  - `pairs\` holds the side-by-side effect sheets.
  - `surfpairs\` holds the side-by-side surface sheets.

## Fix first: bugs

These are bugs, not differences in look. Fix them before any of the parity items.

1. **Effects with a backdrop layout have no controls in Nano Q. Live, M.**
   - Chaos Engine (`core/ui/assets/layouts/chaos-engine/layout.json`, `useDefaultControls: true`,
     offset 126,102, scale 0.7) draws its artwork and nothing else. Scrolling doesn't help.
     Neither does widening the window. The effect can't be edited in Nano Q at all.
   - The same happens to the new Verbal layout (`core/ui/assets/layouts/verbal/`, untracked). That
     layout also uses `defaultControlsArea` (970 x 464). `uiclient/EffectLayouts.cpp:367-369`
     parses `useDefaultControls`, `defaultControlsOffset` and `defaultControlsScale`, but not
     `defaultControlsArea`.
   - The path to look at: `effects/EffectPage.cpp:1655-1677` builds `standard` with
     `layRows(layoutWidth)` and passes it to `BuildEffectLayout`. `effects/LayoutView.cpp:599-610`
     then adds it through `Scaled(...)` at the offset. Somewhere between those two the block comes
     out empty or isn't drawn. Suspects:
     - `Scaled` inside `PlacedLayer`;
     - rows wrapped at the full layout width, where the web wraps at `defaultControlsArea`.
   - Change:
     - find where the controls are lost and fix it;
     - parse `defaultControlsArea` and wrap the controls at that width, as `layoutRenderer.ts`
       does;
     - add a DevDriver or unit check that a backdrop layout produces focusable controls.
2. **Nano Q deletes Practice Tool data the web UI saved. Code (checked), M.**
   - `uiclient/PracticeTool.cpp:374` (`ParseProject`) rejects a project whose `filePath` is empty.
     That drops YouTube-only projects, which the web allows (`ts/practiceTool/types.ts`: `filePath`
     "is then empty unless a backing track was also loaded").
   - `ProjectJson` (`:454-504`) writes no `tab` or `youtube` fields.
   - Every Nano Q save, delete or link edit rewrites the whole shared `practiceTool.projects` list
     (`:1415`). That permanently loses YouTube-only projects, every project's tab binding and every
     sync point.
   - Change:
     - keep unknown fields when reading and writing;
     - accept projects that have only a YouTube video;
     - add a shared test vector that both UIs read.
3. **"Use Multiple CPU Cores" shows the wrong state in a plugin. Code, S.**
   - `settings/SettingsPage.cpp:335` defaults both keys to `true`. The web
     (`ts/settings/multiCore.ts:251-254`) and the engine (`SettingsKeys.h:58-59`) default the
     plugin key to false.
   - In a DAW with nothing stored, the switch shows On while the engine runs on one core.
   - Change: default it to `environment.standalone`.
4. **Removing an effect in the Solo scene did nothing. Live, root cause not found.**
   - Seen live:
     - With the Solo scene active, `cmd remove` and the card's own remove button marked the preset
       dirty, but the chain didn't change.
     - After switching to Rhythm, the same remove worked.
   - What is ruled out:
     - Nano Q sends the same `deleteSignalPathNode` as the web (`uiclient/UiCommands.cpp:459`).
     - The engine's handler (`PluginControllerSignalPath.cpp:1546`) edits the active scene's graph
       through `ResolveEditTarget()`. It then runs `SyncActivePresetSceneGraph()` and
       `ApplyActivePresetInItsSlot()`.
   - Change: repeat this in the web UI first. If the web also fails, it's an engine bug in the
     scene path. If not, compare the node ids each UI sends while a non-base scene is active.
5. **Wrong or stale text. Code, S.**
   - In a plugin, Settings still points at "Alt+Space" for the audio device
     (`SettingsPage.cpp:293`). In a plugin the host chooses the device.
   - The Channels hint says "stereo, left, right" (`:301`). The real choice is Follow / Mono.
   - Help/About says riff capture is only in Soundshed Guitar (`:550-551`), but riffs are ported.
   - The Multi-Rig empty text points at "a preset's menu" (`chain/MultiRig.cpp:325-327`). Cards
     have "+ Mixer".
   - The Settings product name is always "Soundshed Nano Q", and About says "Interface: QPlug and
     Elements" (`:237`, `:541-543`), even when Nano Q is Soundshed Guitar's native editor.
6. **An extra "Nominal Level" slider. Code, S.** It is at `SettingsPage.cpp:408-409`. The web
   control is commented out (`settings-performance.html:77-81`). Remove it.
7. **Folder-file Hz never shows. Code, S.** `library/ResourceBrowserParts.h:131-164` reads
   `sample_rate`, but the field is `metadata.sampleRate`.
8. **A new, unsaved preset saves without asking for a name. Code, S-M.**
   - The web opens "Save Preset" and files the preset in the open folder
     (`ts/presets/actions.ts:126-128`, `saveModal.ts:31-52`).
   - Nano Q saves at once (`shell/Menus.cpp:240-258`). The engine already sends `created: true`
     (`PluginControllerPresetEdits.cpp:181`), but uiclient ignores it.
9. **Confirms opened from a dialog strand the user. Code, M.**
   - `ShowOverlay` closes the open dialog first (`dialogs/Dialogs.cpp:30-32`). A confirm or prompt
     opened from a dialog therefore replaces it. Its Cancel (`:305-306`) leaves the user on the
     page, with the parent dialog gone. Examples:
     - Riff Library delete (`RiffDialogs.cpp:501`);
     - Loops Open and Delete (`LooperStrip.cpp:647-651`);
     - effect preset delete (`EffectPage.cpp:1827`).
   - The web stacks `.modal-interrupt` over the open modal.
   - Change: stack overlays, or at least reopen the parent dialog on Cancel.

## Seen live: visual differences in the effect editor

10. **Editor width. Live, S.**
    - Web (`css/signal-path/params-panel.css:8-21`): the panel is as wide as its content, at least
      50% of the stage, and centred.
    - Nano Q stretches some editors across the whole stage. Graphic EQ spreads its five bands over
      about 1400 px where the web's are about 700. Signal Analyzer puts nine tiles in one row where
      the web wraps four per row.
    - Change: apply the content width and 50% minimum to every editor, including the analyzer and
      graphic EQ bodies.
11. **IR Cabinet layout. Live, M.**
    - Web:
      - BLEND, L/R SPLIT and MICS sit in one row of their own under the IR A and IR B cards.
      - Each IR card has LEVEL, PAN and RADIAL in a row, with PROXIMITY under them.
      - Radial and Proximity are clearly dimmed while Mics is off.
    - Nano Q (`effects/EffectPage.cpp:62-79, 352, 463`):
      - It puts BLEND, L/R SPLIT and MICS in a narrow column between the two cards.
      - It draws the four knobs in one row with a divider.
      - It has a fade for these knobs (`:352`), but in the capture they look no dimmer than Level
        and Pan.
    - Change: use the web's arrangement, and make the fade as strong as the web's.
12. **Empty model or IR slot. Live, S-M.**
    - Web: an empty slot is a drop zone. It has "+ Choose… or drop" and an accent **Browse** button.
      The second IR shows "+ Add a second IR".
    - Nano Q (`effects/NodeControls.cpp:233-333`): a "Choose…" drop-down with a ×; the second IR
      reads "None".
    - Change: draw the drop-zone empty state, and use "Add a second IR" for IR B.
13. **Graphic EQ bands. Live, S.**
    - Web: each band has an editable number box ("0.0" plus "dB") above the slider and an editable
      Hz box below it, and the thumb is a round accent dot.
    - Nano Q (`effects/GraphicEq.cpp`): plain "0.0 dB" and "60 Hz" text, a grey rectangular thumb,
      and no Hz editing.
    - The editor review said the number fields match. The capture shows they don't.
    - Change: add the number boxes and match the thumb.
14. **Editor header type. Live, S.**
    - Web: the title is about 11 px bold. The subtitle ("CAB · IR CABINET") is bold capitals,
      letter-spaced.
    - Nano Q: the title is about 13 px, and the subtitle is regular weight with no tracking.
    - Change: match the sizes and tracking, as part of the header items 26-30.
15. **Signal Analyzer value tiles. Live, S.**
    - Web: values are bold and larger than the labels, with an "Updated: <time>" line.
    - Nano Q: values are the label size.
    - In the capture, Nano Q showed "Waiting for live analyzer data…" while the web had numbers. The
      demo audio may not have been playing for Nano Q, so check this with audio running before
      treating it as a bug.

## Effect editor and visualisations (code)

The per-effect tables match for most effects:
- the EQ curve, Spatial panner, IR alignment, Simple Cab response and tools, gate learn;
- the multiband plot, now also used by Multi-band Gate and Multi-band Distortion;
- the analyzer, Pickup Match curve, IR A/B cards and value formatting.

The gaps follow.

16. **Exposed resource slots are missing: composites and Custom Effect guest files. M-L.**
    - Web: `resourceSelector.ts:92-241`, `presetV2.ts:289-323`, broadcast in
      `PluginControllerBroadcast.cpp:501-561`.
    - Nano Q:
      - uiclient never parses `exposedResources` (`ClientState.h`).
      - `EffectPage.cpp:443-444, 1472-1497` builds only the primary slot.
      - `EffectLayoutPage.cpp:107-133` returns nothing for `__resource__:<exposed>:<i>`.
    - A composite such as Supercharged Neural Amp therefore shows no amp or cab slot at all. A
      Custom Effect's module slot is labelled "Resource".
    - Change:
      - parse `exposedResources` (resourceId, displayName, resourceType, resourceIndex,
        allowBrowseFile, parameterId/Value), including the merges from the Custom Effect
        descriptor;
      - build one slot per exposed resource, labelled with its displayName;
      - support the layout `__resource__` slots;
      - take artwork from the exposed resources too.
17. **Mixer "Input Channels" section is missing. M.**
    - Web: `mixerInput.ts:16-125` gives each connected input port Level (-60..12 dB), Pan, Delay
      (0-60 ms) and Mute. These map to the dynamic `level_N`/`pan_N`/`delay_N`/`mute_N` params
      (`MixerEffect.h:489-493`). With no inputs it shows "No inputs connected…".
    - Nano Q: nothing.
18. **NAM Blend editing. M-L.**
    - Web:
      - `signalPathBlend.ts:129-230, 471-500` maps the knobs to the blend's paramIds over the
        captured range, marks the mapped points and snaps to them;
      - it shows the "Playing: / Mix:" line;
      - `resourceSelector.ts:384-402` shows the legacy "Model N" selectors when there is no
        blendId.
    - Nano Q: `NodeControls.cpp:585-657` shows the name, count and mode, with "edited in Soundshed
      Guitar", over plain knobs.
    - Change: map, mark and snap the knobs, and add the match line and the legacy selectors. The
      edit pencil and the mode select can stay web-only.
19. **Header peak-meter rail. S-M.**
    - Web: `panel.ts:594-597`, `telemetry.ts:99-141`, `params-panel.css:313-367`. A 4 px strip
      from green to red under the header, with inactive and clipped states and the tooltip "Node
      peak … Headroom". It is hidden at 900 px wide or less.
    - Nano Q: only `HeaderLed`.
20. **Blend-unit params are knobs. S.**
    - Web: `panel.ts:238-261` and `paramControls.ts:129-149` draw `unit == "blend"` as an A-B
      slider reading "A", "B" or "NN%" (for example Simple Cab Mic Blend).
    - Nano Q: `EffectPage.cpp:650-715` draws a knob reading "70/30".
21. **The enum picker is a plain menu. M.**
    - Web: `enumPicker.ts:79-110, 203-256` has:
      - a trigger with an icon and a subtitle;
      - a popover with search ("Search N choices"), group tiles with counts and subgroup headings;
      - an icon and detail line on each option.
    - Nano Q: `NodeControls.cpp:335-376`, and `ClientState.h` has no `labelInfo`.
    - This matters most for Pickup Match (397 pickups) and the Verbal machine descriptions.
    - Change: carry `labelInfo` into ClientState and build the popover.
22. **Main/Advanced tabs. S.**
    - Web (`panel.ts:326-332`):
      - with no main params it shows every param, with no tabs;
      - each new node opens on Main.
    - Nano Q (`EffectPage.cpp:496-513`, `Shell.h:1020`):
      - an effect whose params are all advanced shows an empty Main tab;
      - `mShowAdvanced` carries over to the next node.
23. **Small editor gaps. S each.**
    - **DSP status:**
      - Web: an overlay anchored to the DSP chip (`params-panel.css:369-381`).
      - Nano Q: an inline 40 px row that pushes the body down (`EffectPage.cpp:1465-1470`).
    - **Full Rig note:** the IR Cabinet note "Signal chain already includes a Cabinet Model…" is
      missing (`panel.ts:449-451`, `chainRules.ts:61-66`).
    - **Slot path:** the muted file-path line under a resource slot is missing
      (`resourceSlotMarkup.ts:87`).
    - **Plugin Host:**
      - The web shows an inline load-error block, a spinner while loading, the path on each item,
        Remove with a confirm, and a custom-file row (`hostedPlugins.ts:171-437`).
      - Nano Q only shows a toast on error.
    - **Splitter text:** the hint should read "New branches start empty. Only an empty branch can be
      removed." (`splitBranches.ts:99-106`). Whether to keep "Back to one lane" is a product call.
    - **Presets chip:**
      - Web: shown whenever the type has params (`panel.ts:574-577`).
      - Nano Q: hidden for Splitter and Mixer (`EffectPage.cpp:1326`).
    - **Artwork:**
      - Web: gap 16, and at 900 px or less a 52 x 34 thumbnail beside the title
        (`resource-slots.css`).
      - Nano Q: gap 12, and no thumbnail.
24. **Custom layout styling. M.**
    - Web:
      - `layoutRenderer.ts:445, 519` draws control and cast shadows;
      - `499-500, 712` sets the control and label font families;
      - `554-591` draws strip-image toggles and sliders.
    - Nano Q: `uiclient/EffectLayouts.cpp:171-176` parses only the on/off image pairs, and
      `LayoutControlStyle` has no shadow or font fields.
25. **Web bug, don't port: Pitch Tracking EQ.** The web gives it an interactive parametric curve
    with four handles that write params the effect doesn't have (`panel.ts:335` `isEqNode`,
    `eq.ts:44-105`). Nano Q is right to leave the curve out. Fix the web: leave `kEqPitchTracking`
    out of `isEqNode`.

## Header and control bar (code unless marked)

26. **Toolbar buttons have boxes. S.**
    - Web: `.preset-action-btn` is transparent and borderless, radius 8, 28 x 30, with a 12 px
      icon.
    - Nano Q: `WideFrame.cpp:192-331` uses `IconButton::Look::Boxed`, 30 x 28, with a 14 px icon.
    - Change: use Ghost, or a new transparent "Tool" look with a fill on hover only. Keep the themes
      that draw `skin.keys.tool`.
27. **Toolbar widths and spacing. S.**
    - Labels:
      - Web: the NEW and PUBLISH labels hide at a viewport of 1320 or less (`toolbar.css:460-489`).
      - Nano Q: they show from about 1050 (`WideFrame.cpp:31-37`).
    - Separators: the web's are 1 x 24; Nano Q's are 16 px tall.
    - Prev/next:
      - Web: 28 x 28 with a 20 px arrow.
      - Nano Q: 30 x 30 with a 16 px arrow.
    - Row gaps:
      - Web: about 14 between the toolbar and the selector, a selector row gap of 6, and a
        `.preset-group` max of 800.
      - Nano Q: 9, 8 and 330-680.
28. **Preset pill. S.**
    - Web: the name is 11 px bold with 0.05em tracking. When unsaved, the border is accent at 55%,
      the status chip is accent, and Save has a red tint.
    - Nano Q (`FrameWidgets.h` `PresetPill`): 13 px SemiBold. When unsaved it appends " *" (seen
      live) and changes no border.
    - Status chip:
      - Web: 9 px bold, padding 2 7, radius 4, hidden at 760 or less.
      - Nano Q: 9.5 px, always shown.
    - Heart:
      - Web: uses `--color-preset-favorite` (#c8a090; the accent only on Dark).
      - Nano Q: always the accent.
29. **The preset name can't be dragged onto a folder or pad. M.** The web supports it
    (`drag.ts`). `PresetPill` has no drag source.
30. **Control bar details. S each.**
    - **Gate opener:**
      - Web: an 18 x 16 chevron beside the threshold value.
      - Nano Q: a 70 x 16 "Settings" text button under the knob (seen live).
    - **Out status and Mute:**
      - Web: 18 x 16 pills inside the OUT meter, with muted shown as #f08a65.
      - Nano Q: a "Status" text button under the knob (seen live), and Mute in the footer.
    - **Input chevron:** 18 x 16, with an accent border while calibration is active.
    - **EQ button:**
      - Web: 48 x 48 with the web's bar glyph, and an accent glow when on.
      - Nano Q: 46 x 46 with a "sliders" glyph (seen live).
    - **Control titles:** 9 px bold with .08em tracking, text-muted outside Dark.
    - **Tabs and gear:**
      - Web: nav tabs are 11 px bold with .10em tracking. The gear is 26 x 26 with a border, and
        accent when active.
      - Nano Q: 12 px tabs and a 28 x 28 gear.
    - **Pickup Match:** the indicator beside IN is missing (behind the `pickupMatch` flag).
    - **IN/OUT values:** in the captures, Nano Q's OUT read "+1.6 dB" where the web's read
      "+0.0 dB". Check with audio running whether both read the same source.
31. **The control bar and the chain can't collapse. M.**
    - Web: `.control-bar-collapse-btn` and `.signal-path-collapse-btn` are chevrons at .38
      opacity. Collapsing leaves a "CONTROLS" or "SIGNAL CHAIN" label, and the state persists.
    - Nano Q: there are none. `ShellFrame` even leaves an empty `HGap(26)` where the chain's
      chevron would go.

## Scenes and chain

32. **Scenes can't be reordered. M.**
    - Web: `sceneBadges.ts` supports drag with drop marks and Alt+Left/Right, sending `moveScene`.
    - Nano Q: `UiCommands` has no `MoveScene`.
    - Change: add the command first, then drag, then the keys.
33. **Scene rename is a prompt. M.** The web renames inline on double click and F2. Keep Nano Q's
    menu as well.
34. **Scene chip details. S.**
    - The label should stop at 160 px with an ellipsis.
    - The add chip should have a dashed border; Nano Q's is solid (seen live).
    - The close button should be 14 px at .55, turning red on hover.
    - Hide "+ Scene" while editing a composite (`sceneBadges.ts`). This needs a live check.
35. **Chain resize handle. M.**
    - Web: `layout.ts` has two modes, full (96) and compact (48), switched by a 12 px drag. Double
      click goes to full, and the arrow keys, Home, End and Enter work.
    - Web look (`nodes.css`): 8 px tall, with a 48 x 3 grip and an accent hover.
    - Web cap: 50% of the window, or 45% of the stage in a compact split.
    - Nano Q (`WideFrame.cpp:335-388`, `FrameWidgets.h` `SplitHandle`):
      - it drags to any height, with no keys and no double click;
      - it is 10 px tall with a 36 x 4 grip;
      - its height is a third of the view.
    - Also, `kSplitCardsMinStage` is 470 where the web's is 480.
36. **Node card look. M.**
    - Web: none of the web's skins draw an underglow or a category wash on the card.
      - The border is the accent mixed 72% into the face, 88% on hover, and full when selected,
        with a 2 px halo at 24%.
      - The shadow is `--node-raised`.
      - Bypassed cards are dashed, saturate .35, at .45 opacity, with a red "OFF" badge at the top
        right.
      - The missing-resource badge is a red boxed ⚠ at the top right; it moves to the bottom left
        only in icon mode.
      - The LED is 6 px at 5/5. It is grey at .6 when inactive, and it stays on for bypassed
        nodes.
      - Type text is 8 px, muted with 26% accent. Names have .06em tracking.
      - Hover lifts the card 1 px.
    - Nano Q (`NodeCard.h:394-480`): draws a radial category wash and an underglow, with
      `Mix(tint, Border, 0.42)` borders.
      - Bypassed cards have a solid border and no OFF badge.
      - The ⚠ is at the bottom left, unboxed.
      - The LED hides when bypassed.
      - Detail text is 9 px.
      - There is no hover lift.
    - Change: match the web's look in each of these.
37. **Corners, drag marker, boundary nodes. S.**
    - Web:
      - corner buttons are 16 px at a 2 px inset;
      - bypass hover is green, bypass when bypassed is red .2, delete hover is red;
      - the drag target has a #48e078 border and a 12 px dot, and the source dims to .5 and
        shrinks to .95;
      - Input uses amp.svg in #68a868, and Output uses speaker.svg in #6888a8.
    - Nano Q:
      - corners are at a 4 px inset;
      - the drag target has an accent ring;
      - Input and Output use "guitar" in Success() and "output".
38. **Parallel paths. L.**
    - Web (`parallel.css`):
      - the splitter is a full node card, with 32 px split and join spacers;
      - branches sit in a dashed box (border-medium at 55%, radius 8);
      - each branch row is tinted;
      - branch cables are coloured #f0a06b, #8794ff, #48e078 and #e048a8, with 12 px rounded
        elbows.
    - Nano Q (`ChainStrip.cpp`, `ChainWires.h`): the splitter is a rail with an 11 px icon circle.
      There is no box and no tint, and every cable is one colour with square elbows.
39. **+ buttons. S.**
    - The add trigger on hover:
      - Web: fills with the accent gradient (#e07848 to #c85830), with a white +, a glow and
        scale 1.1.
      - Nano Q: only the border changes.
    - The cable +:
      - Web: a 9 px ring at 40% along the cable; the hover button is 24 px with a #4a4a58 border,
        and accent on its own hover.
      - Nano Q: a ring of radius 4.5 at 55%, and a 22 px hover button with no accent.
    - The + menu: the web's reads "Add Scene…" with an ellipsis and opens beside the +. Nano Q's
      reads "Add Scene" and opens below it (seen live).
40. **Wrapped chain spacing. S, needs a live look.**
    - The web's row gap is 28, or 16 in compact. Nano Q's cards end up about 36 apart.
    - The web's wrap button sits at the top right at .38 opacity, and is accent when pressed.
41. **No tooltips anywhere in the shell or chain. M.**
    - The web has about 420 `title=` attributes. On a node it reads "name · type · badge".
    - Nano Q's only tooltip is the EQ band's (`ResponsePlot.cpp:669`).
    - Change: add a hover tooltip for node cards and every icon-only button, and expose the same
      text as UIA HelpText.

## Footer (code unless marked)

42. **Footer groups. M.**
    - Web (`footer.css`):
      - five groups (history | demo | tools | tempo | view), each flex 1 1 0, with a divider and
        16 px padding between them;
      - the bar is 42 tall, padding 7 18, gap 10;
      - buttons are 28 px tall, transparent, and accent 62/24% when active;
      - A/B is 28 tall with 24-wide buttons.
    - Nano Q (`WideFrame.cpp:540-611`):
      - it has dividers only after history and before theme, with Stretch around the tools;
      - its buttons are Boxed at 26 or 28;
      - A/B is 54 x 25;
      - the theme button is Boxed 84 x 28.
    - Change: give the groups equal shares, use a transparent footer look, and measure the
      tool-label threshold (330) on the tools group, not the frame.
43. **Footer tools. S.**
    - Remove the footer Mute; item 30 moves it into the OUT meter.
    - The web's tool order is Setlists, Tuner, MIDI, Rec, Loop. Rec's position is already accepted
      (2026-10-29), so only move Loop.
44. **The demo ⋯ menu repeats the clip list and can't set the render rate. S.**
    - Web: Rate and Render WAV.
    - Nano Q: `kDemoRenderRateSetting` is only read (`DemoClips.cpp:76`).

## Compact layout

45. **No portrait stack layout. L.**
    - Web: `compactMode.ts:117-141` stacks in portrait, and splits at a width of 600 or more and a
      height of 800 or more.
    - Nano Q: compact is always the rail, and `WantsSplit` checks only that the height is 520 or
      more.
46. **No short mode. M.**
    - Web: `COMPACT_SHORT_MAX_HEIGHT = 500` tightens the top bar, scenes and chain. The preset
      library and pads use the same 500 rule.
    - Nano Q: uses one 620 rule (`PresetChooser.cpp:30-33`).
47. **Compact details. S, needs a live look.**
    - The top bar's gaps are irregular (`ShellFrame.cpp:106-218`: 15/6/6/10/4/4/8/8/4/19).
    - In a touch split, the web moves the + to the scene row at 44 px.

## Theme flyout

48. **App UI segment. S.** The web has "Native (Nano)" with a separate BETA badge (9 px bold,
    accent border), joined segments 28 tall, tiles with 5 px padding and gap, and a header rule.
    Nano Q has "Native (Nano) Beta" in one label, a 34 px row, and tiles with 10/8 padding and gap.
    Check that the arrow keys move between tiles, as the web's do.

## Settings

49. **The Library section is missing. L.**
    - Web: Library holds the Resource Library manager (filters, Group by Tone, Cleanup unused, and
      per-row Browse, Edit, Copy path, used-by and Delete Group). Its Advanced part lists
      Composites, Blends (+ New, Import .namz) and Layouts.
    - The web shows the section only while Blend Tools is on, so Nano Q should do the same.
50. **Tone3000 settings. S-M.**
    - Missing in Nano Q:
      - the "Connection mode: …" line;
      - the API health row with "Check API Health" (seen live);
      - your own API key: a masked field with Save and Clear, shown while the relay is off;
      - the "Tone3000 API Key Required" prompt on import.
    - Today, turning the relay off in Nano Q leaves the user with no way to set a key.
51. **Software update. M.**
    - The update dialog uses `OpenConfirm`, whose fixed 86 px body cuts the notes off and shows raw
      markdown (`SoftwareUpdate.cpp:114-137`). The web renders the notes, shows "No release notes
      provided." when there are none, and has Later / Download buttons.
    - The web shows a "Version X is available" banner at the top of every Settings section.
    - The web's header chip shows the version.
    - The web's Current Version is a read-only field; Nano Q's is plain text (seen live).
    - The App Updates hint is missing (seen live).
52. **Audio & MIDI. S.**
    - Rename "Driver" to "Audio Driver".
    - Add the Control Panel and Reset Device row.
    - Add the shared-mode sentence to the sample-rate hint, and the "(x ms in, y ms out)" split to
      the latency hint.
    - Use ToggleSwitch, not PowerSwitch, for Mute Input and the MIDI inputs.
    - Use 230 px capital labels, as the rest of Settings does.
    - Give MIDI Devices its own card with both hints, and keep a disconnected output listed as
      "(disconnected)".
    - Add the Signal Path Test Run button.
53. **DSP Performance. S-M.**
    - Ceiling and Interface Calibration should be number fields (step 0.1 and 0.5), not sliders.
    - Quality should have Min/Max end labels, not a %.
    - Add the plugin's per-instance line for Mono Output Unity.
    - Fill in the missing and shortened hints.
    - Diagnostics tab:
      - add the hint, the "Enabled" status, the Designed Peak / Raw Input Peak readout and
        "Waiting for signal data.";
      - Clip should read "OK" or "Clipping (n)", not a dot.
54. **User Interface. M.**
    - Effect Panel Background is missing (Choose Image…, Remove, preview, Fade).
    - The Layout Density and Mouse Wheel hints are missing.
55. **Features and Help. S.**
    - Features is missing the intro, the group and feature descriptions, and the Factory Preset
      Archives card.
    - Hide the Resource Library row unless Blend Tools is on.
    - Help is missing the Jam topic.
    - Library export: show it only while Resource Library is on, with the drop-down and the button
      on one row (seen live: two rows).

## Input mode and levelling

56. **The input mode menu has no disabled modes, fallback or notes. M.**
    - Web (`inputMode.ts:124-215`):
      - it greys out modes the device can't carry, with the reason;
      - it shows "Running mono" when the mode falls back;
      - it notes shared hosted plugins, the track deciding, one-input devices and dual mono;
      - it says "Stereo/Mono, as the track is".
    - uiclient reads only three flags from `inputModeChanged`
      (`UiClientLibraryHandlers.cpp:217-222`). It also needs inputChannels, outputChannels,
      hostControlled, effectiveMode and dualMonoShared.
57. **Input levelling. M-L.**
    - While Pickup Match is on, the web has a Pickup Match tab: a switch, saved guitars, the curve,
      Advanced, and the target beside IN. All of it is missing.
    - Training dialog:
      - the title should be "Input Level Training";
      - add the three metric cards;
      - the description should be a 3-row field with maxlengths of 80 and 200.
    - Flyout rows should show the name over the note, and the Train row's note is missing.

## MIDI and automation

58. **Foot-controller tools. M.**
    - Missing:
      - per-node mappings (`nodeSelector: id:<nodeId>`, from issue 57);
      - latch/toggle learn on bypass buttons;
      - the mode picker (Absolute / Toggle / Relative / Pickup), with tooltips;
      - renaming custom slots;
      - learn on setlist pads and banks (`SetlistPadLearnTarget` exists but is never called);
      - the × to clear a key.
    - In uiclient:
      - `MidiLearnTarget` has no nodeId;
      - `SetAutomationSlot` sends no nodeSelector;
      - `ArmMidiLearn` takes no mode;
      - nothing can set `midiMap.mode`.

## Presets, Multi-Rig, setlists, pads

59. **Preset chooser. S-M.**
    - Seen live:
      - Nano Q's folder column adds icons and a "FOLDERS" heading where the web has a plain,
        indented tree.
      - Rating stars are outlined where the web's are filled grey.
      - Setlists is a small "Setlists ▾" where the web has a full-width bar with a chevron.
      - The popover is 780 wide where the web's is 760; the folder column is 210 where the web's
        is 220.
    - From code:
      - The badge shows User/Factory where the web shows the category, and only when set.
      - Cards lack the tag chips and description.
      - The rating should read "3/5" or "Not Rated".
      - "✓ In Mixer" should toggle off on a second click.
      - The card being loaded has no dim and spinner.
      - The chooser doesn't open on the playing preset's folder.
      - The empty texts and the Browse Tone Sharing button are missing.
      - Export should be disabled at 0 and named after the folder.
      - Folder counts should be each folder's own presets.
      - The short-window folder picker lacks counts.
      - Tag chips should wrap.
60. **Library drag and drop. M.** Presets onto folders, setlists and pads, folder nesting, and
    setlist slot reorder. Nano Q has menus for each of these.
61. **Multi-Rig. S-M.**
    - Search doesn't filter Multi-Rigs, and there's no no-match text.
    - Rows should be cards: name, "N presets", ×, preset chips, description and tags, with Enter
      and Delete.
    - The Setlists fold shows under the Multi-Rig tab.
    - Save asks for a name only; the web also asks for a description and tags.
62. **Save dialog. S.**
    - The category should default to "User".
    - The description should be a 3-row field.
    - An empty name should show "Preset name is required".
    - Save As should keep the name (no " copy") and use the source preset's folder first.
    - The folder list should start with "All Presets", sorted, and the dialog should be 560 wide.
    - The toasts "Preset updated" and "Preset deleted" are missing.
63. **Setlists and pads. S-M.**
    - Setlists:
      - should be a 220 px list with Bank pills and a "Name (Bank N)" editor, not a drop-down;
      - should be created inline with a "Setlist name required" warning; Nano Q drops a blank name
        silently;
      - slots need a visible ×.
    - Pads:
      - should drop to 2 columns at 920 or less and 1 at 560 or less;
      - should show the "Pad N: name" toast after assigning;
      - the bank editor should warn on a blank name and stay open;
      - the no-setlist screen needs the web's wording and strip.
    - Header menus: the New menu lacks "Basic Signal Chain", and the ⋯ menu lacks "Export/Exit
      archive session".

## Resource browser and slots

64. **Opening a slot. M.**
    - The browser doesn't open on the slot's category, so the reverb picker lists cab IRs.
    - A Tone3000 model doesn't open on its source tone.
    - Nothing is remembered between opens: search, filters, tab, sort, page or expanded tone.
    - The selected row isn't scrolled into view.
    - Cancel is drawn as a primary button.
    - Paging hides the footer hint on the Tone 3000 tab.
65. **Slot ◀ ▶. M.**
    - Web: steps through the list the model came from (the library filter, the folder or the
      Tone3000 results), importing as it goes, and disables at the ends.
    - Nano Q: steps the whole library and wraps (`NodeControls.cpp:259-281`).
66. **Library tab. M.**
    - ℹ opens a menu, where the web has a details panel: the Tone3000 card, Open/Find on Tone3000,
      the description, and the ID, path, tags and metadata.
    - Rows lack tag pills, and tags are a drop-down where the web has a chip bar.
    - ◀ ▶ are never disabled at the ends.
    - Delete is offered before the in-use answer arrives.
    - The filter labels differ, and so does the row button order.
    - Edit should be a titled popover with suggestions.
    - File… sends no import category.
67. **Folder tab. M.**
    - File rows lack star, edit, ℹ, ▶ and a details panel.
    - The meta line lacks channels, duration, gear, type and tags.
    - Files of the other type are hidden, where the web shows them disabled.
    - Saving a file to the library drops its category, tags and lookup metadata.
    - The list isn't virtualised.
68. **Tone 3000 tab. M.**
    - The Search | Favourites switch is missing.
    - Gear labels and order differ, Nano Q adds "Any gear", and the sort options differ.
    - An expanded tone has no Models | Details tabs.
    - Tones lack downloads and the Imported badge.
    - Preview:
      - shows "Playing" before the download is done;
      - a second click re-downloads instead of stopping;
      - double click does nothing.
    - Import All has no progress.
    - The empty and error states are missing.
    - The IR search placeholder differs.

## Tones and tone sharing

69. **Feed. S each.**
    - Search should run as you type (250 ms), with "Searching…".
    - Each /home row should be its own strip.
    - Add the "N loaded" line.
    - The empty texts differ.
    - Cards don't show the creator handle.
    - My Content lacks the moderation badge.
    - Owners can't delete their own items (M).
    - Cards / List isn't saved.
    - Installed-pack cards are thinner than the web's.
    - Sign-in should be one Account dialog with the profile editor (M).
    - The filter and preview wording differs.

## Jam, Practice Tool, Scale Guide

70. **Practice Tool. L overall.**
    - The tablature section is missing. Port it, or show a stub, but keep its data (item 2).
    - Faders have no typed value; `ParamSlider` has no InlineEdit.
    - Loops:
      - they are edited through a menu where the web edits inline;
      - a new loop doesn't open for naming;
      - clicking a row doesn't select it.
    - Project bar:
      - it should be inline: a picker, Delete, the name, a Preset checkbox, and a Save that stays
        disabled until the project is ready;
      - it needs the hint line;
      - saving over an existing name should ask first.
    - Links: the section should always show, with title then URL, and removing a link should ask.
    - Sizes: the waveform is 200 px where Nano Q's is 128, Browse and + Add Loop sit over the
      waveform, and the grid is 1.1fr/0.9fr with round 36 px transport buttons.
    - The waveform has no keyboard control.
    - The waveform has no drag-over highlight.
71. **Scale Guide. M.**
    - The progression editor is missing.
    - The Quick reference card is missing.
    - The chord formula should be tone pills lit by LISTEN.
    - The chord wheel hint is missing.
    - Steps lack numerals and done styling, and Nano Q adds a "STEPS" label.
    - Chords with no shape should show a "No shape" box.
    - Play volume should be a 0-100 slider.
    - Left/Right should step the progression page-wide.
    - The disabled Play and the scale options lack tooltips.
    - The LISTEN readout should use the idle, in, out and clean colours.
72. **Riff Library. M.**
    - It is a launcher and a dialog, where the web shows it inline with a Storage card.
    - The library path can't be set (uiclient has no `SetRiffLibraryPath`), and there's no
      Refresh.
    - A WAV can't be imported into a take (no `ImportRiffWav`).
    - The delete confirm lacks "This cannot be undone."

## Tuner, metronome, looper

73. **Small gaps. S.**
    - The tuner puck should slide (0.15 s), and the mute tooltip should change to "Unmute output".
    - The metronome readout should take Shift for tenths.
    - Looper:
      - the count-in should be a big 34 px number over the display;
      - add the progress bar;
      - add the inline loop name with the unsaved dot;
      - "Click while recording" should be a checkbox;
      - loop rows should be title-as-button plus ×.
74. **Looper Layers, Loops and Options are modal. M.** In the web, Layers is an inline panel and
    Loops and Options are drop-downs. While a Nano Q dialog is open, Rec and Stop can't be pressed.

## FX Library

75. **Blends and saved Custom Effects can't be added. M.**
    - The web lists every blend, with a blend badge, and every saved Custom Effect, with a
      "Custom" badge (`ts/fxLibrary/catalog.ts:87-142`).
    - Nano Q skips `kAmpNamBlend` and has no custom effect library in `ClientState`. Neither can be
      added to a chain.
76. **The cable + opens the whole library. M.** The web opens a small "Add Effect" flyout at the +.
    It has a 164 px category list and one effect per row, and opens on Favourites or Recent when
    they hold anything.
77. **Picker details. S.**
    - Search should match type, category and description, not only the name.
    - Keyboard: Enter adds the first match, Down moves into the list, and Up/Down with Enter/Space
      work on cards.
    - Replace mode should keep drag, use the web's hint, an accent title and a "Current" pill.
    - The empty texts differ.
    - The category column should be 208 px, not 176.
    - The docked library should default to "amp".
    - Cards should show layout and Custom Effect thumbnails.

## Dialogs

78. **Prompt and confirm. S each.** (Stacking is item 9.)
    - The prompt has no message line, and its placeholder is always "Name".
    - There is no multi-line prompt, which the description fields need.
    - OK on a blank prompt does nothing, silently.
    - There is no OK-only alert.
    - The confirm body is a fixed 86 px with no scroll.
    - Check whether the prompt selects its default text.
79. **Authoring tools: say where they are. S-M.**
    - Layout picker:
      - the web has Layout / Rules tabs, "Remember this for" (every use, this preset, a keyword),
        a rules list and "New layout…";
      - Nano Q always writes an effect-type rule (`EffectLayoutPage.cpp:181-216`);
      - add the scopes and rules, and a "designed in Soundshed Guitar" note.
    - Custom Effect node:
      - the web has a designer block: Design With AI, Save To My Custom Effects, Use This Effect;
      - add at least the note; Save and Use would make effects reusable (M).
    - Composite edit mode: Nano Q doesn't parse `compositeEditState` or
      `compositeEditModeExited`. With both editors open, it would show the inner chain with no
      banner. Needs a live check.

## Accessibility and keyboard

For keyboard use, Nano Q is ahead of the web:
- knobs step from the keyboard;
- focus is trapped in dialogs and returns to the opener;
- every control has a focus ring.

For screen readers it is thinner than the web:
- controls are one flat list, with no static text, headings or dialog titles;
- there are no announcements;
- disabled controls are hidden;
- nothing is exposed off Windows.

Since the last notes, the Spatial panner, IR alignment and Graphic EQ have gained `Describe()`.
`ResponsePlot`, `MultibandPlot` and `EqPlot` still have none.

### Screen readers

80. **Dialog titles and messages aren't exposed. M.** The root name is always "Soundshed"
    (`input/AccessUiaProviders.h:534`).
    - Give each `KeyRouter` layer a title and description.
    - In the bridge, add a Window fragment per dialog with Name, FullDescription and
      `UIA_IsDialogPropertyId`, and raise `WindowOpened`.
    - Name the prompt field from its label.
81. **No announcements. M.**
    - Add `AccessBridge::Announce(text, importance)` using `UiaRaiseNotificationEvent`.
    - Call it from `ShowToast` (`Dialogs.cpp:312`), and on:
      - preset and scene changes, including from footswitches and MIDI;
      - Space bypass;
      - undo and redo;
      - adding and removing effects.
82. **Custom layout controls can't be reached. M.** `LayoutControlView` (`effects/LayoutView.h:62`)
    isn't Focusable.
    - Describe it as Slider or Toggle with value text.
    - Reuse `KeyedPosition`.
    - Delete resets it, and the menu key opens MIDI Learn.
83. **Disabled controls are invisible. S.** `Pressable::wants_focus` requires `mEnabled`, so
    disabled controls are never collected. Collect them for the bridge with IsEnabled false, but
    keep skipping them for Tab.
84. **Unnamed controls. S.**
    - Text boxes have no name (`Focus.cpp` `DescribeTarget`).
    - Others: `SelectBox` (`DevicePage.cpp:346`) and the favourites `Segmented`
      (`ResourceBrowser.cpp:495`).
    - Add a DevDriver check that flags any Focusable with an empty name.
85. **States. S.**
    - Add `expanded` with ExpandCollapse to combo boxes and menu buttons.
    - Expose a lit `IconButton` with the Toggle pattern.
    - Put "bypassed" in the node card's value text.
    - UIA fixes:
      - RangeValue is in percent, not in units;
      - SmallChange for stepped controls should be 100/(steps-1);
      - `IsReadOnly` returns false where SetValue refuses;
      - AcceleratorKey isn't set.
86. **Structure. M.**
    - Add List, TabList and Group container nodes, with PositionInSet/SizeOfSet and
      SelectionContainer.
    - Expose section titles as headings, and dialog body text (L).
87. **Tone-preview and Jam bars. M.** They aren't in Tab order or UIA (`KeyRouter.h:373`).
88. **Tuner and meters are silent. M.** Expose the note and cents as text, with a throttled
    notification, and the meters as read-only RangeValue.
89. **No bridge off Windows. S then L.**
    - First: detect a screen reader (`SPI_GETSCREENREADER`, VoiceOver, TalkBack, Orca) and offer
      the WebView UI.
    - Later: add NSAccessibility and UIAccessibility bridges over the same `Describe()`.

### Keyboard

90. **`ArrowFocus` spills across groups. S-M.** It moves to the next control of the same role
    anywhere in the layer (`KeyRouter.h:608-690`). Limit it to the same parent (the common prefix
    of `FocusTarget.path`), and wrap within tab rows.
91. **Menus open on the first row. S.** With the keyboard, focus the chosen row instead.
92. **Missing keys. S each.**
    - Alt+Left/Right on a scene chip.
    - Scale Guide Left/Right.
    - The Practice Tool waveform.
    - Alt+Left/Right to move a chain card (M; the web has none either).
93. **Long lists cost one Tab stop per row. M.** Use roving stops: Tab enters a list at its
    selected row, and the arrows move within it.

### Low priority

94. Lower-priority items:
    - **Plot keys:** make the EQ handles and crossovers focusable. Neither UI has this. M.
    - **Reduced motion:**
      - share `ReducedMotion()`;
      - freeze the wire pulses (`ChainStrip.cpp:356`);
      - read the macOS, iOS, GTK and Android settings.
      - S.
    - **High contrast:** read `SPI_GETHIGHCONTRAST` and check that the focus ring has 3:1
      contrast on every theme. M.
    - **Zoom shortcuts:** Ctrl+=, Ctrl+- and Ctrl+0. Neither UI has them. S.

### The web is weaker here: don't copy it, and consider fixing it

- Knobs can't be moved from the keyboard (`knob.ts`).
- Toggle checkboxes have no name and no focus indicator.
- Preset rows can't be reached by keyboard (`views.ts:413`).
- About 21 of 27 modals lack `role=dialog`, and none trap focus.
- The global Space tap tempo calls `preventDefault`, which may break Space on focused buttons
  (`metronome.ts:674`; needs checking).
- Tabbing through the chain selects every node.

## Whole features missing, most missed first

| # | Feature | Items |
| --- | --- | --- |
| 1 | Editing effects that have a backdrop layout (Chaos Engine, Verbal) | 1 |
| 2 | Composite and Custom Effect resource slots | 16 |
| 3 | Foot-controller MIDI: per-node mappings, toggle learn, modes, pad learn | 58 |
| 4 | Stepping a slot through its source list; the browser remembering its place | 64, 65 |
| 5 | Adding blends and saved Custom Effects from the FX Library | 75 |
| 6 | Naming a new preset on first Save; re-downloading missing Tone3000 models on load (`ts/presets/missingResources.ts`) | 8 |
| 7 | Tones > Your Presets and Tones > Tone 3000 tabs (`tones-local-presets.html`, `tone3000Browser.ts`) | — |
| 8 | Parallel paths drawn as the web draws them | 38 |
| 9 | Mixer input channels; NAM Blend knob mapping | 17, 18 |
| 10 | Practice Tool tablature (and keeping its data) | 2, 70 |
| 11 | Your own Tone3000 API key, and Tone3000 favourites | 50, 68 |
| 12 | Scene reorder and inline rename | 32, 33 |
| 13 | Screen-reader dialogs, announcements and layout controls | 80-82 |
| 14 | Library drag and drop | 29, 60 |
| 15 | Scale Guide progression editor | 71 |
| 16 | Portrait stack and short compact modes | 45, 46 |
| 17 | Resource details panel and folder-file actions | 66, 67 |
| 18 | Input mode availability and notes; the Pickup Match tab | 56, 57 |
| 19 | Collapsible control bar and chain | 31 |
| 20 | Settings > Library, Effect Panel Background, Signal Path Test | 49, 52, 54 |

## Suggested order

1. **Bugs:** 1-9. Items 1 and 2 first: one blocks editing, the other loses data.
2. **The look users see on every screen:** the header (26-28, 30), node cards (36-37), footer
   (42-43) and editor width (10). Most are S; together they are about M.
3. **Effect editor gaps:** 11-13, 16, 19-23.
4. **Accessibility P1:** 80-85, 90-91.
5. **The feature table above, in its order.**

## Acceptable as they are

- Nano Q's knob keyboard, focus trapping and focus return. They are better than the web's; keep
  them.
- The effect presets menu does the same jobs as the web's with a different layout.
- Removing the NameInGroup prefix from labels is a known, intentional difference.
- The Tone Builder (AI) stays web-only by design.
- The toast style differs (title and detail, red for errors, 8 s). Either style is defensible.
- Everything under "Acceptable" in the 2026-10-29 review.

## Not checked live

- The light, classic and Studio themes. Only the default dark theme was compared.
- The compact layout: the web capture landed on the Jam page, so the compact chain and editor
  weren't compared side by side.
- Anything with audio running: meters, the analyzer, the IN/OUT values, the LED and peak-rail
  states, the tuner, the looper and LISTEN.
- Nano Q inside a DAW (item 3 is from code).
- The Practice Tool, Scale Guide, Riff Library and Tones pages, beyond the code review.
- Screen-reader behaviour. The accessibility items are from code and need a pass with Narrator
  or NVDA.
- Items marked "needs a live look" above.

## Unrelated, found on the way

The full Nano Q Release build fails in libremidi under MSVC 14.52.37025 with C2065 and C2057 in
`_deps/libremidi-src/include/libremidi/system_error2.hpp`. Building only `nanoq_standalone`
(`nanoq\build.cmd Release nanoq_standalone`) works.
