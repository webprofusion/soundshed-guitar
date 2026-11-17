# Custom Effect authoring guide

A Custom Effect is a WebAssembly module that the `wasm_host` effect runs on the audio thread. This
guide describes the interface such a module must have, **Custom Effect ABI 2**, and how to build
one by hand, from a compiler or from an AI.

The intended readers are people and code generators that emit modules, including another AI. The WAT
example is the exact reference. The Rust, TinyGo and C examples are source templates. Whatever the
language, check the compiled `.wasm` against [Checking a module](#checking-a-module) before you
trust it.

Modules built for ABI 1 (`read_param(index)`, numbered `param.N` blocks, one sample per call) are
refused. See [Moving a module from ABI 1 to ABI 2](#moving-a-module-from-abi-1-to-abi-2).

## The interface

### Exports

| Export | Signature | |
| --- | --- | --- |
| `audiofx_abi_version` | `() -> i32` | Required. Returns `2`. |
| `memory` | memory | Required. |
| `audiofx_descriptor_ptr` | `() -> i32` | Required. The descriptor's address in `memory`. |
| `audiofx_descriptor_len` | `() -> i32` | Required. The descriptor's exact length in bytes. |
| `audiofx_prepare` | `(sampleRate: f32, maxBlockSize: i32) -> i32` | Required. `0` on success; anything else and the module doesn't run. |
| `audiofx_buffer_ptr` | `() -> i32` | Required. The address of the I/O buffer in `memory`. |
| `audiofx_process` | `(frames: i32) -> ()` | Required. Processes `frames` samples in place in the I/O buffer. |
| `audiofx_reset` | `() -> ()` | Required. Clears state such as filter memory and delay lines. |
| `audiofx_get_latency_samples` | `() -> i32` | Optional. The latency the module adds, for the host to compensate. |

Every export must have exactly this signature. The host checks each one when the module loads and
refuses a module with the wrong shape, giving the expected signature and the one it found.

### Imports

The host provides these from module `host`. Import only the ones you use. A module that imports
anything else, from `host` or any other module (including WASI), is refused.

| Import | Signature | |
| --- | --- | --- |
| `param_handle` | `(idPtr: i32, idLen: i32) -> i32` | The handle for the param whose id is the `idLen` bytes at `idPtr`, or `-1` if the descriptor doesn't declare it. |
| `param_value` | `(handle: i32) -> f32` | The param's current value. |
| `tempo_bpm` | `() -> f32` | The host tempo in beats per minute. |
| `resource_handle` | `(idPtr: i32, idLen: i32) -> i32` | As `param_handle`, for a resource the descriptor declares. |
| `resource_size` | `(handle: i32) -> i32` | The resource's size in bytes, `0` when no file is chosen. |
| `resource_read` | `(handle: i32, offset: i32, dstPtr: i32, len: i32) -> i32` | Copies up to `len` bytes of the resource, from `offset`, to `dstPtr` in `memory`. Returns the number copied: it stops at the end of the resource and of memory, and copies nothing for a negative argument. |

**Handles are opaque.** Look each one up once, in `audiofx_prepare`, keep it, and use what the host
returned. The numbers start from a base the host picks afresh at every load, so a module that
hard-codes `0, 1, 2` instead of asking breaks on its first run. Calling `param_value`,
`resource_size` or `resource_read` with a handle the host never gave traps, and the effect passes
audio through dry until it is loaded again.

### How a block runs

1. **Load.** The host checks the exports and imports, instantiates the module, calls
   `audiofx_abi_version`, and reads the descriptor.
2. **Prepare.** The host calls `audiofx_prepare(sampleRate, maxBlockSize)` off the audio thread,
   then `audiofx_buffer_ptr()` once. It calls both again whenever the sample rate or block size
   changes, and when the module or a resource changes. Look up your handles, read your resources
   and size your buffer here.
3. **Process.** For each block, on the audio thread, the host writes the input into the I/O buffer,
   calls `audiofx_process(frames)`, and reads the output from the same place. `frames` is between 1
   and `maxBlockSize`. A host block longer than that goes through in pieces.
4. **Reset.** The host may call `audiofx_reset`, from the audio thread, when playback restarts.

The I/O buffer is `maxBlockSize` left samples (f32) followed by `maxBlockSize` right samples:

```text
audiofx_buffer_ptr()                    + 4 * maxBlockSize
|  L[0] L[1] ... L[frames-1] ...  unused |  R[0] R[1] ... R[frames-1] ...  unused |
```

- It must be 4-byte aligned and lie inside `memory`, or the module is refused with the address it
  returned. Memory never shrinks, so once it fits it keeps fitting.
- It is stereo in and stereo out. If the node has only one live input channel, the host copies it
  to both channels of the buffer first.
- `maxBlockSize` is the host's largest block. It is usually between 32 and 4096 frames, but an
  offline render can ask for more. Either grow `memory` to fit, as the WAT example does, or size a
  static buffer generously (the C example takes 16384 frames) and return a non-zero status for
  anything bigger.

What the host does around the module:

- **Input gain** (`inputGainDb`) is applied before the samples reach the buffer. **Mix**
  (`mix`, dry/wet) and **output gain** (`outputGainDb`) are applied afterwards. Those three, and
  `bpm`, are the host's own params on every Custom Effect node: don't declare them, and don't
  implement your own dry/wet called `mix`.
- **Param values are not clamped.** `param_value` returns whatever was set, which a preset,
  automation or MIDI can put outside your declared range. Clamp what you read.
- **Read params once per block**, at the start of `audiofx_process`. Each import call costs about as
  much as a few dozen multiplies, and the value doesn't change within a block.
- **A non-finite output sample** (NaN or infinity) is replaced by the input sample the module was
  given. Don't rely on it: a module that produces them is broken.

### Limits and trust

A module is code from whoever wrote it, so the host bounds what it can do. It runs in Wasmtime with
only the `host` imports above: no WASI, no files, no network. On top of that:

- **Time.** Every call has a deadline. `audiofx_process` gets, per host block, four times the
  block's duration or 200 ms, whichever is longer. Instantiation (the start function), the
  descriptor exports, `audiofx_prepare` and `audiofx_get_latency_samples` get 5 s. `audiofx_reset`
  gets 200 ms, because it can run on the audio thread. A module that runs past its deadline traps,
  and the node passes audio through dry until it is loaded again.
- **Memory.** 256 MiB of linear memory, one memory, and up to 100,000 table elements. A module that
  declares more fails to instantiate; `memory.grow` past the limit returns `-1`. A resource file
  larger than guest memory can hold is refused at load.
- **Stack.** 256 KiB of wasm stack. Deep recursion traps instead of overflowing the audio thread's
  own stack.
- **Size.** The module file is at most 32 MiB and the descriptor at most 2 MiB. Each descriptor
  value is cut at 1,000 characters.
- **Features.** Threads, shared memory, GC, exceptions, stack switching and memory64 are off. SIMD,
  relaxed SIMD, bulk memory, multi-value, reference types and tail calls are on.
- **What the descriptor can say.** `effect.category` must be 1-40 letters, digits, spaces, `-` or
  `_`, or it becomes `utility`. A thumbnail must be a base64 PNG, JPEG, GIF or WebP: anything else,
  an `https:` URL or SVG included, is dropped.

Where modules come from:

- **Custom Effects must be on** (Settings → Features → Power Features). While they are off, the
  engine compiles and runs no module, whatever a preset or project asks for.
- **Shared files don't install modules, for now.** Preset archives, Tone Sharing items and packs
  install only NAM models and IRs. A Custom Effect node in a shared preset arrives without its
  module and passes audio through until you pick one of your own. Until module sharing is supported
  (it is planned, with a consent step; see `docs/wasm-effects-security-review-2026-10-19.md`), share
  the `.wasm` file itself; the person installing it browses to it on purpose.
- **No network shares.** A module path on a network share (`\\server\share\...`) is refused before
  it is opened.
- A DAW project cannot switch Custom Effects on: the setting is not restored from projects.

## The descriptor

The descriptor is UTF-8 text in `memory`, one `key=value` per line. Blank lines and lines starting
with `#` are skipped, and unknown keys are ignored.

```text
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
resource.impulse.title=Impulse
resource.impulse.type=ir
```

**Effect keys:** `effect.name` (or `effect.title`), `effect.version`, `effect.category`,
`effect.description`, `effect.thumbnailBase64` with `effect.thumbnailMimeType`, or
`effect.thumbnailDataUrl`. `effect.version` is your own version string; the host doesn't interpret
it, so a semantic version such as `1.4.2` is a good choice.

**Params** are keyed by id: `param.<id>.<field>`.

- An id is a letter followed by up to 31 letters, digits or underscores, and it is what presets,
  automation and MIDI mappings save the value under. Keep it the same in every version of your
  module, and saved values carry over.
- The order an id first appears in is its order in the UI. Nothing else depends on order: you can
  reorder or insert params in a new version without breaking anything.
- An id may not be one of the host's own params: `mix`, `inputGainDb`, `outputGainDb`, `bpm`.
- Fields: `title` (or `name`), `default`, `min`, `max`, `unit`, `group`, `advanced` (`true` or
  `false`), `step`, and `labels` (`|`-separated, for a stepped choice: `0` is the first label).
  A param with labels is shown as a selector, with `unit` defaulting to `enum` and `step` to `1`.
- At most 64 params.

**Resources** are files the user picks for your module, such as an impulse response or a table of
data: `resource.<id>.<field>`.

- Ids follow the same rules as params, and `module` is taken (the module is the node's own first
  resource).
- Fields: `title` (or `name`), `type` (`ir`, `model` or `blob`, the default), `allowBrowseFile`
  (`true` by default), `parameterId` and `parameterValue`.
- At most 16 resources. Presets keep each file with its resource id, so reordering resources in a
  new version is safe too.

**What is refused**, with a message naming the problem: a numbered block (`param.0.title`, which is
how ABI 1 numbered them), a `slot` or `id` field, a malformed id, a host param's id, the resource id
`module`, and more than 64 params or 16 resources.

## Example: a stereo gain

The same module in four languages: one param, `gain`, multiplies both channels.

Descriptor:

```text
effect.name=AI Gain
effect.version=1.0.0
effect.category=utility
effect.description=Simple stereo gain module generated for the AudioFX WASM host.
param.gain.title=Gain
param.gain.default=0.5
param.gain.min=0.0
param.gain.max=2.0
param.gain.unit=amount
```

The graph node for it in a preset:

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

A module with resources lists one more node resource per file. The node's `wasmResourceIds` config,
a JSON array with `""` for the module, says which resource each file is, for example
`"config": { "wasmResourceIds": "[\"\",\"impulse\"]" }`. Without it, the files are taken in the
order the descriptor declares the resources.

### WAT

The exact form. Memory holds the descriptor at 0, the param id at 1024 and the I/O buffer at 4096;
`audiofx_prepare` grows memory until the buffer fits. Build it with `wat2wasm` (from
[wabt](https://github.com/WebAssembly/wabt)), or see `tools/wasm-samples/audiofx-phaser-module` for a
larger module and a build script that checks the descriptor length and runs the module.

```wat
(module
  (import "host" "param_handle" (func $param_handle (param i32 i32) (result i32)))
  (import "host" "param_value" (func $param_value (param i32) (result f32)))

  (memory (export "memory") 1)

  ;; audiofx_descriptor_len must return this text's exact length in bytes.
  (data (i32.const 0)
    "effect.name=AI Gain\n"
    "effect.version=1.0.0\n"
    "effect.category=utility\n"
    "effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n"
    "param.gain.title=Gain\n"
    "param.gain.default=0.5\n"
    "param.gain.min=0.0\n"
    "param.gain.max=2.0\n"
    "param.gain.unit=amount\n")

  ;; The id audiofx_prepare passes to param_handle.
  (data (i32.const 1024) "gain")

  (global $buffer i32 (i32.const 4096))
  (global $max_block (mut i32) (i32.const 0))
  (global $gain_handle (mut i32) (i32.const -1))

  (func (export "audiofx_abi_version") (result i32)
    (i32.const 2))

  (func (export "audiofx_prepare") (param $sample_rate f32) (param $max_block_size i32) (result i32)
    (local $pages_needed i32)

    (if (i32.or (i32.lt_s (local.get $max_block_size) (i32.const 1))
                (i32.gt_s (local.get $max_block_size) (i32.const 1048576)))
      (then (return (i32.const 2))))

    ;; Grow memory until the buffer fits: 8 bytes a frame, a left and a right f32.
    (local.set $pages_needed
      (i32.sub
        (i32.shr_u
          (i32.add (i32.add (global.get $buffer) (i32.shl (local.get $max_block_size) (i32.const 3)))
                   (i32.const 65535))
          (i32.const 16))
        (memory.size)))
    (if (i32.gt_s (local.get $pages_needed) (i32.const 0))
      (then
        (if (i32.eq (memory.grow (local.get $pages_needed)) (i32.const -1))
          (then (return (i32.const 3))))))
    (global.set $max_block (local.get $max_block_size))

    (global.set $gain_handle (call $param_handle (i32.const 1024) (i32.const 4)))
    (if (i32.lt_s (global.get $gain_handle) (i32.const 0))
      (then (return (i32.const 1))))
    (i32.const 0))

  (func (export "audiofx_buffer_ptr") (result i32)
    (global.get $buffer))

  (func (export "audiofx_reset"))

  (func (export "audiofx_process") (param $frames i32)
    (local $gain f32)
    (local $i i32)
    (local $left i32)
    (local $right i32)

    ;; Once per block, clamped: the host doesn't clamp param values.
    (local.set $gain
      (f32.min (f32.max (call $param_value (global.get $gain_handle)) (f32.const 0)) (f32.const 2)))
    (local.set $left (global.get $buffer))
    (local.set $right (i32.add (global.get $buffer) (i32.shl (global.get $max_block) (i32.const 2))))

    (block $done
      (loop $frame
        (br_if $done (i32.ge_s (local.get $i) (local.get $frames)))
        (f32.store (local.get $left) (f32.mul (f32.load (local.get $left)) (local.get $gain)))
        (f32.store (local.get $right) (f32.mul (f32.load (local.get $right)) (local.get $gain)))
        (local.set $left (i32.add (local.get $left) (i32.const 4)))
        (local.set $right (i32.add (local.get $right) (i32.const 4)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $frame))))

  (func (export "audiofx_get_latency_samples") (result i32)
    (i32.const 0))

  (func (export "audiofx_descriptor_ptr") (result i32)
    (i32.const 0))

  (func (export "audiofx_descriptor_len") (result i32)
    (i32.const 253)))
```

### Rust

A `cdylib` crate, built with `cargo build --release --target wasm32-unknown-unknown`. Set
`panic = "abort"` in the release profile. The target exports `memory` itself, and `std`'s allocator
works on it, so the buffer is a `Vec` sized in prepare.

```rust
#[link(wasm_import_module = "host")]
extern "C" {
    fn param_handle(id_ptr: *const u8, id_len: i32) -> i32;
    fn param_value(handle: i32) -> f32;
}

static DESCRIPTOR: &str = "effect.name=AI Gain\n\
effect.version=1.0.0\n\
effect.category=utility\n\
effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n\
param.gain.title=Gain\n\
param.gain.default=0.5\n\
param.gain.min=0.0\n\
param.gain.max=2.0\n\
param.gain.unit=amount\n";

struct State {
    buffer: Vec<f32>,
    max_block: usize,
    gain: i32,
}

static mut STATE: State = State { buffer: Vec::new(), max_block: 0, gain: -1 };

// The host calls the module from one thread at a time.
fn state() -> &'static mut State {
    unsafe { &mut *std::ptr::addr_of_mut!(STATE) }
}

#[no_mangle]
pub extern "C" fn audiofx_abi_version() -> i32 {
    2
}

#[no_mangle]
pub extern "C" fn audiofx_prepare(_sample_rate: f32, max_block_size: i32) -> i32 {
    if max_block_size < 1 {
        return 2;
    }
    let s = state();
    s.max_block = max_block_size as usize;
    s.buffer = vec![0.0; 2 * s.max_block];
    s.gain = unsafe { param_handle(b"gain".as_ptr(), 4) };
    if s.gain < 0 {
        return 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn audiofx_buffer_ptr() -> i32 {
    state().buffer.as_mut_ptr() as usize as i32
}

#[no_mangle]
pub extern "C" fn audiofx_reset() {}

#[no_mangle]
pub extern "C" fn audiofx_process(frames: i32) {
    let s = state();
    let gain = unsafe { param_value(s.gain) }.clamp(0.0, 2.0);
    let frames = (frames.max(0) as usize).min(s.max_block);
    let (left, right) = s.buffer.split_at_mut(s.max_block);
    for sample in left[..frames].iter_mut().chain(right[..frames].iter_mut()) {
        *sample *= gain;
    }
}

#[no_mangle]
pub extern "C" fn audiofx_get_latency_samples() -> i32 {
    0
}

#[no_mangle]
pub extern "C" fn audiofx_descriptor_ptr() -> i32 {
    DESCRIPTOR.as_ptr() as usize as i32
}

#[no_mangle]
pub extern "C" fn audiofx_descriptor_len() -> i32 {
    DESCRIPTOR.len() as i32
}
```

### TinyGo

Build with `tinygo build -target=wasm-unknown -no-debug -o ai_gain.wasm .`. The `wasm-unknown`
target matters: the default `wasm` and `wasip1` targets import WASI, and the host refuses those
modules.

```go
package main

import "unsafe"

//go:wasmimport host param_handle
func paramHandle(idPtr unsafe.Pointer, idLen int32) int32

//go:wasmimport host param_value
func paramValue(handle int32) float32

var descriptor = []byte("" +
	"effect.name=AI Gain\n" +
	"effect.version=1.0.0\n" +
	"effect.category=utility\n" +
	"effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n" +
	"param.gain.title=Gain\n" +
	"param.gain.default=0.5\n" +
	"param.gain.min=0.0\n" +
	"param.gain.max=2.0\n" +
	"param.gain.unit=amount\n")

var gainID = []byte("gain")

var (
	buffer     []float32
	maxBlock   int32
	gainHandle int32 = -1
)

//export audiofx_abi_version
func audiofxAbiVersion() int32 { return 2 }

//export audiofx_prepare
func audiofxPrepare(sampleRate float32, maxBlockSize int32) int32 {
	if maxBlockSize < 1 {
		return 2
	}
	maxBlock = maxBlockSize
	buffer = make([]float32, 2*int(maxBlockSize))
	gainHandle = paramHandle(unsafe.Pointer(&gainID[0]), int32(len(gainID)))
	if gainHandle < 0 {
		return 1
	}
	return 0
}

//export audiofx_buffer_ptr
func audiofxBufferPtr() int32 { return int32(uintptr(unsafe.Pointer(&buffer[0]))) }

//export audiofx_reset
func audiofxReset() {}

//export audiofx_process
func audiofxProcess(frames int32) {
	gain := paramValue(gainHandle)
	if gain < 0 {
		gain = 0
	} else if gain > 2 {
		gain = 2
	}
	left := buffer[:frames]
	right := buffer[maxBlock : maxBlock+frames]
	for i := range left {
		left[i] *= gain
		right[i] *= gain
	}
}

//export audiofx_get_latency_samples
func audiofxGetLatencySamples() int32 { return 0 }

//export audiofx_descriptor_ptr
func audiofxDescriptorPtr() int32 { return int32(uintptr(unsafe.Pointer(&descriptor[0]))) }

//export audiofx_descriptor_len
func audiofxDescriptorLen() int32 { return int32(len(descriptor)) }

func main() {}
```

### C

Build with `clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -o ai_gain.wasm ai_gain.c`. There is
no libc, so the buffer is static, sized for 16384 frames, and prepare refuses a larger block.
`wasm-ld` exports `memory` by default.

```c
#include <stdint.h>

#define MAX_FRAMES 16384

__attribute__((import_module("host"), import_name("param_handle")))
int32_t param_handle(const char *id, int32_t id_len);

__attribute__((import_module("host"), import_name("param_value")))
float param_value(int32_t handle);

static const char descriptor[] =
    "effect.name=AI Gain\n"
    "effect.version=1.0.0\n"
    "effect.category=utility\n"
    "effect.description=Simple stereo gain module generated for the AudioFX WASM host.\n"
    "param.gain.title=Gain\n"
    "param.gain.default=0.5\n"
    "param.gain.min=0.0\n"
    "param.gain.max=2.0\n"
    "param.gain.unit=amount\n";

static float buffer[2 * MAX_FRAMES];
static int32_t max_block;
static int32_t gain_handle = -1;

__attribute__((export_name("audiofx_abi_version")))
int32_t audiofx_abi_version(void) { return 2; }

__attribute__((export_name("audiofx_prepare")))
int32_t audiofx_prepare(float sample_rate, int32_t max_block_size) {
  (void)sample_rate;
  if (max_block_size < 1 || max_block_size > MAX_FRAMES) return 2;
  max_block = max_block_size;
  gain_handle = param_handle("gain", 4);
  return gain_handle < 0 ? 1 : 0;
}

__attribute__((export_name("audiofx_buffer_ptr")))
int32_t audiofx_buffer_ptr(void) { return (int32_t)(uintptr_t)buffer; }

__attribute__((export_name("audiofx_reset")))
void audiofx_reset(void) {}

__attribute__((export_name("audiofx_process")))
void audiofx_process(int32_t frames) {
  float gain = param_value(gain_handle);
  gain = gain < 0.0f ? 0.0f : (gain > 2.0f ? 2.0f : gain);
  float *left = buffer;
  float *right = buffer + max_block;
  for (int32_t i = 0; i < frames; ++i) {
    left[i] *= gain;
    right[i] *= gain;
  }
}

__attribute__((export_name("audiofx_get_latency_samples")))
int32_t audiofx_get_latency_samples(void) { return 0; }

__attribute__((export_name("audiofx_descriptor_ptr")))
int32_t audiofx_descriptor_ptr(void) { return (int32_t)(uintptr_t)descriptor; }

__attribute__((export_name("audiofx_descriptor_len")))
int32_t audiofx_descriptor_len(void) { return (int32_t)(sizeof(descriptor) - 1); }
```

## Reading params, the tempo and resources

The pattern in every language is the same:

- **In `audiofx_prepare`:** call `param_handle` once per param id and `resource_handle` once per
  resource id, and keep the handles. Return non-zero if any is `-1`: it means the code and the
  descriptor disagree about an id. Read each resource into your own memory here, with
  `resource_size` and one `resource_read`, and parse it. Prepare runs off the audio thread with a 5
  s budget, which is enough for real work.
- **At the start of `audiofx_process`:** call `param_value` once per param, and `tempo_bpm` if you
  use the tempo, then process the block.

A resource read in WAT:

```wat
(import "host" "resource_handle" (func $resource_handle (param i32 i32) (result i32)))
(import "host" "resource_size" (func $resource_size (param i32) (result i32)))
(import "host" "resource_read" (func $resource_read (param i32 i32 i32 i32) (result i32)))
(data (i32.const 1100) "impulse")
;; in audiofx_prepare, with $handle and $size locals and a destination at $dst:
(local.set $handle (call $resource_handle (i32.const 1100) (i32.const 7)))
(if (i32.lt_s (local.get $handle) (i32.const 0)) (then (return (i32.const 1))))
(local.set $size (call $resource_size (local.get $handle)))
;; ... make room for $size bytes at $dst (memory.grow), then:
(drop (call $resource_read (local.get $handle) (i32.const 0) (local.get $dst) (local.get $size)))
```

A size of `0` means the user hasn't chosen a file yet: process as if the resource were empty, for
instance by passing audio through.

## Optional thumbnail

Add either `effect.thumbnailBase64=<base64 bytes>` with `effect.thumbnailMimeType=image/png`, or
`effect.thumbnailDataUrl=data:image/png;base64,...`. Only base64 PNG, JPEG, GIF and WebP are kept.

## Checking a module

Before you accept a module, check:

1. It exports `memory`, `audiofx_abi_version` (returning `2`), `audiofx_descriptor_ptr`,
   `audiofx_descriptor_len`, `audiofx_prepare`, `audiofx_buffer_ptr`, `audiofx_process` and
   `audiofx_reset`, with exactly the signatures in the table, and optionally
   `audiofx_get_latency_samples`.
2. It imports only from `host`, and only the six functions listed. A compiler that adds WASI
   imports (`wasi_snapshot_preview1`) or an `env` module needs a different target or flags.
3. `audiofx_descriptor_len` is the descriptor's exact byte length.
4. Every id the code passes to `param_handle` or `resource_handle` is declared in the descriptor,
   with the same spelling and length.
5. No param id is `mix`, `inputGainDb`, `outputGainDb` or `bpm`.
6. `audiofx_buffer_ptr` returns a 4-byte aligned address with `8 * maxBlockSize` bytes of memory
   behind it after prepare, and `audiofx_process` reads and writes only the first `frames` samples
   of each channel.
7. Params are read once per block and clamped to their range.
8. The output stays finite (no NaN or infinity) for any param value in range and for silence, a
   sine and a full-scale signal.

Two ways to run these checks:

- **`tools/wasm-samples/audiofx-phaser-module/build.mjs`** builds the phaser and then runs it in
  Node the way the host does: opaque handles, prepare, the buffer address, blocks of a sine. Copy
  its `smokeTest` for your own module.
- **`WasmEffectHostTests --check <module.wasm> [resource files...]`**, from a desktop build tree,
  loads the module in the real host, prepares it at 48 kHz, runs a few seconds of audio through it
  in blocks of several sizes, and prints its params, resources and output level, or the host's
  error. Resource files are taken in the order the descriptor declares the resources.

## Generating modules with an AI

Give the AI this guide's [interface](#the-interface) and [descriptor](#the-descriptor) sections, or
the whole guide, and these constraints. Replace every `<...>` before sending it.

### Prompt template

```text
Generate a single WebAssembly module for a Soundshed Custom Effect, using Custom Effect ABI 2.

Follow this interface exactly.

Exports (exactly these signatures):
- memory
- audiofx_abi_version() -> i32, returning 2
- audiofx_descriptor_ptr() -> i32 and audiofx_descriptor_len() -> i32: the descriptor's address
  and exact byte length in memory
- audiofx_prepare(sampleRate: f32, maxBlockSize: i32) -> i32: 0 on success
- audiofx_buffer_ptr() -> i32: the address of the I/O buffer, maxBlockSize f32 left samples
  followed by maxBlockSize f32 right samples, 4-byte aligned
- audiofx_process(frames: i32) -> (): process frames (1..maxBlockSize) samples in place in the
  I/O buffer
- audiofx_reset() -> (): clear all audio state
- audiofx_get_latency_samples() -> i32 (optional)

Imports, from module "host" only, and only the ones you use:
- param_handle(idPtr: i32, idLen: i32) -> i32: a param's handle by id, -1 if undeclared
- param_value(handle: i32) -> f32
- tempo_bpm() -> f32
- resource_handle(idPtr: i32, idLen: i32) -> i32
- resource_size(handle: i32) -> i32
- resource_read(handle: i32, offset: i32, dstPtr: i32, len: i32) -> i32

Rules:
- In audiofx_prepare, call param_handle once per param id and keep the handles; return 1 if any is
  -1. Never hard-code a handle number. Size or place the I/O buffer for maxBlockSize frames here
  (grow memory if needed) and return 2 if you can't.
- In audiofx_process, call param_value once per param at the start of the block and clamp each
  value to its declared range; the host does not clamp.
- Read resources in audiofx_prepare with resource_size and resource_read into your own memory.
- The host applies input gain before the module and dry/wet mix and output gain after it. Do not
  declare params with the ids mix, inputGainDb, outputGainDb or bpm.
- Stereo in, stereo out. Keep the output finite for every param value in range.
- No WASI, no other imports, no threads.

The descriptor is UTF-8 text, one key=value per line, keyed by id:
- effect.name, effect.version, effect.category, effect.description
- param.<id>.title, .default, .min, .max, .unit, .group, .advanced, .step, .labels
- resource.<id>.title, .type, .allowBrowseFile
- an id is a letter followed by up to 31 letters, digits or underscores
- the order params first appear in is their order in the UI
- no numbered blocks (param.0.*) and no slot or id fields

Source language: <WAT | Rust | TinyGo | C>. If that language cannot produce exactly these export
and import signatures, write WAT instead.

Module:
- file name: <module_file_name.wasm>
- effect name: <human-readable name>
- category: <utility | spatial | drive | modulation | dynamics | eq | delay | reverb | other>
- description: <one concise sentence>
- latency samples: <integer, or 0>
- thumbnail: <none | base64 PNG>

DSP behaviour:
- <rule 1>
- <rule 2>
- <rule 3>

Params (in UI order):
- <id>: title <title>, default <number>, min <number>, max <number>, unit <unit>,
  group <group or none>, advanced <true | false>, step <number>, labels <none | A|B|C>
- ...only the params the module needs

Resources:
- <id>: title <title>, type <ir | model | blob>, what the module does with it
- or: no resources

Return, in this order:
1. Summary
2. Source code
3. Descriptor text and its exact byte length
4. Imports and exports, with signatures
5. Example graph node JSON
6. How you checked it against the rules above
```

### Filled example

```text
Generate a single WebAssembly module for a Soundshed Custom Effect, using Custom Effect ABI 2.

[the interface, rules and descriptor sections from the template, unchanged]

Source language: WAT.

Module:
- file name: tremolo_pan.wasm
- effect name: Tremolo Pan
- category: modulation
- description: Stereo tremolo and auto-pan driven by one oscillator.
- latency samples: 0
- thumbnail: none

DSP behaviour:
- apply slow amplitude modulation to both channels
- move the image left and right with the same oscillator, a quarter cycle apart
- keep the oscillator's phase running across blocks, and reset it in audiofx_reset

Params (in UI order):
- rate: title Rate, default 0.25, min 0, max 1, unit amount, group modulation, advanced false,
  step 0.01, labels none
- depth: title Depth, default 0.5, min 0, max 1, unit amount, group modulation, advanced false,
  step 0.01, labels none

Resources:
- no resources

Return, in this order:
1. Summary
2. Source code
3. Descriptor text and its exact byte length
4. Imports and exports, with signatures
5. Example graph node JSON
6. How you checked it against the rules above
```

### Which language

WAT says exactly what the module is, so it is the safest choice for a generator. With block
processing there is no multi-value return any more, so any compiler that targets `wasm32` without
WASI produces the right signatures; what goes wrong in practice is extra imports (WASI, `env`) and a
missing `memory` export. Check the compiled module either way.

## Moving a module from ABI 1 to ABI 2

1. Export `audiofx_abi_version` returning `2`. The descriptor is now required, with `memory`,
   `audiofx_descriptor_ptr` and `audiofx_descriptor_len`.
2. Rename every `param.N.<field>` to `param.<id>.<field>`, using the old `param.N.id` value, and
   delete the `id` and `slot` lines. Do the same for `resource.N.*`. If a param's id was `mix`,
   `inputGainDb`, `outputGainDb` or `bpm`, give it another.
3. `audiofx_prepare` loses its third argument (`resourceSlotCount`).
4. In `audiofx_prepare`, call `param_handle` once for each id and keep the results. Return non-zero
   if any of them is -1.
5. Replace `read_param(n)` with `param_value(handle)`, and `read_param(8)` with `tempo_bpm()`.
6. Replace `read_resource_size(slot)` and a `read_resource_byte` loop with
   `resource_handle("<id>")` in prepare, then `resource_size(handle)` and one
   `resource_read(handle, 0, dst, size)`.
7. Replace the per-sample `audiofx_process(inLeft, inRight) -> (outLeft, outRight)` with a block:
   export `audiofx_buffer_ptr` and `audiofx_process(frames)`, and loop over the frames in the
   buffer. Read params once before the loop.
8. Rename any `guitarfx_*` exports to `audiofx_*`.

Saved presets keep their values for every param whose id you kept. Values saved under the old
generic `param1..param32` keys are not carried over.

```wat
;; ABI 1
(import "host" "read_param" (func $read_param (param i32) (result f32)))
(func (export "audiofx_process") (param $l f32) (param $r f32) (result f32 f32)
  (f32.mul (local.get $l) (call $read_param (i32.const 0)))
  (f32.mul (local.get $r) (call $read_param (i32.const 0))))

;; ABI 2
(import "host" "param_handle" (func $param_handle (param i32 i32) (result i32)))
(import "host" "param_value" (func $param_value (param i32) (result f32)))
(global $gain (mut i32) (i32.const -1))
(data (i32.const 1024) "gain")
;; in audiofx_prepare:
(global.set $gain (call $param_handle (i32.const 1024) (i32.const 4)))
(if (i32.lt_s (global.get $gain) (i32.const 0)) (then (return (i32.const 1))))
;; in audiofx_process, once, before the loop over the buffer:
(local.set $g (call $param_value (global.get $gain)))
```

The generator in `tools/wasm-module-generator` and the phaser in `tools/wasm-samples` are both
ABI 2 modules to compare against.
