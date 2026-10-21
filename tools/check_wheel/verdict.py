from __future__ import annotations

import base64
import csv
import hashlib
import struct
import sys
import threading
import zipfile
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import NamedTuple

from elf import (
    ELF_MACHINES,
    ELF_MAGIC,
    MANYLINUX_TAG,
    beyond_the_glibc_floor,
    elf_exports,
)
from macho import MACHO_64_MAGIC, MACHO_MACHINES, macho_exports
from pe import PE_MACHINES, pe_exports, pe_header
from wasm import WASM_MAGIC, wasm_exports, wasm_machine

CHECK_SECONDS = 30.0


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
    "pyemscripten_2026_0_wasm32": Shape("wasm", "wasm32"),
}
LINKER_EXPORTS: dict[str, frozenset[str]] = {
    "macho": frozenset({"__dso_handle", "_mh_dylib_header"}),
    "wasm": frozenset({"__wasm_call_ctors", "__wasm_apply_data_relocs"}),
}


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

    if binary.startswith(WASM_MAGIC):
        return Shape("wasm", wasm_machine(binary))

    return Shape("unknown", "unknown")


def check(path: Path) -> Iterator[Failure]:
    name_parts = path.stem.split("-")

    if len(name_parts) != 5:
        yield Failure(str(path), f"filename is not distribution-version-python-abi-platform: {name_parts}")
        return

    distribution, version, python_tag, abi_tag, platform_tag = name_parts
    archive = zipfile.ZipFile(path)
    names = set(archive.namelist())
    dist_info = f"{distribution}-{version}.dist-info"

    for required in (f"{dist_info}/METADATA", f"{dist_info}/WHEEL", f"{dist_info}/RECORD"):
        if required not in names:
            yield Failure(str(path), f"missing {required}")

    if f"{dist_info}/WHEEL" in names:
        declared = [
            line.removeprefix("Tag:").strip()
            for line in archive.read(f"{dist_info}/WHEEL").decode().splitlines()
            if line.startswith("Tag:")
        ]
        if declared != [f"{python_tag}-{abi_tag}-{platform_tag}"]:
            yield Failure(str(path), f"WHEEL tag {declared} does not match filename")

    if f"{dist_info}/RECORD" in names:
        yield from check_record(path, archive, f"{dist_info}/RECORD")

    payloads = [
        name
        for name in names
        if not name.startswith(f"{dist_info}/") and name.endswith((".so", ".pyd"))
    ]

    if len(payloads) != 1:
        yield Failure(str(path), f"expected exactly one binary payload, found {sorted(payloads)}")
        return

    modules = payloads
    binary = archive.read(modules[0])
    expected = EXPECTED.get(platform_tag)

    if expected is None:
        yield Failure(str(path), f"no expected shape registered for platform tag {platform_tag}")
        return

    actual = identify(binary)

    if actual != expected:
        yield Failure(str(path), f"{modules[0]} is {actual}, tag {platform_tag} implies {expected}")
        return

    symbol = init_symbol(modules[0])
    linker = LINKER_EXPORTS.get(actual.container, frozenset())
    exported = EXPORT_READERS[actual.container](binary)

    if symbol not in exported or not exported - {symbol} <= linker:
        yield Failure(
            str(path),
            f"{modules[0]} exports {sorted(exported)}, not {symbol} alone or with the linker's {sorted(linker)}",
        )

    manylinux = MANYLINUX_TAG.match(platform_tag)

    if manylinux is not None:
        floor = tuple(int(part) for part in manylinux.groups())
        yield from (
            Failure(str(path), f"{modules[0]} {problem}")
            for problem in beyond_the_glibc_floor(binary, actual.machine, floor)
        )


def init_symbol(payload: str) -> str:
    directory, _, filename = payload.rpartition("/")
    stem = filename.split(".")[0]
    module = directory.rpartition("/")[2] if stem == "__init__" else stem

    return "PyInit_" + module


EXPORT_READERS: dict[str, Callable[[bytes], frozenset[str]]] = {
    "elf": elf_exports,
    "macho": macho_exports,
    "pe": pe_exports,
    "wasm": wasm_exports,
}


def check_record(path: Path, archive: zipfile.ZipFile, record_name: str) -> Iterator[Failure]:
    rows = list(csv.reader(archive.read(record_name).decode().splitlines()))
    listed = {row[0] for row in rows}

    for name in archive.namelist():
        if name not in listed:
            yield Failure(str(path), f"{name} is missing from RECORD")

    for row in rows:
        name, digest, size = row

        if name == record_name:
            continue

        payload = archive.read(name)
        expected = base64.urlsafe_b64encode(hashlib.sha256(payload).digest()).rstrip(b"=").decode()

        if digest != f"sha256={expected}":
            yield Failure(str(path), f"RECORD hash mismatch for {name}")

        if int(size) != len(payload):
            yield Failure(str(path), f"RECORD size mismatch for {name}")


def checked(path: Path) -> list[Failure]:
    found: list[Failure] = []
    worker = threading.Thread(target=check_into, args=(path, found), daemon=True)
    worker.start()
    worker.join(CHECK_SECONDS)

    if worker.is_alive():
        return [*found, Failure(str(path), f"could not be checked within {CHECK_SECONDS} s")]

    return found


def check_into(path: Path, found: list[Failure]) -> None:
    try:
        found.extend(check(path))
    except Exception as error:  # noqa: BLE001
        found.append(Failure(str(path), f"could not be checked: {error!r}"))


def main() -> None:
    wheels = [Path(argument) for argument in sys.argv[1:]]

    if not wheels:
        raise SystemExit("usage: check_wheel WHEEL [WHEEL ...]")

    failures = [failure for wheel in wheels for failure in checked(wheel)]

    for wheel in wheels:
        print(f"{'FAIL' if any(f.wheel == str(wheel) for f in failures) else 'ok  '} {wheel.name}")

    for failure in failures:
        print(f"  {failure.wheel}: {failure.problem}", file=sys.stderr)

    raise SystemExit(1 if failures else 0)
