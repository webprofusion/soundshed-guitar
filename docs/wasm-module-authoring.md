# WASM Module Authoring Guide

This document describes how to create a guest WebAssembly module that works with the AudioFX WASM host in this repo.

The target audience is another AI or code generator that needs to emit new modules dynamically.

## Assumptions

- New modules should use the current `audiofx_*` export names.
- The host still accepts legacy `guitarfx_*` names for backward compatibility, but new modules should not emit them unless explicitly required.
- The examples below all implement the same basic module: a stereo gain effect with one self-described parameter named `gain`.
- The WAT example is the authoritative exact ABI reference.
- Higher-level language examples are source templates. After compilation, the generated `.wasm` must still expose the exact import/export signatures listed below.

## Exact ABI

The guest module should export these required functions:

- `audiofx_prepare(sampleRate: f32, maxBlockSize: i32, resourceSlotCount: i32) -> i32`
- `audiofx_reset() -> void`
- `audiofx_process(inLeft: f32, inRight: f32) -> (f32, f32)`

The guest module may also export:

- `audiofx_get_latency_samples() -> i32`

If the module self-describes via an embedded descriptor blob, it must also export:

- `memory`
- `audiofx_descriptor_ptr() -> i32`
- `audiofx_descriptor_len() -> i32`

The host provides these imports from module `host`:

- `read_param(index: i32) -> f32`
- `read_resource_size(slot: i32) -> i32`
- `read_resource_byte(slot: i32, offset: i32) -> i32`

## Runtime Semantics

- Processing is sample-by-sample, stereo in and stereo out.
- If the upstream source is effectively mono, the host mirrors the one live channel to both guest inputs before calling `audiofx_process`.
- `audiofx_prepare` must return `0` on success. Any non-zero return is treated as failure.
- `audiofx_get_latency_samples` is optional. If present, it can return `0` for zero-latency modules.
- Parameter slots are fixed:
  - `0..7` => `param1..param8`
  - `8` => `bpm`
- Resource slots are fixed:
  - `0` => the `.wasm` module itself
  - `1..N` => optional external binary resources

## Self-Description Format

The descriptor is UTF-8 text stored in guest memory.

- One `key=value` entry per line.
- Unknown keys are ignored.
- Comments and blank lines are allowed only if the parser can skip them. The current host ignores blank lines and lines starting with `#`.

Supported effect-level keys:

- `effect.name`
- `effect.title`
- `effect.version`
- `effect.category`
- `effect.description`
- `effect.thumbnailBase64`
- `effect.thumbnailMimeType`
- `effect.thumbnailDataUrl`

Supported parameter keys:

- `param.N.id`
- `param.N.title`
- `param.N.name`
- `param.N.slot`
- `param.N.default`
- `param.N.min`
- `param.N.max`
- `param.N.unit`
- `param.N.group`
- `param.N.advanced`
- `param.N.step`
- `param.N.labels`

Supported resource keys:

- `resource.N.id`
- `resource.N.title`
- `resource.N.name`
- `resource.N.slot`
- `resource.N.type`
- `resource.N.allowBrowseFile`
- `resource.N.parameterId`
- `resource.N.parameterValue`

Notes:

- Prefer `effect.thumbnailBase64` plus `effect.thumbnailMimeType` over `effect.thumbnailDataUrl` if generating from raw bytes.
- `effect.version` is an opaque module author version string. The host does not interpret its format, so semantic versions like `1.4.2` are recommended.
- Parameter slot numbers must stay in the `0..7` range.
- Resource slot `0` is reserved for the module itself and must not be used for `resource.N.slot`.

## Shared Example: Simple Gain Module

Behavior:

- Read parameter slot `0` with `host.read_param(0)`.
- Multiply both channels by that gain.
- Self-describe as a utility effect named `AI Gain`.

Shared descriptor blob:

```text
effect.name=AI Gain
effect.version=1.0.0
effect.category=utility
effect.description=Simple stereo gain module generated for the AudioFX WASM host.
param.0.id=gain
param.0.title=Gain
param.0.slot=0
param.0.default=0.5
param.0.min=0.0
param.0.max=2.0
param.0.unit=amount
```

Example graph node JSON for this module:

```json
{
  "type": "wasm_host",
  "category": "utility",
  "label": "AI Gain",
  "params": {
    "gain": 0.5
  },
  "resources": [
    {
      "resourceType": "wasm",
      "filePath": "modules/ai_gain.wasm"
    }
  ]
}
```

## Example 1: Pure WASM / WAT

This is the most exact and reliable form because it directly describes the final WebAssembly module.

```wat
(module
  (type $read_param_t (func (param i32) (result f32)))
  (type $prepare_t (func (param f32 i32 i32) (result i32)))
  (type $reset_t (func))
  (type $process_t (func (param f32 f32) (result f32 f32)))
  (type $latency_t (func (result i32)))

  (import "host" "read_param" (func $read_param (type $read_param_t)))

  (memory (export "memory") 1)

  ;; Replace DESCRIPTOR_BYTES with the exact UTF-8 descriptor string.
  (data (i32.const 0)
    "effect.name=AI Gain\n"
    "effect.version=1.0.0\n"
    "effect.category=utility\n"
    "effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n"
    "param.0.id=gain\n"
    "param.0.title=Gain\n"
    "param.0.slot=0\n"
    "param.0.default=0.5\n"
    "param.0.min=0.0\n"
    "param.0.max=2.0\n"
    "param.0.unit=amount\n")

  (func (export "audiofx_prepare") (type $prepare_t)
    (param $sampleRate f32) (param $maxBlockSize i32) (param $resourceSlotCount i32)
    i32.const 0)

  (func (export "audiofx_reset") (type $reset_t))

  (func (export "audiofx_process") (type $process_t)
    (param $inLeft f32) (param $inRight f32)
    (local $gain f32)

    i32.const 0
    call $read_param
    local.set $gain

    local.get $inLeft
    local.get $gain
    f32.mul

    local.get $inRight
    local.get $gain
    f32.mul)

  (func (export "audiofx_get_latency_samples") (type $latency_t)
    i32.const 0)

  (func (export "audiofx_descriptor_ptr") (type $latency_t)
    i32.const 0)

  ;; Replace 214 with the exact byte length of the descriptor blob above.
  (func (export "audiofx_descriptor_len") (type $latency_t)
    i32.const 214))
```

## Example 2: Rust Template

Use this only if the toolchain preserves the exact Wasm ABI above.

Important:

- Verify that `audiofx_process` compiles to a WebAssembly function with signature `(f32, f32) -> (f32, f32)`.
- Verify that linear memory is exported as `memory`.
- If the compiled module does not match the exact ABI, fall back to WAT.

```rust
#![no_std]

#[link(wasm_import_module = "host")]
extern "C" {
    fn read_param(index: i32) -> f32;
}

static DESCRIPTOR: &[u8] = b"effect.name=AI Gain\n\
effect.version=1.0.0\n\
effect.category=utility\n\
effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n\
param.0.id=gain\n\
param.0.title=Gain\n\
param.0.slot=0\n\
param.0.default=0.5\n\
param.0.min=0.0\n\
param.0.max=2.0\n\
param.0.unit=amount\n";

#[export_name = "audiofx_prepare"]
pub extern "C" fn audiofx_prepare(_sample_rate: f32, _max_block_size: i32, _resource_slot_count: i32) -> i32 {
    0
}

#[export_name = "audiofx_reset"]
pub extern "C" fn audiofx_reset() {}

#[export_name = "audiofx_process"]
pub extern "C" fn audiofx_process(in_left: f32, in_right: f32) -> (f32, f32) {
    let gain = unsafe { read_param(0) };
    (in_left * gain, in_right * gain)
}

#[export_name = "audiofx_get_latency_samples"]
pub extern "C" fn audiofx_get_latency_samples() -> i32 {
    0
}

#[export_name = "audiofx_descriptor_ptr"]
pub extern "C" fn audiofx_descriptor_ptr() -> i32 {
    DESCRIPTOR.as_ptr() as usize as i32
}

#[export_name = "audiofx_descriptor_len"]
pub extern "C" fn audiofx_descriptor_len() -> i32 {
    DESCRIPTOR.len() as i32
}
```

## Example 3: TinyGo Template

Use this only if the toolchain preserves the exact Wasm ABI above.

Important:

- Verify the final exported process signature.
- Verify that the module exports linear memory as `memory`.
- If the compiled module does not match the exact ABI, fall back to WAT.

```go
package main

import "unsafe"

//go:wasmimport host read_param
func read_param(index int32) float32

var descriptor = []byte("" +
    "effect.name=AI Gain\n" +
  "effect.version=1.0.0\n" +
    "effect.category=utility\n" +
    "effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n" +
    "param.0.id=gain\n" +
    "param.0.title=Gain\n" +
    "param.0.slot=0\n" +
    "param.0.default=0.5\n" +
    "param.0.min=0.0\n" +
    "param.0.max=2.0\n" +
    "param.0.unit=amount\n")

//export audiofx_prepare
func audiofx_prepare(sampleRate float32, maxBlockSize int32, resourceSlotCount int32) int32 {
    return 0
}

//export audiofx_reset
func audiofx_reset() {}

//export audiofx_process
func audiofx_process(inLeft float32, inRight float32) (float32, float32) {
    gain := read_param(0)
    return inLeft * gain, inRight * gain
}

//export audiofx_get_latency_samples
func audiofx_get_latency_samples() int32 {
    return 0
}

//export audiofx_descriptor_ptr
func audiofx_descriptor_ptr() int32 {
    return int32(uintptr(unsafe.Pointer(&descriptor[0])))
}

//export audiofx_descriptor_len
func audiofx_descriptor_len() int32 {
    return int32(len(descriptor))
}

func main() {}
```

## Example 4: C / Clang Template

This is the least reliable option for this ABI because many C toolchains lower multi-value returns differently.

Use it only if you can inspect the final `.wasm` and confirm that:

- `audiofx_process` exports as `(f32, f32) -> (f32, f32)`
- the module exports `memory`
- the descriptor pointer and length exports are plain `() -> i32`

If not, fall back to WAT.

```c
#include <stdint.h>

__attribute__((import_module("host"), import_name("read_param")))
extern float read_param(int32_t index);

static const unsigned char descriptor[] =
  "effect.name=AI Gain\n"
  "effect.version=1.0.0\n"
  "effect.category=utility\n"
  "effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n"
  "param.0.id=gain\n"
  "param.0.title=Gain\n"
  "param.0.slot=0\n"
  "param.0.default=0.5\n"
  "param.0.min=0.0\n"
  "param.0.max=2.0\n"
  "param.0.unit=amount\n";

struct stereo_pair {
  float left;
  float right;
};

__attribute__((export_name("audiofx_prepare")))
int32_t audiofx_prepare(float sample_rate, int32_t max_block_size, int32_t resource_slot_count) {
  return 0;
}

__attribute__((export_name("audiofx_reset")))
void audiofx_reset(void) {}

__attribute__((export_name("audiofx_process")))
struct stereo_pair audiofx_process(float in_left, float in_right) {
  float gain = read_param(0);
  struct stereo_pair out = { in_left * gain, in_right * gain };
  return out;
}

__attribute__((export_name("audiofx_get_latency_samples")))
int32_t audiofx_get_latency_samples(void) {
  return 0;
}

__attribute__((export_name("audiofx_descriptor_ptr")))
int32_t audiofx_descriptor_ptr(void) {
  return (int32_t)(uintptr_t)descriptor;
}

__attribute__((export_name("audiofx_descriptor_len")))
int32_t audiofx_descriptor_len(void) {
  return (int32_t)(sizeof(descriptor) - 1);
}
```

## Optional Thumbnail Example

To self-describe a thumbnail, add either:

- `effect.thumbnailBase64=<raw base64 bytes>` plus `effect.thumbnailMimeType=image/png`

or:

- `effect.thumbnailDataUrl=data:image/png;base64,...`

Example:

```text
effect.thumbnailBase64=iVBORw0KGgoAAAANSUhEUgAA...
effect.thumbnailMimeType=image/png
```

## Optional Resource Descriptor Example

If the module expects a binary resource in slot `1`, add lines like:

```text
resource.0.id=impulse
resource.0.title=Impulse Response
resource.0.slot=1
resource.0.type=ir
resource.0.allowBrowseFile=true
```

## Validation Checklist

Before accepting a generated module, verify all of the following:

1. The module exports `audiofx_prepare`, `audiofx_reset`, `audiofx_process`, and optionally `audiofx_get_latency_samples`.
2. If the module exports descriptor pointer/length, it also exports `memory`.
3. `audiofx_process` has the exact signature `(f32, f32) -> (f32, f32)`.
4. `audiofx_prepare` returns `0` on success.
5. Descriptor byte length matches the actual descriptor blob size exactly.
6. Parameter slot values stay within `0..7`.
7. Resource descriptors never use slot `0`.
8. Descriptor keys use UTF-8 text with one `key=value` entry per line.
9. The module does not emit `NaN` or `Inf` outputs during normal processing.
10. New modules prefer `audiofx_*` names, not `guitarfx_*`.

## AI Prompt Hints

If another AI is generating a new module, provide these constraints explicitly:

- Generate a WebAssembly module for the AudioFX host.
- Use `audiofx_*` export names.
- Import host functions from module `host`.
- If self-description is required, export `memory`, `audiofx_descriptor_ptr`, and `audiofx_descriptor_len`.
- Embed descriptor metadata for effect title, category, description, parameter definitions, and optional thumbnail.
- Keep parameter IDs stable and human-readable.
- If the chosen source language cannot emit the exact process signature, generate WAT instead.

## Copy-Paste AI Prompt Template

Use the following prompt as a starting point when asking another AI to generate a new module.

Replace every `<...>` placeholder before sending it.

```text
Generate a single guest WebAssembly module for the AudioFX WASM host used by the Soundshed project.

You must follow this ABI exactly:

- required exports:
  - audiofx_prepare(sampleRate: f32, maxBlockSize: i32, resourceSlotCount: i32) -> i32
  - audiofx_reset() -> void
  - audiofx_process(inLeft: f32, inRight: f32) -> (f32, f32)
- optional export:
  - audiofx_get_latency_samples() -> i32
- if the module self-describes, also export:
  - memory
  - audiofx_descriptor_ptr() -> i32
  - audiofx_descriptor_len() -> i32

The host imports functions from module "host":

- read_param(index: i32) -> f32
- read_resource_size(slot: i32) -> i32
- read_resource_byte(slot: i32, offset: i32) -> i32

Host rules:

- process audio sample-by-sample
- stereo input and stereo output
- if only one input channel exists, the host mirrors it to both inputs before calling audiofx_process
- audiofx_prepare must return 0 on success
- parameter slots 0..7 map to param1..param8
- parameter slot 8 is bpm
- resource slot 0 is reserved for the wasm module itself
- external resources must use slots 1..N

Generate the module in this source language:

- preferred source language: <WAT | Rust | TinyGo | C | raw wasm bytes>
- if that language cannot reliably emit the exact ABI above, fall back to WAT instead of approximating

Module specification:

- module id: <module_id>
- file name: <module_file_name.wasm>
- effect title: <human_readable_title>
- category: <utility | spatial | drive | modulation | dynamics | eq | delay | reverb | other>
- description: <one concise paragraph>
- latency samples: <integer or 0>
- thumbnail: <none | base64 png string | data url>

DSP behavior:

- <behavior rule 1>
- <behavior rule 2>
- <behavior rule 3>

Parameters:

- param 0:
  - id: <id>
  - title: <title>
  - default: <number>
  - min: <number>
  - max: <number>
  - unit: <unit>
  - group: <group or empty>
  - advanced: <true|false>
  - step: <number>
  - labels: <none or pipe-separated labels>
- param 1:
  - id: <id>
  - title: <title>
  - default: <number>
  - min: <number>
  - max: <number>
  - unit: <unit>
  - group: <group or empty>
  - advanced: <true|false>
  - step: <number>
  - labels: <none or pipe-separated labels>
- continue only for the parameters this module actually needs

Resources:

- resource 0:
  - id: <id>
  - title: <title>
  - slot: <1 or higher>
  - type: <blob | ir | model | other>
  - allowBrowseFile: <true|false>
  - parameterId: <id or empty>
  - parameterValue: <number or empty>
- if the module does not need external resources, say "no external resources"

Self-description requirements:

- embed a UTF-8 descriptor blob in guest memory
- use one key=value entry per line
- include at least:
  - effect.name
  - effect.category
  - effect.description
  - one param.N block for each exposed parameter
- if a thumbnail is provided, prefer effect.thumbnailBase64 plus effect.thumbnailMimeType
- descriptor_len must exactly match the descriptor byte length

Output format:

Return your answer in these sections in order:

1. Summary
2. Source Code
3. Descriptor Blob
4. Expected Imports and Exports
5. Example GraphNode JSON
6. Validation Notes

Validation requirements:

- do not use guitarfx_* names unless I explicitly ask for legacy compatibility
- do not invent extra host imports
- do not use param slots outside 0..7 for normal parameters
- do not use resource slot 0 for external resources
- do not return pseudocode
- if the requested source language cannot preserve the exact wasm ABI, produce WAT instead
```

### Filled Example Prompt

```text
Generate a single guest WebAssembly module for the AudioFX WASM host used by the Soundshed project.

Use WAT as the source language.

Create a module with:

- module id: tremolo_pan
- file name: tremolo_pan.wasm
- effect title: Tremolo Pan
- category: modulation
- description: Stereo tremolo and pan utility with one rate control and one depth control.
- latency samples: 0
- thumbnail: none

DSP behavior:

- apply slow amplitude modulation to both channels
- apply a left-right pan motion derived from the same oscillator
- keep output stable and bounded for normal parameter ranges

Parameters:

- param 0:
  - id: rate
  - title: Rate
  - default: 0.25
  - min: 0.0
  - max: 1.0
  - unit: amount
  - group: modulation
  - advanced: false
  - step: 0.01
  - labels: none
- param 1:
  - id: depth
  - title: Depth
  - default: 0.5
  - min: 0.0
  - max: 1.0
  - unit: amount
  - group: modulation
  - advanced: false
  - step: 0.01
  - labels: none

Resources:

- no external resources

Return your answer in these sections in order:

1. Summary
2. Source Code
3. Descriptor Blob
4. Expected Imports and Exports
5. Example GraphNode JSON
6. Validation Notes
```

## Recommendation

If correctness matters more than convenience, generate WAT or raw WASM sections directly.

Use higher-level languages only when the toolchain is known to preserve the exact ABI and the final module is validated after compilation.