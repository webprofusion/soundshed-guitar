# Architecture Overview

## Introduction

This document provides a high-level overview of the NeuronGuitar system architecture. NeuronGuitar is designed as a modular, layered system that separates concerns between audio processing, state management, user interface, and platform integration.

## Design Principles

### 1. Separation of Concerns
- **Audio Engine**: Handles all real-time DSP processing
- **Application Layer**: Manages state, presets, and business logic
- **UI Layer**: Provides user interaction through WebView
- **Platform Layer**: Abstracts plugin format differences

### 2. Technology Agnostic
- Core algorithms expressed without framework dependencies
- Interfaces define contracts, implementations are swappable
- Data models use standard serialization formats

### 3. Performance First
- Real-time audio path has no allocations
- Pre-warmed models and cached resources
- Lock-free communication where possible

### 4. Extensibility
- Plugin architecture for effects
- Type-registered signal graph nodes
- Schema-versioned presets with migration support

## System Layers

### Layer 1: Platform Layer

The lowest layer abstracts the differences between plugin formats and host DAWs.

**Responsibilities:**
- Plugin lifecycle management (activation, deactivation)
- Audio buffer routing from host
- Parameter exposure and automation
- Preset/state persistence hooks
- UI window management

**Supported Formats:**
- VST3 (Windows, macOS)
- Audio Unit (macOS)
- AAX (Windows, macOS)
- Standalone Application

### Layer 2: Audio Engine Layer

The audio engine processes all audio in real-time with minimal latency.

**Components:**
- **Signal Graph Executor**: Routes audio through configured effect chain
- **NAM DSP Manager**: Loads and runs neural amp models
- **IR Manager**: Handles impulse response convolution
- **Effect Processors**: Individual DSP effect implementations
- **Effect Registry**: Dynamic registration of available effect types

**Design Constraints:**
- No heap allocations in audio callback
- Thread-safe model/IR swapping
- Supports variable buffer sizes
- Handles sample rate changes gracefully

### Layer 3: Application Layer

Manages application state and coordinates between subsystems.

**Components:**
- **Plugin Controller**: Central orchestrator
- **Preset Manager**: Handles preset CRUD operations
- **Network Client**: Communicates with remote services
- **Resource Library**: Manages NAM/IR resource catalog

**Responsibilities:**
- Parameter state management
- Preset loading/saving/switching
- Remote preset search and download
- Resource caching and deduplication

### Layer 4: User Interface Layer

Provides visual interaction through a web-based interface.

**Components:**
- **WebView Host**: Native webview container
- **WebUI Bridge**: JSON message serialization/deserialization
- **Web Application**: HTML/CSS/JavaScript SPA

**Communication:**
- Bidirectional JSON message protocol
- Event-based state synchronization
- Debounced parameter updates

## Data Flow

### Audio Processing Flow

```
Host Audio Buffer
       │
       ▼
┌──────────────────┐
│  Plugin Entry    │
│  (Format Layer)  │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│  Signal Graph    │
│    Executor      │
└────────┬─────────┘
         │
    ┌────┴────┬─────────┬─────────┐
    ▼         ▼         ▼         ▼
┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐
│ Node  │ │ Node  │ │ Node  │ │ Node  │
│  1    │ │  2    │ │  3    │ │  N    │
└───┬───┘ └───┬───┘ └───┬───┘ └───┬───┘
    │         │         │         │
    └─────────┴────┬────┴─────────┘
                   │
                   ▼
           Output Buffer
```

### UI Communication Flow

```
┌─────────────────┐        ┌─────────────────┐
│   Web UI        │◀──────▶│  WebUI Bridge   │
│   (JavaScript)  │  JSON  │                 │
└─────────────────┘        └────────┬────────┘
                                    │
                                    ▼
                           ┌────────────────┐
                           │ Plugin         │
                           │ Controller     │
                           └────────┬───────┘
                                    │
              ┌─────────────────────┼─────────────────────┐
              ▼                     ▼                     ▼
     ┌────────────────┐    ┌────────────────┐    ┌────────────────┐
     │ Signal Graph   │    │ Preset Manager │    │ Network Client │
     └────────────────┘    └────────────────┘    └────────────────┘
```

## Threading Model

### Audio Thread
- Processes audio buffers in real-time
- Must complete within buffer deadline
- No blocking operations allowed
- Lock-free parameter updates

### UI Thread
- Handles user interaction
- Renders WebView content
- Processes UI message queue

### Background Thread(s)
- Model/IR loading
- Network requests
- Preset import/export
- Resource hashing

### Thread Communication
- Audio ↔ UI: Lock-free queues
- UI → Background: Task queue
- Background → UI: Completion callbacks

## Error Handling Strategy

### Audio Path Errors
- Graceful bypass on processing failure
- Error state reported to UI
- No audio interruption

### Resource Loading Errors
- Fallback to default/empty state
- User notification through UI
- Automatic retry for transient failures

### Network Errors
- Cached responses when available
- Timeout handling
- Offline mode support

## Configuration Points

| Configuration | Scope | Storage |
|---------------|-------|---------|
| Plugin Parameters | Per-Instance | Host State |
| User Preferences | Global | Settings File |
| Resource Library | Global | Library Index |
| Presets | User | Preset Files |

## Related Documents

- [Audio Engine Specification](./audio-engine.md)
- [Signal Graph Model](./signal-graph-model.md)
- [Preset System Specification](./preset-system.md)
- [User Interface Specification](./user-interface.md)
- [Plugin Format Specification](./plugin-formats.md)
