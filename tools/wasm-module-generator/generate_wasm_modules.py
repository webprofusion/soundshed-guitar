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
F32_ABS = bytes([0x8B])
F32_GT = bytes([0x5E])

I32_ADD = bytes([0x6A])
I32_MUL = bytes([0x6C])
I32_AND = bytes([0x71])

IF_VOID = bytes([0x04, 0x40])
ELSE = bytes([0x05])
END = bytes([0x0B])


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
class DataSegmentDef:
    offset: int
    contents: bytes


@dataclass(frozen=True)
class ParamDoc:
    identifier: str
    title: str
    description: str
    default: float
    min_value: float = 0.0
    max_value: float = 1.0
    unit: str = ""
    group: str = ""
    advanced: bool = False
    step: float = 0.0
    labels: Sequence[str] = field(default_factory=tuple)
    slot: int = 0


@dataclass(frozen=True)
class ResourceDoc:
    identifier: str
    title: str
    slot: int
    file_name: str
    description: str
    resource_type: str = "blob"
    allow_browse_file: bool = True
    parameter_id: str | None = None
    parameter_value: float | None = None
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
    title: str = ""
    thumbnail_base64: str = ""
    thumbnail_mime_type: str = "image/png"
    params: Sequence[ParamDoc] = field(default_factory=tuple)
    resources: Sequence[ResourceDoc] = field(default_factory=tuple)
    notes: Sequence[str] = field(default_factory=tuple)
    generated_resources: Sequence[GeneratedResource] = field(default_factory=tuple)
    build_module: Callable[["ModuleSpec"], bytes] = lambda _spec: b""


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
    *,
    export_memory: bool = False,
    data_segments: Sequence[DataSegmentDef] = (),
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

    if export_memory or data_segments:
        memory_payload = bytearray()
        append_u32_leb(memory_payload, 1)
        memory_payload.append(0x00)
        append_u32_leb(memory_payload, 1)
        module.extend(make_section(5, bytes(memory_payload)))

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
    append_u32_leb(export_payload, len(exports) + (1 if export_memory else 0))
    for name, function_index in exports:
        append_string(export_payload, name)
        export_payload.append(0x00)
        append_u32_leb(export_payload, function_index)
    if export_memory:
        append_string(export_payload, "memory")
        export_payload.append(0x02)
        append_u32_leb(export_payload, 0)
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

    if data_segments:
        data_payload = bytearray()
        append_u32_leb(data_payload, len(data_segments))
        for segment in data_segments:
            data_payload.append(0x00)
            data_payload.extend(i32_const(segment.offset))
            data_payload.append(0x0B)
            append_u32_leb(data_payload, len(segment.contents))
            data_payload.extend(segment.contents)
        module.extend(make_section(11, bytes(data_payload)))

    return bytes(module)


def title_from_identifier(identifier: str) -> str:
    return " ".join(part.capitalize() for part in identifier.split("_"))


def build_descriptor_blob(entries: Sequence[tuple[str, str]]) -> bytes:
    if not entries:
        return b""
    return "".join(f"{key}={value}\n" for key, value in entries).encode("utf-8")


def descriptor_entries_for_spec(spec: ModuleSpec) -> list[tuple[str, str]]:
    entries: list[tuple[str, str]] = [
        ("effect.name", spec.title or title_from_identifier(spec.identifier)),
        ("effect.category", spec.category),
        ("effect.description", spec.description),
    ]
    if spec.thumbnail_base64:
        entries.append(("effect.thumbnailBase64", spec.thumbnail_base64))
        if spec.thumbnail_mime_type:
            entries.append(("effect.thumbnailMimeType", spec.thumbnail_mime_type))
    for index, param in enumerate(spec.params):
        prefix = f"param.{index}"
        entries.extend(
            [
                (f"{prefix}.id", param.identifier),
                (f"{prefix}.title", param.title),
                (f"{prefix}.slot", str(param.slot)),
                (f"{prefix}.default", str(param.default)),
                (f"{prefix}.min", str(param.min_value)),
                (f"{prefix}.max", str(param.max_value)),
            ]
        )
        if param.unit:
            entries.append((f"{prefix}.unit", param.unit))
        if param.group:
            entries.append((f"{prefix}.group", param.group))
        if param.advanced:
            entries.append((f"{prefix}.advanced", "true"))
        if param.step:
            entries.append((f"{prefix}.step", str(param.step)))
        if param.labels:
            entries.append((f"{prefix}.labels", "|".join(param.labels)))
    for index, resource in enumerate(spec.resources):
        prefix = f"resource.{index}"
        entries.extend(
            [
                (f"{prefix}.id", resource.identifier),
                (f"{prefix}.title", resource.title),
                (f"{prefix}.slot", str(resource.slot)),
                (f"{prefix}.type", resource.resource_type),
            ]
        )
        if not resource.allow_browse_file:
            entries.append((f"{prefix}.allowBrowseFile", "false"))
        if resource.parameter_id:
            entries.append((f"{prefix}.parameterId", resource.parameter_id))
        if resource.parameter_value is not None:
            entries.append((f"{prefix}.parameterValue", str(resource.parameter_value)))
    return entries


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
    descriptor_entries: Sequence[tuple[str, str]] = (),
) -> bytes:
    descriptor_blob = build_descriptor_blob(descriptor_entries)
    defined_functions = [
        DefinedFuncDef(prepare_type_index, prepare_ops),
        DefinedFuncDef(reset_type_index, reset_ops),
        DefinedFuncDef(process_type_index, process_ops),
        DefinedFuncDef(latency_type_index, latency_ops),
    ]
    import_count = len(imports)
    exports = [
        ("audiofx_prepare", import_count),
        ("audiofx_reset", import_count + 1),
        ("audiofx_process", import_count + 2),
        ("audiofx_get_latency_samples", import_count + 3),
    ]
    export_memory = False
    data_segments: list[DataSegmentDef] = []
    if descriptor_blob:
        defined_functions.extend(
            [
                DefinedFuncDef(latency_type_index, i32_const(0)),
                DefinedFuncDef(latency_type_index, i32_const(len(descriptor_blob))),
            ]
        )
        exports.extend(
            [
                ("audiofx_descriptor_ptr", import_count + 4),
                ("audiofx_descriptor_len", import_count + 5),
            ]
        )
        export_memory = True
        data_segments.append(DataSegmentDef(0, descriptor_blob))
    return make_module(
        types,
        imports,
        globals_,
        defined_functions,
        exports,
        export_memory=export_memory,
        data_segments=data_segments,
    )


def standard_prepare_ops() -> bytes:
    return i32_const(0)


def standard_latency_ops() -> bytes:
    return i32_const(0)


def clamped_param_ops(index: int) -> bytes:
    return combine_ops(
        i32_const(index),
        call(0),
        f32_const(1.0),
        F32_MIN,
        f32_const(-1.0),
        F32_MAX,
    )


def clamped_unipolar_param_ops(index: int) -> bytes:
    return combine_ops(
        i32_const(index),
        call(0),
        f32_const(1.0),
        F32_MIN,
        f32_const(0.0),
        F32_MAX,
    )


def random_bipolar_target_ops(seed_global: int, base_global: int, amount_global: int) -> bytes:
    return combine_ops(
        global_get(seed_global),
        i32_const(1_664_525),
        I32_MUL,
        i32_const(1_013_904_223),
        I32_ADD,
        global_set(seed_global),
        global_get(seed_global),
        i32_const(0x7FFF_FFFF),
        I32_AND,
        bytes([0xB2]),
        f32_const(2_147_483_647.0),
        F32_DIV,
        f32_const(2.0),
        F32_MUL,
        f32_const(1.0),
        F32_SUB,
        global_get(amount_global),
        F32_MUL,
        global_get(base_global),
        F32_ADD,
        f32_const(1.0),
        F32_MIN,
        f32_const(-1.0),
        F32_MAX,
    )


def random_slew_ops(speed_global: int, sample_rate_global: int) -> bytes:
    return combine_ops(
        f32_const(1.0),
        global_get(speed_global),
        f32_const(11.0),
        F32_MUL,
        F32_ADD,
        global_get(sample_rate_global),
        F32_DIV,
    )


def gain_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def stereo_spatial_module(spec: ModuleSpec) -> bytes:
    types = [
        FuncTypeDef([I32], [F32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_param", 0)]
    globals_ = [
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(0.0)),
    ]

    process_ops = combine_ops(
        clamped_param_ops(0),
        global_set(0),
        clamped_param_ops(1),
        global_set(1),
        global_get(0),
        f32_const(1.0),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        global_set(2),
        f32_const(0.6),
        global_get(2),
        f32_const(0.4),
        F32_MUL,
        F32_ADD,
        global_set(3),
        f32_const(1.0),
        global_get(1),
        F32_SUB,
        f32_const(1.0),
        F32_MIN,
        global_set(4),
        f32_const(1.0),
        global_get(1),
        F32_ADD,
        f32_const(1.0),
        F32_MIN,
        global_set(5),
        local_get(0),
        local_get(1),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        global_set(6),
        global_get(2),
        local_get(0),
        F32_MUL,
        f32_const(1.0),
        global_get(2),
        F32_SUB,
        global_get(6),
        F32_MUL,
        F32_ADD,
        global_get(3),
        F32_MUL,
        global_get(4),
        F32_MUL,
        global_get(2),
        local_get(1),
        F32_MUL,
        f32_const(1.0),
        global_get(2),
        F32_SUB,
        global_get(6),
        F32_MUL,
        F32_ADD,
        global_get(3),
        F32_MUL,
        global_get(5),
        F32_MUL,
    )

    return make_standard_module(
        types=types,
        imports=imports,
        globals_=globals_,
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=standard_prepare_ops(),
        reset_ops=b"",
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def stereo_spatial_random_module(spec: ModuleSpec) -> bytes:
    types = [
        FuncTypeDef([I32], [F32]),
        FuncTypeDef([F32, I32, I32], [I32]),
        FuncTypeDef([], []),
        FuncTypeDef([F32, F32], [F32, F32]),
        FuncTypeDef([], [I32]),
    ]
    imports = [ImportFuncDef("host", "read_param", 0)]
    globals_ = [
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(0.35)),
        GlobalDef(F32, True, f32_const(0.5)),
        GlobalDef(I32, True, i32_const(0x13579BDF)),
        GlobalDef(F32, True, f32_const(48_000.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(0.0)),
    ]

    prepare_ops = combine_ops(local_get(0), global_set(10), i32_const(0))
    reset_ops = combine_ops(
        f32_const(0.0),
        global_set(2),
        f32_const(0.0),
        global_set(3),
        f32_const(0.0),
        global_set(4),
        f32_const(0.0),
        global_set(5),
        i32_const(0x13579BDF),
        global_set(9),
    )

    process_ops = combine_ops(
        clamped_param_ops(0),
        global_set(0),
        clamped_param_ops(1),
        global_set(1),
        clamped_unipolar_param_ops(2),
        global_set(6),
        clamped_unipolar_param_ops(3),
        global_set(7),
        clamped_unipolar_param_ops(4),
        global_set(8),
        global_get(6),
        f32_const(0.5),
        F32_GT,
        IF_VOID,
        global_get(4),
        global_get(2),
        F32_SUB,
        F32_ABS,
        f32_const(0.03),
        bytes([0x5D]),
        global_get(5),
        global_get(3),
        F32_SUB,
        F32_ABS,
        f32_const(0.03),
        bytes([0x5D]),
        I32_AND,
        IF_VOID,
        random_bipolar_target_ops(9, 0, 8),
        global_set(4),
        random_bipolar_target_ops(9, 1, 8),
        global_set(5),
        END,
        global_get(2),
        global_get(4),
        global_get(2),
        F32_SUB,
        random_slew_ops(7, 10),
        F32_MUL,
        F32_ADD,
        global_set(2),
        global_get(3),
        global_get(5),
        global_get(3),
        F32_SUB,
        random_slew_ops(7, 10),
        F32_MUL,
        F32_ADD,
        global_set(3),
        ELSE,
        global_get(0),
        global_set(2),
        global_get(1),
        global_set(3),
        global_get(0),
        global_set(4),
        global_get(1),
        global_set(5),
        END,
        global_get(2),
        f32_const(1.0),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        global_set(11),
        f32_const(0.6),
        global_get(11),
        f32_const(0.4),
        F32_MUL,
        F32_ADD,
        global_set(12),
        f32_const(1.0),
        global_get(3),
        F32_SUB,
        f32_const(1.0),
        F32_MIN,
        global_set(13),
        f32_const(1.0),
        global_get(3),
        F32_ADD,
        f32_const(1.0),
        F32_MIN,
        global_set(14),
        local_get(0),
        local_get(1),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        global_set(15),
        global_get(11),
        local_get(0),
        F32_MUL,
        f32_const(1.0),
        global_get(11),
        F32_SUB,
        global_get(15),
        F32_MUL,
        F32_ADD,
        global_get(12),
        F32_MUL,
        global_get(13),
        F32_MUL,
        global_get(11),
        local_get(1),
        F32_MUL,
        f32_const(1.0),
        global_get(11),
        F32_SUB,
        global_get(15),
        F32_MUL,
        F32_ADD,
        global_get(12),
        F32_MUL,
        global_get(14),
        F32_MUL,
    )

    return make_standard_module(
        types=types,
        imports=imports,
        globals_=globals_,
        prepare_type_index=1,
        reset_type_index=2,
        process_type_index=3,
        latency_type_index=4,
        prepare_ops=prepare_ops,
        reset_ops=reset_ops,
        process_ops=process_ops,
        latency_ops=standard_latency_ops(),
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def stereo_average_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def channel_swap_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def hard_clip_threshold_ops() -> bytes:
    return combine_ops(i32_const(0), call(0), f32_const(0.05), F32_MAX)


def hard_clip_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def bpm_ducker_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def stateful_bias_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def resource_scaler_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def resource_size_gain_module(spec: ModuleSpec) -> bytes:
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
        descriptor_entries=descriptor_entries_for_spec(spec),
    )


def module_specs() -> list[ModuleSpec]:
    return [
        ModuleSpec(
            identifier="gain",
            file_name="gain.wasm",
            description="Stereo gain controlled by a self-described guest parameter named gain.",
            category="utility",
            host_imports=("read_param",),
            params=(
                ParamDoc(
                    identifier="gain",
                    title="Gain",
                    description="Linear gain multiplier.",
                    default=0.5,
                    min_value=0.0,
                    max_value=2.0,
                    unit="amount",
                    step=0.01,
                    slot=0,
                ),
            ),
            notes=("Good first smoke test for param import wiring.",),
            build_module=gain_module,
        ),
        ModuleSpec(
            identifier="stereo_spatial",
            file_name="stereo_spatial.wasm",
            description="Stereo spatial utility with self-described depth and pan parameters.",
            category="spatial",
            host_imports=("read_param",),
            params=(
                ParamDoc(
                    identifier="depth",
                    title="Depth",
                    description="Back/forward pan from -1.0 (back: narrower and quieter) to 1.0 (forward: wider and more present).",
                    default=0.0,
                    min_value=-1.0,
                    max_value=1.0,
                    unit="amount",
                    step=0.01,
                    slot=0,
                ),
                ParamDoc(
                    identifier="pan",
                    title="Pan",
                    description="Left/right pan from -1.0 (left) to 1.0 (right).",
                    default=0.0,
                    min_value=-1.0,
                    max_value=1.0,
                    unit="pan",
                    step=0.01,
                    slot=1,
                ),
            ),
            notes=(
                "Implements a simple stereo-only depth and pan matrix, not an HRTF or surround spatializer.",
                "Back movement narrows toward mono and reduces level; forward movement restores full width and level.",
            ),
            build_module=stereo_spatial_module,
        ),
        ModuleSpec(
            identifier="stereo_spatial_random",
            file_name="stereo_spatial_random.wasm",
            description="Stereo spatial utility with base depth and pan controls plus smooth random wandering.",
            category="spatial",
            host_imports=("read_param",),
            params=(
                ParamDoc(
                    identifier="depth",
                    title="Base Depth",
                    description="Center point for the back/forward image position before random wandering is applied.",
                    default=0.0,
                    min_value=-1.0,
                    max_value=1.0,
                    unit="amount",
                    step=0.01,
                    slot=0,
                ),
                ParamDoc(
                    identifier="pan",
                    title="Base Pan",
                    description="Center point for the left/right image position before random wandering is applied.",
                    default=0.0,
                    min_value=-1.0,
                    max_value=1.0,
                    unit="pan",
                    step=0.01,
                    slot=1,
                ),
                ParamDoc(
                    identifier="randomMode",
                    title="Random Mode",
                    description="Turns smooth random target wandering on or off.",
                    default=1.0,
                    min_value=0.0,
                    max_value=1.0,
                    unit="enum",
                    group="random",
                    step=1.0,
                    labels=("Off", "On"),
                    slot=2,
                ),
                ParamDoc(
                    identifier="speed",
                    title="Random Speed",
                    description="How quickly the module glides toward each new random pan/depth target.",
                    default=0.35,
                    min_value=0.0,
                    max_value=1.0,
                    unit="amount",
                    group="random",
                    step=0.01,
                    slot=3,
                ),
                ParamDoc(
                    identifier="amount",
                    title="Random Amount",
                    description="How far the pan and depth can wander away from their base settings.",
                    default=0.5,
                    min_value=0.0,
                    max_value=1.0,
                    unit="amount",
                    group="random",
                    step=0.01,
                    slot=4,
                ),
            ),
            notes=(
                "Chooses new random pan and depth targets around the base settings, then slews toward them instead of jumping.",
                "The wander seed resets deterministically, so reloading the module restarts the same motion path.",
            ),
            build_module=stereo_spatial_random_module,
        ),
        ModuleSpec(
            identifier="stereo_average",
            file_name="stereo_average.wasm",
            description="Averages left/right input to mono and writes the same signal to both outputs.",
            category="utility",
            host_imports=("read_param",),
            params=(
                ParamDoc(
                    identifier="gain",
                    title="Output Gain",
                    description="Post-average output gain.",
                    default=1.0,
                    min_value=0.0,
                    max_value=2.0,
                    unit="amount",
                    step=0.01,
                    slot=0,
                ),
            ),
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
            description="Hard clips both channels to +/- a self-described threshold parameter.",
            category="drive",
            host_imports=("read_param",),
            params=(
                ParamDoc(
                    identifier="threshold",
                    title="Threshold",
                    description="Clip threshold with an internal minimum floor of 0.05.",
                    default=0.8,
                    min_value=0.05,
                    max_value=1.0,
                    unit="amount",
                    step=0.01,
                    slot=0,
                ),
            ),
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
                    identifier="payload",
                    title="Payload",
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
                    identifier="scale",
                    title="Scale Data",
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
                "resourceType": resource.resource_type,
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
            "audiofx_prepare(sampleRate: f32, maxBlockSize: i32, resourceSlotCount: i32) -> i32",
            "audiofx_reset() -> void",
            "audiofx_process(inLeft: f32, inRight: f32) -> (f32, f32)",
            "audiofx_get_latency_samples() -> i32",
            "memory (optional, required when exporting a descriptor blob)",
            "audiofx_descriptor_ptr() -> i32 (optional descriptor blob offset)",
            "audiofx_descriptor_len() -> i32 (optional descriptor blob byte length)",
        ],
        "imports": [
            "host.read_param(index: i32) -> f32",
            "host.read_resource_size(slot: i32) -> i32",
            "host.read_resource_byte(slot: i32, offset: i32) -> i32",
        ],
        "descriptorBlobFormat": "UTF-8 newline-delimited key=value entries stored in guest memory.",
        "monoInputBehavior": "If only one input channel is present, the host mirrors it to both guest inputs before calling the WASM process function.",
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
                        "title": param.title,
                        "description": param.description,
                        "default": param.default,
                        "min": param.min_value,
                        "max": param.max_value,
                        "unit": param.unit,
                        "group": param.group,
                        "advanced": param.advanced,
                        "step": param.step,
                        "labels": list(param.labels),
                        "slot": param.slot,
                    }
                    for param in spec.params
                ],
                "resources": [
                    {
                        "id": resource.identifier,
                        "title": resource.title,
                        "slot": resource.slot,
                        "resourceType": resource.resource_type,
                        "file": f"resources/{resource.file_name}",
                        "description": resource.description,
                        "allowBrowseFile": resource.allow_browse_file,
                        "parameterId": resource.parameter_id,
                        "parameterValue": resource.parameter_value,
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
        (modules_dir / spec.file_name).write_bytes(spec.build_module(spec))
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