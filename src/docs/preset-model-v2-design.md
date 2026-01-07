# Preset Data Model v2 Design

## Overview

This document outlines a redesigned preset data model that supports:
1. **Top-level summary information** - metadata for browsing/searching
2. **Global parameters section** - master settings independent of signal path
3. **Flexible signal graph** - allowing splits, parallel paths, and mixing

## Design Goals

- **Simplicity** - Keep the model minimal while enabling flexibility
- **Signal Path Flexibility** - Support splitting amp/cab into parallel paths
- **Backwards Compatible** - Support migration from v1 presets
- **Serialization-Friendly** - Easy JSON/binary representation

---

## Proposed Data Model

### Top-Level Preset Structure

```cpp
struct PresetV2 {
    // === Metadata (for browsing/indexing) ===
    std::string id;                    // Unique identifier (UUID)
    std::string name;                  // Display name
    std::string author;                // Creator name (optional)
    std::string category;              // "Clean", "Crunch", "High Gain", "Bass", etc.
    std::string description;           // User-facing description
    int version = 2;                   // Schema version for migration
    std::string createdAt;             // ISO8601 timestamp
    std::string modifiedAt;            // ISO8601 timestamp
    std::vector<std::string> tags;     // Searchable tags
    
    // === Global Parameters ===
    GlobalSettings global;
    
    // === Signal Graph ===
    SignalGraph graph;
    
    // === Attachments (embedded model/IR data) ===
    std::vector<PresetAttachment> attachments;
};
```

### Global Settings

Parameters that apply to the entire preset, independent of signal routing:

```cpp
struct GlobalSettings {
    double inputTrim = 0.0;            // Input gain in dB
    double outputTrim = 0.0;           // Master output in dB
    double mix = 1.0;                  // Wet/dry mix (0.0-1.0)
    int transpose = 0;                 // Pitch shift in semitones
    
    // Noise gate (pre-signal path)
    bool gateEnabled = false;
    double gateThreshold = -60.0;      // dB
    
    // Optional future: input conditioning, tuner reference, etc.
};
```

### Signal Graph Model

A simple directed graph where nodes are processing units and edges define signal flow.

```cpp
// Node types that can appear in the signal graph
enum class NodeType {
    Input,          // Graph entry point
    Output,         // Graph exit point  
    Splitter,       // 1 input → N outputs (signal copy)
    Mixer,          // N inputs → 1 output (sum/average)
    Amp,            // NAM model processor
    Cab,            // IR convolution or simple cab sim
    EQ,             // Parametric EQ
    Delay,          // Delay effect
    Reverb,         // Reverb effect
    // Future: Compressor, Chorus, Phaser, etc.
};

struct GraphNode {
    std::string id;                    // Node identifier (for edge references)
    NodeType type;
    bool enabled = true;               // Bypass toggle
    std::map<std::string, double> params;  // Type-specific parameters
    std::string resourceId;            // Reference to attachment (for Amp/Cab)
};

struct GraphEdge {
    std::string from;                  // Source node ID
    std::string to;                    // Destination node ID
    int fromPort = 0;                  // Output port index (for splitters)
    int toPort = 0;                    // Input port index (for mixers)
    double gain = 1.0;                 // Edge gain (for mixing balance)
};

struct SignalGraph {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
};
```

---

## Example Presets

### Simple Linear Chain (Most Common)

```json
{
  "id": "preset-001",
  "name": "Classic Crunch",
  "version": 2,
  "category": "Crunch",
  "global": {
    "inputTrim": -3.0,
    "outputTrim": 0.0,
    "mix": 1.0,
    "gateEnabled": true,
    "gateThreshold": -55.0
  },
  "graph": {
    "nodes": [
      { "id": "in", "type": "Input" },
      { "id": "amp", "type": "Amp", "resourceId": "att-nam-001", "params": { "drive": 0.6, "tone": 0.5 } },
      { "id": "cab", "type": "Cab", "resourceId": "att-ir-001" },
      { "id": "eq", "type": "EQ", "params": { "lowGain": 2.0, "lowFreq": 80, "midGain": -1.0, "midFreq": 400, "highGain": 1.5, "highFreq": 8000 } },
      { "id": "out", "type": "Output" }
    ],
    "edges": [
      { "from": "in", "to": "amp" },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "eq" },
      { "from": "eq", "to": "out" }
    ]
  },
  "attachments": [
    { "id": "att-nam-001", "type": "nam", "hash": "abc123...", "filePath": "models/plexi.nam" },
    { "id": "att-ir-001", "type": "ir", "hash": "def456...", "filePath": "ir/4x12.wav" }
  ]
}
```

### Dual Cab (Split After Amp)

Classic technique: one amp, two different cabs blended together.

```json
{
  "id": "preset-002",
  "name": "Blended Cabs",
  "version": 2,
  "graph": {
    "nodes": [
      { "id": "in", "type": "Input" },
      { "id": "amp", "type": "Amp", "resourceId": "att-nam-001" },
      { "id": "split", "type": "Splitter" },
      { "id": "cab1", "type": "Cab", "resourceId": "att-ir-sm57" },
      { "id": "cab2", "type": "Cab", "resourceId": "att-ir-421" },
      { "id": "mix", "type": "Mixer" },
      { "id": "out", "type": "Output" }
    ],
    "edges": [
      { "from": "in", "to": "amp" },
      { "from": "amp", "to": "split" },
      { "from": "split", "to": "cab1", "fromPort": 0, "gain": 0.6 },
      { "from": "split", "to": "cab2", "fromPort": 1, "gain": 0.4 },
      { "from": "cab1", "to": "mix", "toPort": 0 },
      { "from": "cab2", "to": "mix", "toPort": 1 },
      { "from": "mix", "to": "out" }
    ]
  }
}
```

### Parallel Amp Paths (Wet/Dry Rig)

One path through the amp/cab, one clean path blended back.

```json
{
  "id": "preset-003",
  "name": "Wet/Dry Blend",
  "graph": {
    "nodes": [
      { "id": "in", "type": "Input" },
      { "id": "split", "type": "Splitter" },
      { "id": "amp", "type": "Amp", "resourceId": "att-nam-001" },
      { "id": "cab", "type": "Cab", "resourceId": "att-ir-001" },
      { "id": "mix", "type": "Mixer" },
      { "id": "out", "type": "Output" }
    ],
    "edges": [
      { "from": "in", "to": "split" },
      { "from": "split", "to": "amp", "fromPort": 0, "gain": 0.7 },
      { "from": "split", "to": "mix", "fromPort": 1, "toPort": 1, "gain": 0.3 },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "mix", "toPort": 0 },
      { "from": "mix", "to": "out" }
    ]
  }
}
```

---

## Implementation Plan

### Phase 1: Core Data Structures
1. Create `PresetTypesV2.h` with new structs
2. Add JSON serialization/deserialization in `PresetStorageV2.cpp`
3. Add migration function: `PresetV1 → PresetV2`

### Phase 2: Graph Execution Engine
1. Create `SignalGraphExecutor` class
2. Topological sort for execution order
3. Buffer management for parallel paths
4. Integrate with `NAMDSPManager`

### Phase 3: UI Integration
1. Update preset JSON format in JavaScript
2. Add visual graph editor (future)
3. Quick presets for common patterns:
   - "Linear" (Amp → Cab → EQ → FX)
   - "Dual Cab" (Amp → Split → 2x Cab → Mix)
   - "Parallel" (Split → Amp Path / Clean Path → Mix)

### Phase 4: Testing & Migration
1. Update `PresetStorageTests.cpp` for v2 format
2. Create graph execution tests
3. Ensure v1 presets auto-migrate on load

---

## Simplifications Made

1. **No arbitrary routing** - Splitter/Mixer pattern keeps graph simple
2. **Single input/output** - No multi-input/output presets (stereo handled internally)
3. **Fixed node types** - Extensible via enum, not arbitrary plugins
4. **Parameters in nodes** - Each node owns its params, no shared parameter buses
5. **Attachments by reference** - Nodes reference attachments by ID, not embedded data

---

## File Locations

```
src/presets/
├── PresetTypes.h          # Keep for backwards compat
├── PresetTypesV2.h        # New data model
├── PresetStorage.h/cpp    # Keep for v1
├── PresetStorageV2.h/cpp  # New serialization
├── PresetMigration.h/cpp  # v1 → v2 conversion
└── SignalGraph.h/cpp      # Graph data structures

src/dsp/
├── SignalGraphExecutor.h/cpp  # Graph execution engine
└── NodeProcessors.h/cpp       # Per-node-type processors
```

---

## Migration Strategy

When loading a preset:
1. Check `version` field (missing = v1)
2. If v1, call `MigrateV1ToV2()`:
   - Copy metadata fields
   - Create linear graph: Input → Amp → Cab → EQ → Delay → Reverb → Output
   - Map `fxChain` to enabled nodes
   - Map parameters to appropriate nodes
3. Save migrated preset in v2 format

---

## Questions to Resolve

1. **Stereo handling** - Should splitter create stereo pairs or mono duplicates?
2. **Parameter smoothing** - Per-node or global smoothing strategy?
3. **Graph validation** - How strict on cycles, disconnected nodes?
4. **Max complexity** - Limit on node count for performance?
