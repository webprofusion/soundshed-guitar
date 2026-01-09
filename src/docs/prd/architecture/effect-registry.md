# Effect Registry Specification

## Overview

The Effect Registry provides a dynamic registration system for audio effect types in NeuronGuitar. It enables extensibility by allowing new effect types to be added without modifying core code, and provides metadata for UI generation and preset validation.

## Design Goals

1. **Extensibility**: Add new effects without core changes
2. **Discoverability**: UI can enumerate available effects
3. **Validation**: Verify effect types and parameters in presets
4. **Versioning**: Support effect type evolution
5. **Documentation**: Self-describing effect types

## Registry Architecture

```
┌─────────────────────────────────────────┐
│            Effect Registry              │
│  ┌─────────────────────────────────┐    │
│  │    Type Registration            │    │
│  ├─────────────────────────────────┤    │
│  │    Factory Creation             │    │
│  ├─────────────────────────────────┤    │
│  │    Metadata Queries             │    │
│  └─────────────────────────────────┘    │
└──────────────────┬──────────────────────┘
                   │
    ┌──────────────┼──────────────┐
    ▼              ▼              ▼
┌────────┐    ┌────────┐    ┌────────┐
│ NAM    │    │ IR Cab │    │ EQ     │
│ Effect │    │ Effect │    │ Effect │
└────────┘    └────────┘    └────────┘
```

## Effect Type Info

### Structure

```
struct EffectTypeInfo:
    // Identity
    type_id: string             # Unique type identifier
    version: int                # Type version number
    display_name: string        # Human-readable name
    description: string         # User-facing description
    
    // Classification
    category: EffectCategory    # UI grouping
    tags: list[string]          # Searchable tags
    
    // Capabilities
    requires_resource: bool     # Needs external file (model, IR)
    resource_types: list[string] # Accepted resource types
    supports_stereo: bool       # Stereo processing capable
    has_sidechain: bool         # Sidechain input support
    
    // Processing
    default_latency: int        # Typical latency in samples
    cpu_intensity: CpuLevel     # Light, Medium, Heavy
    
    // Parameters
    parameters: list[ParameterDef]
    
    // Factory
    create_instance: function() -> EffectProcessor
```

### Effect Categories

| Category | Description | Examples |
|----------|-------------|----------|
| `amp` | Amplifier simulation | NAM amp, clean amp, crunch amp |
| `cab` | Cabinet simulation | IR cab, simple cab |
| `eq` | Equalization | Parametric EQ, graphic EQ, tilt |
| `dynamics` | Dynamics processing | Gate, compressor, limiter |
| `distortion` | Distortion/saturation | Drive, fuzz, overdrive |
| `modulation` | Modulation effects | Chorus, flanger, phaser |
| `delay` | Time-based delay | Digital, tape, analog delay |
| `reverb` | Reverberation | Room, hall, plate, spring |
| `utility` | Utility processing | Gain, splitter, mixer |

### CPU Intensity Levels

| Level | Description | Typical Usage |
|-------|-------------|---------------|
| Light | < 1% CPU | Gain, simple EQ |
| Medium | 1-5% CPU | Dynamics, modulation |
| Heavy | > 5% CPU | Neural models, convolution |

## Parameter Definition

### Structure

```
struct ParameterDef:
    // Identity
    id: string                  # Unique within effect type
    display_name: string        # UI label
    description: string         # Tooltip text
    
    // Value specification
    type: ParameterType         # Float, Int, Bool, Enum
    default_value: any          # Type-appropriate default
    
    // Range (for numeric types)
    min_value: float
    max_value: float
    step: float                 # Increment step
    
    // Display
    unit: string                # dB, Hz, ms, %
    display_curve: DisplayCurve # Linear, Log, Exp
    format_string: string       # e.g., "%.1f dB"
    
    // Enum options (for Enum type)
    enum_options: list[EnumOption]
    
    // Behavior
    smoothing_time: float       # Parameter smoothing in ms
    automatable: bool           # Exposed for DAW automation
```

### Parameter Types

| Type | Description | Example |
|------|-------------|---------|
| Float | Continuous value | Drive (0.0-1.0) |
| Int | Integer value | Delay time in ms |
| Bool | On/off toggle | Bypass |
| Enum | Selection from list | Algorithm type |

### Display Curves

| Curve | Use Case |
|-------|----------|
| Linear | Most parameters |
| Logarithmic | Frequency controls |
| Exponential | Time controls |

## Registry Interface

### Registration

```
interface EffectRegistry:
    // Register effect type
    method register(info: EffectTypeInfo)
    
    // Unregister (for dynamic plugins)
    method unregister(type_id: string)
    
    // Check registration
    method is_registered(type_id: string) -> bool
```

### Queries

```
interface EffectRegistry:
    // Get type info
    method get_type_info(type_id: string) -> EffectTypeInfo?
    
    // List all types
    method get_all_types() -> list[EffectTypeInfo]
    
    // Filter by category
    method get_types_by_category(category: string) -> list[EffectTypeInfo]
    
    // Search by name/tags
    method search_types(query: string) -> list[EffectTypeInfo]
```

### Factory

```
interface EffectRegistry:
    // Create instance
    method create_effect(type_id: string) -> EffectProcessor?
    
    // Create with initial params
    method create_effect(type_id: string, params: map) -> EffectProcessor?
```

## Built-in Effect Types

### NAM Amp (`nam_amp`)

Neural Amp Modeler amplifier simulation.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| drive | 0.0-1.0 | 0.5 | - |
| tone | 0.0-1.0 | 0.5 | - |
| output | -20.0-+20.0 | 0.0 | dB |

**Resource**: NAM model file (.nam)

### IR Cabinet (`ir_cab`)

Impulse response cabinet simulation.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| mix | 0.0-1.0 | 1.0 | - |
| low_cut | 20-500 | 80 | Hz |
| high_cut | 2000-20000 | 12000 | Hz |

**Resource**: IR file (.wav, .aiff)

### Noise Gate (`gate_noise`)

Input noise gate.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| threshold | -80.0-0.0 | -50.0 | dB |
| attack | 0.1-50.0 | 1.0 | ms |
| hold | 0.0-500.0 | 50.0 | ms |
| release | 10.0-1000.0 | 100.0 | ms |

### Parametric EQ (`eq_parametric`)

4-band parametric equalizer.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| low_freq | 20-500 | 80 | Hz |
| low_gain | -15.0-+15.0 | 0.0 | dB |
| low_q | 0.1-10.0 | 0.7 | - |
| mid1_freq | 100-2000 | 400 | Hz |
| mid1_gain | -15.0-+15.0 | 0.0 | dB |
| mid1_q | 0.1-10.0 | 1.0 | - |
| mid2_freq | 500-8000 | 2000 | Hz |
| mid2_gain | -15.0-+15.0 | 0.0 | dB |
| mid2_q | 0.1-10.0 | 1.0 | - |
| high_freq | 2000-20000 | 8000 | Hz |
| high_gain | -15.0-+15.0 | 0.0 | dB |
| high_q | 0.1-10.0 | 0.7 | - |

### Digital Delay (`delay_digital`)

Clean digital delay.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| time | 1-2000 | 300 | ms |
| feedback | 0.0-0.95 | 0.3 | - |
| mix | 0.0-1.0 | 0.3 | - |
| high_cut | 1000-20000 | 8000 | Hz |
| sync | false/true | false | - |

### Room Reverb (`reverb_room`)

Room reverberation.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| size | 0.0-1.0 | 0.5 | - |
| decay | 0.1-10.0 | 1.5 | s |
| damping | 0.0-1.0 | 0.5 | - |
| pre_delay | 0-100 | 10 | ms |
| mix | 0.0-1.0 | 0.2 | - |

### Chorus (`chorus_analog`)

Analog-style chorus.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| rate | 0.1-10.0 | 1.0 | Hz |
| depth | 0.0-1.0 | 0.5 | - |
| mix | 0.0-1.0 | 0.5 | - |
| voices | 1-4 | 2 | - |

## Registration Lifecycle

### Startup Registration

```
function initialize_registry():
    registry = EffectRegistry()
    
    // Register built-in effects
    registry.register(NAMAmpInfo)
    registry.register(IRCabInfo)
    registry.register(NoiseGateInfo)
    registry.register(ParametricEQInfo)
    // ... more built-in effects
    
    // Load external effect plugins (future)
    for plugin in find_effect_plugins():
        plugin.register_effects(registry)
    
    return registry
```

### Validation

```
function validate_effect_type(type_id: string) -> bool:
    return registry.is_registered(type_id)

function validate_effect_params(type_id: string, params: map) -> list[Error]:
    info = registry.get_type_info(type_id)
    if not info:
        return [Error("Unknown effect type")]
    
    errors = []
    for param_def in info.parameters:
        if param_def.id in params:
            value = params[param_def.id]
            if not in_range(value, param_def.min_value, param_def.max_value):
                errors.append(Error(f"Parameter {param_def.id} out of range"))
    
    return errors
```

## UI Integration

### Effect Palette

The registry provides data for the effect palette UI:

```json
{
  "categories": [
    {
      "id": "amp",
      "name": "Amplifiers",
      "effects": [
        {
          "typeId": "nam_amp",
          "name": "NAM Amp",
          "description": "Neural amp modeler",
          "requiresResource": true
        }
      ]
    }
  ]
}
```

### Parameter Editor

Generate parameter UI from definitions:

```
function generate_parameter_ui(effect_type: string):
    info = registry.get_type_info(effect_type)
    
    for param in info.parameters:
        match param.type:
            case Float:
                render_knob(param)
            case Int:
                render_slider(param)
            case Bool:
                render_toggle(param)
            case Enum:
                render_dropdown(param)
```

## Related Documents

- [Audio Engine Specification](./audio-engine.md)
- [Signal Graph Model](./signal-graph-model.md)
- [User Interface Specification](./user-interface.md)
