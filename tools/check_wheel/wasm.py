from __future__ import annotations

from collections.abc import Iterator
from enum import IntEnum
from typing import NamedTuple

WASM_MAGIC = b"\0asm\x01\0\0\0"
WASM_SECTION_IMPORT = 2
WASM_SECTION_MEMORY = 5
WASM_SECTION_EXPORT = 7
WASM_LIMITS_HAS_MAXIMUM = 0x01
WASM_LIMITS_64_BIT = 0x04
WASM_LIMITS_HAS_PAGE_SIZE = 0x08


def wasm_machine(binary: bytes) -> str:
    memories = list(wasm_memories(binary))

    if not memories:
        raise ValueError("the module declares and imports no memory, so its address width is unknown")

    return "wasm64" if any(flags & WASM_LIMITS_64_BIT for flags in memories) else "wasm32"


class WasmImport(IntEnum):
    FUNCTION = 0x00
    TABLE = 0x01
    MEMORY = 0x02
    GLOBAL = 0x03
    TAG = 0x04


class WasmSection(NamedTuple):
    kind: int
    body: memoryview


def leb128(binary: bytes | memoryview, offset: int) -> tuple[int, int]:
    value = 0

    for shift in range(0, 64, 7):
        byte = wasm_byte(binary, offset)
        value |= (byte & 0x7F) << shift
        offset += 1

        if byte < 0x80:
            return value, offset

    raise ValueError(f"LEB128 at {offset} is longer than 64 bits")


def wasm_sections(binary: bytes) -> Iterator[WasmSection]:
    offset = len(WASM_MAGIC)

    while offset < len(binary):
        size, start = leb128(binary, offset + 1)

        if start + size > len(binary):
            raise ValueError(f"section {binary[offset]} at {offset} has size {size}")

        yield WasmSection(binary[offset], memoryview(binary)[start : start + size])
        offset = start + size


def wasm_name(body: memoryview, offset: int) -> tuple[str, int]:
    length, start = leb128(body, offset)

    if start + length > len(body):
        raise ValueError(f"a name of {length} bytes at {start} runs past its section's {len(body)}")

    return bytes(body[start : start + length]).decode(), start + length


def wasm_byte(body: bytes | memoryview, offset: int) -> int:
    if offset >= len(body):
        raise ValueError(f"a read at {offset} runs past {extent(body)}")

    return body[offset]


def extent(body: bytes | memoryview) -> str:
    match body:
        case memoryview():
            return f"its section's {len(body)} bytes"
        case bytes():
            return f"the file's {len(body)} bytes"


def wasm_skip(body: memoryview, offset: int, count: int) -> int:
    if count <= 0 or offset < 0 or offset + count > len(body):
        raise ValueError(f"{count} bytes at {offset} run past its section's {len(body)} bytes")

    return offset + count


def wasm_limits(binary: memoryview, offset: int) -> tuple[int, int]:
    flags = wasm_byte(binary, offset)
    _, offset = leb128(binary, offset + 1)

    if flags & WASM_LIMITS_HAS_MAXIMUM:
        _, offset = leb128(binary, offset)

    if flags & WASM_LIMITS_HAS_PAGE_SIZE:
        _, offset = leb128(binary, offset)

    return flags, offset


def wasm_memories(binary: bytes) -> Iterator[int]:
    for section in wasm_sections(binary):
        if section.kind == WASM_SECTION_MEMORY:
            count, offset = leb128(section.body, 0)

            for _ in range(count):
                flags, offset = wasm_limits(section.body, offset)
                yield flags

        if section.kind == WASM_SECTION_IMPORT:
            count, offset = leb128(section.body, 0)

            for _ in range(count):
                _, offset = wasm_name(section.body, offset)
                _, offset = wasm_name(section.body, offset)

                match WasmImport(wasm_byte(section.body, offset)):
                    case WasmImport.FUNCTION:
                        _, offset = leb128(section.body, offset + 1)
                    case WasmImport.TABLE:
                        _, offset = wasm_limits(section.body, offset + 2)
                    case WasmImport.MEMORY:
                        flags, offset = wasm_limits(section.body, offset + 1)
                        yield flags
                    case WasmImport.GLOBAL:
                        offset = wasm_skip(section.body, offset, 3)
                    case WasmImport.TAG:
                        _, offset = leb128(section.body, offset + 2)


def wasm_exports(binary: bytes) -> frozenset[str]:
    return frozenset(
        name
        for section in wasm_sections(binary)
        if section.kind == WASM_SECTION_EXPORT
        for name in wasm_export_names(section.body)
    )


def wasm_export_names(body: memoryview) -> Iterator[str]:
    count, offset = leb128(body, 0)

    for _ in range(count):
        name, offset = wasm_name(body, offset)
        kind = wasm_byte(body, offset)
        _, offset = leb128(body, offset + 1)
        yield name if kind == WasmImport.FUNCTION else f"{name} (kind {kind})"
