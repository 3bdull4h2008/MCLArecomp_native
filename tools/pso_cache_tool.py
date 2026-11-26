#!/usr/bin/env python3
"""Verify, compact, package, and gate MCLA MPSC v1 PSO-description caches.

Capture recipe for a shipping title seed (title route only):
  1. Choose a fresh MCLA_PSO_CACHE path that does not exist, for example
     build/pso_title_capture.bin. Never point it at an accumulated cache.
  2. Run the title with MCLA_NO_PSO_PREWARM=1 MCLA_NO_PSO_SEED=1
     MCLA_FIXED_SEED=0x2 MCLA_LOG_LEVEL=2 MCLA_PERF_STAGES=1.
  3. Stop after the title/menu route, before driving or any map transition.
  4. Run `python tools/pso_cache_tool.py make-seed build/pso_title_capture.bin
     --exe build/mcla.exe --route title-fixed-seed-0x2`.

Never seed from an accumulated driving/map cache. The seed is a release artifact,
not a Git artifact, and the game opens it read-only.
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

TOOL_VERSION = "1.0"
FILE_VERSION = 1
TRANSLATOR_VERSION = 1
DXC_VERSION = 1
HEADER_BYTES = 12
RECORD_HEADER_BYTES = 16
MAX_RECORDS = 65536
MAX_RECORD_BYTES = 16 * 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_INPUT_ELEMENTS = 64
MAX_SEMANTIC_BYTES = 128
MAX_BLOB_BYTES = 8 * 1024 * 1024
MAX_RESOURCES = 256
MAX_BINDINGS = 64
MAX_RENDER_TARGETS = 8
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
UINT64_MASK = 0xFFFFFFFFFFFFFFFF


class CacheError(Exception):
    pass


def fnv1a(data: bytes, seed: int = 0) -> int:
    value = (seed ^ FNV_OFFSET) & UINT64_MASK
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & UINT64_MASK
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def take(self, count: int) -> bytes:
        if count < 0 or count > len(self.data) - self.offset:
            raise CacheError("truncated payload")
        value = self.data[self.offset:self.offset + count]
        self.offset += count
        return value

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]

    def blob(self) -> bytes:
        count = self.u32()
        if count > MAX_BLOB_BYTES:
            raise CacheError("blob exceeds format bound")
        return self.take(count)

    def string(self) -> str:
        count = self.u32()
        if count > MAX_SEMANTIC_BYTES:
            raise CacheError("semantic exceeds format bound")
        raw = self.take(count)
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise CacheError("semantic is not UTF-8") from exc


class Writer:
    def __init__(self):
        self.data = bytearray()

    def u8(self, value: int) -> None:
        self.data += struct.pack("<B", value)

    def u32(self, value: int) -> None:
        self.data += struct.pack("<I", value)

    def u64(self, value: int) -> None:
        self.data += struct.pack("<Q", value)

    def f32(self, value: float) -> None:
        self.data += struct.pack("<f", value)

    def blob(self, value: bytes) -> None:
        self.u32(len(value))
        self.data += value

    def string(self, value: str) -> None:
        raw = value.encode("utf-8")
        self.u32(len(raw))
        self.data += raw


@dataclass(frozen=True)
class Raster:
    fill_mode: int
    cull_mode: int
    front_ccw: int
    depth_bias: int
    depth_bias_clamp: float
    slope_scaled_depth_bias: float
    depth_clip: int
    multisample: int
    antialiased_line: int
    forced_sample_count: int
    conservative: int


@dataclass(frozen=True)
class BlendTarget:
    blend_enable: int
    logic_enable: int
    src_blend: int
    dest_blend: int
    blend_op: int
    src_blend_alpha: int
    dest_blend_alpha: int
    blend_op_alpha: int
    logic_op: int
    write_mask: int


@dataclass(frozen=True)
class Blend:
    alpha_to_coverage: int
    independent: int
    targets: tuple[BlendTarget, ...]


@dataclass(frozen=True)
class StencilFace:
    fail_op: int
    depth_fail_op: int
    pass_op: int
    func: int


@dataclass(frozen=True)
class DepthStencil:
    depth_enable: int
    depth_write_mask: int
    depth_func: int
    stencil_enable: int
    stencil_read_mask: int
    stencil_write_mask: int
    front: StencilFace
    back: StencilFace


@dataclass(frozen=True)
class PipelineState:
    target_formats: tuple[int, ...]
    depth_format: int
    blend_state: int
    raster_state: int
    depth_state: int
    topology: int
    sample_count: int
    geometry_stage: int
    derivation: int
    flags: int
    viewport_y_flipped: bool
    raster: Raster
    blend: Blend
    depth: DepthStencil


@dataclass(frozen=True)
class InputElement:
    semantic: str
    semantic_index: int
    format: int
    input_slot: int
    aligned_offset: int
    input_class: int
    instance_step: int


@dataclass(frozen=True)
class ResourceBinding:
    type: int
    register: int
    space: int
    byte_size: int
    first_variable_register: int


@dataclass(frozen=True)
class ConstantBinding:
    shader_register: int
    space: int
    required_bytes: int
    root_parameter: int
    valid: int


@dataclass(frozen=True)
class PipelineDesc:
    stream_entries: int
    stream_strides: int
    rasterized_stream: int
    raster: Raster
    blend: Blend
    depth: DepthStencil
    sample_mask: int
    inputs: tuple[InputElement, ...]
    ib_strip_cut: int
    topology_type: int
    render_target_count: int
    rtv_formats: tuple[int, ...]
    dsv_format: int
    sample_count: int
    sample_quality: int
    node_mask: int
    cached_size: int
    flags: int


@dataclass(frozen=True)
class Record:
    key: tuple
    state: PipelineState
    desc: PipelineDesc
    vs_dxil: bytes
    ps_dxil: bytes
    ds_dxil: bytes
    hs_dxil: bytes
    gs_dxil: bytes
    vs_resources: tuple[ResourceBinding, ...]
    ps_resources: tuple[ResourceBinding, ...]
    bindings: tuple[ConstantBinding, ...]
    root_hash: int
    translator_version: int
    dxc_version: int
    payload: bytes


@dataclass
class ParseResult:
    records: list[Record]
    examined: int
    duplicates: int
    size: int
    root_hash: int | None


def read_state(reader: Reader) -> PipelineState:
    targets = tuple(reader.u32() for _ in range(MAX_RENDER_TARGETS))
    depth_format = reader.u32()
    blend_state = reader.u32()
    raster_state = reader.u32()
    depth_state = reader.u32()
    topology = reader.u8()
    sample_count = reader.u8()
    geometry_stage = reader.u8()
    derivation = reader.u8()
    flags = reader.u8()
    viewport_y_flipped = reader.u8()
    if viewport_y_flipped > 1:
        raise CacheError("viewport-Y flag is not boolean")
    raster = read_raster(reader)
    blend = read_blend(reader)
    depth = read_depth(reader)
    return PipelineState(targets, depth_format, blend_state, raster_state, depth_state,
                         topology, sample_count, geometry_stage, derivation, flags,
                         bool(viewport_y_flipped), raster, blend, depth)


def read_raster(reader: Reader) -> Raster:
    fill_mode = reader.u32()
    cull_mode = reader.u32()
    front_ccw = reader.u32()
    depth_bias_bits = reader.u32()
    depth_bias = depth_bias_bits if depth_bias_bits < 0x80000000 else depth_bias_bits - 0x100000000
    depth_bias_clamp = reader.f32()
    slope = reader.f32()
    depth_clip = reader.u32()
    multisample = reader.u32()
    antialiased = reader.u32()
    forced_samples = reader.u32()
    conservative = reader.u32()
    return Raster(fill_mode, cull_mode, front_ccw, depth_bias, depth_bias_clamp, slope,
                  depth_clip, multisample, antialiased, forced_samples, conservative)


def read_blend(reader: Reader) -> Blend:
    alpha_to_coverage = reader.u32()
    independent = reader.u32()
    targets = []
    for _ in range(MAX_RENDER_TARGETS):
        targets.append(BlendTarget(
            reader.u32(), reader.u32(), reader.u32(), reader.u32(), reader.u32(),
            reader.u32(), reader.u32(), reader.u32(), reader.u32(), reader.u32()))
    return Blend(alpha_to_coverage, independent, tuple(targets))


def read_depth(reader: Reader) -> DepthStencil:
    depth_enable = reader.u32()
    depth_write_mask = reader.u32()
    depth_func = reader.u32()
    stencil_enable = reader.u32()
    stencil_read_mask = reader.u8()
    stencil_write_mask = reader.u8()

    def face() -> StencilFace:
        return StencilFace(reader.u32(), reader.u32(), reader.u32(), reader.u32())

    return DepthStencil(depth_enable, depth_write_mask, depth_func, stencil_enable,
                        stencil_read_mask, stencil_write_mask, face(), face())


def read_resources(reader: Reader) -> tuple[ResourceBinding, ...]:
    count = reader.u32()
    if count > MAX_RESOURCES:
        raise CacheError("resource count exceeds format bound")
    return tuple(ResourceBinding(*(reader.u32() for _ in range(5))) for _ in range(count))


def hash_input_layout(inputs: tuple[InputElement, ...]) -> int:
    value = FNV_OFFSET
    for item in inputs:
        fields = struct.pack("<5I", item.semantic_index, item.format, item.aligned_offset,
                             item.input_slot, item.input_class)
        value = fnv1a(fields, value)
        value = fnv1a(struct.pack("<I", item.instance_step), value)
        semantic = item.semantic.encode("utf-8")
        value = fnv1a(struct.pack("<I", len(semantic)), value)
        value = fnv1a(semantic, value)
    return fnv1a(struct.pack("<I", len(inputs)), value)


def parse_payload(payload: bytes) -> Record:
    reader = Reader(payload)
    hashes = tuple(reader.u64() for _ in range(6))
    refuse_mask = reader.u32()
    reserved = reader.u32()
    state = read_state(reader)
    vs_dxil = reader.blob()
    ps_dxil = reader.blob()
    ds_dxil = reader.blob()
    hs_dxil = reader.blob()
    gs_dxil = reader.blob()
    stream_entries = reader.u32()
    stream_strides = reader.u32()
    rasterized_stream = reader.u32()
    raster = read_raster(reader)
    blend = read_blend(reader)
    depth = read_depth(reader)
    sample_mask = reader.u32()
    input_count = reader.u32()
    if input_count == 0 or input_count > MAX_INPUT_ELEMENTS:
        raise CacheError("invalid input-layout count")
    inputs = []
    for _ in range(input_count):
        semantic = reader.string()
        if not semantic:
            raise CacheError("empty input semantic")
        inputs.append(InputElement(semantic, reader.u32(), reader.u32(), reader.u32(),
                                   reader.u32(), reader.u32(), reader.u32()))
    ib_strip_cut = reader.u32()
    topology_type = reader.u32()
    render_target_count = reader.u32()
    rtv_formats = tuple(reader.u32() for _ in range(MAX_RENDER_TARGETS))
    dsv_format = reader.u32()
    sample_count = reader.u32()
    sample_quality = reader.u32()
    node_mask = reader.u32()
    cached_size = reader.u64()
    if cached_size != 0:
        raise CacheError("driver PSO blob is forbidden")
    flags = reader.u32()
    root_hash = reader.u64()
    translator_version = reader.u32()
    dxc_version = reader.u32()
    vs_resources = read_resources(reader)
    ps_resources = read_resources(reader)
    binding_count = reader.u32()
    if binding_count == 0 or binding_count > MAX_BINDINGS:
        raise CacheError("invalid constant-bank binding count")
    bindings = tuple(ConstantBinding(*(reader.u32() for _ in range(5)))
                     for _ in range(binding_count))
    if reader.offset != len(payload):
        raise CacheError("trailing bytes inside PSO payload")
    desc = PipelineDesc(stream_entries, stream_strides, rasterized_stream, raster, blend,
                        depth, sample_mask, tuple(inputs), ib_strip_cut, topology_type,
                        render_target_count, rtv_formats, dsv_format, sample_count, sample_quality,
                        node_mask, cached_size, flags)
    key = hashes + (refuse_mask, reserved, state)
    return Record(key, state, desc, vs_dxil, ps_dxil, ds_dxil, hs_dxil, gs_dxil,
                  vs_resources, ps_resources, bindings, root_hash, translator_version,
                  dxc_version, payload)


def resources_fit_root(resources: Iterable[ResourceBinding]) -> bool:
    for resource in resources:
        if resource.type == 3:  # sampler
            if resource.space != 0 or resource.register >= 32:
                return False
        elif resource.type == 0:  # cbuffer
            if resource.space != 0 or resource.register != 0:
                return False
        elif resource.type in (2, 5, 7):  # texture / structured / byte-address
            if resource.type == 7 and resource.space == 1 and resource.register == 0:
                continue
            if resource.space != 0 or resource.register >= 32:
                return False
    return True


def zero_blend_write_masks(blend: Blend) -> Blend:
    targets = tuple(BlendTarget(t.blend_enable, t.logic_enable, t.src_blend, t.dest_blend,
                                t.blend_op, t.src_blend_alpha, t.dest_blend_alpha,
                                t.blend_op_alpha, t.logic_op, 0) for t in blend.targets)
    return Blend(blend.alpha_to_coverage, blend.independent, targets)


def validate_record(record: Record) -> None:
    if record.translator_version != TRANSLATOR_VERSION or record.dxc_version != DXC_VERSION:
        raise CacheError("cache format version mismatch")
    if record.root_hash == 0:
        raise CacheError("missing root-signature hash")
    if record.key[6] != 0 or record.key[7] != 0:
        raise CacheError("record key carries a refusal/reserved value")
    state = record.state
    if state.derivation not in (1, 2) or state.sample_count == 0:
        raise CacheError("record key has no derived state/sample count")
    if not record.vs_dxil:
        raise CacheError("record has no vertex DXIL")
    if record.ds_dxil or record.hs_dxil:
        raise CacheError("unsupported DS/HS stage")
    desc = record.desc
    if desc.stream_entries != 0 or desc.stream_strides != 0:
        raise CacheError("unsupported stream output")
    if desc.cached_size != 0:
        raise CacheError("driver PSO blob is forbidden")
    if not desc.inputs or hash_input_layout(desc.inputs) != record.key[5]:
        raise CacheError("input layout does not match the key")
    wants_geometry = state.geometry_stage != 0
    if wants_geometry != bool(record.gs_dxil):
        raise CacheError("geometry stage/key mismatch")
    if wants_geometry and state.geometry_stage != 1:
        raise CacheError("unknown geometry stage")
    if record.ps_dxil and len(record.ps_resources) > MAX_RESOURCES:
        raise CacheError("pixel reflection exceeds bounds")
    if not record.ps_dxil and record.ps_resources:
        raise CacheError("PS-less record carries pixel reflection")
    if not resources_fit_root(record.vs_resources) or not resources_fit_root(record.ps_resources):
        raise CacheError("reflection exceeds root signature")
    if not record.bindings:
        raise CacheError("record has no constant-bank binding")
    for binding in record.bindings:
        if binding.valid != 1 or binding.space != 0 or binding.shader_register != 0 or binding.root_parameter != 2:
            raise CacheError("invalid constant-bank binding")
    if desc.render_target_count > MAX_RENDER_TARGETS:
        raise CacheError("render-target count exceeds D3D12 limit")
    for index, format_value in enumerate(desc.rtv_formats):
        if (format_value != 0) != (index < desc.render_target_count):
            raise CacheError("render-target formats are not contiguous")
    if desc.render_target_count == 0 and (desc.dsv_format == 0 or record.ps_dxil):
        raise CacheError("zero render targets on a non-depth-only record")
    if desc.dsv_format == 0 and desc.depth.depth_enable:
        raise CacheError("depth enabled without DSV format")
    if desc.sample_count == 0 or desc.sample_count > 255:
        raise CacheError("invalid sample count")
    topology = {3: 0, 2: 1, 1: 2}.get(desc.topology_type)
    if topology is None:
        raise CacheError("unknown primitive topology type")
    if state.topology != topology or state.target_formats != desc.rtv_formats or \
            state.depth_format != desc.dsv_format or state.sample_count != desc.sample_count:
        raise CacheError("PSO description does not match PipelineKey")
    if state.raster != desc.raster:
        raise CacheError("PSO description does not match PipelineKey")
    expected_blend = desc.blend
    key_blend = state.blend
    if not record.ps_dxil:
        expected_blend = zero_blend_write_masks(expected_blend)
        key_blend = zero_blend_write_masks(key_blend)
    if expected_blend != key_blend or state.depth != desc.depth:
        raise CacheError("PSO description does not match PipelineKey")


def parse_cache(path: Path, expected_root_hash: int | None = None,
                expected_translator: int = TRANSLATOR_VERSION,
                expected_dxc: int = DXC_VERSION) -> ParseResult:
    data = path.read_bytes()
    if len(data) > MAX_FILE_BYTES:
        raise CacheError("cache exceeds file-size bound")
    if len(data) < HEADER_BYTES:
        raise CacheError("truncated MPSC header")
    magic, version, header_bytes = struct.unpack_from("<4sII", data, 0)
    if magic != b"MPSC":
        raise CacheError("bad MPSC magic")
    if version != FILE_VERSION or expected_translator != TRANSLATOR_VERSION or expected_dxc != DXC_VERSION:
        raise CacheError("unsupported cache/translator/DXC version")
    if header_bytes != HEADER_BYTES:
        raise CacheError("bad MPSC header size")
    offset = HEADER_BYTES
    records: list[Record] = []
    by_key: dict[tuple, Record] = {}
    duplicates = 0
    examined = 0
    root_hash: int | None = None
    while offset < len(data):
        if len(data) - offset < RECORD_HEADER_BYTES:
            raise CacheError("truncated record header/trailing bytes")
        record_magic, payload_size, checksum = struct.unpack_from("<4sIQ", data, offset)
        if record_magic != b"PSOR":
            raise CacheError("bad PSOR record magic")
        if payload_size > MAX_RECORD_BYTES or payload_size > len(data) - offset - RECORD_HEADER_BYTES:
            raise CacheError("truncated or oversized PSOR payload")
        start = offset + RECORD_HEADER_BYTES
        payload = data[start:start + payload_size]
        offset = start + payload_size
        examined += 1
        if examined > MAX_RECORDS:
            raise CacheError("record count exceeds format bound")
        if fnv1a(payload) != checksum:
            raise CacheError(f"checksum mismatch at record {examined}")
        record = parse_payload(payload)
        validate_record(record)
        if record.translator_version != expected_translator or record.dxc_version != expected_dxc:
            raise CacheError("translator/DXC version mismatch")
        if expected_root_hash is not None and record.root_hash != expected_root_hash:
            raise CacheError("root-signature hash mismatch")
        if root_hash is None:
            root_hash = record.root_hash
        elif root_hash != record.root_hash:
            raise CacheError("conflicting root-signature hashes")
        existing = by_key.get(record.key)
        if existing is not None:
            if existing.payload != payload:
                raise CacheError(f"conflicting duplicate full key at record {examined}")
            duplicates += 1
            continue
        by_key[record.key] = record
        records.append(record)
    return ParseResult(records, examined, duplicates, len(data), root_hash)


def encode_cache(records: Iterable[Record]) -> bytes:
    output = bytearray(struct.pack("<4sII", b"MPSC", FILE_VERSION, HEADER_BYTES))
    for record in records:
        payload = record.payload
        output += struct.pack("<4sIQ", b"PSOR", len(payload), fnv1a(payload))
        output += payload
    return bytes(output)


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def compact_file(source: Path, output: Path, expected_root_hash: int | None = None,
                 expected_translator: int = TRANSLATOR_VERSION,
                 expected_dxc: int = DXC_VERSION) -> ParseResult:
    result = parse_cache(source, expected_root_hash, expected_translator, expected_dxc)
    atomic_write(output, encode_cache(result.records))
    return result


def parse_hash(value: str) -> int:
    text = value.strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    if not text or any(c not in "0123456789abcdef" for c in text):
        raise ValueError("root hash must be hexadecimal")
    number = int(text, 16)
    if number <= 0 or number > UINT64_MASK:
        raise ValueError("root hash must be a non-zero 64-bit value")
    return number


def make_seed(source: Path, exe: Path, route: str, output: Path, manifest_path: Path,
              expected_root_hash: int | None = None,
              expected_translator: int = TRANSLATOR_VERSION,
              expected_dxc: int = DXC_VERSION) -> dict:
    if not route.strip():
        raise ValueError("route is required")
    result = parse_cache(source, expected_root_hash, expected_translator, expected_dxc)
    if not result.records:
        raise CacheError("refusing to package an empty seed")
    if result.root_hash is None:
        raise CacheError("seed has no root-signature hash")
    seed_bytes = encode_cache(result.records)
    exe_bytes = exe.stat().st_size
    shipping_exe_hash = sha256_file(exe)
    atomic_write(output, seed_bytes)
    manifest = {
        "format": "MPSC",
        "file_version": FILE_VERSION,
        "translator_version": TRANSLATOR_VERSION,
        "dxc_version": DXC_VERSION,
        "root_hash": f"0x{result.root_hash:016X}",
        "records": len(result.records),
        "duplicates_collapsed": result.duplicates,
        "source_records": result.examined,
        "source_bytes": result.size,
        "seed_bytes": len(seed_bytes),
        "exe_bytes": exe_bytes,
        "seed_sha256": hashlib.sha256(seed_bytes).hexdigest(),
        "shipping_exe_sha256": shipping_exe_hash,
        "tool_version": TOOL_VERSION,
        "route": route,
        "timestamp_utc": _datetime.datetime.now(_datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    atomic_write(manifest_path, (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    return manifest


def _field_int(line: str, name: str) -> int | None:
    match = re.search(rf"(?:^|\s){re.escape(name)}=(-?\d+)(?:\s|$)", line)
    return int(match.group(1)) if match else None


def gate_log(path: Path) -> tuple[bool, list[str], dict[str, int]]:
    text = path.read_bytes().decode("utf-8", errors="replace")
    lines = text.splitlines()
    reasons: list[str] = []
    config_ok = any("PSO-SEED-CONFIG" in line and _field_int(line, "enabled") == 1 for line in lines)
    if not config_ok:
        reasons.append("missing enabled PSO-SEED-CONFIG")
    summaries = [line for line in lines if "PSO-PREWARM #0 disabled=0" in line]
    if not summaries:
        reasons.append("missing enabled PSO-PREWARM summary")
        summary = ""
    else:
        summary = summaries[-1]
    values: dict[str, int] = {}
    for name in ("records", "examined", "created", "failed", "stale", "corrupt", "threads",
                 "seed", "user", "merged", "conflicts", "rejected", "duplicates"):
        value = _field_int(summary, name)
        values[name] = -1 if value is None else value
    if summary:
        if values["user"] != 0:
            reasons.append("user cache is not cold (user != 0)")
        if values["seed"] <= 0:
            reasons.append("no accepted seed records")
        if values["merged"] != values["seed"] or values["records"] != values["merged"]:
            reasons.append("cold merge is not seed-only")
        if values["created"] != values["merged"]:
            reasons.append("not every merged PSO prewarmed")
        for name in ("failed", "stale", "corrupt", "conflicts", "rejected"):
            if values[name] != 0:
                reasons.append(f"{name} != 0")
    hits = [line for line in lines if "PC-PSO-PREWARM-HIT" in line]
    seed_hits = [line for line in hits if re.search(r"(?:^|\s)source=seed(?:\s|$)", line)]
    if not seed_hits:
        reasons.append("no source=seed PC-PSO-PREWARM-HIT")
    waits = []
    for line in lines:
        match = re.search(r"psoWait\[n=(\d+) totMs=(\d+) maxMs=(\d+)\]", line)
        if match:
            waits.append(tuple(int(value) for value in match.groups()))
    if not waits:
        reasons.append("no PERF-STAGES psoWait measurement")
    wait_count = sum(item[0] for item in waits)
    wait_total = sum(item[1] for item in waits)
    wait_max = max((item[2] for item in waits), default=0)
    values["prewarm_hits"] = len(hits)
    values["seed_hits"] = len(seed_hits)
    values["pso_wait_n"] = wait_count
    values["pso_wait_tot_ms"] = wait_total
    values["pso_wait_max_ms"] = wait_max
    if wait_count != 0 or wait_total != 0 or wait_max != 0:
        reasons.append("PSO wait observed")
    for token in ("fatal error", "c0000005", "PC-PSO-CREATE-FAILED", "PC-PSO-WAIT-TIMEOUT", "PSO-SEED-REJECTED"):
        if token.lower() in text.lower():
            reasons.append(f"forbidden token: {token}")
    return not reasons, reasons, values


def put_raster(writer: Writer, raster: Raster) -> None:
    writer.u32(raster.fill_mode)
    writer.u32(raster.cull_mode)
    writer.u32(raster.front_ccw)
    writer.u32(raster.depth_bias & 0xFFFFFFFF)
    writer.f32(raster.depth_bias_clamp)
    writer.f32(raster.slope_scaled_depth_bias)
    writer.u32(raster.depth_clip)
    writer.u32(raster.multisample)
    writer.u32(raster.antialiased_line)
    writer.u32(raster.forced_sample_count)
    writer.u32(raster.conservative)


def put_blend(writer: Writer, blend: Blend) -> None:
    writer.u32(blend.alpha_to_coverage)
    writer.u32(blend.independent)
    for target in blend.targets:
        for value in (target.blend_enable, target.logic_enable, target.src_blend, target.dest_blend,
                      target.blend_op, target.src_blend_alpha, target.dest_blend_alpha,
                      target.blend_op_alpha, target.logic_op, target.write_mask):
            writer.u32(value)


def put_depth(writer: Writer, depth: DepthStencil) -> None:
    writer.u32(depth.depth_enable)
    writer.u32(depth.depth_write_mask)
    writer.u32(depth.depth_func)
    writer.u32(depth.stencil_enable)
    writer.u8(depth.stencil_read_mask)
    writer.u8(depth.stencil_write_mask)
    for face in (depth.front, depth.back):
        writer.u32(face.fail_op)
        writer.u32(face.depth_fail_op)
        writer.u32(face.pass_op)
        writer.u32(face.func)


def fixture_state() -> PipelineState:
    target = BlendTarget(0, 0, 1, 1, 1, 1, 1, 1, 0, 0xF)
    blend = Blend(0, 0, tuple(target for _ in range(MAX_RENDER_TARGETS)))
    raster = Raster(2, 3, 0, 0, 0.0, 0.0, 1, 0, 0, 0, 0)
    face = StencilFace(1, 1, 1, 1)
    depth = DepthStencil(1, 1, 4, 0, 0xFF, 0xFF, face, face)
    formats = (28,) + (0,) * (MAX_RENDER_TARGETS - 1)
    return PipelineState(formats, 40, 0, 0, 0, 0, 1, 0, 1, 0, False,
                         raster, blend, depth)


def fixture_payload(seed: int = 1, *, sample_mask: int = 0xFFFFFFFF,
                    ds_dxil: bytes = b"", hs_dxil: bytes = b"", cached_size: int = 0,
                    stream_entries: int = 0, translator: int = TRANSLATOR_VERSION,
                    dxc: int = DXC_VERSION, root_hash: int = 0x0123456789ABCDEF,
                    bad_layout_hash: bool = False, trailing: bytes = b"") -> bytes:
    state = fixture_state()
    inputs = (InputElement("POSITION", 0, 2, 0, 0, 0, 0),)
    layout_hash = hash_input_layout(inputs) ^ (1 if bad_layout_hash else 0)
    writer = Writer()
    for value in (seed, seed + 1, seed + 2, seed + 3, seed + 4, layout_hash):
        writer.u64(value)
    writer.u32(0)
    writer.u32(0)
    for value in state.target_formats:
        writer.u32(value)
    writer.u32(state.depth_format)
    writer.u32(state.blend_state)
    writer.u32(state.raster_state)
    writer.u32(state.depth_state)
    for value in (state.topology, state.sample_count, state.geometry_stage, state.derivation,
                  state.flags, int(state.viewport_y_flipped)):
        writer.u8(value)
    put_raster(writer, state.raster)
    put_blend(writer, state.blend)
    put_depth(writer, state.depth)
    writer.blob(b"VS-DXIL")
    writer.blob(b"PS-DXIL")
    writer.blob(ds_dxil)
    writer.blob(hs_dxil)
    writer.blob(b"")
    writer.u32(stream_entries)
    writer.u32(0)
    writer.u32(0)
    put_raster(writer, state.raster)
    put_blend(writer, state.blend)
    put_depth(writer, state.depth)
    writer.u32(sample_mask)
    writer.u32(len(inputs))
    for item in inputs:
        writer.string(item.semantic)
        writer.u32(item.semantic_index)
        writer.u32(item.format)
        writer.u32(item.input_slot)
        writer.u32(item.aligned_offset)
        writer.u32(item.input_class)
        writer.u32(item.instance_step)
    writer.u32(0)
    writer.u32(3)
    writer.u32(1)
    for value in state.target_formats:
        writer.u32(value)
    writer.u32(state.depth_format)
    writer.u32(1)
    writer.u32(0)
    writer.u32(0)
    writer.u64(cached_size)
    writer.u32(0)
    writer.u64(root_hash)
    writer.u32(translator)
    writer.u32(dxc)
    writer.u32(1)
    for value in (0, 0, 0, 256, 0):
        writer.u32(value)
    writer.u32(0)
    writer.u32(1)
    for value in (0, 0, 256, 2, 1):
        writer.u32(value)
    writer.data += trailing
    return bytes(writer.data)


def fixture_cache(payloads: Iterable[bytes]) -> bytes:
    output = bytearray(struct.pack("<4sII", b"MPSC", FILE_VERSION, HEADER_BYTES))
    for payload in payloads:
        output += struct.pack("<4sIQ", b"PSOR", len(payload), fnv1a(payload))
        output += payload
    return bytes(output)


def _expect_failure(label: str, action) -> None:
    try:
        action()
    except (CacheError, OSError, ValueError):
        return
    raise AssertionError(f"known-negative fixture did not fail: {label}")


def run_selftest() -> None:
    with tempfile.TemporaryDirectory(prefix="mcla-pso-tool-") as temporary:
        root = Path(temporary)
        positive = fixture_cache((fixture_payload(1), fixture_payload(2), fixture_payload(1)))
        positive_path = root / "positive.bin"
        positive_path.write_bytes(positive)
        parsed = parse_cache(positive_path)
        assert len(parsed.records) == 2 and parsed.duplicates == 1
        assert parsed.root_hash == 0x0123456789ABCDEF

        bad_magic = b"BAD!" + positive[4:]
        bad_version = positive[:4] + struct.pack("<I", 2) + positive[8:]
        bad_checksum = positive[:-1] + bytes((positive[-1] ^ 1,))
        negatives = {
            "bad magic": bad_magic,
            "bad version": bad_version,
            "truncation": positive[:-1],
            "checksum": bad_checksum,
            "trailing bytes": positive + b"x",
            "translator mismatch": fixture_cache((fixture_payload(1, translator=2),)),
            "dxc mismatch": fixture_cache((fixture_payload(1, dxc=2),)),
            "driver blob": fixture_cache((fixture_payload(1, cached_size=1),)),
            "unsupported DS": fixture_cache((fixture_payload(1, ds_dxil=b"DS"),)),
            "unsupported HS": fixture_cache((fixture_payload(1, hs_dxil=b"HS"),)),
            "unsupported stream output": fixture_cache((fixture_payload(1, stream_entries=1),)),
            "conflicting duplicate": fixture_cache((fixture_payload(1), fixture_payload(1, sample_mask=0))),
            "payload trailing bytes": fixture_cache((fixture_payload(1, trailing=b"x"),)),
            "layout/key mismatch": fixture_cache((fixture_payload(1, bad_layout_hash=True),)),
        }
        for label, data in negatives.items():
            safe_label = re.sub(r"[^A-Za-z0-9_-]+", "-", label)
            path = root / f"negative-{safe_label}.bin"
            path.write_bytes(data)
            _expect_failure(label, lambda path=path: parse_cache(path))
        _expect_failure("expected root hash", lambda: parse_cache(positive_path, 0x2222222222222222))

        compacted = root / "compacted.bin"
        compact_result = compact_file(positive_path, compacted)
        assert len(compact_result.records) == 2 and compacted.stat().st_size < positive_path.stat().st_size
        assert positive_path.read_bytes() == positive
        corrupt = root / "corrupt.bin"
        corrupt_bytes = bad_checksum
        corrupt.write_bytes(corrupt_bytes)
        _expect_failure("compact preserves source", lambda: compact_file(corrupt, corrupt))
        assert corrupt.read_bytes() == corrupt_bytes

        exe = root / "mcla.exe"
        exe.write_bytes(b"shipping-exe-fixture")
        seed = root / "pso_title_seed.bin"
        manifest_path = root / "pso_title_seed.json"
        manifest = make_seed(positive_path, exe, "title-fixed-seed-0x2", seed, manifest_path)
        assert manifest["records"] == 2 and manifest["route"] == "title-fixed-seed-0x2"
        assert manifest["seed_sha256"] == sha256_file(seed)
        assert manifest["shipping_exe_sha256"] == sha256_file(exe)
        assert json.loads(manifest_path.read_text(encoding="utf-8")) == manifest

        good_log = root / "good.log"
        good_log.write_text(
            "PSO-SEED-CONFIG #0 enabled=1 path=seed seedOnly=0 userPersistence=1 positiveControl=1\n"
            "PSO-PREWARM #0 disabled=0 records=2 examined=2 created=2 failed=0 stale=0 corrupt=0 "
            "threads=4 seed=2 user=0 merged=2 conflicts=0 rejected=0 duplicates=0 positiveControl=1\n"
            "PC-PSO-PREWARM-HIT #1 key=1 source=seed totalHits=1 prewarmFailed=0 persistFailed=0 -- no psoWait\n"
            "PERF-STAGES psoWait[n=0 totMs=0 maxMs=0]\n", encoding="utf-8")
        passed, reasons, _ = gate_log(good_log)
        assert passed and not reasons
        no_hit = root / "no-hit.log"
        no_hit.write_text(good_log.read_text(encoding="utf-8").replace("PC-PSO-PREWARM-HIT", "OTHER"), encoding="utf-8")
        passed, reasons, _ = gate_log(no_hit)
        assert not passed and any("PREWARM-HIT" in reason for reason in reasons)
        waited = root / "waited.log"
        waited.write_text(good_log.read_text(encoding="utf-8").replace("psoWait[n=0 totMs=0 maxMs=0]",
                                                                      "psoWait[n=1 totMs=25 maxMs=25]"), encoding="utf-8")
        passed, reasons, _ = gate_log(waited)
        assert not passed and "PSO wait observed" in reasons
    print("SELFTEST OK")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--selftest", action="store_true", help="run known-positive/known-negative fixtures and exit")
    subparsers = parser.add_subparsers(dest="command")

    verify = subparsers.add_parser("verify", help="strictly verify an MPSC v1 cache")
    verify.add_argument("cache", type=Path)
    verify.add_argument("--root-hash", type=parse_hash)
    verify.add_argument("--translator-version", type=lambda value: int(value, 0), default=TRANSLATOR_VERSION)
    verify.add_argument("--dxc-version", type=lambda value: int(value, 0), default=DXC_VERSION)
    verify.add_argument("--json", action="store_true")

    compact = subparsers.add_parser("compact", help="collapse identical duplicates and write atomically")
    compact.add_argument("cache", type=Path)
    compact.add_argument("-o", "--output", type=Path)
    compact.add_argument("--root-hash", type=parse_hash)
    compact.add_argument("--translator-version", type=lambda value: int(value, 0), default=TRANSLATOR_VERSION)
    compact.add_argument("--dxc-version", type=lambda value: int(value, 0), default=DXC_VERSION)

    seed = subparsers.add_parser("make-seed", help="package a verified title-only cache and JSON manifest")
    seed.add_argument("cache", type=Path)
    seed.add_argument("--exe", required=True, type=Path)
    seed.add_argument("--route", required=True)
    seed.add_argument("-o", "--output", type=Path, default=Path("pso_title_seed.bin"))
    seed.add_argument("--manifest", type=Path)
    seed.add_argument("--root-hash", type=parse_hash)
    seed.add_argument("--translator-version", type=lambda value: int(value, 0), default=TRANSLATOR_VERSION)
    seed.add_argument("--dxc-version", type=lambda value: int(value, 0), default=DXC_VERSION)

    gate = subparsers.add_parser("gate", help="enforce the cold-user-cache title-seed acceptance criteria")
    gate.add_argument("log", type=Path)
    subparsers.add_parser("selftest", help="run known-positive/known-negative fixtures and exit")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest or args.command == "selftest":
        try:
            run_selftest()
            return 0
        except Exception as exc:
            print(f"SELFTEST FAILED: {exc}", file=sys.stderr)
            return 1
    if not args.command:
        parser.error("a command is required")
    try:
        if args.command == "verify":
            result = parse_cache(args.cache, args.root_hash, args.translator_version, args.dxc_version)
            report = {
                "ok": True,
                "path": str(args.cache),
                "records": len(result.records),
                "examined": result.examined,
                "duplicates": result.duplicates,
                "bytes": result.size,
                "root_hash": None if result.root_hash is None else f"0x{result.root_hash:016X}",
                "file_version": FILE_VERSION,
                "translator_version": TRANSLATOR_VERSION,
                "dxc_version": DXC_VERSION,
            }
            print(json.dumps(report, indent=2, sort_keys=True) if args.json else
                  f"VERIFY OK path={args.cache} records={len(result.records)} duplicates={result.duplicates} "
                  f"bytes={result.size} rootHash={report['root_hash']} translator={TRANSLATOR_VERSION} dxc={DXC_VERSION}")
        elif args.command == "compact":
            output = args.output or args.cache
            result = compact_file(args.cache, output, args.root_hash, args.translator_version, args.dxc_version)
            print(f"COMPACT OK source={args.cache} output={output} records={len(result.records)} "
                  f"duplicates={result.duplicates} bytes={output.stat().st_size}")
        elif args.command == "make-seed":
            manifest_path = args.manifest or args.output.with_suffix(".json")
            manifest = make_seed(args.cache, args.exe, args.route, args.output, manifest_path,
                                 args.root_hash, args.translator_version, args.dxc_version)
            print(f"MAKE-SEED OK seed={args.output} manifest={manifest_path} records={manifest['records']} "
                  f"rootHash={manifest['root_hash']} seedSha256={manifest['seed_sha256']}")
        elif args.command == "gate":
            passed, reasons, values = gate_log(args.log)
            print(f"PSO-SEED-GATE {'PASS' if passed else 'FAIL'} log={args.log} "
                  f"prewarmHits={values.get('prewarm_hits', 0)} seedHits={values.get('seed_hits', 0)} "
                  f"psoWaitN={values.get('pso_wait_n', 0)} psoWaitMaxMs={values.get('pso_wait_max_ms', 0)}")
            for reason in reasons:
                print(f"  - {reason}")
            return 0 if passed else 1
        return 0
    except (CacheError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
