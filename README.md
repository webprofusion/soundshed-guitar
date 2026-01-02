# NeuronGuitar

NeuronGuitar is an experimental iPlug2-based guitar processing plugin that fuses the Neural Amp Modeler (NAM) engine with a modern web-driven user interface. The project targets VST3, AU, and AAX formats and focuses on an extensible FX chain with preset sharing capabilities.

## Features

- Neural Amp Modeler Core DSP integration for amp and pedal captures
- Flexible FX chain with NAM model slots and impulse response (IR) loading
- Preset management with category support and model hashing to avoid duplication
- Remote preset search/download API hook for community-driven sharing
- WebView-driven UI authored with standard HTML/CSS/JavaScript

## Repository Layout

```
.
├─ src/
│  ├─ CMakeLists.txt            # Root CMake entry point
│  ├─ _deps/                    # Third-party sources populated via CMake FetchContent
│  │  ├─ iplug2-src/            # iPlug2 framework
│  │  └─ nam-src/               # NeuralAmpModelerCore DSP engine
│  ├─ config/
│  │  └─ NAMGuitarConfig.h      # Branding and plugin configuration
│  ├─ resources/
│  │  ├─ ui/                    # Web UI assets served by the WebView
│  │  │  ├─ index.html
│  │  │  ├─ main.js
│  │  │  ├─ styles.css
│  │  │  └─ ts/                 # TypeScript source files
│  │  ├─ amps/                  # Default NAM models
│  │  └─ ir/                    # Default impulse responses
│  ├─ src/
│  │  ├─ NAMGuitarPlugin.cpp    # iPlug2 plugin entry points
│  │  ├─ NAMGuitarPlugin.h
│  │  ├─ dsp/                   # Audio processing core (NAMDSPManager, IRManager)
│  │  ├─ models/                # Model hashing helpers
│  │  ├─ presets/               # Preset data management (PresetManager, PresetStorage)
│  │  ├─ network/               # Preset service client for remote API
│  │  ├─ ui/                    # WebView bridge helpers
│  │  ├─ util/                  # File system utilities
│  │  └─ platform/              # Format-specific wrappers (VST3, AU, AAX)
│  ├─ tests/                    # Unit and integration tests
│  └─ tools/                    # Debug tools (VST3 host)
└─ build/                       # CMake build output directory
```

## Getting Started

1. **Clone the repository**
   ```powershell
   git clone https://github.com/<your-account>/neuron-guitar.git
   cd neuron-guitar\src
   ```

2. **Configure dependencies**
   CMake scripts will fetch the required third-party libraries (iPlug2, NeuralAmpModelerCore, and their dependencies) automatically via FetchContent when `NAMGUITAR_FETCH_DEPENDENCIES` is enabled (default). Dependencies are downloaded to the `_deps/` folder.

3. **Configure the build**
   ```powershell
   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```

4. **Build your desired format**
   ```powershell
   cmake --build build --config RelWithDebInfo
   ```

   iPlug2 provides format-specific targets once the SDK locations are configured (see below).

## Using the Plugin

After building, the VST3 plugin can be found in the build output directory. To install:
- **Windows**: Copy the `.vst3` bundle from `build\src\platform\vst3\Release\` to `C:\Program Files\Common Files\VST3\`
- **macOS**: Copy the `.vst3` bundle to `~/Library/Audio/Plug-Ins/VST3/`

For AU (macOS only), the plugin will be in `build/src/platform/au/Release/`.

## SDK Requirements

To produce binary plugins you must provide the following SDKs:

- **VST3** – Install the official VST3 SDK and set the `VST3_SDK_ROOT` environment variable.
- **AU** – Available on macOS; ensure XCode command-line tools are installed.
- **AAX** – Requires the Avid AAX SDK and legal access to the SDK. Set `AAX_SDK_ROOT`.

The `cmake/Toolchain` logic emits warnings when a requested SDK is unavailable; builds will continue for the supported targets only.

## Branding

Plugin branding is configured in `src/config/NAMGuitarConfig.h`:
- `NAM_BRAND_COMPANY_STR` – Manufacturer name (currently "Soundshed")
- `NAM_BRAND_PRODUCT_STR` – Product name (currently "Neuron FX")
- `NAM_BRAND_DISPLAY_STR` – Combined display name
- `NAM_BRAND_DOMAIN_STR` – Bundle identifier domain

These macros propagate to `PLUG_NAME`, `PLUG_MFR`, and all plugin format wrappers. Rebuild after editing to update resources and plugin metadata.

## Preset API

The preset service is implemented in `src/src/network/PresetServiceClient`. Remote preset search and download capabilities are abstracted for future integration with community-driven preset sharing services.

## Development Notes

- Audio-related code is documented inline to clarify processing decisions.
- The UI bridge exposes a message-based API between JavaScript and the C++ core. See `src/src/ui/WebUIBridge.*` and `src/resources/ui/main.js`.
- The UI is authored in TypeScript (see `src/resources/ui/ts/`) and compiled to JavaScript.
- Unit tests are located in `src/tests/` and can be run via CTest after building with `NAMGUITAR_ENABLE_TESTS=ON`.
- Debug tools including a VST3 test host are available when building with `NAMGUITAR_ENABLE_TOOLS=ON`.

### Offline Processing Test Utility

The project includes an offline WAV processing utility for testing and debugging DSP behavior outside of the plugin environment. This tool processes audio files through the complete DSP pipeline with NAM models and impulse responses, making it useful for comparing offline vs. live processing behavior.

**Building:**
```powershell
cmake --build build --config Debug --target NAMGuitarFX_OfflineProcessingTest
```

**Usage:**
```powershell
NAMGuitarFX_OfflineProcessingTest.exe <input.wav> <output.wav> [modelPath] [irPath]
```

**Examples:**
```powershell
# Passthrough mode (DSP processing without model/IR):
./NAMGuitarFX_OfflineProcessingTest.exe input.wav output.wav

# With NAM model only:
./NAMGuitarFX_OfflineProcessingTest.exe input.wav output.wav "amps/Guitar/model.nam"

# Full DSP chain (model + cabinet IR):
./NAMGuitarFX_OfflineProcessingTest.exe input.wav output.wav "amps/Guitar/model.nam" "ir/cabinet.wav"


```

Model and IR paths are relative to `src/resources/` by default, or can be specified as absolute paths. The tool processes audio in 512-sample blocks at the input file's sample rate and outputs 16-bit stereo WAV files.

### IR Processing Optimizations

Impulse Response (cabinet simulation) processing has been optimized for real-time performance using industry-standard libraries and multi-threading:

**PocketFFT Library:**
- **Header-Only FFT**: Integrated PocketFFT, a modern header-only FFT library with no external dependencies
- **Optimized Performance**: Highly efficient FFT implementation with good SIMD utilization
- **Zero Configuration**: Header-only library fetched automatically via CMake - no manual installation required
- **Pre-computed IR FFT**: The IR frequency domain representation is computed once at initialization

**Previous FFT Convolution Optimizations (kept for reference):**
- **Partitioned Convolution**: For large IRs (>1024 samples), the impulse response is split into segments
- **Reduced Memory Allocations**: Complex number vector allocations are minimized through reuse of buffers

**Multi-threaded Channel Processing:**
- Stereo channels (L/R) are processed in parallel using `std::thread` for improved CPU utilization on multi-core systems
- Adaptive threading: Single-threaded for small block sizes (<128 samples) where thread overhead exceeds benefits
- Channel processing (gate, drive, tone, NAM model, IR) can run concurrently without contention

**Expected Performance:**
- FFT-based convolution significantly faster than time-domain convolution for IRs >64 samples
- IR processing overhead reduced to sub-millisecond per block (in Release mode)
- Multi-threading on dual-core systems: ~60-70% total CPU reduction vs. single-threaded
- For real-time operation at 512-sample blocks/44.1kHz: stable real-time performance well under 1ms per block

**Building:**
```powershell
# PocketFFT is automatically fetched via CMake - no manual installation needed
cmake -G "Visual Studio 18 2026" -A x64 -S ./src -B ./src/build
cmake --build ./src/build --config Release
```

## License

NAMGuitarFX is distributed under the MIT license. Third-party components retain their upstream licenses (iPlug2, NeuralAmpModelerCore, and other dependencies).
