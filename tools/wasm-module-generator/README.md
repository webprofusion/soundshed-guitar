# Soundshed WASM Module Generator

Standalone CLI for generating a small set of simple WebAssembly guest modules that match the Soundshed generic WASM host ABI.

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
- parameter descriptions and default values
- optional resource slot metadata
- an `exampleGraphNode` object for each module showing how to wire it into the `wasm_host` effect

## Included Modules

- `gain`: multiplies both channels by `param1`
- `stereo_average`: averages left/right to mono and writes the same signal to both channels, scaled by `param1`
- `channel_swap`: swaps the left and right channels
- `hard_clip`: clips both channels to `+/- param1` with a minimum threshold floor
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

Supported host imports:

- `host.read_param(index: i32) -> f32`
- `host.read_resource_size(slot: i32) -> i32`
- `host.read_resource_byte(slot: i32, offset: i32) -> i32`

Parameter index mapping:

- `0..7` => `param1..param8`
- `8` => `bpm`

Resource slot mapping:

- slot `0` => the `.wasm` module itself
- slot `1..N` => optional binary resource blobs

For optional resources, the manifest's `exampleGraphNode.resources[*].metadata.resourceSlotIndex` field matches what the current host expects.