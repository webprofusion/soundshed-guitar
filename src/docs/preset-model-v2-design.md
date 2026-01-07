# Preset Data Model v2 Design

## Overview

This document outlines a redesigned preset data model that supports:
1. **Top-level summary information** - metadata for browsing/searching
2. **Global parameters section** - master settings independent of signal path
3. **Flexible signal graph** - allowing any effect in any order, splits, parallel paths, and mixing

## Design Goals

- **Simplicity** - Keep the model minimal while enabling flexibility
- **Extensibility** - Support arbitrary effect types via string identifiers
- **Signal Path Flexibility** - Any effect in any order, parallel paths, multiple instances
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
    
    // === Embedded Resources (optional, for custom files only) ===
    std::vector<EmbeddedResource> embeddedResources;
};
```

### Global Settings

Parameters that apply to the entire preset, independent of signal routing:

```cpp
struct GlobalSettings {
    double inputTrim = 0.0;            // Input gain in dB
    double outputTrim = 0.0;           // Master output in dB
    int transpose = 0;                 // Pitch shift in semitones
    // Optional future: tuner reference, MIDI channel, etc.
};
```

### Signal Graph Model

A directed graph where nodes are processing units identified by string type.
Any effect can appear in any order and multiple times.

```cpp
/**
 * Reference to a resource (NAM model, IR, etc.)
 * 
 * Resources can be referenced in two ways:
 * 1. Library resource: Use resourceType + resourceId to reference a pre-defined
 *    resource from the library. When the library resource is updated, all presets
 *    using it automatically get the updated version.
 * 2. Custom file: Use filePath for user-provided files not in the library.
 *    Optionally reference an embeddedResource by embeddedId for portable presets.
 */
struct ResourceRef {
    // Option 1: Library resource (preferred)
    std::string resourceType;          // "nam", "ir", etc.
    std::string resourceId;            // Library ID (e.g., "plexi-bright", "4x12-sm57")
    
    // Option 2: Custom file path
    std::filesystem::path filePath;    // Direct file path (relative or absolute)
    
    // Option 3: Embedded resource reference (for portable presets)
    std::string embeddedId;            // References EmbeddedResource.id in preset
};

/**
 * Embedded resource for custom files (optional, for portability).
 * Only needed when sharing presets with custom files not in the library.
 */
struct EmbeddedResource {
    std::string id;                    // Reference ID within this preset
    std::string type;                  // "nam", "ir", etc.
    std::string name;                  // Display name
    std::string hash;                  // SHA-256 for verification
    std::string data;                  // Base64-encoded file data (optional)
    std::filesystem::path originalPath; // Original file path (for reference)
};

/**
 * A node in the signal graph.
 * 
 * Node types are arbitrary strings allowing unlimited extensibility.
 * The category helps UI grouping but doesn't restrict placement.
 * 
 * Reserved types for routing:
 *   "input"    - Graph entry point (exactly one required)
 *   "output"   - Graph exit point (exactly one required)
 *   "splitter" - 1 input → N outputs (signal copy)
 *   "mixer"    - N inputs → 1 output (sum with optional gain per input)
 */
struct GraphNode {
    std::string id;                    // Unique node ID within this graph
    std::string type;                  // Effect type identifier (e.g., "nam_amp", "ir_cab", "eq_parametric")
    std::string category;              // UI grouping: "amp", "cab", "eq", "dynamics", "modulation", "delay", "reverb", "utility"
    std::string label;                 // Optional display name override
    bool enabled = true;               // Bypass toggle
    
    // Parameters - flexible key/value for any effect type
    std::map<std::string, double> params;      // Numeric parameters
    std::map<std::string, std::string> config; // String config (e.g., algorithm selection)
    
    // Resource reference (for effects that need external files)
    std::optional<ResourceRef> resource;
};

struct GraphEdge {
    std::string from;                  // Source node ID
    std::string to;                    // Destination node ID
    int fromPort = 0;                  // Output port index (for splitters: 0, 1, 2...)
    int toPort = 0;                    // Input port index (for mixers: 0, 1, 2...)
    double gain = 1.0;                 // Edge gain multiplier (for mixing balance)
};

struct SignalGraph {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
};
```

---

## Resource Library

Pre-defined resources are stored in a library and referenced by type + ID.

```cpp
/**
 * A resource in the library (NAM model, IR, etc.)
 * These are managed separately from presets.
 */
struct LibraryResource {
    std::string type;                  // "nam", "ir", etc.
    std::string id;                    // Unique ID within type (e.g., "plexi-bright")
    std::string name;                  // Display name
    std::string category;              // Grouping (e.g., "Marshall", "Fender", "Mesa")
    std::string description;           // User-facing description
    std::filesystem::path filePath;    // Actual file location
    std::string hash;                  // SHA-256 for verification
    std::vector<std::string> tags;     // Searchable tags
};

/**
 * Library of pre-defined resources.
 * When a resource is updated, all presets using it get the update.
 */
class ResourceLibrary {
public:
    void AddResource(const LibraryResource& resource);
    void UpdateResource(const std::string& type, const std::string& id, const LibraryResource& updated);
    void RemoveResource(const std::string& type, const std::string& id);
    
    std::optional<LibraryResource> FindResource(const std::string& type, const std::string& id) const;
    std::vector<LibraryResource> GetResourcesByType(const std::string& type) const;
    std::vector<LibraryResource> GetResourcesByCategory(const std::string& type, const std::string& category) const;
    
    // Resolve a ResourceRef to an actual file path
    std::optional<std::filesystem::path> ResolveResource(const ResourceRef& ref) const;
};
```

---

## Effect Type Registry (Runtime)

Effects are registered at runtime. The preset only stores type strings.

```cpp
/**
 * Describes an available effect type.
 * This is NOT stored in presets - it's runtime metadata.
 */
struct EffectTypeInfo {
    std::string type;                  // Unique type ID (e.g., "nam_amp_v1")
    std::string displayName;           // "NAM Amp"
    std::string category;              // "amp", "cab", "eq", "dynamics", "modulation", "delay", "reverb", "utility"
    std::string description;           // User-facing description
    bool requiresResource = false;     // Does this need an external file (model, IR, etc.)?
    std::vector<ParameterDef> params;  // Parameter definitions for UI
};

struct ParameterDef {
    std::string id;
    std::string displayName;
    double defaultValue;
    double minValue;
    double maxValue;
    std::string unit;                  // "dB", "Hz", "ms", "%", etc.
};
```

### Example Effect Types

| Type ID | Category | Description |
|---------|----------|-------------|
| `nam_amp` | amp | NAM neural amp model |
| `amp_clean` | amp | Built-in clean amp sim |
| `amp_crunch` | amp | Built-in crunch amp sim |
| `ir_cab` | cab | IR-based cabinet (convolution) |
| `cab_simple` | cab | Algorithmic cabinet simulation |
| `eq_parametric` | eq | 4-band parametric EQ |
| `eq_graphic` | eq | 10-band graphic EQ |
| `eq_tilt` | eq | Simple tilt EQ |
| `comp_opto` | dynamics | Optical compressor |
| `comp_vca` | dynamics | VCA compressor |
| `comp_fet` | dynamics | FET compressor |
| `gate_noise` | dynamics | Noise gate |
| `chorus_analog` | modulation | Analog-style chorus |
| `chorus_digital` | modulation | Digital chorus |
| `flanger` | modulation | Flanger |
| `phaser` | modulation | Phaser |
| `tremolo` | modulation | Tremolo |
| `vibrato` | modulation | Vibrato |
| `delay_digital` | delay | Digital delay |
| `delay_tape` | delay | Tape delay emulation |
| `delay_analog` | delay | Analog bucket-brigade delay |
| `reverb_room` | reverb | Room reverb |
| `reverb_hall` | reverb | Hall reverb |
| `reverb_plate` | reverb | Plate reverb |
| `reverb_spring` | reverb | Spring reverb |
| `reverb_shimmer` | reverb | Shimmer reverb |
| `gain` | utility | Simple gain stage |
| `splitter` | utility | Signal splitter (1→N) |
| `mixer` | utility | Signal mixer (N→1) |

---

## Example Presets

### Simple Linear Chain (Library Resources)

Using pre-defined library resources - no embedded data needed.

```json
{
  "id": "preset-001",
  "name": "Classic Crunch",
  "version": 2,
  "category": "Crunch",
  "global": {
    "inputTrim": -3.0,
    "outputTrim": 0.0
  },
  "graph": {
    "nodes": [
      { "id": "in", "type": "input" },
      { "id": "gate", "type": "gate_noise", "category": "dynamics", "params": { "threshold": -55.0, "release": 50 } },
      { 
        "id": "amp", 
        "type": "nam_amp", 
        "category": "amp", 
        "resource": { "resourceType": "nam", "resourceId": "plexi-bright" },
        "params": { "drive": 0.6, "tone": 0.5 } 
      },
      { 
        "id": "cab", 
        "type": "ir_cab", 
        "category": "cab", 
        "resource": { "resourceType": "ir", "resourceId": "4x12-sm57" }
      },
      { "id": "eq", "type": "eq_parametric", "category": "eq", "params": { "lowGain": 2.0, "lowFreq": 80, "midGain": -1.0, "midFreq": 400, "highGain": 1.5, "highFreq": 8000 } },
      { "id": "out", "type": "output" }
    ],
    "edges": [
      { "from": "in", "to": "gate" },
      { "from": "gate", "to": "amp" },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "eq" },
      { "from": "eq", "to": "out" }
    ]
  }
}
```

### Custom File Path (User's Own NAM Model)

Using a file path for a custom NAM model not in the library.

```json
{
  "id": "preset-002",
  "name": "My Custom Amp",
  "version": 2,
  "graph": {
    "nodes": [
      { "id": "in", "type": "input" },
      { 
        "id": "amp", 
        "type": "nam_amp", 
        "category": "amp", 
        "resource": { "filePath": "C:/Users/me/NAM Models/my-custom-amp.nam" },
        "params": { "drive": 0.5 } 
      },
      { 
        "id": "cab", 
        "type": "ir_cab", 
        "category": "cab", 
        "resource": { "resourceType": "ir", "resourceId": "4x12-sm57" }
      },
      { "id": "out", "type": "output" }
    ],
    "edges": [
      { "from": "in", "to": "amp" },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "out" }
    ]
  }
}
```

### Portable Preset with Embedded Resource

For sharing presets with custom files - embed the data.

```json
{
  "id": "preset-003",
  "name": "Shared Custom Tone",
  "version": 2,
  "graph": {
    "nodes": [
      { "id": "in", "type": "input" },
      { 
        "id": "amp", 
        "type": "nam_amp", 
        "category": "amp", 
        "resource": { "embeddedId": "emb-001" },
        "params": { "drive": 0.7 } 
      },
      { 
        "id": "cab", 
        "type": "ir_cab", 
        "category": "cab", 
        "resource": { "resourceType": "ir", "resourceId": "2x12-jazz" }
      },
      { "id": "out", "type": "output" }
    ],
    "edges": [
      { "from": "in", "to": "amp" },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "out" }
    ]
  },
  "embeddedResources": [
    {
      "id": "emb-001",
      "type": "nam",
      "name": "Custom Plexi Capture",
      "hash": "abc123def456...",
      "data": "base64encodedNAMfiledata..."
    }
  ]
}
```

### Dual Cab with Different Cab Types

Blend a library IR cab with an algorithmic simple cab.

```json
{
  "id": "preset-004",
  "name": "Blended Cabs",
  "version": 2,
  "graph": {
    "nodes": [
      { "id": "in", "type": "input" },
      { 
        "id": "amp", 
        "type": "nam_amp", 
        "category": "amp", 
        "resource": { "resourceType": "nam", "resourceId": "jcm800-hot" }
      },
      { "id": "split", "type": "splitter", "category": "utility" },
      { 
        "id": "cab1", 
        "type": "ir_cab", 
        "category": "cab", 
        "resource": { "resourceType": "ir", "resourceId": "4x12-sm57" }
      },
      { 
        "id": "cab2", 
        "type": "cab_simple", 
        "category": "cab", 
        "params": { "bass": 0.6, "presence": 0.5, "brightness": 0.4 } 
      },
      { "id": "mix", "type": "mixer", "category": "utility" },
      { "id": "out", "type": "output" }
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

### Effects Before Amp (Unconventional Chain)

Reverb before amp for ambient textures - any order is valid.

```json
{
  "id": "preset-005",
  "name": "Ambient Swells",
  "version": 2,
  "graph": {
    "nodes": [
      { "id": "in", "type": "input" },
      { "id": "comp", "type": "comp_opto", "category": "dynamics", "params": { "threshold": -20, "ratio": 4, "attack": 10, "release": 200 } },
      { "id": "reverb1", "type": "reverb_shimmer", "category": "reverb", "params": { "decay": 0.8, "shimmer": 0.6, "mix": 0.4 } },
      { "id": "amp", "type": "amp_clean", "category": "amp", "params": { "gain": 0.3, "tone": 0.6 } },
      { "id": "cab", "type": "cab_simple", "category": "cab" },
      { "id": "delay", "type": "delay_tape", "category": "delay", "params": { "time": 375, "feedback": 0.4, "mix": 0.3 } },
      { "id": "out", "type": "output" }
    ],
    "edges": [
      { "from": "in", "to": "comp" },
      { "from": "comp", "to": "reverb1" },
      { "from": "reverb1", "to": "amp" },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "delay" },
      { "from": "delay", "to": "out" }
    ]
  }
}
```

### Multiple Instances of Same Effect

Two EQs and two compressors in one chain.

```json
{
  "id": "preset-006",
  "name": "Studio Polish",
  "version": 2,
  "graph": {
    "nodes": [
      { "id": "in", "type": "input" },
      { "id": "eq_pre", "type": "eq_parametric", "category": "eq", "label": "Pre EQ", "params": { "lowCut": 80 } },
      { "id": "comp1", "type": "comp_fet", "category": "dynamics", "label": "FET Comp", "params": { "ratio": 8, "attack": 1 } },
      { 
        "id": "amp", 
        "type": "nam_amp", 
        "category": "amp", 
        "resource": { "resourceType": "nam", "resourceId": "twin-reverb" }
      },
      { 
        "id": "cab", 
        "type": "ir_cab", 
        "category": "cab", 
        "resource": { "resourceType": "ir", "resourceId": "2x12-jazz" }
      },
      { "id": "eq_post", "type": "eq_parametric", "category": "eq", "label": "Post EQ", "params": { "highShelf": 3.0, "highFreq": 8000 } },
      { "id": "comp2", "type": "comp_opto", "category": "dynamics", "label": "Opto Comp", "params": { "ratio": 2, "attack": 30 } },
      { "id": "out", "type": "output" }
    ],
    "edges": [
      { "from": "in", "to": "eq_pre" },
      { "from": "eq_pre", "to": "comp1" },
      { "from": "comp1", "to": "amp" },
      { "from": "amp", "to": "cab" },
      { "from": "cab", "to": "eq_post" },
      { "from": "eq_post", "to": "comp2" },
      { "from": "comp2", "to": "out" }
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

### Phase 2: Effect Type Registry
1. Create `EffectRegistry` class for runtime effect type registration
2. Define `EffectProcessor` interface for all effects
3. Implement processor classes for each built-in effect type
4. Support dynamic loading of effect types (future: plugins)

### Phase 3: Graph Execution Engine
1. Create `SignalGraphExecutor` class
2. Topological sort for execution order
3. Buffer management for parallel paths
4. Handle unknown effect types gracefully (bypass with warning)

### Phase 4: UI Integration
1. Update preset JSON format in JavaScript
2. Effect browser organized by category
3. Drag-and-drop graph editor (future)
4. Quick templates for common patterns

### Phase 5: Testing & Migration
1. Update `PresetStorageTests.cpp` for v2 format
2. Create graph execution tests
3. Ensure v1 presets auto-migrate on load
4. Test with unknown/missing effect types

---

## Simplifications Made

1. **String-based types** - No compile-time enum, unlimited extensibility
2. **Categories for UI only** - Don't restrict placement, just help browsing
3. **Single input/output** - Stereo handled internally per node
4. **Parameters as maps** - Flexible key/value, no fixed schema per effect
5. **Graceful degradation** - Unknown types bypass, don't crash
6. **Resource references** - Library resources by type+id, custom files by path
7. **Optional embedding** - Only embed data when sharing presets with custom files

---

## File Locations

```
src/presets/
├── PresetTypes.h          # Keep for backwards compat (v1)
├── PresetTypesV2.h        # New data model
├── PresetStorage.h/cpp    # Keep for v1
├── PresetStorageV2.h/cpp  # New serialization
├── PresetMigration.h/cpp  # v1 → v2 conversion
└── SignalGraph.h/cpp      # Graph data structures

src/resources/
├── ResourceLibrary.h/cpp  # Library resource management
├── ResourceRef.h          # Resource reference types
└── EmbeddedResource.h     # Embedded resource handling

src/dsp/
├── EffectRegistry.h/cpp       # Effect type registration
├── EffectProcessor.h          # Base interface for all effects
├── SignalGraphExecutor.h/cpp  # Graph execution engine
└── effects/                   # Effect implementations by category
    ├── amps/
    │   ├── NAMAmp.cpp
    │   ├── CleanAmp.cpp
    │   └── CrunchAmp.cpp
    ├── cabs/
    │   ├── IRCab.cpp
    │   └── SimpleCab.cpp
    ├── dynamics/
    │   ├── NoiseGate.cpp
    │   ├── OptoCompressor.cpp
    │   ├── VCACompressor.cpp
    │   └── FETCompressor.cpp
    ├── eq/
    │   ├── ParametricEQ.cpp
    │   ├── GraphicEQ.cpp
    │   └── TiltEQ.cpp
    ├── modulation/
    │   ├── Chorus.cpp
    │   ├── Flanger.cpp
    │   ├── Phaser.cpp
    │   └── Tremolo.cpp
    ├── delay/
    │   ├── DigitalDelay.cpp
    │   ├── TapeDelay.cpp
    │   └── AnalogDelay.cpp
    ├── reverb/
    │   ├── RoomReverb.cpp
    │   ├── HallReverb.cpp
    │   ├── PlateReverb.cpp
    │   └── SpringReverb.cpp
    └── utility/
        ├── Gain.cpp
        ├── Splitter.cpp
        └── Mixer.cpp
```

---

## Migration Strategy

When loading a preset:
1. Check `version` field (missing = v1)
2. If v1, call `MigrateV1ToV2()`:
   - Copy metadata fields
   - Create linear graph based on v1 `fxChain`:
     - Input → [Gate if enabled] → Amp → Cab → [EQ if enabled] → [Delay if enabled] → [Reverb if enabled] → Output
   - Map `audioFxModelId` → `nam_amp` node with resourceId
   - Map `irId` → `ir_cab` node with resourceId
   - Map parameters to appropriate node params
3. Save migrated preset in v2 format

---

## Effect Processor Interface

```cpp
/**
 * Base interface for all effect processors.
 * Each effect type implements this interface.
 */
class EffectProcessor {
public:
    virtual ~EffectProcessor() = default;
    
    // Lifecycle
    virtual void Prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void Reset() = 0;
    
    // Processing (stereo in/out)
    virtual void Process(float** inputs, float** outputs, int numSamples) = 0;
    
    // Parameters
    virtual void SetParam(const std::string& key, double value) = 0;
    virtual void SetConfig(const std::string& key, const std::string& value) = 0;
    virtual double GetParam(const std::string& key) const = 0;
    
    // Resource loading (for effects that need external files)
    virtual bool LoadResource(const std::filesystem::path& path) { return true; }
    virtual bool RequiresResource() const { return false; }
    
    // Bypass
    virtual void SetEnabled(bool enabled) { mEnabled = enabled; }
    virtual bool IsEnabled() const { return mEnabled; }
    
protected:
    bool mEnabled = true;
};

/**
 * Factory function type for creating effect processors.
 */
using EffectFactory = std::function<std::unique_ptr<EffectProcessor>()>;

/**
 * Registry for effect types.
 */
class EffectRegistry {
public:
    static EffectRegistry& Instance();
    
    void Register(const std::string& type, const EffectTypeInfo& info, EffectFactory factory);
    std::unique_ptr<EffectProcessor> Create(const std::string& type);
    
    std::vector<EffectTypeInfo> GetAllTypes() const;
    std::vector<EffectTypeInfo> GetTypesByCategory(const std::string& category) const;
    bool HasType(const std::string& type) const;
    
private:
    std::map<std::string, EffectTypeInfo> mTypeInfo;
    std::map<std::string, EffectFactory> mFactories;
};
```

---

## Questions to Resolve

1. **Stereo handling** - Should each node declare mono/stereo preference?
2. **Parameter smoothing** - Per-effect or centralized smoothing?
3. **Graph validation** - How strict? Allow cycles for feedback effects?
4. **Max complexity** - Limit on node count for performance?
5. **Unknown types** - Bypass silently, show warning, or block preset load?
6. **Version compatibility** - How to handle presets using newer effect types?
7. **Resource resolution priority** - Library > filePath > embedded? Or configurable?
8. **Missing resources** - What happens if library resource is deleted?
