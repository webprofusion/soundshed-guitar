# soundshed-guitar-core — Integration Guide

## Overview

Both **soundshed-guitar** (iPlug2) and **soundshed-guitar-juce** (JUCE) consume
this shared core library via `add_subdirectory()`. The core library provides:

- **PluginController** — all business logic (DSP routing, preset management, UI messaging)
- **MessageDispatcher** — unified WebView message routing (~50 message types)
- **IPluginHost** — abstract interface that framework adapters implement
- **DSP engine** — DAG signal graph, effect registry, NAM inference, convolver, pitch tuner
- **Preset system** — serialization, storage, composite effects
- **Resource library** — NAM models, IR cabs, blend definitions
- **Web UI** — 453 TypeScript/HTML/CSS files for the plugin editor

## Architecture

```
┌──────────────────┐     ┌──────────────────────┐
│  iPlug2 Adapter  │     │    JUCE Adapter       │
│  (GuitarFXPlugin │     │  (PluginProcessor     │
│   Adapter.h/.cpp)│     │   Adapter.h/.cpp)     │
└────────┬─────────┘     └──────────┬────────────┘
         │  implements IPluginHost  │
         └────────────┬─────────────┘
                      │
         ┌────────────▼────────────┐
         │  SoundshedGuitarCore    │
         │  (PluginController,     │
         │   MessageDispatcher,    │
         │   DSP engine, Presets,  │
         │   Resources, Web UI)    │
         └─────────────────────────┘
```

## iPlug2 Integration (soundshed-guitar)

Already integrated. Key files:

- `src/CMakeLists.txt` line 27: `add_subdirectory(../../soundshed-guitar-core)`
- `src/src/CMakeLists.txt`: `GuitarFXCore` links `SoundshedGuitarCore` (transitive)
- `src/src/GuitarFXPluginAdapter.h/.cpp`: Thin iPlug2 adapter
- Platform targets (app, vst3, au, aax) compile `GuitarFXPluginAdapter.cpp`

Build: `cmake --build build --target SoundshedGuitar_App --config Release`

## JUCE Integration (soundshed-guitar-juce)

Already integrated. Key files:

- `CMakeLists.txt`: `add_subdirectory(../soundshed-guitar-core)`
- `CMakeLists.txt`: SharedCode uses explicit 6-file source list
- `source/PluginProcessorAdapter.h/.cpp`: Thin JUCE adapter
- `source/PluginEditor.h/.cpp`: Editor using adapter

## Verified Build Status

- **SoundshedGuitarCore.lib** — compiles cleanly (Release)
- **GuitarFXCore.lib** — compiles cleanly (Release)
- **SoundshedGuitar.exe** (standalone) — compiles and links successfully
