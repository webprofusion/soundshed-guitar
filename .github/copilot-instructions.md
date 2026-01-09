# GuitarFX - AI Coding Agent Instructions

## Project Overview

GuitarFX is a cross-platform audio plugin (VST3/AU/AAX/Standalone) built on **iPlug2** with **Neural Amp Modeler (NAM)** DSP. The architecture consists of:

- **C++ core** (`src/src/`) - Plugin logic, DSP processing, preset management
- **TypeScript UI** (`src/resources/ui/ts/`) - WebView-based interface
- **CMake build system** - FetchContent auto-downloads dependencies

## Architecture Patterns

### Signal Graph DSP
The DSP engine uses a **graph-based signal chain** (`SignalGraphExecutor`). Effects are nodes connected by edges:
- Node types: `amp_nam`, `ir_cab`, `eq_parametric`, `delay`, `reverb`, `noise_gate`, etc.
- Reference [src/src/dsp/effects/](src/src/dsp/effects/) for effect implementations
- New effects: Implement `EffectProcessor` interface, register via `REGISTER_EFFECT` macro

### WebView↔Plugin Communication
JSON messages flow between UI and plugin via `OnMessageFromWebView` / `SendMessageToUI`:
- Message types: `state`, `presetLoaded`, `loadPreset`, `setParameter`, `browseModel`
- UI handler: [src/resources/ui/ts/messages.ts](src/resources/ui/ts/messages.ts)
- Plugin handler: `HandleUIMessage()` in [src/src/GuitarFXPlugin.cpp](src/src/GuitarFXPlugin.cpp)

### Resource References
Resources (NAM models, IRs) use `ResourceRef` with three modes:
1. **Library ref**: `resourceType` + `resourceId` (e.g., `"nam"`, `"plexi-bright"`)
2. **File path**: Direct `filePath` for custom files
3. **Embedded**: `embeddedId` for portable preset sharing

## Build Commands

```powershell
# Configure (run once from src/)
cmake -G "Visual Studio 18 2026" -A x64 -S . -B build

# Build targets (use VS Code tasks or):
cmake --build build --config Debug --target GuitarFX_App     # Standalone app
cmake --build build --config Debug --target GuitarFX_VST3    # VST3 plugin
cmake --build build --config Release --target GuitarFX_App   # Release build

# Build UI (TypeScript → JavaScript)
cd src/resources/ui && npm run build
```

## Testing

Tests are in `src/tests/` and run via CTest (Debug builds only):
```powershell
cd src/build && ctest --build-config Debug --output-on-failure
```

Key test files:
- `PresetDSPLoadingTests.cpp` - Verifies model/IR loading
- `PresetDSPProcessingTests.cpp` - Audio processing validation
- `IRConvolutionTests.cpp` - Convolution algorithm correctness

## Key Files

| Purpose | Location |
|---------|----------|
| Plugin entry point | [src/src/GuitarFXPlugin.cpp](src/src/GuitarFXPlugin.cpp) |
| Effect base class | [src/src/dsp/EffectProcessor.h](src/src/dsp/EffectProcessor.h) |
| Effect registry | [src/src/dsp/EffectRegistry.h](src/src/dsp/EffectRegistry.h) |
| Preset types | [src/src/presets/PresetTypes.h](src/src/presets/PresetTypes.h) |
| Graph executor | [src/src/dsp/SignalGraphExecutor.h](src/src/dsp/SignalGraphExecutor.h) |
| Config/branding | [src/config/GuitarFXConfig.h](src/config/GuitarFXConfig.h) |
| UI entry point | [src/resources/ui/ts/main.ts](src/resources/ui/ts/main.ts) |

## Conventions

- **Namespace**: All C++ code in `guitarfx::`
- **C++ standard**: C++20 required
- **Parameters**: Defined as `ParameterId` enum in `GuitarFXPlugin.h`
- **UI state**: Centralized in `uiState` object ([src/resources/ui/ts/state.ts](src/resources/ui/ts/state.ts))
- **JSON library**: `nlohmann::json` for serialization
- **Design docs**: See [src/docs/](src/docs/) for preset model and customization plans

Before starting a new task in the above plan, update progress in the plan.
-->
- Work through each checklist item systematically.
- Keep communication concise and focused.
- Follow development best practices.
