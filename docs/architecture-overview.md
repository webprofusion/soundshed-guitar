# Architecture Overview

## Key Files
- `core/src/PluginController.cpp` — Main application controller, state management, UI messaging
- `core/src/MessageDispatcher.cpp` — Message routing across UI, mixer, preset, and settings domains
- `core/src/UiBridge.cpp` — Native UI bridge plumbing
- `core/src/dsp/MultiPresetMixer.cpp` — Shared mixer and global chain orchestration
- `juce/` — Active front-end host that integrates the shared core engine

## Overview

Soundshed Guitar is a modular audio application built around a shared core engine. The system separates concerns across four layers: front-end hosts, audio engine, application controller, and UI.

## System Layers

```
┌─────────────────────────────────────────┐
│           User Interface Layer          │  WebView SPA (HTML/CSS/TypeScript)
├─────────────────────────────────────────┤
│           Application Layer             │  PluginController, MessageDispatcher, resource and preset state
├─────────────────────────────────────────┤
│           Audio Engine Layer            │  SignalGraphExecutor, Effect Processors, NAM/IR
├─────────────────────────────────────────┤
│           Platform Layer                │  JUCE front-end and host integration
└─────────────────────────────────────────┘
```

### Platform Layer
Front-end projects host the shared core engine and UI bridge.
- Host lifecycle and audio callback integration
- Parameter exposure and automation
- Standalone application packaging and plugin targets
- **Current repo structure**: shared engine in `core/`, active front end in `juce/`

### Audio Engine Layer
Real-time DSP processing with zero allocations in the audio callback.
- **Signal Graph Executor**: Routes audio through effect nodes in topological order
- **NAM DSP Manager**: Loads/runs neural amp models with pre-warming
- **IR Manager**: Partitioned FFT convolution for cabinet simulation
- **Effect Processors**: Registered effect types (gate, EQ, delay, reverb, etc.)

### Application Layer
Coordinates state and business logic.
- **PluginController**: Main orchestration point for presets, settings, messaging, and host state
- **MessageDispatcher**: Routes JSON UI messages by domain
- **Resource Library**: NAM/IR catalog with content-addressed deduplication
- **API integration**: Remote discovery and sharing are implemented in the UI client modules `core/ui/ts/tone3000Api.ts`, `core/ui/ts/tone3000Browser.ts`, and `core/ui/ts/toneSharingPanel.ts`, which talk to the Tone3000 and Soundshed endpoints.

### User Interface Layer
Web-based SPA in a native WebView container.
- Bidirectional JSON message protocol with the plugin
- Event-based state synchronization
- Platform-specific WebView: WebView2 (Windows), WKWebView (macOS)

## Design Principles

1. **Separation of Concerns** — Each layer has distinct responsibilities
2. **Technology Agnostic** — Core algorithms free of framework dependencies
3. **Performance First** — No allocations in audio thread, lock-free communication
4. **Extensibility** — Plugin architecture for effects, schema-versioned presets

## Threading Model

| Thread | Priority | Purpose |
|--------|----------|---------|
| Audio | Realtime | Audio processing (no blocking) |
| UI | Normal | User interaction, WebView rendering |
| Background | Below Normal | Model loading, network, file I/O |

**Communication:**
- Audio ↔ UI: Lock-free queues, atomic parameter updates
- UI → Background: Task queue
- Background → UI: Completion callbacks

## Performance Targets

| Metric | Target |
|--------|--------|
| Processing Latency | < 10ms @ 44.1kHz, 256 samples |
| CPU Usage (typical) | < 30% single core |
| NAM Model Load | < 2s cold, < 500ms hot swap |
| IR Load | < 200ms |
| Initial Startup | < 3s until UI responsive |
| Memory (typical) | < 300MB |

### Buffer & Sample Rate Support
- **Buffer sizes**: 32–2048 samples (256 recommended)
- **Sample rates**: 44.1, 48, 88.2, 96 kHz (full support); 176.4, 192 kHz (best effort)

## Security Model

### Transport
- HTTPS required for all network communication
- TLS 1.2+ with certificate validation
- Optional certificate pinning for enhanced MITM protection

### Input Validation
- File size and format validation for NAM/IR files
- JSON schema validation for presets with depth limits
- Sanitized search queries and API inputs

### WebView Sandboxing
- No file system access from web context
- Communication only through message bridge
- Content Security Policy restricts script/resource origins

## Error Handling

| Context | Strategy |
|---------|----------|
| Audio path | Graceful bypass, error flag to UI, no interruption |
| Resource loading | Fallback to default/bypass, user notification |
| Network | Cached responses, timeout handling, offline mode |

## See Also
- [Signal Chain](signal-chain.md) — Graph model and execution
- [FX Library](fx-library.md) — Effect definitions and resources
- [User Interface](user-interface.md) — WebView architecture and messaging
- [Data Models](data-models.md) — Preset schema and storage
