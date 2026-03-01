# Supercharged Composite Surface Plan

> **Status: PHASE 1 IMPLEMENTED.** `ExposedResource` is defined in `core/src/presets/PresetTypes.h` and `core/ui/ts/compositeTypes.ts`. Serialization (read/write) in `core/src/presets/PresetTypesJson.h`. Resource routing for composite nodes via `exposedResourceId` handled in `core/src/PluginController.cpp`. UI rendering of surfaced resource controls in `core/ui/ts/signalPath.ts` and `core/ui/ts/compositeEffects.ts`. Factory Supercharged Amp composite definitions in `core/ui/assets/composites/`.

## Goal
Enable Supercharged Neural composites (Amp now, Pedal/FX later) to surface both:
- **Parameters** (existing)
- **Resources** (new): model/IR selection + replacement from the parent composite node UI

Also support **factory-defined custom layouts** so surfaced controls can be positioned against branded backgrounds.

## Scope (Phase 1)
1. Add shared composite schema for surfaced resources.
2. Implement backend serialization + loading + registration plumbing.
3. Include surfaced resources in effect catalog metadata to UI.
4. Start UI plumbing to render surfaced resource controls for composite nodes.
5. Ship first factory Supercharged Amp composite definition using the shared schema.

## Non-Goals (Phase 1)
- No new DSP processor type for Supercharged Amp.
- No breaking changes to existing composite definitions.
- No hard dependency on custom layout (must gracefully fall back to auto layout).

## Design Principles
- **Backward compatible**: existing composites keep working unchanged.
- **Shared contract**: same schema used by amp/pedal/fx composites.
- **Resource-first UX**: end users should change NAM/IR directly on the composite node.
- **Layout decoupling**: layout binds only to surfaced keys, not inner graph internals.

## Data Model Changes

### New `ExposedResource` (C++ + TS)
Represents one surfaced resource control on a composite.

| Field | Type | Required | Description |
| --- | --- | --- | --- |
| `resourceId` | string | Yes | UI-facing key (e.g., `ampModel`, `cabIr`) |
| `displayName` | string | Yes | Label shown in UI |
| `nodeId` | string | Yes | Target inner node ID |
| `resourceType` | string | Yes | `nam`, `ir`, etc. |
| `resourceIndex` | int | No | Resource slot index (default `0`) |
| `allowBrowseFile` | bool | No | Allow file browser replacement (default `true`) |
| `parameterId` | string | No | Optional model mapping key (blend use cases) |
| `parameterValue` | double | No | Optional model mapping value |

### `CompositeEffectDefinition` extension
- Add `std::vector<ExposedResource> exposedResources`.
- JSON read/write includes `exposedResources` when present.

## Engine Changes

### Composite library / registration
- Keep existing `exposedParams` flow unchanged.
- Include surfaced resources metadata in effect catalog entry for `composite:*` types via an optional metadata payload.

### Resource routing
- Extend `updateNodeResource` handling for composite nodes:
  - If payload includes `exposedResourceId`, resolve target `(nodeId, resourceIndex, type)` from composite definition.
  - Apply update to inner graph resource slot.
  - Respect composite edit mode behavior and state broadcast conventions.

## UI Changes
- Extend composite TS types with `exposedResources`.
- In node parameter panel for composite nodes:
  - Render surfaced resource controls like native NAM/IR controls.
  - Use existing `updateNodeResource` message with `exposedResourceId`.
- If custom layout exists for composite type:
  - Place surfaced resource controls as layout controls.
  - Fallback to default panel controls if layout missing.

## Factory Supercharged Amp Definition (Initial)
Inner graph:
`input -> dynamics_gate -> preamp -> eq_parametric -> amp_nam_optimized|amp_nam_blend -> cab_ir -> reverb_room -> output`

Surface:
- Params: gate, preamp drive, pre-EQ tone, amp gain/level, cab mix, reverb mix/decay.
- Resources:
  - `ampModel` -> NAM node resource slot 0 (`nam`)
  - `cabIr` -> cab node resource slot 0 (`ir`)
  - Optional blend model resources for blend variant.

## Validation Plan
- Load existing composites without `exposedResources` (must pass).
- Update surfaced NAM/IR resource on composite node and verify inner graph updates.
- Save/reload composite definitions preserving `exposedResources`.
- Confirm effect catalog contains surfaced resource metadata for composite types.
- UI: resource selection and browse replacement work from composite node panel.

## File Touchpoints (planned)
- `core/src/presets/PresetTypes.h`
- `core/src/presets/PresetTypesJson.h`
- `core/src/dsp/effects/CompositeEffectProcessor.h`
- `core/src/dsp/effects/CompositeEffectProcessor.cpp`
- `core/src/PluginController.cpp`
- `core/ui/ts/compositeTypes.ts`
- `core/ui/ts/signalPath.ts`
- `core/ui/assets/composites/factory/*.json`
