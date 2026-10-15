from __future__ import annotations

import base64
import csv
import hashlib
import struct
import sys
import zipfile
from collections.abc import Iterator
from pathlib import Path
from typing import NamedTuple

ELF_MAGIC = b"\x7fELF"
MACHO_64_MAGIC = b"\xcf\xfa\xed\xfe"


class Shape(NamedTuple):
    container: str
    machine: str


EXPECTED: dict[str, Shape] = {
    "manylinux_2_17_x86_64": Shape("elf", "x86-64"),
    "manylinux_2_17_aarch64": Shape("elf", "aarch64"),
    "macosx_10_13_x86_64": Shape("macho", "x86-64"),
    "macosx_11_0_arm64": Shape("macho", "aarch64"),
    "win_amd64": Shape("pe", "x86-64"),
    "win_arm64": Shape("pe", "aarch64"),
}

ELF_MACHINES = {0x3E: "x86-64", 0xB7: "aarch64"}
MACHO_MACHINES = {0x01000007: "x86-64", 0x0100000C: "aarch64"}
PE_MACHINES = {0x8664: "x86-64", 0xAA64: "aarch64"}

ELF_SECTION_DYNSYM = 11
ELF_BINDINGS_EXPORTED = frozenset({1, 2, 10})
ELF_VISIBILITIES_EXPORTED = frozenset({0, 3})
MACHO_LOAD_SYMTAB = 0x2
MACHO_LOAD_DYSYMTAB = 0xB
PE32_PLUS_MAGIC = 0x20B
PE32_PLUS_DATA_DIRECTORIES = 112
LINKER_EXPORTS: dict[str, frozenset[str]] = {"macho": frozenset({"__dso_handle", "_mh_dylib_header"})}


class Failure(NamedTuple):
    wheel: str
    problem: str


def identify(binary: bytes) -> Shape:
    if binary.startswith(ELF_MAGIC):
        (machine,) = struct.unpack_from("<H", binary, 18)
        return Shape("elf", ELF_MACHINES.get(machine, hex(machine)))

    if binary.startswith(MACHO_64_MAGIC):
        (cpu,) = struct.unpack_from("<I", binary, 4)
        return Shape("macho", MACHO_MACHINES.get(cpu, hex(cpu)))

    if binary.startswith(b"MZ"):
        (machine,) = struct.unpack_from("<H", binary, pe_header(binary) + 4)
        return Shape("pe", PE_MACHINES.get(machine, hex(machine)))

    return Shape("unknown", "unknown")


def check(path: Path) -> Iterator[Failure]:
    name_parts = path.stem.split("-")

    if len(name_parts) != 5:
        yield Failure(path.name, f"filename is not distribution-version-python-abi-platform: {name_parts}")
        return

    distribution, version, python_tag, abi_tag, platform_tag = name_parts
    archive = zipfile.ZipFile(path)
    names = set(archive.namelist())
    dist_info = f"{distribution}-{version}.dist-info"

    for required in (f"{dist_info}/METADATA", f"{dist_info}/WHEEL", f"{dist_info}/RECORD"):
        if required not in names:
            yield Failure(path.name, f"missing {required}")

    if f"{dist_info}/WHEEL" in names:
        declared = [
            line.removeprefix("Tag:").strip()
            for line in archive.read(f"{dist_info}/WHEEL").decode().splitlines()
            if line.startswith("Tag:")
        ]
        if declared != [f"{python_tag}-{abi_tag}-{platform_tag}"]:
            yield Failure(path.name, f"WHEEL tag {declared} does not match filename")

    if f"{dist_info}/RECORD" in names:
        yield from check_record(path, archive, f"{dist_info}/RECORD")

    payloads = [
        name
        for name in names
        if not name.startswith(f"{dist_info}/") and name.endswith((".so", ".pyd"))
    ]

    if len(payloads) != 1:
        yield Failure(path.name, f"expected exactly one binary payload, found {sorted(payloads)}")
        return

    modules = payloads
    binary = archive.read(modules[0])
    expected = EXPECTED.get(platform_tag)

    if expected is None:
        yield Failure(path.name, f"no expected shape registered for platform tag {platform_tag}")
        return

    actual = identify(binary)

    if actual != expected:
        yield Failure(path.name, f"{modules[0]} is {actual}, tag {platform_tag} implies {expected}")
        return

    symbol = init_symbol(modules[0])
    linker = LINKER_EXPORTS.get(actual.container, frozenset())

    try:
        exported = exports(binary, actual.container)
    except (struct.error, ValueError, IndexError) as error:
        yield Failure(path.name, f"{modules[0]} exports could not be read: {error}")
        return

    if symbol not in exported or not exported - {symbol} <= linker:
        yield Failure(
            path.name,
            f"{modules[0]} exports {sorted(exported)}, not {symbol} alone or with the linker's {sorted(linker)}",
        )


def init_symbol(payload: str) -> str:
    directory, _, filename = payload.rpartition("/")
    stem = filename.split(".")[0]
    module = directory.rpartition("/")[2] if stem == "__init__" else stem

    return "PyInit_" + module


def exports(binary: bytes, container: str) -> frozenset[str]:
    match container:
        case "elf":
            return elf_exports(binary)
        case "macho":
            return macho_exports(binary)
        case "pe":
            return pe_exports(binary)
        case _:
            return frozenset()


def c_string(binary: bytes, start: int) -> str:
    return binary[start : binary.index(b"\0", start)].decode()


class ElfSection(NamedTuple):
    kind: int
    offset: int
    size: int
    link: int
    entry_size: int


def elf_section(binary: bytes, start: int) -> ElfSection:
    _, kind, _, _, offset, size, link, _, _, entry_size = struct.unpack_from("<IIQQQQIIQQ", binary, start)
    return ElfSection(kind, offset, size, link, entry_size)


def elf_exports(binary: bytes) -> frozenset[str]:
    (section_table,) = struct.unpack_from("<Q", binary, 0x28)
    section_size, section_count = struct.unpack_from("<HH", binary, 0x3A)

    if section_count == 0:
        raise ValueError("the section headers are stripped")

    sections = [elf_section(binary, section_table + index * section_size) for index in range(section_count)]

    return frozenset(
        c_string(binary, sections[symbols.link].offset + name)
        for symbols in sections
        if symbols.kind == ELF_SECTION_DYNSYM
        for name, info, other, section in (
            struct.unpack_from("<IBBH", binary, start)
            for start in range(symbols.offset, symbols.offset + symbols.size, symbols.entry_size)
        )
        if section != 0
        and info >> 4 in ELF_BINDINGS_EXPORTED
        and other & 3 in ELF_VISIBILITIES_EXPORTED
    )


def macho_load_commands(binary: bytes) -> Iterator[tuple[int, int]]:
    (command_count,) = struct.unpack_from("<I", binary, 16)
    offset = 32

    for _ in range(command_count):
        command, size = struct.unpack_from("<II", binary, offset)

        if size < 8 or offset + size > len(binary):
            raise ValueError(f"load command {command:#x} at {offset} has size {size}")

        yield command, offset
        offset += size


def macho_exports(binary: bytes) -> frozenset[str]:
    commands = dict(macho_load_commands(binary))

    if not {MACHO_LOAD_SYMTAB, MACHO_LOAD_DYSYMTAB} <= commands.keys():
        raise ValueError("there is no LC_SYMTAB and LC_DYSYMTAB pair")

    symbols, _, strings, _ = struct.unpack_from("<IIII", binary, commands[MACHO_LOAD_SYMTAB] + 8)
    _, _, first_defined, defined_count = struct.unpack_from(
        "<IIII", binary, commands[MACHO_LOAD_DYSYMTAB] + 8
    )

    return frozenset(
        c_string(binary, strings + struct.unpack_from("<I", binary, symbols + index * 16)[0]).removeprefix("_")
        for index in range(first_defined, first_defined + defined_count)
    )


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


def check_record(path: Path, archive: zipfile.ZipFile, record_name: str) -> Iterator[Failure]:
    rows = list(csv.reader(archive.read(record_name).decode().splitlines()))
    listed = {row[0] for row in rows}

    for name in archive.namelist():
        if name not in listed:
            yield Failure(path.name, f"{name} is missing from RECORD")

    for row in rows:
        name, digest, size = row

        if name == record_name:
            continue

        payload = archive.read(name)
        expected = base64.urlsafe_b64encode(hashlib.sha256(payload).digest()).rstrip(b"=").decode()

        if digest != f"sha256={expected}":
            yield Failure(path.name, f"RECORD hash mismatch for {name}")

        if int(size) != len(payload):
            yield Failure(path.name, f"RECORD size mismatch for {name}")


def main() -> None:
    wheels = [Path(argument) for argument in sys.argv[1:]]

    if not wheels:
        raise SystemExit("usage: check_wheel.py WHEEL [WHEEL ...]")

    failures = [failure for wheel in wheels for failure in check(wheel)]

    for wheel in wheels:
        print(f"{'FAIL' if any(f.wheel == wheel.name for f in failures) else 'ok  '} {wheel.name}")

    for failure in failures:
        print(f"  {failure.wheel}: {failure.problem}", file=sys.stderr)

    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    main()
