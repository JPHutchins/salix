from __future__ import annotations

import struct
from typing import NamedTuple

from c_strings import c_string

PE_MACHINES = {0x8664: "x86-64", 0xAA64: "aarch64"}
PE32_PLUS_MAGIC = 0x20B
PE32_PLUS_DATA_DIRECTORY_COUNT = 108
PE32_PLUS_DATA_DIRECTORIES = 112


class PeSection(NamedTuple):
    virtual_size: int
    virtual_start: int
    raw_size: int
    raw_start: int


def pe_offset(sections: list[PeSection], address: int) -> int:
    offset = next(
        (
            section.raw_start + address - section.virtual_start
            for section in sections
            if section.raw_start != 0
            and section.virtual_start <= address < section.virtual_start + min(section.virtual_size, section.raw_size)
        ),
        None,
    )

    if offset is None:
        raise ValueError(f"RVA {address:#x} is in no file-backed section")

    return offset


def pe_header(binary: bytes) -> int:
    (header,) = struct.unpack_from("<I", binary, 0x3C)
    return int(header)


def pe_exports(binary: bytes) -> frozenset[str]:
    header = pe_header(binary)
    (section_count,) = struct.unpack_from("<H", binary, header + 6)
    (optional_size,) = struct.unpack_from("<H", binary, header + 20)
    optional = header + 24
    (magic,) = struct.unpack_from("<H", binary, optional)

    if magic != PE32_PLUS_MAGIC:
        raise ValueError(f"optional header magic {magic:#x} is not PE32+")

    (directory_count,) = struct.unpack_from("<I", binary, optional + PE32_PLUS_DATA_DIRECTORY_COUNT)

    if directory_count == 0:
        raise ValueError("the optional header declares no data directories")

    (export_address,) = struct.unpack_from("<I", binary, optional + PE32_PLUS_DATA_DIRECTORIES)
    sections = [
        PeSection(*struct.unpack_from("<IIII", binary, optional + optional_size + index * 40 + 8))
        for index in range(section_count)
    ]

    if export_address == 0:
        return frozenset()

    directory = pe_offset(sections, export_address)
    name_count, _, name_table = struct.unpack_from("<III", binary, directory + 24)
    names = pe_offset(sections, name_table)

    return frozenset(
        c_string(binary, pe_offset(sections, struct.unpack_from("<I", binary, names + index * 4)[0]))
        for index in range(name_count)
    )
