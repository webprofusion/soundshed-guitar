#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Sequence


I32 = 0x7F
F32 = 0x7D

F32_NEG = bytes([0x8C])
F32_ADD = bytes([0x92])
F32_SUB = bytes([0x93])
F32_MUL = bytes([0x94])
F32_DIV = bytes([0x95])
F32_MIN = bytes([0x96])
F32_MAX = bytes([0x97])


@dataclass(frozen=True)
class FuncTypeDef:
    params: Sequence[int]
    results: Sequence[int]


@dataclass(frozen=True)
class ImportFuncDef:
    module_name: str
    field_name: str
    type_index: int


@dataclass(frozen=True)
class GlobalDef:
    value_type: int
    is_mutable: bool
    init_expr: bytes


@dataclass(frozen=True)
class DefinedFuncDef:
    type_index: int
    ops: bytes


@dataclass(frozen=True)
class ParamDoc:
    identifier: str
    description: str
    default: float


@dataclass(frozen=True)
class ResourceDoc:
    slot: int
    file_name: str
    description: str
    required: bool = True


@dataclass(frozen=True)
class GeneratedResource:
    file_name: str
    contents: bytes
    description: str


@dataclass(frozen=True)
class ModuleSpec:
    identifier: str
    file_name: str
    description: str
    category: str
    host_imports: Sequence[str]
    params: Sequence[ParamDoc] = field(default_factory=tuple)
    resources: Sequence[ResourceDoc] = field(default_factory=tuple)
    notes: Sequence[str] = field(default_factory=tuple)
    generated_resources: Sequence[GeneratedResource] = field(default_factory=tuple)
    build_module: Callable[[], bytes] = lambda: b""


def append_u32_leb(target: bytearray, value: int) -> None:
    while True:
        byte = value & 0x7F
        value >>= 7
        if value != 0:
            byte |= 0x80
        target.append(byte)
        if value == 0:
            break


def append_i32_leb(target: bytearray, value: int) -> None:
    more = True
    while more:
        byte = value & 0x7F
        value >>= 7
        sign_bit_set = (byte & 0x40) != 0
        more = not ((value == 0 and not sign_bit_set) or (value == -1 and sign_bit_set))
        if more:
            byte |= 0x80
        target.append(byte)


def append_f32(target: bytearray, value: float) -> None:
    target.extend(struct.pack("<f", value))


def append_string(target: bytearray, value: str) -> None:
    encoded = value.encode("utf-8")
    append_u32_leb(target, len(encoded))
    target.extend(encoded)


def make_section(section_id: int, payload: bytes) -> bytes:
    out = bytearray([section_id])
    append_u32_leb(out, len(payload))
    out.extend(payload)
    return bytes(out)


def local_get(index: int) -> bytes:
    out = bytearray([0x20])
    append_u32_leb(out, index)
    return bytes(out)


def global_get(index: int) -> bytes:
    out = bytearray([0x23])
    append_u32_leb(out, index)
    return bytes(out)


def global_set(index: int) -> bytes:
    out = bytearray([0x24])
    append_u32_leb(out, index)
    return bytes(out)


def call(index: int) -> bytes:
    out = bytearray([0x10])
    append_u32_leb(out, index)
    return bytes(out)


def i32_const(value: int) -> bytes:
    out = bytearray([0x41])
    append_i32_leb(out, value)
    return bytes(out)


def f32_const(value: float) -> bytes:
    out = bytearray([0x43])
    append_f32(out, value)
    return bytes(out)


def combine_ops(*ops: bytes) -> bytes:
    out = bytearray()
    for op in ops:
        out.extend(op)
    return bytes(out)


def make_module(
    types: Sequence[FuncTypeDef],
    imports: Sequence[ImportFuncDef],
    globals_: Sequence[GlobalDef],
    defined_functions: Sequence[DefinedFuncDef],
    exports: Sequence[tuple[str, int]],
) -> bytes:
    module = bytearray([0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00])

    type_payload = bytearray()
    append_u32_leb(type_payload, len(types))
    for func_type in types:
        type_payload.append(0x60)
        append_u32_leb(type_payload, len(func_type.params))
        type_payload.extend(func_type.params)
        append_u32_leb(type_payload, len(func_type.results))
        type_payload.extend(func_type.results)
    module.extend(make_section(1, bytes(type_payload)))

    if imports:
        import_payload = bytearray()
        append_u32_leb(import_payload, len(imports))
        for imported in imports:
            append_string(import_payload, imported.module_name)
            append_string(import_payload, imported.field_name)
            import_payload.append(0x00)
            append_u32_leb(import_payload, imported.type_index)
        module.extend(make_section(2, bytes(import_payload)))

    if defined_functions:
        function_payload = bytearray()
        append_u32_leb(function_payload, len(defined_functions))
        for defined in defined_functions:
            append_u32_leb(function_payload, defined.type_index)
        module.extend(make_section(3, bytes(function_payload)))

    if globals_:
        global_payload = bytearray()
        append_u32_leb(global_payload, len(globals_))
        for global_def in globals_:
            global_payload.append(global_def.value_type)
            global_payload.append(0x01 if global_def.is_mutable else 0x00)
            global_payload.extend(global_def.init_expr)
            global_payload.append(0x0B)
        module.extend(make_section(6, bytes(global_payload)))

    export_payload = bytearray()
    append_u32_leb(export_payload, len(exports))
    for name, function_index in exports:
        append_string(export_payload, name)
        export_payload.append(0x00)
        append_u32_leb(export_payload, function_index)
    module.extend(make_section(7, bytes(export_payload)))

    code_payload = bytearray()
    append_u32_leb(code_payload, len(defined_functions))
    for defined in defined_functions:
        body = bytearray()
        append_u32_leb(body, 0)
        body.extend(defined.ops)
        body.append(0x0B)
        append_u32_leb(code_payload, len(body))
        code_payload.extend(body)
    module.extend(make_section(10, bytes(code_payload)))

    return bytes(module)


def make_standard_module(
    *,
    types: Sequence[FuncTypeDef],
    imports: Sequence[ImportFuncDef],
    globals_: Sequence[GlobalDef],
    prepare_type_index: int,
    reset_type_index: int,
    process_type_index: int,
    latency_type_index: int,
    prepare_ops: bytes,
    reset_ops: bytes,
    process_ops: bytes,
    latency_ops: bytes,
) -> bytes:
    defined_functions = [
        DefinedFuncDef(prepare_type_index, prepare_ops),
        DefinedFuncDef(reset_type_index, reset_ops),
        DefinedFuncDef(process_type_index, process_ops),
        DefinedFuncDef(latency_type_index, latency_ops),
    ]
    import_count = len(imports)
    exports = [
        ("guitarfx_prepare", import_count),
        ("guitarfx_reset", import_count + 1),
        ("guitarfx_process", import_count + 2),
        ("guitarfx_get_latency_samples", import_count + 3),
    ]
    return make_module(types, imports, globals_, defined_functions, exports)


def standard_prepare_ops() -> bytes:
    return i32_const(0)


def standard_latency_ops() -> bytes:
    return i32_const(0)


def gain_module() -> bytes:
    types = [
        FuncTypeDef([I32], [F32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_param", 0)]
    process_ops = combine_ops(
        local_get(0),
        i32_const(0),
        call(0),
        F32_MUL,
        local_get(1),
        i32_const(0),
        call(0),
        F32_MUL,
    )
    return make_standard_module(
        types=types,
        imports=imports,
        globals_=[],
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def stereo_average_module() -> bytes:
    types = [
        FuncTypeDef([I32], [F32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_param", 0)]
    process_ops = combine_ops(
        local_get(0),
        local_get(1),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        i32_const(0),
        call(0),
        F32_MUL,
        local_get(1),
        local_get(0),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        i32_const(0),
        call(0),
        F32_MUL,
    )
    return make_standard_module(
        types=types,
        imports=imports,
        globals_=[],
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def channel_swap_module() -> bytes:
    types = [
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    process_ops = combine_ops(local_get(1), local_get(0))
    return make_standard_module(
        types=types,
        imports=[],
        globals_=[],
        prepare_type_index=0,
        reset_type_index=1,
        process_type_index=2,
        latency_type_index=3,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def hard_clip_threshold_ops() -> bytes:
    return combine_ops(i32_const(0), call(0), f32_const(0.05), F32_MAX)


def hard_clip_module() -> bytes:
    types = [
        FuncTypeDef([I32], [F32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_param", 0)]
    threshold = hard_clip_threshold_ops()
    process_ops = combine_ops(
        local_get(0),
        threshold,
        F32_MIN,
        threshold,
        F32_NEG,
        F32_MAX,
        local_get(1),
        threshold,
        F32_MIN,
        threshold,
        F32_NEG,
        F32_MAX,
    )
    return make_standard_module(
        types=types,
        imports=imports,
        globals_=[],
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def bpm_ducker_module() -> bytes:
    types = [
        FuncTypeDef([I32], [F32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_param", 0)]
    process_ops = combine_ops(
        local_get(0),
        f32_const(120.0),
        i32_const(8),
        call(0),
        F32_DIV,
        F32_MUL,
        local_get(1),
        f32_const(120.0),
        i32_const(8),
        call(0),
        F32_DIV,
        F32_MUL,
    )
    return make_standard_module(
        types=types,
        imports=imports,
        globals_=[],
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def stateful_bias_module() -> bytes:
    types = [
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    globals_ = [GlobalDef(F32, True, f32_const(0.0))]
    reset_ops = combine_ops(f32_const(0.0), global_set(0))
    process_ops = combine_ops(
        global_get(0),
        f32_const(0.01),
        F32_ADD,
        global_set(0),
        local_get(0),
        global_get(0),
        F32_ADD,
        local_get(1),
        global_get(0),
        F32_ADD,
    )
    return make_standard_module(
        types=types,
        imports=[],
        globals_=globals_,
        prepare_type_index=0,
        reset_type_index=1,
        process_type_index=2,
        latency_type_index=3,
        prepare_ops=standard_prepare_ops(),
        reset_ops=reset_ops,
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def resource_scaler_module() -> bytes:
    types = [
        FuncTypeDef([I32, I32], [I32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_resource_byte", 0)]
    process_ops = combine_ops(
        local_get(0),
        i32_const(1),
        i32_const(0),
        call(0),
        bytes([0xB2]),
        f32_const(64.0),
        F32_DIV,
        F32_MUL,
        local_get(1),
        i32_const(1),
        i32_const(0),
        call(0),
        bytes([0xB2]),
        f32_const(64.0),
        F32_DIV,
        F32_MUL,
    )
    return make_standard_module(
        types=types,
        imports=imports,
        globals_=[],
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def resource_size_gain_module() -> bytes:
    types = [
        FuncTypeDef([I32], [I32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_resource_size", 0)]
    process_ops = combine_ops(
        local_get(0),
        i32_const(1),
        call(0),
        bytes([0xB2]),
        f32_const(64.0),
        F32_DIV,
        F32_MUL,
        local_get(1),
        i32_const(1),
        call(0),
        bytes([0xB2]),
        f32_const(64.0),
        F32_DIV,
        F32_MUL,
    )
    return make_standard_module(
        types=types,
        imports=imports,
        globals_=[],
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
    )


def module_specs() -> list[ModuleSpec]:
    return [
        ModuleSpec(
            identifier="gain",
            file_name="gain.wasm",
            description="Stereo gain controlled by param1.",
            category="utility",
            host_imports=("read_param",),
            params=(ParamDoc("param1", "Linear gain multiplier.", 0.5),),
            notes=("Good first smoke test for param import wiring.",),
            build_module=gain_module,
        ),
        ModuleSpec(
            identifier="stereo_average",
            file_name="stereo_average.wasm",
            description="Averages left/right input to mono and writes the same signal to both outputs.",
            category="utility",
            host_imports=("read_param",),
            params=(ParamDoc("param1", "Post-average output gain.", 1.0),),
            notes=("Useful for checking multivalue output ordering and utility-style processing.",),
            build_module=stereo_average_module,
        ),
        ModuleSpec(
            identifier="channel_swap",
            file_name="channel_swap.wasm",
            description="Swaps the left and right channels.",
            category="utility",
            host_imports=(),
            notes=("Useful for validating stereo routing without params or resources.",),
            build_module=channel_swap_module,
        ),
        ModuleSpec(
            identifier="hard_clip",
            file_name="hard_clip.wasm",
            description="Hard clips both channels to +/- param1.",
            category="drive",
            host_imports=("read_param",),
            params=(ParamDoc("param1", "Clip threshold with an internal minimum floor of 0.05.", 0.8),),
            notes=("Demonstrates a simple effect rather than a utility process.",),
            build_module=hard_clip_module,
        ),
        ModuleSpec(
            identifier="bpm_ducker",
            file_name="bpm_ducker.wasm",
            description="Scales signal level by 120 / bpm using the guest tempo slot.",
            category="modulation",
            host_imports=("read_param",),
            notes=("Demonstrates use of host param index 8 for BPM.", "Assumes BPM stays above zero."),
            build_module=bpm_ducker_module,
        ),
        ModuleSpec(
            identifier="stateful_bias",
            file_name="stateful_bias.wasm",
            description="Adds a tiny cumulative bias each sample and clears it on reset.",
            category="utility",
            host_imports=(),
            notes=("Useful for verifying instance state and reset semantics.",),
            build_module=stateful_bias_module,
        ),
        ModuleSpec(
            identifier="resource_size_gain",
            file_name="resource_size_gain.wasm",
            description="Scales both channels from the byte size of an external resource blob in slot 1.",
            category="utility",
            host_imports=("read_resource_size",),
            resources=(
                ResourceDoc(
                    slot=1,
                    file_name="resource_size_gain_payload.bin",
                    description="Binary payload whose byte length is divided by 64 to produce a gain scalar. Default example length: 32 bytes.",
                ),
            ),
            notes=("Useful for validating resource-size lookups separately from byte reads.",),
            generated_resources=(
                GeneratedResource(
                    file_name="resource_size_gain_payload.bin",
                    contents=bytes(32),
                    description="Default payload for resource_size_gain.",
                ),
            ),
            build_module=resource_size_gain_module,
        ),
        ModuleSpec(
            identifier="resource_scaler",
            file_name="resource_scaler.wasm",
            description="Scales both channels from the first byte of an external resource blob in slot 1.",
            category="utility",
            host_imports=("read_resource_byte",),
            resources=(
                ResourceDoc(
                    slot=1,
                    file_name="resource_scaler_scale.bin",
                    description="First byte is interpreted as a gain numerator and divided by 64. Default example byte: 32.",
                ),
            ),
            notes=("Useful for validating optional resource loading and resourceSlotIndex metadata.",),
            generated_resources=(
                GeneratedResource(
                    file_name="resource_scaler_scale.bin",
                    contents=bytes([32]),
                    description="Default scale resource for resource_scaler.",
                ),
            ),
            build_module=resource_scaler_module,
        ),
    ]


def example_graph_node(spec: ModuleSpec) -> dict[str, object]:
    resources: list[dict[str, object]] = [
        {
            "resourceType": "wasm",
            "filePath": f"modules/{spec.file_name}",
        }
    ]
    for resource in spec.resources:
        resources.append(
            {
                "resourceType": "binary",
                "filePath": f"resources/{resource.file_name}",
                "metadata": {"resourceSlotIndex": str(resource.slot)},
            }
        )

    return {
        "type": "wasm_host",
        "category": spec.category,
        "label": spec.identifier,
        "params": {param.identifier: param.default for param in spec.params},
        "resources": resources,
    }


def manifest_for(specs: Sequence[ModuleSpec]) -> dict[str, object]:
    host_abi = {
        "exports": [
            "guitarfx_prepare(sampleRate: f32, maxBlockSize: i32, resourceSlotCount: i32) -> i32",
            "guitarfx_reset() -> void",
            "guitarfx_process(inLeft: f32, inRight: f32) -> (f32, f32)",
            "guitarfx_get_latency_samples() -> i32",
        ],
        "imports": [
            "host.read_param(index: i32) -> f32",
            "host.read_resource_size(slot: i32) -> i32",
            "host.read_resource_byte(slot: i32, offset: i32) -> i32",
        ],
        "paramIndices": {**{f"param{index}": index - 1 for index in range(1, 9)}, "bpm": 8},
        "resourceSlots": {
            "0": "WASM module bytes",
            "1..N": "Optional binary resource blobs",
        },
    }

    modules: list[dict[str, object]] = []
    for spec in specs:
        modules.append(
            {
                "id": spec.identifier,
                "file": f"modules/{spec.file_name}",
                "description": spec.description,
                "category": spec.category,
                "hostImports": list(spec.host_imports),
                "parameters": [
                    {
                        "id": param.identifier,
                        "description": param.description,
                        "default": param.default,
                    }
                    for param in spec.params
                ],
                "resources": [
                    {
                        "slot": resource.slot,
                        "file": f"resources/{resource.file_name}",
                        "description": resource.description,
                        "required": resource.required,
                    }
                    for resource in spec.resources
                ],
                "notes": list(spec.notes),
                "exampleGraphNode": example_graph_node(spec),
            }
        )

    return {
        "tool": {
            "name": "soundshed-wasm-module-generator",
            "description": "Raw WASM guest-module generator for the Soundshed generic WASM host.",
        },
        "hostAbi": host_abi,
        "modules": modules,
    }


def write_output(root: Path, specs: Sequence[ModuleSpec]) -> None:
    modules_dir = root / "modules"
    resources_dir = root / "resources"
    modules_dir.mkdir(parents=True, exist_ok=True)
    resources_dir.mkdir(parents=True, exist_ok=True)

    for spec in specs:
        (modules_dir / spec.file_name).write_bytes(spec.build_module())
        for generated_resource in spec.generated_resources:
            (resources_dir / generated_resource.file_name).write_bytes(generated_resource.contents)

    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest_for(specs), indent=2) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate simple host-compatible WASM guest modules.")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent / "output",
        help="Directory to write generated modules and manifest into.",
    )
    parser.add_argument(
        "--only",
        nargs="+",
        metavar="MODULE_ID",
        help="Generate only the specified module ids.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available module ids and exit.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    specs = module_specs()
    spec_by_id = {spec.identifier: spec for spec in specs}

    if args.list:
        for spec in specs:
            print(f"{spec.identifier}: {spec.description}")
        return 0

    selected_specs = specs
    if args.only:
        unknown = [module_id for module_id in args.only if module_id not in spec_by_id]
        if unknown:
            available = ", ".join(spec_by_id)
            raise SystemExit(f"Unknown module id(s): {', '.join(unknown)}. Available: {available}")
        selected_specs = [spec_by_id[module_id] for module_id in args.only]

    output_root = args.output.resolve()
    write_output(output_root, selected_specs)

    print(f"Generated {len(selected_specs)} WASM module(s) in {output_root}")
    for spec in selected_specs:
        print(f"- {spec.identifier} -> modules/{spec.file_name}")
    print("- manifest -> manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())