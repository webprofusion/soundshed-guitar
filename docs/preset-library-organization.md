# Preset Library Organization

Date: 2026-01-23

## Goal
Define a flexible preset organization model with hierarchical folders and ordered setlists without duplicating preset data.

## Core Concepts
- **Preset**: The canonical preset data (content) stored once by `id`.
- **Folder**: A hierarchical container used to organize preset references.
- **Setlist**: An ordered list of slots referencing presets by `id`. Slots can repeat the same preset.
- **Bank**: A numeric identifier assigned to a setlist for future hardware mapping. Only one setlist can claim a given bank number.

## Data Model (Conceptual)
- **Preset**: `{ id, name, ... }`
- **Folder**:
  - `id`
  - `name`
  - `children`: `Folder[]`
  - `presetIds`: `string[]` (references only)
- **Setlist**:
  - `id`
  - `name`
  - `bank` (optional, unique, integer)
  - `slots`: `{ presetId: string }[]` (ordered, repeatable)

## Rules
1. **No duplication of preset data**: folders and setlists only reference `presetId`.
2. **Setlist ordering**: `slots` preserve order and can include repeated presets.
3. **Bank uniqueness**: only one setlist per bank number.
4. **Folder hierarchy**: folders can be nested arbitrarily.
5. **Name matching**: folder name matching is case-insensitive for merge purposes.
6. **Selecting a slot switches presets**: stepping the setlist cursor (`setSetlistCursor`
   from the UI, or a footswitch/MIDI `setlist.presetN` trigger) makes that slot's preset
   *the* active preset — the mixer swaps down to a single instance using the gapless
   crossfade, and the outgoing preset's delay and reverb keep ringing over the new one for
   up to `audio.presetSwitch.tailBars` bars at the current tempo (default 2, 0 = off). It
   never stacks another instance on top, which is the separate Multi-Rig "add preset to the
   mix" action. The backend performs the load and reports it with `presetLoaded`; the UI must
   not issue its own `loadPreset` for the same step.

## Import Behavior
- **Default imports**: presets without folder paths remain unassigned and appear in `All Presets` until the user moves them into a folder.
- **Folder paths provided**: imported presets with folder paths merge into root (top-level) folders using case-insensitive name matching.
- **Full exports**: merge folders at the same tree level using case-insensitive name matching to avoid duplicates.
- **ID variance**: if imports vary by `id`, keep both entries.

## Export Behavior
- Exported data should serialize presets once and include folder + setlist structures referencing `presetId`.
- Round-trip should not create duplicate presets.

## UI Notes
- Folder navigation provides a tree view.
- Setlists display ordered slots (drag/drop reorder), allow repeated presets.
- Bank number is edited in setlist settings and validated for uniqueness.

## Multi-Rig Presets
- A Multi-Rig preset captures the mixer: which presets are running, each slot's
  mix, pan, mute and solo, and the mix's own output level. It references the
  presets by `presetId` and does not copy their graphs, so editing a preset
  changes every Multi-Rig that uses it.
- The Master strip's Out knob is the mix's level in dB (−24 to +12), applied to
  the summed presets ahead of the global post-chain and the header's OUT gain,
  and independent of both. It is saved with the Multi-Rig and restored on load;
  OUT, like every global setting, is left alone. Loading a single preset resets
  it to 0 dB so a lone preset never plays through a leftover trim. There is no
  per-mix limiter switch — the limiter applies to the whole output, so it lives
  under Settings → General → Advanced DSP Level Targets, next to the ceiling it
  aims at (`audio.dsp.outputLimiterEnabled`, default off).
- The feature is on by default (`features.multiRig.enabled`) and can be turned
  off under Settings → Feature Toggles. Turning it off hides the entry points —
  the Add to Mixer button, the Multi-Rig tab and the Save/Delete toolbar — but
  never the mixer strips, so two presets already running can still be removed.
- Entry points: **+ Mixer** on a preset card adds it as a slot; with two or more
  slots the signal path shows one tab per preset plus a **Mix** tab holding the
  strips, the mix's Out knob and the Save/Update and Delete buttons stacked beside
  it; the library's **Multi-Rig** tab lists saved
  mixes, filtered by the same search box as presets (name, description, tags and
  the names of the presets in each slot).
- Loading a Multi-Rig replaces every mixer slot. If a referenced preset no longer
  exists that slot is skipped and reported as an error; the rest still load.
- The mixer toolbar tracks which Multi-Rig it was loaded from or last saved as.
  While that link holds, Save becomes **Update** (same id, original creation
  date kept) and Delete is enabled. Adding or removing a slot — from the UI, a
  setlist step, MIDI or a DAW project restore — breaks the link, so the next
  Save creates a new Multi-Rig rather than overwriting one that no longer
  describes the mix.
- Multi-Rigs are stored in the document store (one row per id) and are not yet
  part of preset export, tone sharing packs or setlists.
