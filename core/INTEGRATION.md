# soundshed-guitar-core — Integration Guide

## Overview

The **JUCE** host project consumes this shared core library via
`add_subdirectory()`. The core library provides:

- **PluginController** — all business logic (DSP routing, preset management, UI messaging)
- **MessageDispatcher** — unified WebView message routing (~50 message types)
- **IPluginHost** — abstract interface that framework adapters implement
- **DSP engine** — DAG signal graph, effect registry, NAM inference, convolver, pitch tuner
- **Preset system** — serialization, storage, composite effects
- **Resource library** — NAM models, IR cabs, blend definitions
- **Web UI** — 453 TypeScript/HTML/CSS files for the plugin editor

## Architecture

```
┌──────────────────────┐
│    JUCE Adapter      │
│  (PluginProcessor    │
│   Adapter.h/.cpp)    │
└──────────┬───────────┘
           │  implements IPluginHost
           ▼
┌─────────────────────────────┐
│      SoundshedGuitarCore    │
│  (PluginController,         │
│   MessageDispatcher,        │
│   DSP engine, Presets,      │
│   Resources, Web UI)        │
└─────────────────────────────┘
```

## Legacy Host Integration Notes

Legacy host-framework references may still appear in historical documents,
but the active and supported integration path is JUCE.

## JUCE Integration

Already integrated. Key files:

- `CMakeLists.txt`: `add_subdirectory(../soundshed-guitar-core)`
- `CMakeLists.txt`: SharedCode uses explicit 6-file source list
- `source/PluginProcessorAdapter.h/.cpp`: Thin JUCE adapter
- `source/PluginEditor.h/.cpp`: Editor using adapter

## Verified Build Status

- **SoundshedGuitarCore.lib** — compiles cleanly (Release)
- **GuitarFXCore.lib** — compiles cleanly (Release)
- **SoundshedGuitar.exe** (standalone) — compiles and links successfully
