# Soundshed Guitar (GuitarFX)

GuitarFX is a cross-format audio plugin (VST3/AU/AAX) powered by iPlug2 and the Neural Amp Modeler (NAM) DSP core. It delivers a flexible guitar processing chain that combines Neural Amp models, cabinet impulse responses, and supporting FX in a web-based user interface.

## Features

- Neural Amp Modeler integration with per-channel processing and automatic prewarm
- Configurable FX chain with noise gate, soft-saturation drive, and spectral tilt tone shaping
- Convolution-based cabinet section powered by user-supplied impulse responses
- Categorised preset system with JSON persistence and content-addressed resource deduplication
- WebView-driven UI (HTML/JS/CSS) with live parameter synchronisation and preset browsing

## Project Layout

```
CMakeLists.txt
cmake/FetchDependencies.cmake   # Dependency bootstrap (iPlug2, NAM Core, nlohmann::json)
config/GuitarFXConfig.h        # Plug-in metadata shared by all formats
resources/ui/                   # WebView UI bundle (HTML/JS/CSS) and TypeScript source
src/                            # Shared plug-in code
  GuitarFXPlugin.*              # Main iPlug2 plug-in implementation
  dsp/                          # Neural Amp + FX processing
  presets/                      # Preset management and storage
  ui/                           # WebView bridge helpers
  util/                         # Filesystem helpers
  platform/                     # Format-specific wrapper targets (VST3/AU/AAX)
```

## Building

> **Prerequisites**
>
> - CMake 3.22+
> - A C++20 toolchain (MSVC 2026, Clang 15+, or GCC 11+)
> - Platform SDKs for the desired plug-in formats (VST3 SDK, macOS Audio Unit tools, Avid AAX SDK)
> - Git (required by `FetchContent` to materialise dependencies)
>
> The Steinberg VST3 SDK must be downloaded separately (see https://www.steinberg.net/vst3sdk) and extracted. Point `VST3_SDK_ROOT` at the extracted folder or drop it into `[_deps/iplug2-src/Dependencies/IPlug/VST3_SDK](./_deps/iplug2-src/Dependencies/IPlug/VST3_SDK)` before configuring.
>
> iPlug2 and NeuralAmpModelerCore are fetched automatically the first time you configure the project. The build will also fetch `nlohmann::json` for preset management.

```powershell
mkdir build
cd build
cmake -G "Visual Studio 18 2026" -A x64 ..
cmake --build . --config Release
```

Format targets are generated beneath `src/platform` and link against the common `GuitarFXCore` static library. Additional SDK configuration (such as environment variables for the AAX SDK) may be required before building those wrappers successfully.

e.g. `cmake --build build --config Release --target SoundshedGuitar_App`
## Preset Workflow

Presets are stored as JSON in the user data folder (`~/.guitarfx/`) and embed:

- Category metadata and descriptive text
- Parameter snapshots keyed by stable IDs (`input_trim`, `drive`, `tone`, etc.)
- Optional attachment metadata for `.nam` models and impulse response files, content-addressed by SHA-256

Preset operations are local-only in the C++ backend.

## Web UI Messaging

The WebView bridge exchanges JSON messages between the HTML UI and the plug-in runtime:

- `state` broadcasts the full parameter snapshot and the local preset catalogue
- `presetSearchResults` delivers remote search responses
- `presetLoaded` notifies the UI when a preset is loaded or downloaded
- UI-originated messages include `search`, `downloadPreset`, and `loadPreset`

The TypeScript client (`resources/ui/ts/messages.ts`) registers `window.IPlugReceiveData` to render incoming updates and uses `window.NAMBridge.postMessage` to push user actions back into the engine.

## Agent Quickstart

For a minimal, high-signal overview, start with docs/agent-quickstart.md.

## Next Steps

- Integrate additional FX modules (delay, modulation, reverberation) and expose them through the preset schema
- Harden the HTTP layer with authentication, retry policies, and HTTPS support (`CPPHTTPLIB_OPENSSL_SUPPORT`)
- Extend the UI with parameter editors, model/IR browsers, and real-time meters
- Flesh out the format-specific targets with proper resource bundling, signing, and installer scripts
