# Soundshed WASM Module Generator

Standalone CLI for generating a small set of simple WebAssembly guest modules for Custom Effects (Custom Effect ABI 2).

For an AI-oriented authoring guide with the same basic example shown in multiple source languages, see [docs/wasm-module-authoring.md](../../docs/wasm-module-authoring.md).

Generated modules are self-contained by default: each `.wasm` file can embed its own descriptor blob and any guest-side constant data it needs. Only the explicit `resource_*` examples require extra external blobs.

The generator does not require `clang`, `wabt`, `emscripten`, or any other external WASM toolchain. It writes raw `.wasm` binaries directly, mirroring the approach already used by the core WASM host tests.

## Usage

From the repo root:

```powershell
python tools/wasm-module-generator/generate_wasm_modules.py
```

Generate into a custom directory:

```powershell
python tools/wasm-module-generator/generate_wasm_modules.py --output C:\temp\soundshed-wasm-modules
```

Generate only a subset:

```powershell
python tools/wasm-module-generator/generate_wasm_modules.py --only gain hard_clip bpm_ducker
```

List available module ids:

```powershell
python tools/wasm-module-generator/generate_wasm_modules.py --list
```

## Output

Default output root: `tools/wasm-module-generator/output/`

Generated layout:

- `modules/*.wasm`
- `resources/*` for resource-backed examples
- `manifest.json`

The manifest includes:

- the host ABI summary
- a list of generated modules
- parameter metadata including id, title, default, range, unit, group, advanced flag, step and labels
- optional resource metadata
- an `exampleGraphNode` object for each module showing how to wire it into the `wasm_host` effect

## Included Modules

- `gain`: multiplies both channels by a self-described `gain` parameter
- `stereo_spatial`: stereo spatial utility where self-described `depth` moves the image back/forward and self-described `pan` moves it left/right
- `stereo_spatial_random`: stereo spatial utility with base `depth`/`pan` plus `randomMode`, `speed`, and `amount` for smooth wandering movement
- `stereo_average`: averages left/right to mono and writes the same signal to both channels, scaled by a self-described `gain` parameter
- `channel_swap`: swaps the left and right channels
- `hard_clip`: clips both channels to `+/- threshold` with a minimum threshold floor
- `bpm_ducker`: scales level by `120 / bpm`, reading the host tempo with `tempo_bpm`
- `stateful_bias`: adds a tiny cumulative bias each sample to demonstrate state and reset behavior
- `resource_size_gain`: scales both channels from the byte size of its `payload` resource
- `resource_scaler`: scales both channels from the first byte of its `scale` resource

## Host ABI

The modules use Custom Effect ABI 2. [docs/wasm-module-authoring.md](../../docs/wasm-module-authoring.md) is the reference; in short:

Guest exports:

- `audiofx_abi_version() -> i32`, returning `2`
- `audiofx_prepare(sampleRate: f32, maxBlockSize: i32) -> i32`, `0` on success
- `audiofx_buffer_ptr() -> i32`: the in-place I/O buffer in `memory`, `maxBlockSize` left samples then as many right
- `audiofx_process(frames: i32) -> void`: processes `frames` samples in place in the I/O buffer
- `audiofx_reset() -> void`
- `audiofx_get_latency_samples() -> i32`
- `memory`, `audiofx_descriptor_ptr() -> i32` and `audiofx_descriptor_len() -> i32`

Host imports, from module `host`:

- `param_handle(idPtr: i32, idLen: i32) -> i32` and `param_value(handle: i32) -> f32`
- `tempo_bpm() -> f32`
- `resource_handle(idPtr: i32, idLen: i32) -> i32`, `resource_size(handle: i32) -> i32` and `resource_read(handle: i32, offset: i32, dstPtr: i32, len: i32) -> i32`

The descriptor keys params and resources by id: `param.<id>.<field>`, `resource.<id>.<field>`. Ids are a letter followed by up to 31 letters, digits or underscores, and a param may not take one of the host's own ids (`mix`, `inputGainDb`, `outputGainDb`, `bpm`). The generator checks both.

## How the Generated Modules Work

Every module is the same wrapper around a small per-sample kernel, `(inLeft, inRight) -> (outLeft, outRight)`:

- All six host imports are imported, in a fixed order, so function indices never move.
- `audiofx_prepare` refuses a `maxBlockSize` below 1 or above 2^20 (status 2), grows memory to hold the I/O buffer (status 3 if it can't), and looks up the handle for every param and resource id (status 1 for an id the descriptor doesn't declare). A module's own prepare code, such as reading a resource, runs after that.
- `audiofx_process` reads every param once, and the tempo, at the start of each block, then runs the kernel over each frame of the buffer in place.

Guest memory: param handles at 0, the param values read for this block at 256, the tempo at 512, `maxBlockSize` at 516, a 16-byte resource scratch area at 528, resource handles at 544, the id strings at 1024, the descriptor at 4096, and the I/O buffer on the first page boundary after the descriptor.

Mono input: if the host has only one live input channel for a node, it mirrors that signal to both channels of the buffer, so stereo modules such as `stereo_spatial` behave sensibly on mono sources.

For the generated `stereo_spatial` module:

- `depth`: `-1.0` = back, `0.0` = centered, `1.0` = forward
- `pan`: `-1.0` = left, `0.0` = centered, `1.0` = right

For the generated `stereo_spatial_random` module:

- `depth` and `pan` define the base center that the random motion wanders around
- `randomMode`: `0` = off, `1` = on
- `speed`: controls how quickly the motion glides toward each new random target
- `amount`: controls how far the motion can move away from the base depth and pan
- the motion is continuous and slewed; it does not jump directly between random targets

## Resources

`node.resources[0]` is the module itself. Each further node resource is one of the module's declared resources, and the node's `wasmResourceIds` config (a JSON array, `""` for the module) says which, so a file stays with its resource if a later version of the module declares them in another order. Without it, the files are taken in declaration order. The manifest's `exampleGraphNode` shows both for the resource examples.
