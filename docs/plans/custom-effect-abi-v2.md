# Custom Effect ABI v2

A Custom Effect is a WASM module run by `WasmEffect` (`core/src/dsp/effects/WasmEffect.{h,cpp}`).
Presets, MIDI mapping and the UI already address its parameters by string id. The module does not:
it reads them by number, and the numbers come from arithmetic the author has to get right with
nothing to tell them when they don't. ABI v2 has the module bind by id, once, and fail loudly
when an id is wrong.

No Custom Effect modules are known to be in use (Chris, 2026-10-29), so v2 replaces v1. There is
no compatibility layer; a v1 module is refused with a message that points at the migration guide
below.

**Status (2026-11-17):** phases 1 (params, 5e9795b8), 2 (resources, 355696ae) and 5 (block
processing, 3cc997b4) are committed, and phase 4 (docs and samples) is done. The host calls the
guest through Wasmtime's unchecked API, with every export's signature checked at load. The UI
still parses the cached `key=value` entries itself until phase 3.

## What is wrong with v1

1. **The tempo hole.** `read_param(8)` is the tempo, so slots 8..31 are read one index higher
   (`ReadGuestParam`, `WasmEffect.cpp`). Reading slot 10 with `read_param(10)` gets slot 9;
   reading slot 8 with `read_param(8)` gets 120.0 as a knob value. Neither is an error.
2. **Two numberings in the descriptor.** A param is `param.N.*` with a `param.N.slot`, and the
   slot defaults to N. Insert or reorder a block without explicit slots and every later param
   moves slot: presets still load (they are keyed by id) but the DSP reads the wrong knobs.
3. **Three namespaces in one `SetParam`.** Host params (`mix`, `inputGainDb`, `outputGainDb`,
   `bpm`), generic `paramN` keys and guest ids, with no check between them. A guest id `mix` is
   swallowed by the host branch and listed twice in the UI (`presetV2.ts` concatenates guest and
   base params); a guest id `param3` goes to slot 2 whatever slot it declared.
4. **Resources are positional too.** `node.resources[i]` is stamped `resourceSlotIndex = i` by
   `SignalGraphExecutor`; slot 0 is the module, `resource.N.slot` defaults to N + 1. Reorder the
   resource blocks and a saved IR lands on the wrong resource. `read_resource_byte` reads one byte
   per host call.
5. **The descriptor is parsed twice**, in C++ (`BuildDescriptorFromEntries`) and in TS
   (`buildWasmNodeEffectInfo`, `presetV2.ts`), and they already disagree: max < min is an error
   in one and clamped in the other.
6. **Unknown keys are parked from the audio thread.** `SetParam` inserts into
   `mPendingGuestParamValues`, and automation calls `SetParam` on the audio thread.

## Standards considered

The question was whether an existing format could replace our own ABI.

| Format | What it offers | Verdict |
| --- | --- | --- |
| **WCLAP** (CLAP built for wasm32; `wclap-bridge` hosts it on Wasmtime, BSL-1.0) | A real plugin standard: stable plugin-chosen param ids, full param info, sample-accurate param events, block processing, state, latency. clap-wrapper builds existing C++/JUCE CLAPs as WCLAPs (Surge XT and CHOW Tape have been ported). | Not for Custom Effects. The bridge is early: no releases, tested properly only on macOS, and it calls `abort()` on catastrophic errors, which takes our process down. It needs WASI and wasi-threads, where we deliberately allow no WASI. The CLAP ABI is far too large to hand-write or generate in WAT. Worth watching as a separate hosting path for real third-party plugins next to native CLAP, not as a replacement for this. |
| **Faust** (DSL compiled to wasm plus a JSON UI description; params addressed by path) | A compact DSL with a mature DSP library: a very good thing for people, and AIs, to write effects in. | Not the ABI: the generated wasm interface isn't a formal stable standard and changes between compiler versions. It could become an authoring route later, by compiling Faust to our ABI. |
| **Cmajor** (patches with named, annotated endpoints; wasm export via `cmaj generate`) | Named parameters with ranges declared on the endpoint. | Same as Faust: the generated wasm interface is not a standard, and the licence is GPLv3 or commercial. |
| **Web Audio Modules 2.0** | Params by id with full info. | Its API is JavaScript on AudioWorklet; there is no JS runtime in our host. |
| **WebAssembly Component Model / WIT** | A standard IDL with typed records and string ids. | The Wasmtime C API's component support is thinner than its core-module support, and components can't be written in WAT or by most generators. Our ABI is eight functions; an IDL buys little. |
| **LV2** | Ports addressed by a stable `symbol` string beside the index. | Not wasm, but it is the same idea as v2: bind by symbol, not position. |

So v2 stays our own small core-wasm ABI. The things it borrows are CLAP's: ids that are the
module's own and stable across versions, param info declared once, and no meaning attached to a
param's position.

## The v2 ABI

### Exports

| Export | Signature | Notes |
| --- | --- | --- |
| `audiofx_abi_version` | `() -> i32` | Required. Returns `2`. |
| `memory` | memory | Required (the descriptor and handle lookups read it). |
| `audiofx_descriptor_ptr` / `_len` | `() -> i32` | Required. A module must declare what it reads. |
| `audiofx_prepare` | `(sampleRate: f32, maxBlockSize: i32) -> i32` | 0 on success. `resourceSlotCount` is gone. Size the I/O buffer for `maxBlockSize` here. |
| `audiofx_buffer_ptr` | `() -> i32` | Required. The I/O buffer's address in `memory`, asked once after each prepare: `maxBlockSize` f32 left samples, then `maxBlockSize` f32 right ones. 4-byte aligned. |
| `audiofx_reset` | `()` | |
| `audiofx_process` | `(frames: i32) -> ()` | Processes `frames` (1..`maxBlockSize`) in place in the I/O buffer: the host writes the input there, calls, and reads the output from the same place. |
| `audiofx_get_latency_samples` | `() -> i32` | Optional. |

The `guitarfx_*` aliases go.

Every export must have exactly the signature above. The host checks each one when the module
loads, because every call it makes into the guest is unchecked (D1), and a module with the wrong
shape is refused with a message giving both signatures. The per-sample `(f32, f32) -> (f32, f32)`
process that ABI 2 had before D1 is refused this way, with a pointer to `audiofx_buffer_ptr`.

The buffer is checked once, after prepare: it must be aligned and lie inside `memory`, or the
module is refused with the address it returned. Memory never shrinks, so it stays inside. The host
looks the memory's base up again for every block, because `memory.grow` can move it. A host block
longer than `maxBlockSize` goes through in pieces. A non-finite output sample is replaced with the
input sample the guest was given.

### Imports (module `host`)

| Import | Signature | Notes |
| --- | --- | --- |
| `param_handle` | `(idPtr: i32, idLen: i32) -> i32` | The id's handle, or -1 if the descriptor doesn't declare it. Call it in `audiofx_prepare` and keep the result. |
| `param_value` | `(handle: i32) -> f32` | The current value. An invalid handle traps, and the node bypasses with the bad handle in `lastError`. |
| `tempo_bpm` | `() -> f32` | Host tempo. Never shares a namespace with params again. |
| `resource_handle` | `(idPtr: i32, idLen: i32) -> i32` | As `param_handle`, for `resource.<id>`. |
| `resource_size` | `(handle: i32) -> i32` | Bytes, 0 when no file is chosen. |
| `resource_read` | `(handle: i32, offset: i32, dstPtr: i32, len: i32) -> i32` | Copies into guest memory, returns the bytes copied: stops at the end of the resource and of guest memory, 0 for a negative argument. Replaces byte-at-a-time reads. |

`resource_size` and `resource_read` trap on a handle `resource_handle` never gave, as
`param_value` does; resource handles have their own per-load base, so a param handle is never
taken for one. A resource file larger than guest memory can hold (256 MB) is refused at load.

The import names differ from v1's on purpose: a v1 module that imports `read_param` fails before
it is instantiated, so it cannot be half-loaded. The host lists the module's imports
(`wasmtime_module_imports`) before linking and turns any `host.*` import it doesn't provide into
a message: "built for Custom Effect ABI 1; see the migration guide" for `read_param` and the
resource readers, the import's name for anything else.

**Handles are opaque.** The host numbers them from a base chosen afresh at every load, so a
module that hard-codes `0, 1, 2` instead of calling `param_handle` traps on its first test run
rather than working by accident. Hard-coding is exactly the mistake a generated module makes.

`param_handle` reads the id out of guest memory through `wasmtime_caller_export_get(caller,
"memory")`, bounds-checks `idPtr + idLen` against the memory's size and `idLen` against the id
limit, and compares against the declared ids without allocating. It is cheap enough to be safe
anywhere, but the guide says call it in prepare.

### Descriptor

Still one `key=value` per line (easy to emit from WAT, C, Rust or Go, with no escaping), but
keyed by id instead of position:

```
effect.name=Tremolo Pan
effect.category=modulation
effect.description=Stereo tremolo and auto-pan.
param.rate.title=Rate
param.rate.default=0.25
param.rate.min=0
param.rate.max=1
param.rate.unit=amount
param.depth.title=Depth
param.depth.default=0.5
resource.ir.title=Impulse
resource.ir.type=ir
```

- The order an id first appears in is its order in the UI. Nothing else depends on order.
- Ids match `[A-Za-z][A-Za-z0-9_]{0,31}`. An id that is one of the host's own params
  (`WasmEffect::kHostParamIds`: `mix`, `inputGainDb`, `outputGainDb`, `bpm`; a test holds it to
  the registered params) is an error naming it. `paramN` is an ordinary id now that the generic
  `paramN` keys are gone.
- `audiofx_abi_version` is the only version signal; the descriptor carries no `abi=` line.
- Fields are v1's: `title`, `name`, `default`, `min`, `max`, `unit`, `group`, `advanced`, `step`,
  `labels`; for resources `title`, `type`, `allowBrowseFile`, `parameterId`, `parameterValue`.
  `slot` is an error, and so is a numeric id, so `param.0.id=gain` reports "numbered param blocks
  are ABI 1" rather than quietly declaring a param called `0`.
- At most 64 params (`kMaxGuestParams`) and 16 resources. Values live in a fixed array indexed by
  declaration order, so nothing on the audio thread allocates.

## Host changes

- **`WasmEffect`.** `mGuestMacros[32]` becomes a fixed `kMaxGuestParams` array indexed by
  declaration order; the id-to-index map is built when the descriptor is applied, on the loading
  thread. `SetParam` for an id the loaded descriptor doesn't declare is dropped; only before any
  descriptor is loaded is a value parked, which is never on the audio thread. Delete
  `ParseGuestMacroIndex`, `kTempoParamIndex`, the slot shift in `ReadGuestParam`, generic
  `paramN` keys and the `guitarfx_*` aliases.
- **Resources by id.** `ResourceRef::metadata` is never saved, so the ids live in node config
  instead: `wasmResourceIds` is a JSON array of guest resource ids, one per node resource, `""`
  for the module at `node.resources[0]`. `RefreshWasmNodeDescriptor` calls
  `WasmEffect::RekeyNodeResources`, which puts each file at its resource's place in the new
  descriptor's declaration order (place = order + 1, which is the `resourceIndex` the UI's picker
  writes) and rewrites the ids; a file whose id the module no longer declares is dropped and
  logged. A file with no saved id is taken as the resource the cached (previous) descriptor
  declares at its place. The executor sets config before it loads resources, so `WasmEffect`
  reads the ids in `SetConfig` and binds each slot by its saved id, falling back to declaration
  order. `resource.module.*` is refused (`kModuleResourceId`). Sharing strips the descriptor
  cache but not the ids, which are validated on read.
- **One parser.** C++ is the only parser. `wasmGuestDescriptor` holds the parsed descriptor as
  JSON (`{abi, name, category, description, thumbnail, params: [...], resources: [...]}`) rather
  than the raw entries, and `getNodeEffectInfo` in `presetV2.ts` maps it to an `EffectTypeInfo`
  instead of parsing `key=value` itself. A cache in the old array form is ignored and refreshed
  from the module. `sanitize.ts` and `PresetArchiveSupport.cpp` already strip the key on share
  and are unaffected. `CustomEffectLibrary`'s `descriptorSummary` and the designer's
  `descriptorText` only display text; check them, don't expect changes.
- **Guest memory** is looked up again on every `param_handle` and `resource_read` call, since
  `memory.grow` can move it.

## Tests

`core/tests/WasmEffectHostTests.cpp` is rewritten on v2 WAT fixtures:

- handles: a declared id resolves, an undeclared one returns -1, a hard-coded handle traps and
  the node bypasses with the handle in `lastError`;
- reordering params, or resources, in the descriptor keeps a preset's values, and files, by id;
- a host param id, a numeric id, a malformed id, `slot`, `id` and a 65th param are each refused,
  with a message naming the problem;
- a v1 module (imports `read_param`), one without `audiofx_abi_version`, one declaring ABI 3 and
  one importing anything outside `host` are refused before they run;
- `tempo_bpm` follows `bpm`; `resource_read` clamps at the end of the resource and at the end of
  guest memory, and handles an id-length past the limit;
- `SetParam` for an unknown id after load parks nothing.

A UI test covers `getNodeEffectInfo` from the parsed form, guest params listed before the host's.

## Phases

1. **Params.** Descriptor v2 parser, `param_handle` / `param_value` / `tempo_bpm`, the
   `audiofx_abi_version` check, the import check, removal of the v1 param path. Tests.
2. **Resources.** `resource_handle` / `resource_size` / `resource_read`, resources by id in
   presets and the reconcile.
3. **UI.** The parsed descriptor cache, the TS parser deleted.
4. **Docs and samples** (done 2026-11-17). `docs/wasm-module-authoring.md` is rewritten for the
   block ABI: the interface, how a block runs, the descriptor, the gain example in WAT, Rust,
   TinyGo and C, the AI prompt template and the migration guide. The WAT and C examples were
   built (wabt, clang) and run in the host; Rust and TinyGo were not, as neither toolchain was to
   hand. The phaser sample is ported (its `mix` param, a host id, is now `blend`) and renders bit
   for bit as before; `build.mjs` runs it like the host does. `tools/wasm-module-generator` and
   the private Designer's `module-generation.ts` wrap each per-sample kernel in a block loop that
   reads params once per block. `WasmEffectHostTests --check <module> [resources...]` runs any
   module through the real host, and a test loads the committed phaser.
5. **Block processing** (D1, taken 2026-11-17). `audiofx_buffer_ptr`, `audiofx_process(frames)`,
   unchecked calls and imports, export signatures checked at load. Tests: a long in-place host
   block, the per-sample process and bad buffer addresses refused. The private Emissary module is
   ported and renders bit for bit as before.

## Decisions

- **D1. Block processing in the same break? Taken (Chris, 2026-11-17).** The host called into the
  guest once per sample, and a module made a host call per param per sample on top. Breaking the
  ABI once is cheaper than twice, and ABI 2 has not shipped (1.7.1 predates it).
  `audiofx_process(frames)` over a guest-owned buffer removes both costs.
  - The buffer's address comes from `audiofx_buffer_ptr`, not from prepare, which keeps its status
    return.
  - Measured on 2026-11-17 (research `wasm-runtimes-2026-11-17` in the private repo): Wasmtime's
    checked `wasmtime_func_call` costs about 130–200 ns per call. That is 20–25% of an amp
    module's per-sample time and about five times all of a gain module's.
- **D2. Opaque handles** (a per-load base) as above, or dense `0..N-1`, which is easier to read
  in a debugger but lets hard-coded numbers keep working by accident. Recommendation: opaque.
- **D3. Limits**: 64 params and 16 resources.
- **D4. Resource ids in presets**, recommended. The alternative is to keep resources positional,
  which keeps item 4 of the problem list.

## Migration guide (draft for the authoring doc)

> **Moving a module from ABI 1 to ABI 2**
>
> 1. Export `audiofx_abi_version` returning `2`. A descriptor is now required (it can be empty).
> 2. Rename every `param.N.<field>` to `param.<id>.<field>`, using the old `param.N.id` value,
>    and delete the `id` and `slot` lines. Do the same for `resource.N.*`.
> 3. `audiofx_prepare` loses its third argument (`resourceSlotCount`).
> 4. In `audiofx_prepare`, call `param_handle` once for each id and keep the results in
>    globals. Return non-zero if any of them is -1.
> 5. Replace `read_param(n)` with `param_value(handle)`, and `read_param(8)` with `tempo_bpm()`.
> 6. Replace `read_resource_size(slot)` and a `read_resource_byte` loop with
>    `resource_handle("<id>")`, `resource_size(handle)` and one
>    `resource_read(handle, 0, dst, size)`.
> 7. Rename any `guitarfx_*` exports to `audiofx_*`.
>
> Saved presets keep their values for every param whose id you kept. Values saved under the old
> generic `param1..param32` keys are not carried over.
>
> ```wat
> ;; ABI 1
> (import "host" "read_param" (func $read_param (param i32) (result f32)))
> ...
> (call $read_param (i32.const 0))
>
> ;; ABI 2
> (import "host" "param_handle" (func $param_handle (param i32 i32) (result i32)))
> (import "host" "param_value" (func $param_value (param i32) (result f32)))
> (global $gain (mut i32) (i32.const -1))
> (data (i32.const 2048) "gain")
> ;; in audiofx_prepare:
> (global.set $gain (call $param_handle (i32.const 2048) (i32.const 4)))
> (if (i32.lt_s (global.get $gain) (i32.const 0)) (then (return (i32.const 1))))
> ;; in audiofx_process:
> (call $param_value (global.get $gain))
> ```

## Sources

- WCLAP: [Reusing the CLAP standard for Web Audio](https://ressources.ircam.fr/en/media/f35a4a8dd95eedc41f3b)
  (Web Audio Conference 2025), [wclap-bridge](https://github.com/WebCLAP/wclap-bridge),
  [web-clap draft](https://github.com/free-audio/web-clap),
  [clap-wrapper](https://github.com/defiantnerd/clap-wrapper),
  [char-clap-host](https://github.com/charCulbert/char-clap-host)
- Faust: [faustwasm](https://github.com/Fr0stbyteR/faustwasm)
- Cmajor: [patch format](https://cesaref.github.io/docs-dev/docs/PatchFormat), [examples](https://cmajor.dev/docs/Examples)
