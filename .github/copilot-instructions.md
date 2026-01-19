# GuitarFX Agent Playbook

## Prime Directives

- Do not write code before stating assumptions.
- Do not claim correctness you haven't verified.
- Do not handle only the happy path.
- Under what conditions does this work?

## Project Map
- C++ core: src/src/ (DSP, presets, plugin entry)
- UI: src/resources/ui/ts/ (WebView TypeScript)
- Build: CMake + FetchContent; targets for App, VST3, tests
- Docs: docs/ (architecture, data models, UI, API)

## DSP Graph Essentials
- Graph runner: SignalGraphExecutor with nodes of type amp_nam, cab_ir, eq_parametric, delay_digital, reverb_room, dynamics_gate, etc.
- Effects live in src/src/dsp/effects/; new effects implement EffectProcessor and register via EffectRegistry.
- Validate parameter ranges and resource presence; fail fast with clear errors instead of silent defaults.
- Full spec: docs/signal-chain.md, docs/fx-library.md

## UI ↔ Plugin Messaging
- Messaging via OnMessageFromWebView / SendMessageToUI.
- Common payloads: state, presetLoaded, loadPreset, setParameter, browseModel, addSignalPathNode, removeSignalPathNode.
- UI handler: src/resources/ui/ts/messages.ts; plugin handler: HandleUIMessage in src/src/GuitarFXPlugin.cpp.
- Keep messages backward compatible; guard against missing fields and unknown message types.
- Full spec: docs/user-interface.md

## Resource References
- ResourceRef supports library refs (resourceType + resourceId), filePath for user files, embeddedId for portable presets.
- When loading, prefer library refs; fall back to file/embedded only when provided. Validate existence and log meaningful errors.
- Full spec: docs/fx-library.md, docs/data-models.md

## Build Quickstart
- Configure (once, from src/):
	powershell: cmake -G "Visual Studio 18 2026" -A x64 -S . -B build
- Build (tasks available in VS Code):
	Debug App: cmake --build build --config Debug --target SoundshedGuitar_App
	Debug VST3: cmake --build build --config Debug --target SoundshedGuitar_VST3
	Release App: cmake --build build --config Release --target SoundshedGuitar_App
- UI bundle: cd src/resources/ui && npm run build

## Testing
- From src/build (Debug only):
	powershell: ctest --build-config Debug --output-on-failure
- Key suites: PresetDSPLoadingTests.cpp (model/IR loading), PresetDSPProcessingTests.cpp (processing), IRConvolutionTests.cpp (convolution correctness).

## Coding Conventions
- Namespace guitarfx::; require C++20.
- Parameter IDs: ParameterId enum in GuitarFXPlugin.h.
- UI state centralized in src/resources/ui/ts/state.ts; keep one source of truth.
- JSON serialization uses nlohmann::json; maintain stable field names and defaults.
- Keep DSP real-time safe: avoid allocations and locks in audio thread; prefer preallocation and lock-free patterns.

## Key Files
- Plugin entry: src/src/GuitarFXPlugin.cpp
- Effect base: src/src/dsp/EffectProcessor.h
- Registry: src/src/dsp/EffectRegistry.h
- Preset types: src/src/presets/PresetTypes.h
- Graph executor: src/src/dsp/SignalGraphExecutor.h
- Config/branding: src/config/GuitarFXConfig.h
- UI entry: src/resources/ui/ts/main.ts

## Documentation
- Architecture: docs/architecture-overview.md
- Signal chain: docs/signal-chain.md
- Effects/resources: docs/fx-library.md
- Presets/storage: docs/data-models.md
- UI/messaging: docs/user-interface.md
- Network/API: docs/network-api.md
- Theming: docs/theme-system.md
- PRD: docs/prd/PRD.md

## Change Checklist
- Assumptions stated and confirmed where needed.
- Error paths covered; log actionable messages.
- Build or relevant tests executed (note which ones). For UI changes, run npm build.
- Backward compatibility considered for presets, resources, and UI messages.
- Docs or comments updated when behavior changes.

## Communication
- Keep updates concise and scoped; reference affected files.
- If blockers or unexpected changes appear, pause and ask before proceeding.
