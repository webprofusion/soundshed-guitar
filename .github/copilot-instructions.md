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
  Handler definitions are grouped by feature across core/src/controller/ — see
  docs/agent-quickstart.md for the map.
- Common payloads: state, presetLoaded, loadPreset, setParameter, browseModel, addSignalPathNode, removeSignalPathNode.
- UI bridge lives in core/ui/ts/bridge.ts and core/ui/ts/messages.ts; native host glue is in juce/source/PluginProcessorAdapter.cpp and juce/source/PluginEditor.cpp.
- Keep messages backward compatible; guard against missing fields and unknown message types.
- **Dropped-file paths**: WebView2 is standard Chromium, so a dropped `File`'s `.path` property is always `undefined` — that's an Electron-only extension, not a web standard. Never gate a drop handler on `file.path`. To get file content into the native engine, read bytes in JS (`file.arrayBuffer()`) and send them over the bridge as base64 — see settings.ts's NAM/IR library drop (`importDroppedLibraryFile`) for the established pattern. If the native loader needs a real filesystem path (e.g. to avoid base64-inflating a large file), have the native handler write the received bytes to a temp file and hand off to the existing path-based loader, rather than trying to recover a path client-side.
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
- For UI-facing changes, a clean build/typecheck is not sufficient proof of correctness — layout/DOM regressions (e.g. insertBefore() against a node that moved parent) throw at runtime with no compile error. Verify against the live, backend-connected app: launch the Standalone build with WebView2's remote-debugging port and drive it via Chrome DevTools Protocol (browser-automation tools cannot see this native window). See tools/agent-ui-debug/README.md and docs/agent-quickstart.md ("Live UI Testing").

## Coding Conventions
- Namespace guitarfx::; require C++20.
- Parameter IDs are handled in the plugin/controller path; keep them aligned with the current UI message contract in core/ui/ts/messages.ts.
- UI state is centralized in core/ui/ts/state.ts; keep one source of truth.
- JSON serialization uses nlohmann::json; maintain stable field names and defaults.
- Keep DSP real-time safe: avoid allocations and locks in audio thread; prefer preallocation and lock-free patterns.
- In-app dragging is pointer-driven via core/ui/ts/pointerDrag.ts — do not add `draggable="true"` for it. WebKitGTK (the Linux WebView) never delivers the HTML5 `drop` event to our targets. Native drag-and-drop is only for drags that start outside the WebView, i.e. files from the OS.

### C++ formatting
- `core/` is formatted by the repo-root `.clang-format` (Microsoft base: Allman
  braces, 4-space indent, 120 columns, `T* p` / `T& p`, flush namespaces, and
  braces inserted on every control statement).
  Run it before committing C++:
  `clang-format --style=file -i <files>` — the binary ships with Visual Studio at
  `VC/Tools/Llvm/x64/bin/clang-format.exe`.
- **`juce/` is out of scope.** That subtree has its own `.clang-format` (JUCE
  house style, `SortIncludes: true`). Do not reformat it with the root config.
- Include order is hand-maintained (own header, then project, then std) and
  sorting is disabled on purpose — reordering can break implicit header
  dependencies.

### Braces
Every `if`, `for`, `while` and `do` gets braces, even when the body is a single
statement. `.clang-format` enforces this (`InsertBraces: true`), so you do not
have to add them by hand — but write them anyway, because a formatter run is not
a substitute for reading correct code.

```cpp
// Good
if (!node)
{
    return;
}

// Avoid — clang-format will rewrite it, but do not author it this way
if (!node) return;
```

### Vertical whitespace
Three rules. Rules 1 and 2 are enforced by **uncrustify**
(`tools/uncrustify.cfg`) because clang-format has no option that inserts blank
lines around control flow. Rule 3 no tool can do — uncrustify's `nl_before_*`
family covers struct, class and namespace but has no enum entry — so it stays on
the author and the reviewer.

1. **Before a control statement.** A blank line before `if`, `for`, `while`,
   `switch` and `do` when ordinary code precedes it. *(uncrustify)*
2. **After a control statement.** A blank line after its closing `}`.
   *(uncrustify)*
3. **Around an `enum` declaration.** A blank line before and after it.
   *(by hand)*

All three have the same exception: **skip the blank line when it would land at
the very start or the very end of a scope.** No blank line straight after an
opening `{`, none straight before a closing `}`. For rules 1 and 2 uncrustify
handles this via `eat_blanks_after_open_brace` / `eat_blanks_before_close_brace`.

Run uncrustify FIRST and clang-format SECOND — always both, in that order.
Uncrustify inserts the blank lines; clang-format then normalises everything else
and preserves them (`MaxEmptyLinesToKeep: 1`), so the two compose rather than
fight. The pair is confirmed convergent: a second run is a no-op.

```bash
uncrustify -c tools/uncrustify.cfg -l CPP --no-backup <files>
clang-format --style=file -i <files>
```

```cpp
// Good
void Configure(Node* node)
{
    enum class Phase
    {
        Setup,
        Run
    };

    int retries = 0;

    if (!node)
    {
        return;
    }

    retries = node->Retries();
}

// Avoid
void Configure(Node* node)
{

    enum class Phase
    {
        Setup,
        Run
    };
    int retries = 0;
    if (!node)
    {
        return;
    }
    retries = node->Retries();

}
```

## Key Files
- Controller + message routing: core/src/PluginController.cpp (feature groups in
  core/src/controller/PluginController*.cpp; shared helpers in
  core/src/controller/internal/), core/src/MessageDispatcher.cpp
  (routing tables in core/src/dispatcher/)
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
- C++ under core/ run through uncrustify then clang-format before committing
  (see Coding Conventions → Vertical whitespace for the two commands).
- Build or relevant tests executed (note which ones). For UI changes, run npm build.
- For UI-facing changes, verified in the live running app (see Testing → live UI verification), not just typecheck/build.
- Backward compatibility considered for presets, resources, and UI messages.
- Docs or comments updated when behavior changes.

## Communication
- Keep updates concise and scoped; reference affected files.
- If blockers or unexpected changes appear, pause and ask before proceeding.

## Git Workflow
- **Never auto-commit.** Always wait for explicit user approval before committing changes to git.
- Users control the commit lifecycle: review diffs in the UI, approve changes, and trigger commits manually.
- If you need to commit as part of a task, ask the user first and show them the diff before proceeding.
