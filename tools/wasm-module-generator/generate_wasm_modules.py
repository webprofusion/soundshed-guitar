#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
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
I32_SUB = bytes([0x6B])
I32_MUL = bytes([0x6C])
I32_AND = bytes([0x71])
I32_SHL = bytes([0x74])
I32_SHR_U = bytes([0x76])
I32_EQ = bytes([0x46])
I32_LT_S = bytes([0x48])
I32_GT_S = bytes([0x4A])
I32_GE_S = bytes([0x4E])
F32_CONVERT_I32_S = bytes([0xB2])

IF_VOID = bytes([0x04, 0x40])
BLOCK_VOID = bytes([0x02, 0x40])
LOOP_VOID = bytes([0x03, 0x40])
ELSE = bytes([0x05])
END = bytes([0x0B])
RETURN = bytes([0x0F])
DROP = bytes([0x1A])
MEMORY_SIZE = bytes([0x3F, 0x00])
MEMORY_GROW = bytes([0x40, 0x00])

PAGE_BYTES = 65536

# Custom Effect ABI 2 (docs/wasm-module-authoring.md). The host imports, all six of them, are
# always imported in this order, so their function indices are fixed.
ABI_VERSION = 2
FN_PARAM_HANDLE = 0
FN_PARAM_VALUE = 1
FN_TEMPO_BPM = 2
FN_RESOURCE_HANDLE = 3
FN_RESOURCE_SIZE = 4
FN_RESOURCE_READ = 5
IMPORT_COUNT = 6

# The defined functions follow the imports. A module supplies the kernel: a per-sample function
# (inLeft, inRight) -> (outLeft, outRight) that the standard audiofx_process runs over each frame.
FN_PREPARE = IMPORT_COUNT
FN_RESET = IMPORT_COUNT + 1
FN_KERNEL = IMPORT_COUNT + 2

# Guest memory layout. Everything a kernel reads from memory has address 0 and the field's place as
# the load's static offset, so the kernels need no address arithmetic.
MEM_PARAM_HANDLES = 0  # i32 per param, from param_handle in prepare
MEM_PARAM_VALUES = 256  # f32 per param, read once per block before the kernel runs
MEM_TEMPO = 512  # f32, tempo_bpm, once per block
MEM_MAX_BLOCK = 516  # i32, maxBlockSize from prepare
MEM_RESOURCE_SCRATCH = 528  # 16 bytes a module may fill from its resources in prepare
MEM_RESOURCE_HANDLES = 544  # i32 per resource, from resource_handle in prepare
MEM_IDS = 1024  # the param and resource id strings param_handle and resource_handle are given
MEM_DESCRIPTOR = 4096  # the descriptor text; the I/O buffer starts at the next page after it
MAX_PARAMS = 64
MAX_RESOURCES = 16
# prepare refuses a larger maxBlockSize (status 2) rather than overflow the buffer arithmetic.
MAX_BLOCK_FRAMES = 1 << 20


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
    locals_: Sequence[int] = ()


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


@dataclass(frozen=True)
class ResourceDoc:
    identifier: str
    title: str
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


def local_set(index: int) -> bytes:
    out = bytearray([0x21])
    append_u32_leb(out, index)
    return bytes(out)


def memory_op(opcode: int, align_log2: int, offset: int) -> bytes:
    out = bytearray([opcode])
    append_u32_leb(out, align_log2)
    append_u32_leb(out, offset)
    return bytes(out)


def i32_load(offset: int = 0) -> bytes:
    return memory_op(0x28, 2, offset)


def f32_load(offset: int = 0) -> bytes:
    return memory_op(0x2A, 2, offset)


def i32_load8_u(offset: int = 0) -> bytes:
    return memory_op(0x2D, 0, offset)


def i32_store(offset: int = 0) -> bytes:
    return memory_op(0x36, 2, offset)


def f32_store(offset: int = 0) -> bytes:
    return memory_op(0x38, 2, offset)


def br(depth: int) -> bytes:
    out = bytearray([0x0C])
    append_u32_leb(out, depth)
    return bytes(out)


def br_if(depth: int) -> bytes:
    out = bytearray([0x0D])
    append_u32_leb(out, depth)
    return bytes(out)


def combine_ops(*ops: bytes) -> bytes:
    out = bytearray()
    for op in ops:
        out.extend(op)
    return bytes(out)


def param_ops(index: int) -> bytes:
    """A param's value as audiofx_process read it at the start of this block."""
    return combine_ops(i32_const(0), f32_load(MEM_PARAM_VALUES + 4 * index))


def tempo_ops() -> bytes:
    """The host tempo in BPM as audiofx_process read it at the start of this block."""
    return combine_ops(i32_const(0), f32_load(MEM_TEMPO))


def resource_handle_ops(index: int) -> bytes:
    """The handle prepare got from resource_handle for the module's index-th resource."""
    return combine_ops(i32_const(0), i32_load(MEM_RESOURCE_HANDLES + 4 * index))


def make_module(
    types: Sequence[FuncTypeDef],
    imports: Sequence[ImportFuncDef],
    globals_: Sequence[GlobalDef],
    defined_functions: Sequence[DefinedFuncDef],
    exports: Sequence[tuple[str, int]],
    *,
    export_memory: bool = False,
    data_segments: Sequence[DataSegmentDef] = (),
    memory_min_pages: int = 1,
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
        append_u32_leb(memory_payload, memory_min_pages)
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
        append_u32_leb(body, len(defined.locals_))
        for local_type in defined.locals_:
            append_u32_leb(body, 1)
            body.append(local_type)
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


HOST_PARAM_IDS = ("mix", "inputGainDb", "outputGainDb", "bpm")
GUEST_ID_PATTERN = re.compile(r"[A-Za-z][A-Za-z0-9_]{0,31}")


def check_guest_id(kind: str, identifier: str) -> None:
    if not GUEST_ID_PATTERN.fullmatch(identifier):
        raise ValueError(f"{kind} id '{identifier}' must be a letter followed by up to 31 letters, digits or underscores.")
    if kind == "param" and identifier in HOST_PARAM_IDS:
        raise ValueError(f"param id '{identifier}' is one of the host's own params; pick another.")
    if kind == "resource" and identifier == "module":
        raise ValueError("resource id 'module' is the module's own resource; pick another.")


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
    for param in spec.params:
        check_guest_id("param", param.identifier)
        prefix = f"param.{param.identifier}"
        entries.extend(
            [
                (f"{prefix}.title", param.title),
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
    for resource in spec.resources:
        check_guest_id("resource", resource.identifier)
        prefix = f"resource.{resource.identifier}"
        entries.extend(
            [
                (f"{prefix}.title", resource.title),
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


# Every generated module has the same types and imports; see FN_* for the function indices.
STANDARD_TYPES = [
    FuncTypeDef([I32, I32], [I32]),  # 0: param_handle, resource_handle
    FuncTypeDef([I32], [F32]),  # 1: param_value
    FuncTypeDef([], [F32]),  # 2: tempo_bpm
    FuncTypeDef([I32], [I32]),  # 3: resource_size
    FuncTypeDef([I32, I32, I32, I32], [I32]),  # 4: resource_read
    FuncTypeDef([F32, I32], [I32]),  # 5: audiofx_prepare
    FuncTypeDef([], []),  # 6: audiofx_reset
    FuncTypeDef([F32, F32], [F32, F32]),  # 7: the per-sample kernel (internal)
    FuncTypeDef([], [I32]),  # 8: latency, descriptor, abi version, buffer pointer
    FuncTypeDef([I32], []),  # 9: audiofx_process
]

STANDARD_IMPORTS = [
    ImportFuncDef("host", "param_handle", 0),
    ImportFuncDef("host", "param_value", 1),
    ImportFuncDef("host", "tempo_bpm", 2),
    ImportFuncDef("host", "resource_handle", 0),
    ImportFuncDef("host", "resource_size", 3),
    ImportFuncDef("host", "resource_read", 4),
]


def lay_out_ids(identifiers: Sequence[str], base: int) -> tuple[bytes, list[tuple[int, int]]]:
    blob = bytearray()
    spans: list[tuple[int, int]] = []
    for identifier in identifiers:
        encoded = identifier.encode("utf-8")
        spans.append((base + len(blob), len(encoded)))
        blob.extend(encoded)
    return bytes(blob), spans


def lookup_handles_ops(spans: Sequence[tuple[int, int]], lookup_function: int, handle_base: int) -> bytes:
    """Asks the host for each id's handle and keeps it; prepare fails (status 1) on an unknown id."""
    ops = bytearray()
    for index, (address, length) in enumerate(spans):
        handle_address = handle_base + 4 * index
        ops.extend(
            combine_ops(
                i32_const(0),
                i32_const(address),
                i32_const(length),
                call(lookup_function),
                i32_store(handle_address),
                i32_const(0),
                i32_load(handle_address),
                i32_const(0),
                I32_LT_S,
                IF_VOID,
                i32_const(1),
                RETURN,
                END,
            )
        )
    return bytes(ops)


def make_standard_module(
    spec: ModuleSpec,
    *,
    globals_: Sequence[GlobalDef],
    kernel_ops: bytes,
    prepare_ops: bytes = b"",
    reset_ops: bytes = b"",
    latency_samples: int = 0,
) -> bytes:
    """Wraps a per-sample kernel in Custom Effect ABI 2.

    The kernel takes (inLeft, inRight) as locals 0 and 1 and leaves (outLeft, outRight) on the
    stack. It reads params with param_ops(i) (in declaration order) and the tempo with tempo_ops().
    audiofx_process reads every param once per block, then runs the kernel over each frame of the
    in-place I/O buffer. prepare_ops run at the end of audiofx_prepare, with the sample rate in
    local 0, maxBlockSize in local 1 and every handle already looked up; they must leave the stack
    as they found it. Module globals keep the indices the kernel gives them.
    """
    if len(spec.params) > MAX_PARAMS or len(spec.resources) > MAX_RESOURCES:
        raise ValueError(f"{spec.identifier}: at most {MAX_PARAMS} params and {MAX_RESOURCES} resources.")

    descriptor_blob = build_descriptor_blob(descriptor_entries_for_spec(spec))
    id_blob, id_spans = lay_out_ids(
        [param.identifier for param in spec.params] + [resource.identifier for resource in spec.resources], MEM_IDS
    )
    if MEM_IDS + len(id_blob) > MEM_DESCRIPTOR:
        raise ValueError(f"{spec.identifier}: the param and resource ids don't fit below the descriptor.")
    param_spans = id_spans[: len(spec.params)]
    resource_spans = id_spans[len(spec.params) :]

    # The I/O buffer starts on the page after the descriptor; prepare grows memory to fit
    # maxBlockSize left samples then as many right ones.
    buffer_offset = -(-(MEM_DESCRIPTOR + len(descriptor_blob)) // PAGE_BYTES) * PAGE_BYTES

    prepare = combine_ops(
        local_get(1),
        i32_const(1),
        I32_LT_S,
        IF_VOID,
        i32_const(2),
        RETURN,
        END,
        local_get(1),
        i32_const(MAX_BLOCK_FRAMES),
        I32_GT_S,
        IF_VOID,
        i32_const(2),
        RETURN,
        END,
        # Pages the buffer needs beyond what memory has: (offset + 8 * frames + PAGE - 1) / PAGE - size.
        local_get(1),
        i32_const(3),
        I32_SHL,
        i32_const(buffer_offset + PAGE_BYTES - 1),
        I32_ADD,
        i32_const(16),
        I32_SHR_U,
        MEMORY_SIZE,
        I32_SUB,
        local_set(2),
        local_get(2),
        i32_const(0),
        I32_GT_S,
        IF_VOID,
        local_get(2),
        MEMORY_GROW,
        i32_const(-1),
        I32_EQ,
        IF_VOID,
        i32_const(3),
        RETURN,
        END,
        END,
        i32_const(0),
        local_get(1),
        i32_store(MEM_MAX_BLOCK),
        lookup_handles_ops(param_spans, FN_PARAM_HANDLE, MEM_PARAM_HANDLES),
        lookup_handles_ops(resource_spans, FN_RESOURCE_HANDLE, MEM_RESOURCE_HANDLES),
        prepare_ops,
        i32_const(0),
    )

    # Locals: 0 frames, 1 frame index, 2 right channel's distance from the left in bytes,
    # 3 this frame's left address, 4 and 5 the kernel's left and right output.
    read_params = bytearray()
    for index in range(len(spec.params)):
        read_params.extend(
            combine_ops(
                i32_const(0),
                i32_const(0),
                i32_load(MEM_PARAM_HANDLES + 4 * index),
                call(FN_PARAM_VALUE),
                f32_store(MEM_PARAM_VALUES + 4 * index),
            )
        )
    process = combine_ops(
        bytes(read_params),
        i32_const(0),
        call(FN_TEMPO_BPM),
        f32_store(MEM_TEMPO),
        i32_const(0),
        i32_load(MEM_MAX_BLOCK),
        i32_const(2),
        I32_SHL,
        local_set(2),
        BLOCK_VOID,
        LOOP_VOID,
        local_get(1),
        local_get(0),
        I32_GE_S,
        br_if(1),
        local_get(1),
        i32_const(2),
        I32_SHL,
        i32_const(buffer_offset),
        I32_ADD,
        local_set(3),
        local_get(3),
        f32_load(),
        local_get(3),
        local_get(2),
        I32_ADD,
        f32_load(),
        call(FN_KERNEL),
        local_set(5),
        local_set(4),
        local_get(3),
        local_get(4),
        f32_store(),
        local_get(3),
        local_get(2),
        I32_ADD,
        local_get(5),
        f32_store(),
        local_get(1),
        i32_const(1),
        I32_ADD,
        local_set(1),
        br(0),
        END,
        END,
    )

    defined_functions = [
        DefinedFuncDef(5, prepare, (I32,)),  # FN_PREPARE
        DefinedFuncDef(6, reset_ops),  # FN_RESET
        DefinedFuncDef(7, kernel_ops),  # FN_KERNEL
        DefinedFuncDef(8, i32_const(latency_samples)),
        DefinedFuncDef(8, i32_const(MEM_DESCRIPTOR)),
        DefinedFuncDef(8, i32_const(len(descriptor_blob))),
        DefinedFuncDef(8, i32_const(ABI_VERSION)),
        DefinedFuncDef(9, process, (I32, I32, I32, F32, F32)),
        DefinedFuncDef(8, i32_const(buffer_offset)),
    ]
    exports = [
        ("audiofx_prepare", FN_PREPARE),
        ("audiofx_reset", FN_RESET),
        ("audiofx_get_latency_samples", IMPORT_COUNT + 3),
        ("audiofx_descriptor_ptr", IMPORT_COUNT + 4),
        ("audiofx_descriptor_len", IMPORT_COUNT + 5),
        ("audiofx_abi_version", IMPORT_COUNT + 6),
        ("audiofx_process", IMPORT_COUNT + 7),
        ("audiofx_buffer_ptr", IMPORT_COUNT + 8),
    ]
    data_segments = [DataSegmentDef(MEM_DESCRIPTOR, descriptor_blob)]
    if id_blob:
        data_segments.insert(0, DataSegmentDef(MEM_IDS, id_blob))
    return make_module(
        STANDARD_TYPES,
        STANDARD_IMPORTS,
        globals_,
        defined_functions,
        exports,
        export_memory=True,
        data_segments=data_segments,
        memory_min_pages=buffer_offset // PAGE_BYTES,
    )


def clamped_param_ops(index: int) -> bytes:
    return combine_ops(
        param_ops(index),
        f32_const(1.0),
        F32_MIN,
        f32_const(-1.0),
        F32_MAX,
    )


def clamped_unipolar_param_ops(index: int) -> bytes:
    return combine_ops(
        param_ops(index),
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
    kernel_ops = combine_ops(
        local_get(0),
        param_ops(0),
        F32_MUL,
        local_get(1),
        param_ops(0),
        F32_MUL,
    )
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops)


def stereo_spatial_module(spec: ModuleSpec) -> bytes:
    globals_ = [
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(0.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(1.0)),
        GlobalDef(F32, True, f32_const(0.0)),
    ]

    kernel_ops = combine_ops(
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
    return make_standard_module(spec, globals_=globals_, kernel_ops=kernel_ops)


def stereo_spatial_random_module(spec: ModuleSpec) -> bytes:
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

    prepare_ops = combine_ops(local_get(0), global_set(10))
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

    kernel_ops = combine_ops(
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
        spec, globals_=globals_, kernel_ops=kernel_ops, prepare_ops=prepare_ops, reset_ops=reset_ops
    )


def stereo_average_module(spec: ModuleSpec) -> bytes:
    kernel_ops = combine_ops(
        local_get(0),
        local_get(1),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        param_ops(0),
        F32_MUL,
        local_get(1),
        local_get(0),
        F32_ADD,
        f32_const(0.5),
        F32_MUL,
        param_ops(0),
        F32_MUL,
    )
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops)


def channel_swap_module(spec: ModuleSpec) -> bytes:
    kernel_ops = combine_ops(local_get(1), local_get(0))
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops)


def hard_clip_threshold_ops() -> bytes:
    return combine_ops(param_ops(0), f32_const(0.05), F32_MAX)


def hard_clip_module(spec: ModuleSpec) -> bytes:
    threshold = hard_clip_threshold_ops()
    kernel_ops = combine_ops(
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
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops)


def bpm_ducker_module(spec: ModuleSpec) -> bytes:
    kernel_ops = combine_ops(
        local_get(0),
        f32_const(120.0),
        tempo_ops(),
        F32_DIV,
        F32_MUL,
        local_get(1),
        f32_const(120.0),
        tempo_ops(),
        F32_DIV,
        F32_MUL,
    )
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops)


def stateful_bias_module(spec: ModuleSpec) -> bytes:
    globals_ = [GlobalDef(F32, True, f32_const(0.0))]
    reset_ops = combine_ops(f32_const(0.0), global_set(0))
    kernel_ops = combine_ops(
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
    return make_standard_module(spec, globals_=globals_, kernel_ops=kernel_ops, reset_ops=reset_ops)


def resource_scaler_module(spec: ModuleSpec) -> bytes:
    # Copies the resource's first byte into scratch once, in prepare; 0 when no file is chosen.
    prepare_ops = combine_ops(
        i32_const(0),
        i32_const(0),
        i32_store(MEM_RESOURCE_SCRATCH),
        resource_handle_ops(0),
        i32_const(0),
        i32_const(MEM_RESOURCE_SCRATCH),
        i32_const(1),
        call(FN_RESOURCE_READ),
        DROP,
    )
    scale = combine_ops(
        i32_const(0),
        i32_load8_u(MEM_RESOURCE_SCRATCH),
        F32_CONVERT_I32_S,
        f32_const(64.0),
        F32_DIV,
    )
    kernel_ops = combine_ops(local_get(0), scale, F32_MUL, local_get(1), scale, F32_MUL)
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops, prepare_ops=prepare_ops)


def resource_size_gain_module(spec: ModuleSpec) -> bytes:
    # Keeps the resource's size in scratch, asked once in prepare; 0 when no file is chosen.
    prepare_ops = combine_ops(
        i32_const(0),
        resource_handle_ops(0),
        call(FN_RESOURCE_SIZE),
        i32_store(MEM_RESOURCE_SCRATCH),
    )
    scale = combine_ops(
        i32_const(0),
        i32_load(MEM_RESOURCE_SCRATCH),
        F32_CONVERT_I32_S,
        f32_const(64.0),
        F32_DIV,
    )
    kernel_ops = combine_ops(local_get(0), scale, F32_MUL, local_get(1), scale, F32_MUL)
    return make_standard_module(spec, globals_=[], kernel_ops=kernel_ops, prepare_ops=prepare_ops)


def module_specs() -> list[ModuleSpec]:
    return [
        ModuleSpec(
            identifier="gain",
            file_name="gain.wasm",
            description="Stereo gain controlled by a self-described guest parameter named gain.",
            category="utility",
            host_imports=("param_handle", "param_value"),
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
            host_imports=("param_handle", "param_value"),
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
            host_imports=("param_handle", "param_value"),
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
            host_imports=("param_handle", "param_value"),
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
                ),
            ),
            notes=("Useful for checking left/right output ordering and utility-style processing.",),
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
            host_imports=("param_handle", "param_value"),
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
                ),
            ),
            notes=("Demonstrates a simple effect rather than a utility process.",),
            build_module=hard_clip_module,
        ),
        ModuleSpec(
            identifier="bpm_ducker",
            file_name="bpm_ducker.wasm",
            description="Scales signal level by 120 / bpm using the host tempo.",
            category="modulation",
            host_imports=("tempo_bpm",),
            notes=("Demonstrates the tempo_bpm import.", "Assumes BPM stays above zero."),
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
            description="Scales both channels from the byte size of an external resource blob.",
            category="utility",
            host_imports=("resource_handle", "resource_size"),
            resources=(
                ResourceDoc(
                    identifier="payload",
                    title="Payload",
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
            description="Scales both channels from the first byte of an external resource blob.",
            category="utility",
            host_imports=("resource_handle", "resource_read"),
            resources=(
                ResourceDoc(
                    identifier="scale",
                    title="Scale Data",
                    file_name="resource_scaler_scale.bin",
                    description="First byte is interpreted as a gain numerator and divided by 64. Default example byte: 32.",
                ),
            ),
            notes=("Useful for validating resource_read and resources bound by id.",),
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
            }
        )

    node: dict[str, object] = {
        "type": "wasm_host",
        "category": spec.category,
        "label": spec.identifier,
        "params": {param.identifier: param.default for param in spec.params},
        "resources": resources,
    }
    if spec.resources:
        # Which resource id each node resource holds, "" for the module itself; without it the
        # files are taken in the order the descriptor declares the resources.
        resource_ids = [""] + [resource.identifier for resource in spec.resources]
        node["config"] = {"wasmResourceIds": json.dumps(resource_ids, separators=(",", ":"))}
    return node


def manifest_for(specs: Sequence[ModuleSpec]) -> dict[str, object]:
    host_abi = {
        "abiVersion": ABI_VERSION,
        "exports": [
            "audiofx_abi_version() -> i32 (returns 2)",
            "audiofx_prepare(sampleRate: f32, maxBlockSize: i32) -> i32 (0 on success)",
            "audiofx_buffer_ptr() -> i32 (the in-place I/O buffer: maxBlockSize left samples, then as many right)",
            "audiofx_process(frames: i32) -> void (processes frames in place in the I/O buffer)",
            "audiofx_reset() -> void",
            "audiofx_get_latency_samples() -> i32",
            "memory",
            "audiofx_descriptor_ptr() -> i32",
            "audiofx_descriptor_len() -> i32",
        ],
        "imports": [
            "host.param_handle(idPtr: i32, idLen: i32) -> i32",
            "host.param_value(handle: i32) -> f32",
            "host.tempo_bpm() -> f32",
            "host.resource_handle(idPtr: i32, idLen: i32) -> i32",
            "host.resource_size(handle: i32) -> i32",
            "host.resource_read(handle: i32, offset: i32, dstPtr: i32, len: i32) -> i32",
        ],
        "descriptorBlobFormat": "UTF-8 newline-delimited key=value entries stored in guest memory, keyed by id: param.<id>.<field>, resource.<id>.<field>.",
        "monoInputBehavior": "If only one input channel is present, the host mirrors it to both channels of the I/O buffer.",
        "generatedLayout": "Each module runs a per-sample kernel over the block, reading every param once per block. All six host imports are imported.",
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
                    }
                    for param in spec.params
                ],
                "resources": [
                    {
                        "id": resource.identifier,
                        "title": resource.title,
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
            "description": "Raw WASM guest-module generator for Custom Effect ABI 2.",
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