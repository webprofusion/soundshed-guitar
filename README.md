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
├─ CMakeLists.txt              # Root CMake entry point
├─ extern/                     # Third-party sources populated via CMake
│  ├─ iPlug2/ (fetched)        # iPlug2 framework
│  └─ NeuralAmpModelerCore/    # NAM DSP engine
├─ resources/
│  └─ ui/                      # Web UI assets served by the WebView
│     ├─ index.html
│     ├─ app.js
│     └─ styles.css
└─ src/
   ├─ NeuronGuitar.cpp         # iPlug2 plugin entry points
   ├─ NeuronGuitar.h
   ├─ dsp/                     # Audio processing core
   ├─ io/                      # File hashing/caching helpers
   ├─ presets/                 # Preset data management
   └─ ui/                      # WebView bridge helpers
```

## Getting Started

1. **Clone the repository**
   ```powershell
   git clone https://github.com/<your-account>/neuron-guitar.git
   cd neuron-guitar
   ```

2. **Configure submodules / dependencies**
   CMake scripts will fetch the required third-party libraries (iPlug2, NAM core, httplib, nlohmann-json) automatically. If you prefer to manage them manually, place them under the `extern/` folder before configuring the build.

3. **Configure the build**
   ```powershell
   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```

4. **Build your desired format**
   ```powershell
   cmake --build build --config RelWithDebInfo
   ```

   iPlug2 provides format-specific targets once the SDK locations are configured (see below).

## Using the app/plugin
To use the plugin copy \src\build\src\platform\vst3\Release\*.dll to C:\Program Files\CommonFiles\VST3 as *.vst

## SDK Requirements

To produce binary plugins you must provide the following SDKs:

- **VST3** – Install the official VST3 SDK and set the `VST3_SDK_ROOT` environment variable.
- **AU** – Available on macOS; ensure XCode command-line tools are installed.
- **AAX** – Requires the Avid AAX SDK and legal access to the SDK. Set `AAX_SDK_ROOT`.

The `cmake/Toolchain` logic emits warnings when a requested SDK is unavailable; builds will continue for the supported targets only.

## Branding

Standalone and plugin builds read their company/product labels from `src/config/NAMGuitarConfig.h`. Update `NAM_BRAND_COMPANY_STR` and `NAM_BRAND_PRODUCT_STR` (or the combined `NAM_BRAND_DISPLAY_STR`) to rename the app once and propagate the change everywhere `PLUG_NAME`/`PLUG_MFR` are consumed. Rebuild after editing the config so resources and wrappers pick up the new values.

## Preset API

The preset service is abstracted behind `PresetRemoteService`. By default it points to `https://api.example.com/presets`. Override the endpoint through the preferences JSON (`resources/config/defaults.json`) or set the `NEURON_GUITAR_PRESET_ENDPOINT` environment variable.

## Development Notes

- Audio-related code is documented inline to clarify processing decisions.
- The UI bridge exposes a message-based API between JavaScript and the C++ core. See `src/ui/WebViewBridge.*` and `resources/ui/app.js`.
- Unit tests (future work) can be added under `tests/` and driven by CTest.

## License

NeuronGuitar is distributed under the MIT license. Third-party components keep their upstream licenses (iPlug2, NAM Core, httplib, nlohmann-json).
