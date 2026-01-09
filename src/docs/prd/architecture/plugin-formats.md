# Plugin Format Specification

## Overview

NeuronGuitar supports multiple plugin formats to ensure compatibility with all major digital audio workstations (DAWs). Each format wrapper adapts the core plugin functionality to the specific API requirements of the target platform.

## Supported Formats

| Format | Platform | Host Examples |
|--------|----------|---------------|
| VST3 | Windows, macOS | Cubase, Ableton Live, FL Studio, Reaper |
| Audio Unit (AU) | macOS | Logic Pro, GarageBand, Ableton Live |
| AAX | Windows, macOS | Pro Tools |
| Standalone | Windows, macOS | Direct execution |

## Common Plugin Interface

All format wrappers implement a common internal interface:

```
interface PluginInterface:
    // Lifecycle
    method initialize(sample_rate, max_block_size)
    method shutdown()
    
    // Audio Processing
    method process(inputs, outputs, num_samples)
    method set_sample_rate(rate)
    method set_block_size(size)
    
    // Parameters
    method get_parameter_count() -> int
    method get_parameter_info(index) -> ParameterInfo
    method get_parameter(index) -> float
    method set_parameter(index, value)
    
    // State
    method get_state() -> bytes
    method set_state(data)
    
    // Presets
    method get_preset_count() -> int
    method get_preset_name(index) -> string
    method set_preset(index)
    
    // UI
    method create_editor(parent_window) -> EditorHandle
    method destroy_editor()
    method get_editor_size() -> (width, height)
```

## VST3 Format

### Overview

VST3 (Virtual Studio Technology 3) is Steinberg's plugin standard. It provides:
- Modern component-based architecture
- Efficient parameter handling
- Advanced bus configurations
- Note expression support

### Implementation Requirements

**Required Interfaces:**
- `IComponent` - Audio processor component
- `IAudioProcessor` - Audio processing callbacks
- `IEditController` - Parameter editing
- `IPlugView` - UI view management

**Categories:**
- Instrument: `Fx|Guitar`
- Sub-categories: `Amp Simulator`, `Effect`

### Parameter Mapping

VST3 parameters are identified by integer IDs:

| Param ID | Name | Range | Default |
|----------|------|-------|---------|
| 0 | Input Trim | -40..+20 | 0 |
| 1 | Output Trim | -40..+20 | 0 |
| 2+ | Graph parameters | Varies | Varies |

### State Serialization

State saved as:
1. Version header
2. Parameter values (normalized 0-1)
3. Preset JSON blob (compressed)

### Bus Configuration

- Input: 1 stereo bus or 2 mono buses
- Output: 1 stereo bus
- Side-chain: Optional (future)

## Audio Unit Format

### Overview

Audio Unit is Apple's native plugin format for macOS. Supported types:
- `aufx` - Audio effect
- `aumu` - Music device (future)

### Implementation Requirements

**Component Types:**
- Audio Unit v2 (legacy compatibility)
- Audio Unit v3 (App Extension)

**Required Properties:**
- Manufacturer code (4 chars)
- Type code (4 chars)
- Subtype code (4 chars)

### Parameter Mapping

AU parameters use:
- Parameter ID (uint32)
- Parameter scope (global, input, output)
- Element index

### State Format

Binary property list containing:
- Parameter dictionary
- Preset data blob

### Sandboxing

AU v3 requires:
- App Extension target
- Entitlements configuration
- IPC for UI communication

## AAX Format

### Overview

AAX (Avid Audio eXtension) is Avid's format for Pro Tools. Requires:
- Avid developer agreement
- AAX SDK access
- Code signing for release

### Implementation Requirements

**Component Types:**
- AAX Native (CPU processing)
- AAX DSP (future, for HDX hardware)

**Plugin Categories:**
- `AAX_ePlugInCategory_Effect`
- `AAX_ePlugInCategory_Dynamics`

### Parameter Pages

AAX organizes parameters into pages:
- Page 1: Global parameters
- Page 2+: Effect-specific parameters

### State Management

AAX uses chunk-based state:
1. Plugin settings chunk
2. Preset data chunk
3. Custom data chunks

### Licensing

Release builds require:
- Avid PACE iLok wrapping (optional)
- Code signing certificate
- Avid approval process

## Standalone Application

### Overview

Standalone mode allows running without a DAW for:
- Quick testing
- Live performance
- Simplified usage

### Audio I/O

Standalone handles:
- Audio device enumeration
- Input/output selection
- Buffer size configuration
- Sample rate selection

**Supported APIs:**
- Windows: WASAPI, ASIO
- macOS: CoreAudio

### MIDI Support

Optional MIDI for:
- Program change (preset switching)
- CC mapping (parameter control)
- Future: MIDI learn

### Window Management

- Resizable window
- Fullscreen support
- Multiple monitor awareness
- System tray option

## Format-Specific Considerations

### Parameter Automation

| Format | Automation Support |
|--------|-------------------|
| VST3 | Full, per-parameter |
| AU | Full, with flags |
| AAX | Full, with pages |
| Standalone | N/A |

### Preset Management

| Format | Preset Handling |
|--------|-----------------|
| VST3 | Factory presets + host |
| AU | Factory + user presets |
| AAX | Factory + session presets |
| Standalone | File-based |

### Latency Reporting

All formats report processing latency:
- VST3: `getLatencySamples()`
- AU: `kAudioUnitProperty_Latency`
- AAX: Delay compensation API

### Tail Time

For reverb/delay effects:
- VST3: `getTailSamples()`
- AU: `kAudioUnitProperty_TailTime`
- AAX: Effect tail handling

## Build Configuration

### SDK Requirements

| Format | SDK | Version |
|--------|-----|---------|
| VST3 | VST3 SDK | 3.7.0+ |
| AU | macOS SDK | 10.13+ |
| AAX | AAX SDK | 2.4.0+ |

### Environment Variables

```
VST3_SDK_ROOT=/path/to/vst3sdk
AAX_SDK_ROOT=/path/to/aax-sdk
```

### Build Targets

```
NAMGuitarFX_VST3    # VST3 plugin bundle
NAMGuitarFX_AU      # Audio Unit bundle (macOS)
NAMGuitarFX_AAX     # AAX plugin (requires SDK)
NAMGuitarFX_App     # Standalone application
```

## Distribution

### Plugin Installation

| Format | Windows Path | macOS Path |
|--------|--------------|------------|
| VST3 | `C:\Program Files\Common Files\VST3\` | `~/Library/Audio/Plug-Ins/VST3/` |
| AU | N/A | `~/Library/Audio/Plug-Ins/Components/` |
| AAX | `C:\Program Files\Common Files\Avid\Audio\Plug-Ins\` | `/Library/Application Support/Avid/Audio/Plug-Ins/` |

### Code Signing

| Platform | Requirement |
|----------|-------------|
| Windows | Authenticode (recommended) |
| macOS | Developer ID + Notarization |
| AAX | PACE signing (required) |

### Installers

- Windows: MSI or NSIS installer
- macOS: PKG installer or DMG
- Include: Plugin files, presets, documentation

## Related Documents

- [Architecture Overview](./overview.md)
- [Audio Engine Specification](./audio-engine.md)
- [Build System Documentation](./build-system.md)
