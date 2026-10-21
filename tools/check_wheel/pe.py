from __future__ import annotations

import struct
from typing import NamedTuple

from c_strings import c_string

PE_MACHINES = {0x8664: "x86-64", 0xAA64: "aarch64"}
PE32_PLUS_MAGIC = 0x20B
PE32_PLUS_DATA_DIRECTORY_COUNT = 108
PE32_PLUS_DATA_DIRECTORIES = 112
PE_EXPORT_DIRECTORY_SIZE = 40


class PeSection(NamedTuple):
    virtual_size: int
    virtual_start: int
    raw_size: int
    raw_start: int

    def backs(self, address: int, size: int) -> bool:
        return (
            self.raw_start != 0
            and self.virtual_start <= address
            and address + size <= self.virtual_start + self.backed_size()
        )

    def backed_size(self) -> int:
        return min(self.virtual_size, self.raw_size)

    def backed_end(self) -> int:
        return self.raw_start + self.backed_size()

    def offset_of(self, address: int) -> int:
        return self.raw_start + address - self.virtual_start

    def string_at(self, binary: bytes, address: int) -> str:
        return c_string(binary, self.offset_of(address), (self.raw_start, self.backed_end()))


def pe_section(sections: list[PeSection], address: int, size: int) -> PeSection:
    section = next((section for section in sections if section.backs(address, size)), None)

    if section is None:
        raise ValueError(f"{size} bytes at RVA {address:#x} are in no file-backed section")

    return section


def pe_offset(sections: list[PeSection], address: int, size: int) -> int:
    return pe_section(sections, address, size).offset_of(address)


def pe_string(binary: bytes, sections: list[PeSection], address: int) -> str:
    return pe_section(sections, address, 1).string_at(binary, address)


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
    section_table = optional + optional_size

    if section_table + 40 * section_count > len(binary):
        raise ValueError(f"{section_count} section headers at {section_table} run past the file's {len(binary)} bytes")

    sections = [
        PeSection(*struct.unpack_from("<IIII", binary, section_table + index * 40 + 8)) for index in range(section_count)
    ]
    past_the_file = [
        section.raw_start for section in sections if section.raw_start != 0 and section.backed_end() > len(binary)
    ]

    if past_the_file:
        raise ValueError(f"sections at {past_the_file} run past the file's {len(binary)} bytes")

    if export_address == 0:
        return frozenset()

    directory = pe_offset(sections, export_address, PE_EXPORT_DIRECTORY_SIZE)
    name_count, _, name_table = struct.unpack_from("<III", binary, directory + 24)
    names = pe_offset(sections, name_table, 4 * name_count)

    return frozenset(
        pe_string(binary, sections, address)
        for address in {struct.unpack_from("<I", binary, names + index * 4)[0] for index in range(name_count)}
    )
