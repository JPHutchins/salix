from __future__ import annotations

import struct
from collections.abc import Iterator
from typing import NamedTuple

from c_strings import c_string

MACHO_64_MAGIC = b"\xcf\xfa\xed\xfe"
MACHO_MACHINES = {0x01000007: "x86-64", 0x0100000C: "aarch64"}
MACHO_LOAD_SYMTAB = 0x2
MACHO_LOAD_DYSYMTAB = 0xB
MACHO_REQUIRED_COMMANDS = {MACHO_LOAD_SYMTAB: ("LC_SYMTAB", 24), MACHO_LOAD_DYSYMTAB: ("LC_DYSYMTAB", 80)}


class MachoCommand(NamedTuple):
    offset: int
    size: int


def macho_load_commands(binary: bytes) -> Iterator[tuple[int, MachoCommand]]:
    (command_count,) = struct.unpack_from("<I", binary, 16)
    offset = 32

    for _ in range(command_count):
        command, size = struct.unpack_from("<II", binary, offset)

        if size < 8 or offset + size > len(binary):
            raise ValueError(f"load command {command:#x} at {offset} has size {size}")

        yield command, MachoCommand(offset, size)
        offset += size


def macho_exports(binary: bytes) -> frozenset[str]:
    commands = dict(macho_load_commands(binary))
    unusable = [
        f"{name} is missing or shorter than {minimum} bytes"
        for command, (name, minimum) in MACHO_REQUIRED_COMMANDS.items()
        if commands.get(command, MachoCommand(0, 0)).size < minimum
    ]

    if unusable:
        raise ValueError("; ".join(unusable))

    symbols, symbol_count, strings, _ = struct.unpack_from("<IIII", binary, commands[MACHO_LOAD_SYMTAB].offset + 8)
    _, _, first_defined, defined_count = struct.unpack_from(
        "<IIII", binary, commands[MACHO_LOAD_DYSYMTAB].offset + 8
    )

    if first_defined + defined_count > symbol_count:
        raise ValueError(
            f"defined symbols {first_defined}..{first_defined + defined_count} are outside the {symbol_count} symbols"
        )

    return frozenset(
        c_string(binary, strings + struct.unpack_from("<I", binary, symbols + index * 16)[0]).removeprefix("_")
        for index in range(first_defined, first_defined + defined_count)
    )
