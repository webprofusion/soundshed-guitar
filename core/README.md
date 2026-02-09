# soundshed-guitar-core

Framework-agnostic shared library for the Soundshed Guitar audio plugin, consumed by both the **iPlug2** and **JUCE** plugin projects.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                   soundshed-guitar-core                         │
│                                                                 │
│  IPluginHost ◄── abstract interface (file dialogs, WebView,    │
│  │                threading, paths, DAW integration)            │
│  │                                                              │
│  PluginController  ── owns all business logic                   │
│  │  ├── MultiPresetMixer (DSP engine)                          │
│  │  ├── ResourceLibrary (NAM/IR assets)                        │
│  │  ├── CompositeEffectLibrary                                 │
│  │  ├── PresetStorage                                          │
│  │  └── ... (tuner, metronome, signal test, NAM calibration)   │
│  │                                                              │
│  MessageDispatcher ── routes ~50 JSON message types             │
│  │                                                              │
│  DSP (31 effects, signal graph, SIMD, NAM inference)           │
│  Presets / Resources / Models / Network / Utils                │
│  Web UI (TypeScript SPA, shared between both frameworks)       │
└─────────────────────────────────────────────────────────────────┘
          ▲                                    ▲
          │                                    │
  ┌───────┴────────┐                 ┌─────────┴──────────┐
  │ iPlug2 Adapter │                 │   JUCE Adapter     │
  │ (~300 lines)   │                 │   (~400 lines)     │
  │                │                 │                    │
  │ GuitarFXPlugin │                 │ PluginProcessor    │
  │ Adapter.h/.cpp │                 │ Adapter.h/.cpp     │
  │                │                 │                    │
  │ Implements:    │                 │ Implements:        │
  │ IPluginHost    │                 │ IPluginHost        │
  │ iplug::Plugin  │                 │ AudioProcessor     │
  └────────────────┘                 └────────────────────┘
```

## Key Components

| Component | File(s) | Purpose |
|---|---|---|
| **IPluginHost** | `src/IPluginHost.h` | Abstract interface for framework-specific services |
| **PluginController** | `src/PluginController.h/.cpp` | Central orchestration — all business logic |
| **MessageDispatcher** | `src/MessageDispatcher.h/.cpp` | Unified JSON message routing (~50 types) |
| **MultiPresetMixer** | `src/dsp/MultiPresetMixer.h/.cpp` | DAG-based DSP engine with parallel preset mixing |
| **31 Effects** | `src/dsp/effects/` | Self-registering via `EffectRegistry` |
| **PresetStorage** | `src/presets/PresetStorage.h/.cpp` | JSON preset serialization |
| **ResourceLibrary** | `src/resources/ResourceLibrary.h/.cpp` | NAM model and IR catalog |
| **Web UI** | `ui/` | TypeScript SPA (shared WebView UI) |
| **Tests** | `tests/` | 17 Catch2 test files |

## Integration

Both consumer projects include this library via CMake `add_subdirectory()`:

```cmake
add_subdirectory(path/to/soundshed-guitar-core ${CMAKE_BINARY_DIR}/soundshed-guitar-core)
target_link_libraries(MyTarget PRIVATE SoundshedGuitarCore)
```

Dependencies (nlohmann/json, NeuralAmpModelerCore, Signalsmith headers) are provided by the consumer project. Set `GUITARFX_CORE_FETCH_DEPS=ON` for standalone builds.

See [INTEGRATION.md](INTEGRATION.md) for detailed per-project instructions.

## Building Tests

```bash
cmake -B build -DGUITARFX_CORE_FETCH_DEPS=ON -DGUITARFX_CORE_BUILD_TESTS=ON
cmake --build build
or
cmake --build build --parallel

ctest --test-dir build
```
