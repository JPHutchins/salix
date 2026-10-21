from __future__ import annotations

import functools
import itertools
import re
import struct
from collections.abc import Iterator
from typing import NamedTuple

from c_strings import c_string

ELF_MAGIC = b"\x7fELF"
ELF_MACHINES = {0x3E: "x86-64", 0xB7: "aarch64"}
ELF_SECTION_NULL = 0
ELF_SECTION_STRTAB = 3
ELF_SECTION_RELA = 4
ELF_SECTION_NOBITS = 8
ELF_SECTION_DYNSYM = 11
ELF_SECTION_VERNEED = 0x6FFFFFFE
ELF_VERSION_ENTRY_SIZE = 16
ELF_SECTION_HEADER_SIZE = 64
ELF_ENTRY_SIZES = {ELF_SECTION_RELA: 24, ELF_SECTION_DYNSYM: 24}
ELF_BINDINGS_EXPORTED = frozenset({1, 2, 10})
ELF_VISIBILITIES_EXPORTED = frozenset({0, 3})
ELF_RELOCATIONS_REFUSED: dict[str, dict[int, str]] = {"x86-64": {36: "R_X86_64_TLSDESC"}}
MANYLINUX_TAG = re.compile(r"manylinux_(\d+)_(\d+)_")
NUMBERED_GLIBC = re.compile(r"GLIBC_([0-9]+(?:\.[0-9]+)*)")


def beyond_the_glibc_floor(binary: bytes, machine: str, floor: tuple[int, ...]) -> Iterator[str]:
    sections = elf_sections(binary)
    numbers = {
        version: glibc_release(version)
        for version in elf_needed_versions(binary, sections)
        if version.startswith("GLIBC_")
    }
    unnumbered = sorted(version for version, number in numbers.items() if number is None)
    beyond = sorted(version for version, number in numbers.items() if number is not None and number > floor)

    if unnumbered:
        yield f"needs {unnumbered}, unnumbered glibc version nodes that a glibc {'.'.join(map(str, floor))} floor cannot cover"

    if beyond:
        yield f"needs {beyond}, which glibc {'.'.join(map(str, floor))} lacks"

    refused = ELF_RELOCATIONS_REFUSED.get(machine)

    if refused is not None:
        descriptors = sorted(refused[kind] for kind in elf_relocation_kinds(binary, sections) & refused.keys())

        if descriptors:
            yield f"reaches its thread-locals through {descriptors}, the gnu2 TLS dialect"


def glibc_release(version: str) -> tuple[int, ...] | None:
    numbered = NUMBERED_GLIBC.fullmatch(version)
    return tuple(int(part) for part in numbered[1].split(".")) if numbered is not None else None


class ElfSection(NamedTuple):
    kind: int
    offset: int
    size: int
    link: int
    info: int
    entry_size: int


def elf_section(binary: bytes, start: int) -> ElfSection:
    _, kind, _, _, offset, size, link, info, _, entry_size = struct.unpack_from("<IIQQQQIIQQ", binary, start)
    return ElfSection(kind, offset, size, link, info, entry_size)


@functools.lru_cache(maxsize=1)
def elf_sections(binary: bytes) -> tuple[ElfSection, ...]:
    (section_table,) = struct.unpack_from("<Q", binary, 0x28)
    section_size, section_count = struct.unpack_from("<HH", binary, 0x3A)

    if section_count == 0:
        raise ValueError("the section headers are stripped")

    if section_size != ELF_SECTION_HEADER_SIZE:
        raise ValueError(f"the section headers are {section_size} bytes each, not {ELF_SECTION_HEADER_SIZE}")

    sections = tuple(elf_section(binary, section_table + index * section_size) for index in range(section_count))
    stored = [section for section in sections if section.kind not in (ELF_SECTION_NULL, ELF_SECTION_NOBITS)]
    past_the_file = [section.offset for section in stored if section.offset + section.size > len(binary)]
    overlapping = [
        later.offset
        for earlier, later in itertools.pairwise(
            sorted((section for section in stored if section.size), key=lambda section: section.offset)
        )
        if earlier.offset + earlier.size > later.offset
    ]
    misshapen = [
        section.offset
        for section in sections
        if section.kind in ELF_ENTRY_SIZES
        and (section.entry_size != ELF_ENTRY_SIZES[section.kind] or section.size % ELF_ENTRY_SIZES[section.kind] != 0)
    ]

    if past_the_file:
        raise ValueError(f"sections at {past_the_file} run past the file's {len(binary)} bytes")

    if overlapping:
        raise ValueError(f"sections at {overlapping} overlap the section before them")

    if misshapen:
        raise ValueError(f"sections at {misshapen} misstate their entry size")

    return sections


def linked_string(binary: bytes, sections: tuple[ElfSection, ...], section: ElfSection, offset: int) -> str:
    strings = sections[section.link] if section.link < len(sections) else ElfSection(ELF_SECTION_NULL, 0, 0, 0, 0, 0)

    if strings.kind != ELF_SECTION_STRTAB or offset >= strings.size:
        raise ValueError(f"name {offset} is not a string in section {section.link}, which a link must name")

    return c_string(binary, strings.offset + offset, (strings.offset, strings.offset + strings.size))


def elf_exports(binary: bytes) -> frozenset[str]:
    sections = elf_sections(binary)

    return frozenset(
        linked_string(binary, sections, symbols, name)
        for symbols in sections
        if symbols.kind == ELF_SECTION_DYNSYM
        for name in {
            name
            for name, info, other, section in (
                struct.unpack_from("<IBBH", binary, start)
                for start in range(symbols.offset, symbols.offset + symbols.size, symbols.entry_size)
            )
            if section != 0
            and info >> 4 in ELF_BINDINGS_EXPORTED
            and other & 3 in ELF_VISIBILITIES_EXPORTED
        }
    )


def elf_needed_versions(binary: bytes, sections: tuple[ElfSection, ...]) -> frozenset[str]:
    return frozenset(
        linked_string(binary, sections, needs, name)
        for needs in sections
        if needs.kind == ELF_SECTION_VERNEED
        for name in set(elf_version_names(binary, needs))
    )


def elf_version_names(binary: bytes, needs: ElfSection) -> list[int]:
    slots = needs.size // ELF_VERSION_ENTRY_SIZE
    walked = 0
    declared = 0
    names: list[int] = []

    for need, (_, version_count, _, first_version, _) in chained_entries(binary, needs, needs.offset, "<HHIII"):
        walked += 1
        declared += 1 + version_count

        if walked > needs.info or declared > slots:
            raise ValueError(
                f"the version needs outgrow their section: {walked} needs of {needs.info} declared,"
                f" {declared} entries in {slots} slots"
            )

        if first_version < ELF_VERSION_ENTRY_SIZE:
            raise ValueError(f"the version need at {need} puts its first version {first_version} bytes in, inside itself")

        versions = [
            name
            for _, (_, _, _, name, _) in itertools.islice(
                chained_entries(binary, needs, need + first_version, "<IHHII"), version_count + 1
            )
        ]

        if len(versions) != version_count:
            raise ValueError(
                f"the version need at {need} declares {version_count} versions but chains"
                f" {'more' if len(versions) > version_count else len(versions)}"
            )

        names.extend(versions)

    if walked != needs.info:
        raise ValueError(f"the version-need section declares {needs.info} needs but chains {walked}")

    return names


def chained_entries(
    binary: bytes, section: ElfSection, first: int, layout: str
) -> Iterator[tuple[int, tuple[int, ...]]]:
    entry = first

    while True:
        if not section.offset <= entry <= section.offset + section.size - ELF_VERSION_ENTRY_SIZE:
            raise ValueError(f"a version entry at {entry} is outside its section at {section.offset}+{section.size}")

        fields = struct.unpack_from(layout, binary, entry)
        yield entry, fields

        if fields[-1] == 0:
            return

        if fields[-1] < ELF_VERSION_ENTRY_SIZE:
            raise ValueError(f"the version entry at {entry} links {fields[-1]} bytes ahead, into itself")

        entry += fields[-1]


def elf_relocation_kinds(binary: bytes, sections: tuple[ElfSection, ...]) -> frozenset[int]:
    return frozenset(
        info & 0xFFFFFFFF
        for relocations in sections
        if relocations.kind == ELF_SECTION_RELA
        for (info,) in (
            struct.unpack_from("<Q", binary, start + 8)
            for start in range(relocations.offset, relocations.offset + relocations.size, relocations.entry_size)
        )
    )
