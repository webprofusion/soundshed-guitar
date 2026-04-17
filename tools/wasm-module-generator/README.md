# Soundshed WASM Module Generator

Standalone CLI for generating a small set of simple WebAssembly guest modules that match the Soundshed generic WASM host ABI.

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
- parameter metadata including id, title, default, range, unit, group, advanced flag, step, labels, and guest slot
- optional resource slot metadata
- an `exampleGraphNode` object for each module showing how to wire it into the `wasm_host` effect

## Included Modules

- `gain`: multiplies both channels by a self-described `gain` parameter
- `stereo_spatial`: stereo spatial utility where self-described `depth` moves the image back/forward and self-described `pan` moves it left/right
- `stereo_average`: averages left/right to mono and writes the same signal to both channels, scaled by a self-described `gain` parameter
- `channel_swap`: swaps the left and right channels
- `hard_clip`: clips both channels to `+/- threshold` with a minimum threshold floor
- `bpm_ducker`: scales level by `120 / bpm` using the guest tempo slot
- `stateful_bias`: adds a tiny cumulative bias each sample to demonstrate state and reset behavior
- `resource_size_gain`: scales both channels from the byte size of an external resource blob in slot `1`
- `resource_scaler`: scales both channels from the first byte of an external resource blob in slot `1`

## Host ABI

Expected guest exports:

- `guitarfx_prepare(sampleRate: f32, maxBlockSize: i32, resourceSlotCount: i32) -> i32`
- `guitarfx_reset() -> void`
- `guitarfx_process(inLeft: f32, inRight: f32) -> (f32, f32)`
- `guitarfx_get_latency_samples() -> i32`
- `memory` (optional, required when exporting a descriptor blob)
- `guitarfx_descriptor_ptr() -> i32` (optional descriptor blob offset)
- `guitarfx_descriptor_len() -> i32` (optional descriptor blob byte length)

Supported host imports:

- `host.read_param(index: i32) -> f32`
- `host.read_resource_size(slot: i32) -> i32`
- `host.read_resource_byte(slot: i32, offset: i32) -> i32`

Parameter index mapping:

- `0..7` => `param1..param8`
- `8` => `bpm`

Descriptor blob format:

- UTF-8 text stored in guest memory
- one `key=value` entry per line
- extensible by convention; unknown keys are ignored by the current host
- current keys include:
	- `effect.name`, `effect.category`, `effect.description`
	- `param.N.id`, `param.N.title`, `param.N.slot`, `param.N.default`, `param.N.min`, `param.N.max`, `param.N.unit`, `param.N.group`, `param.N.advanced`, `param.N.step`, `param.N.labels`
	- `resource.N.id`, `resource.N.title`, `resource.N.slot`, `resource.N.type`, `resource.N.allowBrowseFile`, `resource.N.parameterId`, `resource.N.parameterValue`

For the generated `stereo_spatial` module:

- `depth`: `-1.0` = back, `0.0` = centered, `1.0` = forward
- `pan`: `-1.0` = left, `0.0` = centered, `1.0` = right

Resource slot mapping:

- slot `0` => the `.wasm` module itself
- slot `1..N` => optional binary resource blobs

For optional resources, the manifest's `exampleGraphNode.resources[*].metadata.resourceSlotIndex` field matches what the current host expects.