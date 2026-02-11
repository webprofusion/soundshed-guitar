# Soundshed Guitar

Soundshed Guitar is an iPlug2-based guitar processing plugin combining Neural Amp Modeler (NAM) with a modern web-driven UI. Targets VST3, AU, and AAX formats with an extensible signal graph architecture.

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
   git clone 
   cd /src
   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```
   CMake fetches dependencies (iPlug2, NeuralAmpModelerCore) via FetchContent.

2. **Build**
   ```powershell
   cmake --build build --config RelWithDebInfo
   ```

Clean and refetch-deps (/src):

`Remove-Item -Recurse -Force .\build`
Copy the vst3sdk content to `src/build/_deps/iplug2-src/Dependencies/IPlug/VST3_SDK`
-- `cmake -S . -B build -DGUITARFX_FETCH_DEPENDENCIES=ON`

Configure cmake in /src :
` cmake -S . -B build -G "Visual Studio 18 2026" `
Run configured release build (under /src):
`cmake --build build --config Release`
`cmake --build build --config Release --target SoundshedGuitar_App`
`cmake --build build --config Release --target SoundshedGuitar_App --parallel`

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

Edit `src/config/GuitarFXConfig.h` to customize plugin name and manufacturer. Rebuild after changes.

## Development

- **UI source**: `src/resources/ui/ts/` (TypeScript)
- **Build UI**: `cd src/resources/ui && npm run build`
- **Tests**: Enable with `GUITARFX_ENABLE_TESTS=ON`, run via CTest
- **Debug tools**: Enable with `GUITARFX_ENABLE_TOOLS=ON`

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
Testing plugin (version with external resources) with pluginval:
`C:\Tools>pluginval.exe  src\build\SoundshedGuitar.vst3`


# IPlug WebView DPI scaling issues
The webview rendering does not properly compensate for scaled desktop sizes on windows e.g. 150%.
Patch IPlugWebView_win.cpp at

src\out\build\x64-Debug\_deps\iplug2-src\IPlug\Extras\WebView\IPlugWebView_win.cpp:L492

```
void IWebViewImpl::SetWebViewBounds(float x, float y, float w, float h, float scale)
{
  auto s = GetScaleForHWND(mParentWnd);
  s= 1; // hack otherwise scale for window at 150 is 1.5 and webview bound is off the screen;
  mWebViewBounds = GetScaledRect(x, y, w, h, s);

  if (mWebViewCtrlr)
  {
    mWebViewCtrlr->SetBoundsAndZoomFactor(mWebViewBounds, scale);
  }
}
```

## License

MIT. Third-party components retain their upstream licenses.
