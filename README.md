# Sepia

Sepia is a free, open-source guitar amp simulator: plug a guitar into an audio interface, pick a
preset, play. It runs as a standalone app and as a VST3 plugin on Windows and Linux.

It is a fork of [Soundshed Guitar](https://github.com/webprofusion/soundshed-guitar) by
Webprofusion Pty Ltd, which does the heavy lifting: the audio engine, the Neural Amp Modeler
integration, the built-in amp and cabinet models and over fifty effects. Sepia is not affiliated
with or endorsed by that project. See [Copyright](#copyright).

## What Sepia adds

- **Lo-fi cabinets.** Five new Simple Cabinet voicings: AM Radio, Telephone, Boombox, Practice
  Amp and VHS.
- **Tape.** Cassette and video tape character without an echo: saturation, wow and flutter,
  hiss, dropouts and a worn top end.
- **Bitcrusher.** Fewer bits and a lower sample rate, aliasing left in.
- **Sustainer.** A held note blooms into feedback and carries on after the string fades, as
  it does in front of a loud amp.
- **Vinyl.** A worn record's crackle, dust and duller top end.
- **Acoustic.** Voices an electric toward a steel-string acoustic.
- **Stutter.** Catches a slice of what was just played and repeats it in time.
- **Vowel Filter.** A filter shaped like a mouth: by hand, by LFO, or by how hard you pick.
- **Eight more amp voicings** for the built-in amp, from Glass Clean and Warm Jazz to Doom Wall
  and Liquid Lead: sixteen in all, in the amp's Presets menu.
- **A hundred and thirty-six factory presets** from indie cleans and lo-fi to grunge and metal,
  level-matched, built only from the built-in amp, cabinets and effects. Nothing to download.
- **Its own look.** A Sepia theme (the default) alongside the Light, Dark and Vintage ones.

## What Sepia leaves out

- **It is offline.** The upstream app's preset sharing, update check, tone search relay and
  YouTube embed belong to the upstream project's own servers, which forks may not use. They
  are switched off. Tone3000 browsing still works with your own Tone3000 key.
- **The upstream factory pack** is not bundled: it is made of third-party amp captures.
  You can still load your own NAM models and impulse responses.

How the factory pack is built and measured: [tools/sepia-presets](tools/sepia-presets/README.md).
What still needs a guitar and ears: [docs/sepia-testing.md](docs/sepia-testing.md).
How to use it, in Turkish: [docs/sepia-kullanim.md](docs/sepia-kullanim.md).
What Sepia adds, in Turkish: [docs/sepia-neler-var.md](docs/sepia-neler-var.md).

## Features

- Neural Amp Modeler DSP for amp/pedal captures, with optional global 2x–32x time-scaled oversampling
- Flexible signal graph with arbitrary effect ordering and parallel paths
- Preset management with category support and content-addressed resource deduplication
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
- **Android**: see [docs/android-build.md](docs/android-build.md)
- **iOS and iPadOS**: see [docs/ios-build.md](docs/ios-build.md)
- **Core tests**: Enable with `GUITARFX_CORE_BUILD_TESTS=ON`, run via CTest

## Prerequisites
`git submodule update --init --recursive `
- node/npm, ts (`npm install -f typescript`)
```
cd /core/ui/ 
npm install
```
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

## Third-party Libraries and Components

This repository depends on or vendors the following third-party libraries and SDK components:

- JUCE and the CLAP JUCE extensions in `juce/`
- Microsoft WebView2 for the Windows WebView host path
- NeuralAmpModelerCore, NAM-Oversampler AudioDSPTools, and Eigen used by the NAM DSP stack in `core/`
- nlohmann/json, Signalsmith Audio linear/stretch, stftPitchShift, miniz, and the Wasmtime C API in the core CMake dependency setup
- jszip, TypeScript, and Vitest in `core/ui/`

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), the upstream LICENSE files,
and package manifests for the exact license terms of each dependency.

## License

AGPL-3.0. Third-party components retain their upstream licenses.

## Copyright

The upstream project's notice, which Sepia follows:

- Soundshed and Soundshed Guitar are copyright Webprofusion Pty Ltd via soundshed.com and permission to re-use the Soundshed/Soundshed Guitar brand is not granted.
- All services/APIs used by the app are excusively for use by our own software unless permission to use has been granted in writing to your project.

Accordingly Sepia ships under its own name and ids, and calls none of those services. The
source is AGPL-3.0, as upstream's is; Sepia's changes are released under the same licence.
- All content resources (NAM, IR etc) are copyright their respective creators. 
- For anything about Sepia, open an issue on this repository. Do not contact the upstream project about this fork.
