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
        (offset,) = struct.unpack_from("<I", binary, 0x3C)
        (machine,) = struct.unpack_from("<H", binary, offset + 4)
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

    symbol = init_symbol(modules[0])

    if symbol not in binary:
        yield Failure(path.name, f"{modules[0]} does not contain {symbol.decode()}")


def init_symbol(payload: str) -> bytes:
    directory, _, filename = payload.rpartition("/")
    stem = filename.split(".")[0]
    module = directory.rpartition("/")[2] if stem == "__init__" else stem

    return b"PyInit_" + module.encode()


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
