# Custom Effect Local Library Model

## Goal

Define how generated `Custom Effect` modules should live in the local Soundshed library and how they should appear in the effect chooser, while still using the existing `wasm_host` runtime under the hood.

This document is the concrete follow-up to [AI Module Generation API Architecture](2026-04-17-ai-module-generation-api-architecture.md).

## Constraints From The Current Codebase

The current codebase already gives us three important building blocks:

1. The backend can persist arbitrary local library resources through `ResourceLibrary` and save them to `settings/resources/indexes/resources-index.json`.
2. The active preset graph already resolves library-backed resources through `ResourceRef { resourceType, resourceId }`.
3. The FX selector already merges multiple sources of chooser entries such as built-in effects, blends, and composites.

That means the clean solution is not to create a second DSP runtime or a one-off import path.

It is to:

- store generated module binaries as normal local library resources
- store chooser-facing `Custom Effect` entries in a dedicated local index
- hydrate chooser cards from that local index
- instantiate standard `wasm_host` nodes with the module resource preselected

## Recommendation

Use a two-layer local model:

1. `ResourceLibrary` remains the authoritative store for the actual module binary and optional related files.
2. A new `CustomEffectLibrary` index becomes the authoritative store for chooser-facing entries and apply-time metadata.

This mirrors the way the product already treats:

- raw resources in the resource library
- chooser-facing blends in the blend library
- chooser-facing composites in the composite library

## Why Not Use ResourceLibrary Alone

`ResourceLibrary` is currently optimized for generic file-backed resources:

- `type`
- `id`
- `name`
- `category`
- `description`
- `filePath`
- `hash`
- `tags`
- `metadata: map<string, string>`

That is good for module bytes, but it is not enough by itself for the full `Custom Effect` UX because chooser entries also need:

- stable effect-entry ids across revisions
- an explicit link to the latest revision
- chooser thumbnail and card summary fields
- default node params and label behavior
- a clean split between binary module resources and user-facing effect cards

The generic string-only metadata map can carry some of this, but it becomes brittle quickly and makes chooser hydration harder than it needs to be.

## Proposed Local Data Model

### Layer 1: Module Resource Entry

The actual `.wasm` file should be stored as a normal local library resource.

Recommended resource type:

- `wasm`

Recommended resource id pattern:

- `custom-effect:<effect-id>:<revision-id>`

Example `ResourceLibrary` entry:

```json
{
  "type": "wasm",
  "id": "custom-effect:airy-widen:rev-0003",
  "name": "Airy Widen",
  "category": "modulation",
  "description": "Gentle stereo widener with subtle movement.",
  "filePath": "content/local/custom-effects/airy-widen/rev-0003/module.wasm",
  "hash": "sha256:abcd1234",
  "tags": ["custom-effect", "ai-generated", "modulation"],
  "metadata": {
    "provider": "local",
    "entryKind": "custom_effect_module",
    "customEffectId": "airy-widen",
    "customEffectRevisionId": "rev-0003",
    "displayName": "Airy Widen",
    "effectCategory": "modulation",
    "descriptorSummaryJson": "{\"parameterCount\":2}",
    "thumbnailDataUrl": "data:image/png;base64,..."
  }
}
```

The resource entry is the thing the graph resolves at runtime.

### Layer 2: Custom Effect Chooser Entry

The chooser-facing effect should be stored in a dedicated local index.

Recommended file:

- `settings/custom-effects/indexes/custom-effects-index.json`

Recommended structure:

```json
[
  {
    "id": "airy-widen",
    "name": "Airy Widen",
    "category": "modulation",
    "description": "Gentle stereo widener with subtle movement.",
    "baseEffectType": "wasm_host",
    "moduleResourceType": "wasm",
    "moduleResourceId": "custom-effect:airy-widen:rev-0003",
    "latestRevisionId": "rev-0003",
    "thumbnailDataUrl": "data:image/png;base64,...",
    "tags": ["custom", "generated", "stereo"],
    "defaultParams": {
      "depth": 0.35,
      "rate": 0.22
    },
    "descriptorSummary": {
      "displayName": "Airy Widen",
      "category": "modulation",
      "parameterCount": 2,
      "resourceCount": 0
    },
    "origin": "generated",
    "createdAt": "2026-04-17T12:34:56Z",
    "updatedAt": "2026-04-17T13:10:00Z"
  }
]
```

The chooser entry is the thing the UI browses and drags onto the signal chain.

## Storage Layout

### Current Storage Shape

Today, local resources are persisted to:

- index: `settings/resources/indexes/resources-index.json`
- content root: `settings/resources/content/`

The current generic local-save path writes inline resources into:

- `settings/resources/content/local/`

### Recommended Custom Effect Layout

For generated `Custom Effect` revisions, use a dedicated subfolder layout instead of a flat file dump:

```text
settings/
  resources/
    indexes/
      resources-index.json
    content/
      local/
        custom-effects/
          airy-widen/
            rev-0003/
              module.wasm
              descriptor.txt
              manifest.json
              validation-report.json
              thumbnail.png
              preview.wav
  custom-effects/
    indexes/
      custom-effects-index.json
```

This gives us:

- stable effect ids
- explicit revision folders
- room for companion artifacts
- clean cleanup and replacement rules

## Required Backend Changes

### 1. Add A Local CustomEffectLibrary Model

Add a new in-memory/backend model similar to the existing blend/composite libraries.

Suggested C++ shape:

```cpp
struct CustomEffectLibraryEntry {
  std::string id;
  std::string name;
  std::string category;
  std::string description;
  std::string baseEffectType; // always wasm_host
  std::string moduleResourceType; // wasm
  std::string moduleResourceId;
  std::string latestRevisionId;
  std::string thumbnailDataUrl;
  std::vector<std::string> tags;
  std::map<std::string, double> defaultParams;
  nlohmann::json descriptorSummary;
  std::string origin; // generated, imported, duplicated
  std::string createdAt;
  std::string updatedAt;
};
```

### 2. Load And Save A Dedicated Index

Add backend load/save helpers for:

- `settings/custom-effects/indexes/custom-effects-index.json`

Do not overload `resources-index.json` with chooser-entry semantics.

### 3. Extend Local Save Helpers For Subfolders

The current local resource helper writes inline data into `settings/resources/content/local/` and does not yet expose a first-class subfolder model for custom effects.

Recommended change:

- extend the local-save helper to accept a sanitized `subfolder`

or:

- add a dedicated `saveCustomEffectRevision` backend path that writes the revision bundle into the recommended custom-effects subfolder layout

The second option is cleaner for this feature.

## Required UI State Changes

### 1. Add `customEffectLibrary` To UI State

The current UI already receives:

- `resourceLibrary`
- `blendLibrary`
- `compositeLibrary`

Add:

- `customEffectLibrary`

Suggested TypeScript type:

```ts
export interface CustomEffectLibraryEntry {
  id: string;
  name: string;
  category: string;
  description?: string;
  baseEffectType: string;
  moduleResourceType: string;
  moduleResourceId: string;
  latestRevisionId?: string;
  thumbnailDataUrl?: string;
  tags?: string[];
  defaultParams?: Record<string, number>;
  descriptorSummary?: {
    displayName?: string;
    category?: string;
    parameterCount?: number;
    resourceCount?: number;
  };
  origin?: "generated" | "imported" | "duplicated";
  createdAt?: string;
  updatedAt?: string;
}
```

### 2. Broadcast It With State

The backend state payload should include `customEffectLibrary` beside the other library payloads.

### 3. Hydrate Chooser Items From It

Add a new chooser merge path similar to blends/composites:

- `getCustomEffectFxItems()`

Each chooser item should map to:

- display name from the custom-effect entry
- category from the custom-effect entry
- description from the custom-effect entry
- thumbnail from the custom-effect entry
- underlying effect type set to `wasm_host`

## Effect Chooser Hydration Model

### Chooser Entry Identity

Each saved custom effect should have its own chooser identity.

That identity is not a new DSP processor type.

It is a library entry that says:

- when dropped, instantiate `wasm_host`
- immediately bind resource slot `0` to this module resource id

### Drag-And-Drop Payload

When a saved custom effect is dragged from the chooser, the payload should include enough information to create a ready-to-use `wasm_host` node.

Suggested drag payload:

```json
{
  "entryKind": "customEffect",
  "customEffectId": "airy-widen",
  "baseEffectType": "wasm_host",
  "moduleResourceType": "wasm",
  "moduleResourceId": "custom-effect:airy-widen:rev-0003",
  "name": "Airy Widen",
  "category": "modulation",
  "defaultParams": {
    "depth": 0.35,
    "rate": 0.22
  }
}
```

### Node Creation Result

Dropping the chooser entry should create a standard graph node that looks conceptually like:

```json
{
  "type": "wasm_host",
  "category": "modulation",
  "label": "Airy Widen",
  "params": {
    "depth": 0.35,
    "rate": 0.22
  },
  "config": {
    "customEffectId": "airy-widen"
  },
  "resources": [
    {
      "resourceType": "wasm",
      "resourceId": "custom-effect:airy-widen:rev-0003"
    }
  ]
}
```

After the module loads, the existing descriptor-refresh path should remain authoritative for final node metadata.

## `Use This Effect` Apply Flow

When the user is working inside a `Custom Effect` node and clicks `Use This Effect`, the flow should be:

1. create or update the local module resource entry
2. create or update the custom-effect chooser entry
3. set the current node’s slot `0` resource ref to `{ resourceType: "wasm", resourceId: <moduleResourceId> }`
4. persist `customEffectId` into node config
5. trigger the existing descriptor refresh path so the node label, category, parameters, and thumbnail align with the generated module

This means the active node becomes a normal `wasm_host` node pointing at a local library-backed module.

## `Save To My Custom Effects` Flow

When the user clicks `Save To My Custom Effects` without applying it to the current node:

1. create or update the local module resource entry
2. create or update the chooser entry
3. refresh UI state
4. do not modify the active node

This keeps “save” and “apply” as intentionally separate actions.

## Revision Semantics

Recommended rule:

- chooser entry id is stable across revisions
- module resource id changes per revision
- chooser entry points at the latest selected revision

This allows:

- updating an existing custom effect without losing its library identity
- keeping older revision folders for rollback or debug
- later adding explicit revision history UI without changing the base model

## Thumbnail And Summary Hydration

Chooser cards should not parse `.wasm` binaries on every render.

Instead:

- the import/apply pipeline should extract descriptor metadata once
- the backend should persist normalized summary fields into the custom-effect chooser index

Recommended cached chooser fields:

- `name`
- `category`
- `description`
- `thumbnailDataUrl`
- `parameterCount`
- `resourceCount`
- `defaultParams`

That keeps chooser rendering cheap and predictable.

## Migration And Compatibility

### Existing `wasm_host` Nodes

Existing raw `wasm_host` nodes without a saved chooser entry should continue to work unchanged.

They represent ad hoc module usage.

### Saved Custom Effects

Saved custom effects are a product-level convenience layer on top of the same runtime.

The system should tolerate both states:

- raw `wasm_host` node with direct module resource
- `wasm_host` node linked to a saved custom-effect chooser entry

## Suggested First Implementation Slice

If the goal is to move quickly, implement this in order:

1. add `customEffectLibrary` backend model and JSON index
2. save generated module binaries as local `wasm` resources
3. broadcast `customEffectLibrary` to the UI
4. add `getCustomEffectFxItems()` to the FX selector
5. add `Use This Effect` current-node apply flow
6. add `Save To My Custom Effects` without current-node mutation

That is enough to prove the local product loop:

- generate
- save
- apply
- browse later
- drag onto the chain again

without having to solve cloud publishing or revision history UI in the first slice.

## Summary

The cleanest local model is:

- store module binaries as local `wasm` resources
- store chooser entries in a dedicated `CustomEffectLibrary`
- hydrate chooser cards from that dedicated index
- always instantiate the existing `wasm_host` runtime

That gives users a simple `Custom Effect` mental model while keeping the engine architecture honest and consistent.