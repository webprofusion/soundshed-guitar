# Product Requirements Document: NeuronGuitar

## Document Information

| Field | Value |
|-------|-------|
| Product Name | NeuronGuitar |
| Version | 1.0 |
| Status | Draft |
| Last Updated | 2026-01-09 |

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Product Vision](#2-product-vision)
3. [User Personas](#3-user-personas)
4. [Functional Requirements](#4-functional-requirements)
5. [Non-Functional Requirements](#5-non-functional-requirements)
6. [Architecture Overview](#6-architecture-overview)
7. [System Components](#7-system-components)
8. [Data Models](#8-data-models)
9. [User Interface](#9-user-interface)
10. [Integration Requirements](#10-integration-requirements)
11. [Security & Privacy](#11-security--privacy)
12. [Success Metrics](#12-success-metrics)
13. [Appendices](#13-appendices)

---

## 1. Executive Summary

NeuronGuitar is a cross-platform audio plugin that combines Neural Amp Modeling (NAM) technology with a modern, extensible effects processing chain. The product enables guitarists and audio engineers to achieve studio-quality amp tones and effects through machine learning-based amp captures, impulse response (IR) cabinet simulation, and traditional DSP effects—all within a unified, web-based user interface.

**Key Differentiators:**
- Neural network-based amp modeling for authentic tone reproduction
- Flexible signal graph architecture supporting any effect in any order
- Community-driven preset sharing ecosystem
- Modern web-based UI with real-time parameter synchronization
- Cross-format plugin support (VST3, AU, AAX)

---

## 2. Product Vision

### 2.1 Vision Statement

To democratize professional guitar tone by providing accessible, high-quality neural amp modeling within a flexible, user-friendly plugin that grows with its community.

### 2.2 Goals

| Goal | Description | Priority |
|------|-------------|----------|
| G1 | Deliver professional-quality neural amp modeling | P0 |
| G2 | Enable flexible signal chain configuration | P0 |
| G3 | Support community preset sharing | P1 |
| G4 | Provide intuitive, responsive UI | P1 |
| G5 | Ensure cross-platform/format compatibility | P0 |

### 2.3 Non-Goals

- Real-time model training within the plugin
- Hardware integration (MIDI foot controllers - future phase)
- Mobile platform support (iOS/Android)
- Standalone hardware product

---

## 3. User Personas

### 3.1 Primary Personas

| Persona | Description | Key Needs |
|---------|-------------|-----------|
| **Home Recordist** | Amateur/semi-pro recording at home | Easy preset browsing, quality tones without complexity |
| **Gigging Musician** | Live performer needing reliable tones | Low latency, preset recall, stability |
| **Audio Engineer** | Professional mixing/mastering engineer | Flexibility, automation, format compatibility |
| **Tone Enthusiast** | Hobbyist exploring amp models | Model browsing, community presets, customization |

### 3.2 User Stories

See: [User Stories Document](./user-stories.md)

---

## 4. Functional Requirements

### 4.1 Audio Processing

| ID | Requirement | Priority | Details |
|----|-------------|----------|---------|
| FR-AP-001 | Neural Amp Model Loading | P0 | Load and process NAM model files |
| FR-AP-002 | Impulse Response Loading | P0 | Load and convolve IR files for cabinet simulation |
| FR-AP-003 | Noise Gate | P0 | Adjustable threshold and release |
| FR-AP-004 | Drive/Saturation | P1 | Soft-clipping saturation stage |
| FR-AP-005 | Parametric EQ | P1 | Multi-band parametric equalization |
| FR-AP-006 | Delay Effects | P2 | Digital, tape, and analog delay types |
| FR-AP-007 | Reverb Effects | P2 | Room, hall, plate, spring, shimmer types |
| FR-AP-008 | Modulation Effects | P2 | Chorus, flanger, phaser, tremolo |
| FR-AP-009 | Signal Graph Routing | P1 | Flexible effect ordering with parallel paths |

See: [Audio Engine Specification](./architecture/audio-engine.md)

### 4.2 Preset Management

| ID | Requirement | Priority | Details |
|----|-------------|----------|---------|
| FR-PM-001 | Local Preset Storage | P0 | Save/load presets to local filesystem |
| FR-PM-002 | Preset Categories | P0 | Organize presets by category |
| FR-PM-003 | Preset Search | P1 | Search local presets by name/tags |
| FR-PM-004 | Remote Preset Search | P1 | Search community preset repository |
| FR-PM-005 | Preset Download | P1 | Download presets from remote server |
| FR-PM-006 | Preset Export | P2 | Export presets with embedded resources |
| FR-PM-007 | Asset Hashing | P1 | Hash-based caching for NAM/IR files |
| FR-PM-008 | Version Migration | P2 | Migrate presets between schema versions |

See: [Preset System Specification](./architecture/preset-system.md)

### 4.3 User Interface

| ID | Requirement | Priority | Details |
|----|-------------|----------|---------|
| FR-UI-001 | Parameter Controls | P0 | Real-time parameter adjustment |
| FR-UI-002 | Preset Browser | P0 | Browse and select presets |
| FR-UI-003 | Remote Search UI | P1 | Search interface for community presets |
| FR-UI-004 | Signal Chain Visualizer | P2 | Visual representation of effect chain |
| FR-UI-005 | Level Metering | P2 | Input/output level visualization |
| FR-UI-006 | Model/IR Browser | P1 | Browse available models and IRs |
| FR-UI-007 | State Synchronization | P0 | Real-time sync between UI and engine |

See: [User Interface Specification](./architecture/user-interface.md)

### 4.4 Plugin Formats

| ID | Requirement | Priority | Details |
|----|-------------|----------|---------|
| FR-PF-001 | VST3 Support | P0 | Steinberg VST3 format |
| FR-PF-002 | AU Support | P1 | Apple Audio Unit format (macOS) |
| FR-PF-003 | AAX Support | P2 | Avid AAX format (Pro Tools) |
| FR-PF-004 | Standalone App | P1 | Standalone application mode |
| FR-PF-005 | DAW Automation | P0 | Parameter automation support |

See: [Plugin Format Specification](./architecture/plugin-formats.md)

---

## 5. Non-Functional Requirements

### 5.1 Performance

| ID | Requirement | Target | Measurement |
|----|-------------|--------|-------------|
| NFR-P-001 | Audio Latency | < 10ms @ 44.1kHz | End-to-end processing time |
| NFR-P-002 | CPU Usage | < 30% single core | At nominal load |
| NFR-P-003 | Memory Usage | < 500MB | Typical preset loaded |
| NFR-P-004 | Model Load Time | < 2s | Cold load from disk |
| NFR-P-005 | Preset Switch | < 500ms | Including model/IR changes |
| NFR-P-006 | UI Responsiveness | < 16ms | Frame render time |

### 5.2 Reliability

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-R-001 | Crash-free sessions | 99.9% |
| NFR-R-002 | Audio dropout prevention | No dropouts under normal load |
| NFR-R-003 | Graceful degradation | Continue operation if remote service unavailable |
| NFR-R-004 | Data integrity | No preset corruption on crash |

### 5.3 Scalability

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-S-001 | Concurrent effects | Up to 20 nodes in signal graph |
| NFR-S-002 | Local preset library | Up to 10,000 presets |
| NFR-S-003 | Remote search results | Handle 1,000+ results efficiently |

### 5.4 Compatibility

| ID | Requirement | Details |
|----|-------------|---------|
| NFR-C-001 | Windows | Windows 10/11 (x64) |
| NFR-C-002 | macOS | macOS 11+ (x64, ARM64) |
| NFR-C-003 | Sample Rates | 44.1kHz, 48kHz, 88.2kHz, 96kHz |
| NFR-C-004 | Buffer Sizes | 32-2048 samples |

See: [Performance Requirements](./architecture/performance.md)

---

## 6. Architecture Overview

NeuronGuitar follows a layered architecture with clear separation between audio processing, state management, and user interface layers.

```
┌─────────────────────────────────────────────────────────────┐
│                    User Interface Layer                      │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              WebView UI (HTML/CSS/JS)                │    │
│  └─────────────────────────────────────────────────────┘    │
│                            ▲                                 │
│                            │ JSON Messages                   │
│                            ▼                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                  WebUI Bridge                        │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                            ▲
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   Application Layer                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │    Plugin    │  │    Preset    │  │     Network      │   │
│  │   Controller │  │   Manager    │  │     Client       │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            ▲
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    Audio Engine Layer                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  Signal Graph │  │   NAM DSP    │  │    IR Manager    │   │
│  │   Executor   │  │   Manager    │  │   (Convolution)  │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              Effect Processors Registry               │   │
│  │   (EQ, Dynamics, Modulation, Delay, Reverb, etc.)    │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            ▲
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    Platform Layer                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │     VST3     │  │      AU      │  │       AAX        │   │
│  │    Wrapper   │  │    Wrapper   │  │     Wrapper      │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

See: [Architecture Overview](./architecture/overview.md)

---

## 7. System Components

### 7.1 Component Index

| Component | Document | Description |
|-----------|----------|-------------|
| Audio Engine | [audio-engine.md](./architecture/audio-engine.md) | Core DSP processing and signal routing |
| NAM DSP Manager | [nam-dsp.md](./architecture/nam-dsp.md) | Neural amp model loading and inference |
| IR Manager | [ir-manager.md](./architecture/ir-manager.md) | Impulse response loading and convolution |
| Effect Registry | [effect-registry.md](./architecture/effect-registry.md) | Dynamic effect type registration |
| Signal Graph Executor | [signal-graph.md](./architecture/signal-graph.md) | Graph-based signal routing |
| Preset Manager | [preset-system.md](./architecture/preset-system.md) | Preset persistence and management |
| Network Client | [network-client.md](./architecture/network-client.md) | Remote API communication |
| WebUI Bridge | [webui-bridge.md](./architecture/webui-bridge.md) | UI-Engine messaging layer |
| Plugin Formats | [plugin-formats.md](./architecture/plugin-formats.md) | Format-specific wrappers |

### 7.2 Component Dependencies

```
┌─────────────────┐      ┌─────────────────┐
│  Preset Manager │─────▶│ Network Client  │
└────────┬────────┘      └─────────────────┘
         │
         ▼
┌─────────────────┐      ┌─────────────────┐
│ Signal Graph    │─────▶│ Effect Registry │
│   Executor      │      └────────┬────────┘
└────────┬────────┘               │
         │                        ▼
         │               ┌─────────────────┐
         │               │  NAM DSP Mgr    │
         │               └─────────────────┘
         │               ┌─────────────────┐
         └──────────────▶│   IR Manager    │
                         └─────────────────┘
```

---

## 8. Data Models

### 8.1 Core Data Structures

| Model | Document | Description |
|-------|----------|-------------|
| Preset v2 | [preset-model-v2-design.md](./preset-model-v2-design.md) | Complete preset data structure |
| Signal Graph | [signal-graph-model.md](./architecture/signal-graph-model.md) | Node/edge graph representation |
| Resource Reference | [resource-model.md](./architecture/resource-model.md) | Library and embedded resource handling |
| Effect Parameters | [parameter-model.md](./architecture/parameter-model.md) | Parameter definitions and validation |

### 8.2 Storage Formats

| Format | Use Case | Specification |
|--------|----------|---------------|
| JSON | Preset files, API responses | UTF-8 encoded |
| Binary | Embedded resources | Platform-native |
| FNV-1a Hash | Asset caching | 64-bit |
| SHA-256 | Resource verification | 256-bit |

See: [Data Model Specification](./architecture/data-models.md)

---

## 9. User Interface

### 9.1 UI Architecture

The user interface is implemented as a web-based single-page application (SPA) hosted within the plugin's WebView control. Communication between the UI and the audio engine occurs through a bidirectional JSON message bridge.

### 9.2 Key Views

| View | Description | Priority |
|------|-------------|----------|
| Main View | Primary controls and preset info | P0 |
| Preset Browser | Local preset list with filtering | P0 |
| Community Browser | Remote preset search | P1 |
| Signal Chain Editor | Visual effect chain configuration | P2 |
| Settings | Application preferences | P1 |

### 9.3 Message Protocol

| Message Type | Direction | Purpose |
|--------------|-----------|---------|
| `state` | Engine → UI | Full parameter and preset state |
| `presetSearchResults` | Engine → UI | Remote search results |
| `presetLoaded` | Engine → UI | Notification of preset load |
| `parameterChanged` | Engine → UI | Single parameter update |
| `search` | UI → Engine | Initiate remote search |
| `downloadPreset` | UI → Engine | Request preset download |
| `loadPreset` | UI → Engine | Load local preset |
| `setParameter` | UI → Engine | Update parameter value |

See: [User Interface Specification](./architecture/user-interface.md)

---

## 10. Integration Requirements

### 10.1 External Services

| Service | Purpose | Protocol |
|---------|---------|----------|
| Preset Repository | Community preset sharing | HTTPS REST API |
| Model Repository | NAM model distribution | HTTPS |
| Analytics (Optional) | Usage telemetry | HTTPS |

### 10.2 API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/presets/search` | GET | Search presets by query/tags |
| `/presets/{id}` | GET | Retrieve preset details |
| `/presets/{id}/download` | GET | Download preset package |
| `/models/{id}` | GET | Download NAM model file |

See: [API Specification](./architecture/api-spec.md)

### 10.3 File Format Support

| Type | Extensions | Description |
|------|------------|-------------|
| NAM Models | `.nam` | Neural amp model captures |
| Impulse Responses | `.wav`, `.aiff` | Cabinet/room IRs |
| Presets | `.neuronpreset` | Portable preset format |

---

## 11. Security & Privacy

### 11.1 Security Requirements

| ID | Requirement | Details |
|----|-------------|---------|
| SEC-001 | Secure Transport | All remote communication over HTTPS |
| SEC-002 | Input Validation | Validate all user inputs and file formats |
| SEC-003 | Resource Verification | Hash verification for downloaded resources |
| SEC-004 | Sandboxed UI | WebView runs in sandboxed context |

### 11.2 Privacy Requirements

| ID | Requirement | Details |
|----|-------------|---------|
| PRI-001 | No Required Account | Core functionality without account creation |
| PRI-002 | Local-First | All processing happens locally |
| PRI-003 | Opt-In Telemetry | Analytics only with explicit consent |
| PRI-004 | Data Minimization | Collect only essential data |

See: [Security Specification](./architecture/security.md)

---

## 12. Success Metrics

### 12.1 Key Performance Indicators

| Metric | Target | Measurement |
|--------|--------|-------------|
| Daily Active Users | 10,000+ | Opt-in telemetry |
| Preset Downloads | 50,000+/month | Server analytics |
| Community Presets | 5,000+ | Repository count |
| Average Session Length | 30+ minutes | Opt-in telemetry |
| Crash Rate | < 0.1% | Crash reporting |

### 12.2 Quality Metrics

| Metric | Target |
|--------|--------|
| User Satisfaction (NPS) | 50+ |
| Bug Reports Resolved | < 7 days average |
| Feature Request Response | < 30 days |

---

## 13. Appendices

### Appendix A: Glossary

| Term | Definition |
|------|------------|
| NAM | Neural Amp Modeler - ML-based amp capture technology |
| IR | Impulse Response - Audio sample capturing acoustic characteristics |
| DSP | Digital Signal Processing |
| DAW | Digital Audio Workstation |
| VST3 | Virtual Studio Technology 3 (Steinberg plugin format) |
| AU | Audio Unit (Apple plugin format) |
| AAX | Avid Audio eXtension (Pro Tools plugin format) |

### Appendix B: Related Documents

| Document | Path | Description |
|----------|------|-------------|
| Architecture Overview | [overview.md](./architecture/overview.md) | System architecture |
| Audio Engine Spec | [audio-engine.md](./architecture/audio-engine.md) | DSP subsystem |
| Preset System Spec | [preset-system.md](./architecture/preset-system.md) | Preset management |
| User Interface Spec | [user-interface.md](./architecture/user-interface.md) | UI architecture |
| API Specification | [api-spec.md](./architecture/api-spec.md) | Remote API |
| Preset Data Model v2 | [preset-model-v2-design.md](./preset-model-v2-design.md) | Data model design |
| User Stories | [user-stories.md](./user-stories.md) | User requirements |

### Appendix C: Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-01-09 | - | Initial draft |
