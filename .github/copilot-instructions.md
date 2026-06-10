# GuitarFX Agent Playbook

## Prime Directives

- Do not write code before stating assumptions.
- Do not claim correctness you haven't verified.
- Do not handle only the happy path.
- Under what conditions does this work?

## Project Map
- C++ core: core/src/ (DSP, presets, controller/dispatcher, resource loading)
- UI: core/ui/ts/ (WebView TypeScript SPA)
- Host integration: juce/ (JUCE standalone/VST3/AU plugin adapter and WebView host)
- Build: CMake + FetchContent; core tests in core/build, JUCE targets in juce/builds
- Docs: docs/ (architecture, data models, UI, network integrations)

## DSP Graph Essentials
- Graph runner: core/src/dsp/SignalGraphExecutor.h with nodes of type amp_nam, cab_ir, eq_parametric, delay_digital, reverb_room, dynamics_gate, etc.
- Effects live in core/src/dsp/effects/; new effects implement EffectProcessor and register via EffectRegistry.
- Validate parameter ranges and resource presence; fail fast with clear errors instead of silent defaults.
- Full spec: docs/signal-chain.md, docs/fx-library.md

## UI ↔ Plugin Messaging
- Messaging flows through core/src/MessageDispatcher.cpp and PluginController::HandleUIMessage().
- Common payloads: state, presetLoaded, loadPreset, setParameter, browseModel, addSignalPathNode, removeSignalPathNode.
- UI bridge lives in core/ui/ts/bridge.ts and core/ui/ts/messages.ts; native host glue is in juce/source/PluginProcessorAdapter.cpp and juce/source/PluginEditor.cpp.
- Keep messages backward compatible; guard against missing fields and unknown message types.
- Full spec: docs/user-interface.md

## Resource References
- ResourceRef supports library refs (resourceType + resourceId), filePath for user files, embeddedId for portable presets.
- When loading, prefer library refs; fall back to file/embedded only when provided. Validate existence and log meaningful errors.
- Full spec: docs/fx-library.md, docs/data-models.md

## Build Quickstart
- Configure the shared core (for tests and core-only work):
	powershell: cmake -S core -B core/build
- Configure the JUCE host (Standalone/VST3/AU):
	powershell: cmake -S juce -B juce/builds -G "Visual Studio 18 2026" -A x64
- Build the host targets:
	Debug Standalone: cmake --build juce/builds --config Debug --target SoundshedGuitar_Standalone
	Debug VST3: cmake --build juce/builds --config Debug --target SoundshedGuitar_VST3
	Release Standalone: cmake --build juce/builds --config Release --target SoundshedGuitar_Standalone
- UI bundle: cd core/ui && npm run build

## Testing
- From core/build (Debug only):
	powershell: ctest -C Debug --output-on-failure
- Key suites are defined in core/tests/CMakeLists.txt; common targets include PresetDSPLoadingTests, PresetManagementWorkflowTests, ResourcePreviewWorkflowTests, and SignalGraphExecutorTests.

## Coding Conventions
- Namespace guitarfx::; require C++20.
- Parameter IDs are handled in the plugin/controller path; keep them aligned with the current UI message contract in core/ui/ts/messages.ts.
- UI state is centralized in core/ui/ts/state.ts; keep one source of truth.
- JSON serialization uses nlohmann::json; maintain stable field names and defaults.
- Keep DSP real-time safe: avoid allocations and locks in audio thread; prefer preallocation and lock-free patterns.

## Key Files
- Controller + message routing: core/src/PluginController.cpp, core/src/MessageDispatcher.cpp
- Effect base + registry: core/src/dsp/EffectProcessor.h, core/src/dsp/EffectRegistry.h
- Preset types: core/src/presets/PresetTypes.h
- Graph executor: core/src/dsp/SignalGraphExecutor.h
- Config/branding: core/config/GuitarFXConfig.h
- UI entry: core/ui/ts/main.ts
- JUCE host glue: juce/source/PluginProcessorAdapter.cpp, juce/source/PluginEditor.cpp

## Documentation
- Architecture: docs/architecture-overview.md
- Signal chain: docs/signal-chain.md
- Effects/resources: docs/fx-library.md
- Presets/storage: docs/data-models.md
- UI/messaging: docs/user-interface.md
- Network / remote integrations: docs/network-api.md
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
