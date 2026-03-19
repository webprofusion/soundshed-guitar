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
