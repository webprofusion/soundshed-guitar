# WASM Custom Effects security review, 2026-10-19

Scope: the `wasm_host` effect (`core/src/dsp/effects/WasmEffect.cpp`), how modules reach it
(controller, preset archives, Tone Sharing, DAW state, the Custom Effect Designer), the build
fetch of Wasmtime, and the UI that renders what a module declares about itself. Reviewed at
`9f4ace56`. No code changed.

## Status (2026-10-19, same day)

Findings 1-5 are fixed in the working tree (not committed), with parts of 6:

| # | Fix | Where |
|---|-----|-------|
| 1 | Shared archives, packs, Tone Sharing items and remote imports install only `nam`/`ir`, in the engine and the UI. Custom Effect nodes in shared presets lose their module and cached descriptor. The engine gates every module load on the Custom Effects setting. That also closes `plugin`-type resources riding in archives. | `IsShareableResourceType`, `SanitizeImportedPreset` (both sides), `HandleImportRemoteResourceRequest`, `WasmEffect::SetHostingEnabled`, `ApplyCustomEffectsSettingFromAppSettings` |
| 2 | One shared engine with epoch interruption (10 ms tick). Deadlines: 4× block or 200 ms for process, 200 ms for reset, 5 s for setup. Store limiter: 256 MiB, 100k table elements, 1 instance. 256 KiB wasm stack. 32 MiB module cap. Cranelift forced; threads, GC, exceptions, stack switching and memory64 off. | `WasmSharedRuntime`, `ArmGuestDeadline` in `WasmEffect.cpp` |
| 3 | Wasmtime 48.0.6 (LTS), with a SHA-256 `URL_HASH` per platform bundle. | `core/CMakeLists.txt` |
| 4 | Category escaped in the add-effect menu. Category and thumbnail are cleaned in the engine (descriptor and library) and in the UI (`safeImageDataUrl`, `presetV2.ts`). | `addEffectDropdown.ts`, `CustomEffectLibrary.cpp` |
| 5 | Network paths are refused for wasm before any filesystem access, and cleared from wasm nodes on every preset load. Projects no longer restore the two Custom Effects switches. | `WasmEffect::IsNetworkPath`, `RefreshWasmNodeDescriptor`, `PluginControllerHostState.cpp` |
| 6 | Done: thumbnails are `data:` only, there is one engine for all instances, and hostile-module tests exist. Open: the loose library fallback for `wasm` ids, server signatures for designer modules, and escaping of `lastError` everywhere it shows. | |

Tests: `WasmEffectHostTests` covers a looping process, recursion, a looping start function, oversized memory, network paths, the gate and a hostile descriptor. Also `CustomEffectsSettingTests`, `CustomEffectLibraryTests`, `PresetArchiveInstallTests`, `sharedArchiveSafety.test.ts` and `addEffectDropdown.test.ts`.

### Sharing modules later

Sharing Custom Effect modules is wanted. The block is meant to be lifted, so it lives in one rule on each side: `IsShareableResourceType` in `PresetArchiveSupport.cpp` and `isShareableResourceType` in `ui/ts/presets/sanitize.ts`. `SanitizeImportedPreset` (both sides) keeps the node and drops only its module. With the runtime limits now in place, a shared module can at worst burn its time budget and pass audio dry. What is still needed to allow it:

- **Consent that names code.** On install, say that the item contains a Custom Effect (a program) and from whom, and install the module only on a yes. The current pack and archive dialogs show counts only. The one-click Tone Sharing single-item install would need a prompt too.
- **Provenance.** Ideally only modules the Tone Sharing service has reviewed or signed, checked against a key the app ships with. Hash-pin each module in the item's manifest so the bytes can't change after review.
- **Gate still applies.** An installed shared module should load only while Custom Effects are on (it already will), and keep the stripped descriptor rules.
- **Wasmtime kept current.** Shared modules make runtime escapes the main remaining risk, so the LTS line has to be kept patched.
- Then: let `wasm` through `IsShareableResourceType` when the source is consented or signed, rather than for every archive. Keep `plugin` blocked, since that is native code.

Still open beyond this review:

- UNC paths for `nam`/`ir` in DAW project state still reach the filesystem (the NTLM leak in finding 5). Archives clear paths; host-state restore does not.
- The archive exporter still writes `wasm` and `plugin` resources, which imports now skip.
- The Release build, the JUCE plugin targets and Linux CI were not rebuilt here.

## Threat model in one paragraph

A `.wasm` module is code from whoever wrote it. The host compiles it with Cranelift, runs its
start function, calls its descriptor exports on the message thread, then calls `audiofx_process`
once per sample on the audio thread. The guest gets three read-only imports (`read_param`,
`read_resource_size`, `read_resource_byte`) and no WASI, so on a correct Wasmtime it cannot touch
files, the network or host memory. What it can do is **use CPU and memory without limit, crash
the host through runtime bugs, and put its own strings into the UI**. A sandbox escape depends on
a Wasmtime bug, so the Wasmtime version matters. The question is who can get a module in front
of that host, and whether the user knows they are running code.

## What is sound

- No WASI and no write-capable imports. Resource reads are bounds-checked per slot and offset
  (`WasmEffect.cpp:1039-1079`).
- The descriptor read checks ptr/len against the exported memory size, in 64-bit arithmetic, so
  it cannot overflow (`WasmEffect.cpp:1562-1572`).
- Descriptor params are slot-checked (0..7) and checked for duplicate ids and slots, and max < min
  is rejected (`WasmEffect.cpp:285-414`).
- Traps and wrong result types put the node into dry passthrough instead of failing
  (`WasmEffect.cpp:1371-1391`). Non-finite output falls back to the input sample.
- The resource slot index comes from the executor's own array index
  (`SignalGraphExecutor.cpp:466`), not from preset data, so a preset can't force a huge
  `resize` there.
- Folder scans ignore `.wasm` (`ResourceFolderScanner.cpp:163-181`). Tone preview stages only
  `nam`/`ir` files (`PluginControllerTonePreview.cpp:195`).
- Most UI templates escape guest strings, including the flyout param rows and the designer
  panels.

## Findings

### 1. High — a shared tone can install and run a module with one click and no code warning

- Preset archives accept any resource `type`:
  - C++: `PresetArchiveSupport.cpp:483-503`.
  - Web UI: `presets/archive.ts:1040-1058`, which sends `importRemoteResource` with
    `resourceType: resource.type`.
  - `HandleImportRemoteResourceRequest` (`PluginControllerResources.cpp:157-188`) stores whatever
    type it is given.
- `SanitizeImportedPreset` and `ui/ts/presets/sanitize.ts` keep `wasm_host` nodes.
- **Tone Sharing single-item install** (`toneSharingPanel/assets.ts:93-100`) imports straight
  away with no dialog. The import then auto-loads the last preset (`archive.ts:1162`), and the
  load runs `InspectModuleFile`, then `LoadResources` and `Prepare`. So **one Install click
  compiles and runs an uploader's wasm**. The pack install and archive-session dialogs show
  counts and storage choices, never "this contains code".
- The UI flag `Features.CustomEffects` is off by default but **only hides UI**
  (`featureFlags.ts:119-126`). C++ registers the effect whenever it is compiled in
  (`BuiltinEffects.h:77-79`), and nothing in `core/src` checks `customEffects.enabled`.
- It is compiled in on Windows x64/arm64 and Linux release builds. It is off on the universal
  macOS release and on Android. (Since 2026-11-12 the universal macOS release has it too: the
  core joins Wasmtime's arm64 and x86_64 libraries, the standalone carries
  `allow-unsigned-executable-memory`, and in a plugin host without it the effect refuses to load
  rather than have the kernel kill the host; `core/src/dsp/effects/WasmPlatform.h`.)

Impact today: the CPU, memory and crash denial of service in findings 2 and 3, served to any
user who installs a tone, plus full exposure to any future Wasmtime sandbox escape.

**Fix:**
- Allow-list resource types (`nam`, `ir`) for every untrusted source: the Tone Sharing API,
  dropped archives and remote imports.
- Have `SanitizeImportedPreset` (C++ and UI) either strip `wasm_host` nodes or mark them
  disabled, with an explicit "this preset contains a Custom Effect (code)" prompt.
- Gate creating `wasm_host` in C++ on the feature flag, not just in the UI.
- Consider rejecting wasm uploads server-side on the Tone Sharing API.

### 2. High — no execution limits: a guest can hang the audio thread or the DAW

- `wasm_engine_new()` uses a default config: no fuel, no epoch interruption, no
  `wasmtime_store_limiter`, and no `max_wasm_stack` change. Nothing in `core/src` calls
  `wasmtime_config_*`.
- An infinite loop in `audiofx_process` freezes the realtime thread for good. In a DAW that
  stalls the whole engine. A loop in the start function or `*_descriptor_ptr` freezes the
  message thread on preset load, since `InspectModuleFile` runs from `ApplyPreset`.
- `memory.grow` and table growth are bounded only by the 4 GiB wasm32 limit per instance. Each
  dual-mono twin and each `InspectModuleFile` probe is another instance.
- Native stack: Wasmtime's 512 KiB default wasm stack assumes the host thread has at least that
  much free. Some audio threads have less, so deep guest recursion could hit the native guard
  page and crash instead of trapping. This is plausible but untested.

**Fix:**
- Enable epoch interruption with a deadline per `process` call and per descriptor/prepare call.
  A watchdog bumps the epoch, and on a trap the node bypasses as it does now. Fuel also works but
  costs more per sample.
- Add a store limiter, for example memory ≤ 64 MiB, tables ≤ 10k elements, instances = 1.
- Set `max_wasm_stack` well under the smallest audio-thread stack, for example 64-128 KiB.
- Move `InspectModuleFile` off the message thread, or put it under the same limits.

### 3. Medium-High — Wasmtime 43.0.1 is unpatched and out of support; the download isn't pinned

- `core/CMakeLists.txt:309` pins 43.0.1 (released around April 2026). That release has the
  April 9 fixes, including the aarch64 Cranelift escape GHSA-jhxm-h53p-jm7w, which is fixed in
  43.0.1. It does **not** have RUSTSEC-2026-0114 / CVE-2026-44216, a host panic when a table is
  allocated larger than the address space, fixed in 43.0.2. A panic across the C API aborts the
  process, so a module that triggers it crashes the DAW.
- 43.x is not an LTS line, so later Cranelift fixes won't be backported. The later advisories
  checked (component-model async, GC/tags in 47+, WASI) don't apply to this core-module,
  no-WASI host, but the next escape will only ship in supported lines.
- `FetchContent_Declare` has no `URL_HASH`, so the build trusts whatever the GitHub release URL
  serves at configure time.

**Fix:** move to the 48.x LTS (48.0.4 or later) or the current release, add `URL_HASH SHA256=…`
for each platform bundle, and add a check that the pinned version is still supported.

### 4. Medium — a module's self-declared category is injected as HTML into the add-effect menu

- `effect.category` from the guest descriptor becomes the custom-effect entry's category
  (`PluginControllerCustomEffects.cpp:387-406`). It is stored unchanged
  (`CustomEffectLibrary.cpp:97`) and becomes the FX item's category (`fxSelector.ts:597`).
- `addEffectDropdown.ts:53` renders `${categoryInfo?.name || categoryId}` without
  `escapeHtml`. A category not in `CATEGORY_METADATA` therefore goes in as raw HTML.
- The CSP (`index.html:52`) blocks inline script and event handlers. It still allows
  `<iframe src="https://…">` (`frame-src https:`), remote `<img>`, `<style>`, and fake controls
  or links over the effect menu. That is enough for UI redressing or phishing inside the plugin
  window.
- The repo's escaping lint (`no-unescaped-html-attribute`) checks attributes only, so it misses
  text-node interpolation like this.

**Fix:** escape it. Allow-list categories, or map unknown ones to `utility`, on the C++ side when
the entry is saved. Consider extending the lint to text interpolation inside templates assigned
to `innerHTML`.

### 5. Medium — DAW project state can point a wasm node at any absolute or UNC path

- Host-state restore (`PluginControllerHostState.cpp:305-340, 511-524`) never calls
  `SanitizeImportedPreset`.
- `ResolveStoredPathForRuntime` returns absolute paths unchanged (`PresetStorage.cpp:37-39`), and
  `ResolveResourcePath` falls back to the raw `filePath` (`SignalGraphExecutor.cpp:73-89`).
- So a shared project can run a `.wasm` from `\\host\share\x.wasm` when it opens. On Windows that
  UNC access also sends the user's NTLM credentials to the share host. The NTLM leak applies to
  `nam`/`ir` paths too, so it is a general issue that the wasm case makes worse.
- A project's `appSettings` merge can also switch on `features.customEffects.enabled`
  (`:428-437`), but that flag gates nothing in C++ (finding 1).

**Fix:** on host-state restore, refuse UNC paths, or ask first. For `wasm_host`, only allow
modules that are already in the library, by id or hash, never a raw `filePath`.

### 6. Low — smaller items

- **Remote thumbnail beacon.** `effect.thumbnailDataUrl` is free-form and is rendered as
  `<img src>` (`fxSelector.ts:465`, `addEffectDropdown.ts:57-60`, `signalPath.ts:935`). It is
  attribute-escaped, but `img-src` allows `https:`/`http:`, so a module can make the app fetch a
  tracking URL every time the node or menu renders. Accept only `data:image/*` here.
- **Loose library fallback.** A missing resource id can resolve to another resource of the same
  type by id suffix, `originalId` or hash (`ResourceLibrary.cpp:337-369`). For `wasm` that means
  a preset can name a module and get a different one that is already installed. Require an exact
  id or hash match for `wasm`.
- **Designer modules are fully trusted.** `moduleBase64` from the Soundshed API
  (`customEffectDesigner.ts:574`) is written and instantiated with no signature, hash or magic
  check. A compromised API or session would push code to every designer user. Sandboxed, so this
  is bounded by findings 2 and 3, but a server signature would close it.
- **Guest text in errors.** Trap messages can include guest-chosen names from the module's name
  section, and they flow into `lastError` and then the UI. Make sure every place that shows it
  escapes it.
- **One engine per instance.** Each `WasmEffect`, and each probe, calls `wasm_engine_new()`. One
  shared engine would be cheaper and would make epoch interruption one global tick.
- **No hostile-module tests and no stated trust model.** `WasmEffectHostTests.cpp` covers
  well-behaved modules only, and `docs/wasm-module-authoring.md` says nothing about limits or
  trust.
  - Add tests for an infinite loop in process, start and descriptor; for `memory.grow` to the
    limit; for a huge table; for deep recursion; and for a category or thumbnail carrying HTML.

## Suggested order

1. Close the delivery paths: a type allow-list for untrusted imports, a `wasm_host` strip or
   prompt in the sanitisers, and a C++ feature gate (finding 1).
2. Add epoch interruption, a store limiter and a stack cap (finding 2).
3. Upgrade Wasmtime and hash-pin it (finding 3).
4. Escape the category and restrict thumbnails to `data:` (findings 4 and 6).
5. Restrict host-state paths (finding 5).
6. Add hostile-module tests.

Wasmtime advisory sources: <https://rustsec.org/packages/wasmtime.html>,
<https://rustsec.org/advisories/RUSTSEC-2026-0114.html>,
<https://bytecodealliance.org/articles/wasmtime-security-advisories>.
