# Soundshed Guitar

Soundshed Guitar is a JUCE-based guitar processing plugin combining Neural Amp Modeler (NAM) with a modern web-driven UI. Targets VST3, AU, and AAX formats with an extensible signal graph architecture.

## Features

- Neural Amp Modeler DSP for amp/pedal captures
- Flexible signal graph with arbitrary effect ordering and parallel paths
- Preset management with category support and content-addressed resource deduplication
- Remote preset search/download for community sharing
- WebView UI (HTML/CSS/TypeScript)

## Documentation

See [docs/](docs/) for architecture and reference documentation:

| Document | Description |
|----------|-------------|
| [Agent Quickstart](docs/agent-quickstart.md) | Minimal, high-signal guide for AI agents |
| [Architecture Overview](docs/architecture-overview.md) | System layers, design principles, performance targets |
| [Signal Chain](docs/signal-chain.md) | Graph model, execution, buffer management |
| [FX Library](docs/fx-library.md) | Effect types, registry, resource configuration |
| [Data Models](docs/data-models.md) | Preset schema, storage layout |
| [User Interface](docs/user-interface.md) | WebView architecture, message protocol |
| [Network API](docs/network-api.md) | REST API, client, plugin formats |
| [Theme System](docs/theme-system.md) | CSS theming |
| [PRD](docs/prd/PRD.md) | Product requirements |

## Agent Context Bundle

For minimal context with high coverage, load these files:

- docs/agent-quickstart.md
- docs/architecture-overview.md
- docs/signal-chain.md
- docs/fx-library.md
- docs/data-models.md
- docs/user-interface.md
- .github/copilot-instructions.md

## Quick Start

1. **Clone and configure**
   ```powershell
   git clone <repo-url>
   cd soundshed-guitar
   cmake -S juce -B juce/builds -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```
   CMake fetches core dependencies (for example NeuralAmpModelerCore) via FetchContent.

2. **Build**
   ```powershell
   cmake --build juce/builds --config RelWithDebInfo
   ```

3. **Install**
   - **Windows VST3**: Copy `.vst3` from `build/SoundshedGuitar.vst3` to `C:\Program Files\Common Files\VST3\`
   - **macOS VST3**: Copy to `~/Library/Audio/Plug-Ins/VST3/`

## SDK Requirements

| Format | Requirement |
|--------|-------------|
| VST3 | Set `VST3_SDK_ROOT` environment variable |
| AU | macOS with Xcode command-line tools |
| AAX | Set `AAX_SDK_ROOT` (requires Avid SDK access) |

## Branding

Edit `core/config/GuitarFXConfig.h` to customize plugin name and manufacturer. Rebuild after changes.

## Development

- **UI source**: `core/ui/ts/` (TypeScript)
- **Build UI**: `cd core/ui && npm run build`
- **Core tests**: Enable with `GUITARFX_CORE_BUILD_TESTS=ON`, run via CTest

## JUCE Linux Release Build

The JUCE target can be built and staged on Linux with the root helper script:

```bash
bash ./build_linux.sh --arch x64 --zip
bash ./build_linux.sh --arch arm64 --zip
```

This configures `juce/builds-linux-x64` and `juce/builds-linux-arm64`, builds the JUCE Standalone, VST3, and CLAP targets, stages Linux distribution layouts in `linux-dist-x64/` and `linux-dist-arm64/`, writes the standalone app to `opt/Soundshed/soundshed-guitar/soundshed-guitar`, and creates per-architecture archives for distribution.

To build both Linux targets in one invocation, use:

```bash
bash ./build_linux.sh --arch all --zip
```

When cross-compiling, the helper uses the repo toolchain files under `cmake/toolchains/`. Install the matching cross compiler plus target sysroot, and set `GUITARFX_LINUX_SYSROOT`, `GUITARFX_LINUX_PKG_CONFIG_LIBDIR`, and `GUITARFX_LINUX_PKG_CONFIG_SYSROOT_DIR` if your distro packages live outside the default search paths.

To include the LV2 bundle in the Linux build and staging layout, enable it explicitly:

```bash
bash ./build_linux.sh --arch x64 --lv2 --zip
bash ./build_linux.sh --arch arm64 --lv2 --zip
```

If you need to match CI dependencies on Ubuntu, install the JUCE Linux packages used by the workflow first:

```bash
sudo apt-get update
sudo apt-get install libasound2-dev libx11-dev libxinerama-dev libxext-dev libfreetype6-dev libwebkit2gtk-4.1-dev libglu1-mesa-dev ninja-build
```
## Windows
Full rebuild:
```
Remove-Item -Recurse -Force juce/builds
cmake -G "Visual Studio 18 2026" -A x64 -S juce -B juce/builds 
cmake --build juce/builds --config Release --target ALL_BUILD --parallel
```

### Debug Tools

**Offline Processing Test**
```powershell
cmake --build build --config Debug --target GuitarFX_OfflineProcessingTest
./GuitarFX_OfflineProcessingTest.exe input.wav output.wav [model.nam] [cabinet.wav]
```

**VST3 Debug Host**
```powershell
cmake --build build --config Debug --target VST3DebugHost
./VST3DebugHost.exe --windowed
```
Testing plugin builds with external resources can be done with pluginval against the generated VST3.

## License

MIT. Third-party components retain their upstream licenses.
