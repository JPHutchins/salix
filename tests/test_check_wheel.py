import base64
import hashlib
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest
from platforms import run_tool, starts_a_subprocess

CHECK_WHEEL = Path(__file__).resolve().parent.parent / "tools" / "check_wheel.py"

pytestmark = [
    pytest.mark.skipif(not CHECK_WHEEL.exists(), reason="tools/ is not beside these tests"),
    starts_a_subprocess,
]

DIST_INFO = "salix-0.1.0a1.dist-info"


def record_line(name: str, data: bytes) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return f"{name},sha256={digest},{len(data)}\n"


def wheel(where: Path, platform: str, payload_name: str, payload: bytes) -> Path:
    files = {
        payload_name: payload,
        f"{DIST_INFO}/METADATA": b"Metadata-Version: 2.1\nName: salix\nVersion: 0.1.0a1\n",
        f"{DIST_INFO}/WHEEL": f"Wheel-Version: 1.0\nTag: cp313-cp313-{platform}\n".encode(),
    }
    record = "".join(record_line(name, data) for name, data in files.items()) + f"{DIST_INFO}/RECORD,,\n"
    path = where / f"salix-0.1.0a1-cp313-cp313-{platform}.whl"
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in {**files, f"{DIST_INFO}/RECORD": record.encode()}.items():
            archive.writestr(name, data)
    return path


def check(*paths: Path) -> subprocess.CompletedProcess[str]:
    return run_tool(CHECK_WHEEL, *paths)


TRUNCATED_ELF = b"\x7fELF" + bytes(14) + struct.pack("<H", 0x3E) + bytes(4)

ELF_MAGIC_ONLY = b"\x7fELF"

ELF_WITH_AN_UNADDRESSABLE_SECTION_TABLE = (
    b"\x7fELF"
    + bytes(14)
    + struct.pack("<H", 0x3E)
    + bytes(0x28 - 20)
    + struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)
    + bytes(0x3A - 0x30)
    + struct.pack("<HH", 64, 1)
    + bytes(2)
)

MACHO_HEADER = b"\xcf\xfa\xed\xfe" + struct.pack("<IIII", 0x01000007, 3, 6, 1)

MACHO_WITH_A_ZERO_SIZE_COMMAND = MACHO_HEADER + struct.pack("<III", 8, 0, 0) + struct.pack("<II", 0x19, 0)

MACHO_WITHOUT_A_SYMBOL_TABLE = MACHO_HEADER + struct.pack("<III", 8, 0, 0) + struct.pack("<II", 0x19, 8)

MACHO_WITH_A_TRUNCATED_SYMBOL_TABLE = MACHO_HEADER + struct.pack("<III", 8, 0, 0) + struct.pack("<II", 0x2, 8)

MACHO_WITH_DEFINED_SYMBOLS_PAST_THE_TABLE = (
    b"\xcf\xfa\xed\xfe"
    + struct.pack("<IIII", 0x01000007, 3, 6, 2)
    + struct.pack("<III", 104, 0, 0)
    + struct.pack("<IIIIII", 0x2, 24, 0, 1, 0, 0)
    + struct.pack("<IIIIII", 0xB, 80, 0, 0, 0, 5)
    + bytes(56)
)


def pe(data_directory_count: int, magic: int = 0x20B) -> bytes:
    return (
        b"MZ"
        + bytes(0x3A)
        + struct.pack("<I", 0x40)
        + b"PE\0\0"
        + struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, 240, 0)
        + struct.pack("<H", magic)
        + bytes(106)
        + struct.pack("<I", data_directory_count)
        + struct.pack("<II", 0x1000, 0x40)
        + bytes(120)
    )


def elf_module(
    machine: int,
    versions: tuple[str, ...],
    relocation_kinds: tuple[int, ...],
    need_count: int = 2,
    last_next_need: int = 0,
    version_count: int | None = None,
    first_version: int = 16,
    needs_link: int = 2,
    last_name: int | None = None,
    relocation_entry_size: int = 24,
    symbol_entry_size: int = 24,
    section_header_size: int = 64,
    duplicated_section: int | None = None,
    empty_section_at: int | None = None,
    symbol_count: int = 1,
    name_length: int | None = None,
) -> bytes:
    strings = b"\0" + b"".join(
        name.encode() + b"\0"
        for name in (
            "PyInit_salix" if name_length is None else "a" * name_length,
            "ld-linux.so.2",
            "GLIBC_2.3",
            "libc.so.6",
            *versions,
        )
    )

    def string(name: str) -> int:
        return strings.index(b"\0" + name.encode() + b"\0") + 1

    symbols = bytes(24) + struct.pack("<IBBHQQ", 1, 0x12, 0, 1, 0, 0) * symbol_count
    needs = (
        struct.pack("<HHIII", 1, 1, string("ld-linux.so.2"), 16, 32)
        + struct.pack("<IHHII", 0, 0, 2, string("GLIBC_2.3"), 0)
        + struct.pack(
            "<HHIII",
            1,
            len(versions) if version_count is None else version_count,
            string("libc.so.6"),
            first_version,
            last_next_need,
        )
        + b"".join(
            struct.pack(
                "<IHHII",
                0,
                0,
                3 + index,
                last_name if last_name is not None and index == len(versions) - 1 else string(version),
                16 if index < len(versions) - 1 else 0,
            )
            for index, version in enumerate(versions)
        )
    )
    first_symbol = 1 << 32
    relocations = b"".join(struct.pack("<QQq", 0, first_symbol | kind, 0) for kind in relocation_kinds)
    bodies = [symbols, strings, needs, relocations]
    starts = [64 + sum(len(body) for body in bodies[:index]) for index in range(len(bodies))]
    headers = [
        struct.pack("<IIQQQQIIQQ", 0, kind, 0, 0, start, len(body), link, info, 8, entry_size)
        for (kind, link, info, entry_size), start, body in zip(
            [
                (11, 2, 1, symbol_entry_size),
                (3, 0, 0, 0),
                (0x6FFFFFFE, needs_link, need_count, 0),
                (4, 1, 0, relocation_entry_size),
            ],
            starts,
            bodies,
            strict=True,
        )
    ]
    assert duplicated_section is None or 1 <= duplicated_section <= len(headers)
    duplicates = [
        *([] if duplicated_section is None else [headers[duplicated_section - 1]]),
        *([] if empty_section_at is None else [struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, empty_section_at, 0, 0, 0, 1, 0)]),
    ]
    section_headers = bytes(64) + b"".join(headers + duplicates)
    header = (
        b"\x7fELF\x02\x01\x01"
        + bytes(9)
        + struct.pack(
            "<HHIQQQIHHHHHH",
            3,
            machine,
            1,
            0,
            0,
            starts[-1] + len(relocations),
            0,
            64,
            0,
            0,
            section_header_size,
            len(bodies) + 1 + len(duplicates),
            2,
        )
    )
    return header + b"".join(bodies) + section_headers


X86_64_RELATIVE_AND_DTPMOD64 = (8, 16)

WASM_HEADER = b"\0asm\x01\0\0\0"


def wasm_section(kind: int, *items: bytes) -> bytes:
    body = bytes([len(items)]) + b"".join(items)
    return bytes([kind, len(body)]) + body


def wasm_name(name: str) -> bytes:
    return bytes([len(name)]) + name.encode()


WASM_MEMORY_IMPORT = wasm_section(2, wasm_name("env") + wasm_name("memory") + b"\x02\x00\x01")


def wasm_exporting(*names: str, kind: int = 0, memory: bytes = WASM_MEMORY_IMPORT) -> bytes:
    return WASM_HEADER + memory + wasm_section(7, *(wasm_name(name) + bytes([kind, 0]) for name in names))


WASM_SIDE_MODULE_EXPORTS = ("__wasm_call_ctors", "__wasm_apply_data_relocs", "PyInit_salix")

WASM_WITH_A_SECTION_PAST_THE_END = WASM_HEADER + bytes([7, 16])

WASM_WITH_AN_UNKNOWN_IMPORT_KIND = WASM_HEADER + wasm_section(2, wasm_name("env") + wasm_name("thing") + b"\x09")

WASM_WITH_AN_ENDLESS_LEB128 = WASM_HEADER + bytes([7]) + b"\x80" * 10

WASM_WITH_AN_IMPORT_NAME_PAST_ITS_SECTION = (
    WASM_HEADER + bytes([2, 3]) + b"\x01\x03en" + wasm_section(7, wasm_name("PyInit_salix") + b"\0\0")
)

WASM_WITH_64_BIT_MEMORY = WASM_HEADER + wasm_section(2, wasm_name("env") + wasm_name("memory") + b"\x02\x04\x01")

ELF = ("manylinux_2_17_x86_64", "salix/__init__.cpython-313-x86_64-linux-gnu.so")
ELF_AARCH64 = ("manylinux_2_17_aarch64", "salix/__init__.cpython-313-aarch64-linux-gnu.so")
MACHO = ("macosx_10_13_x86_64", "salix/__init__.cpython-313-darwin.so")
PE = ("win_amd64", "salix/__init__.cp313-win_amd64.pyd")
WASM = ("pyemscripten_2026_0_wasm32", "salix/__init__.cpython-313-wasm32-emscripten.so")


@pytest.mark.parametrize(
    ("target", "payload", "reason"),
    [
        (ELF, TRUNCATED_ELF, "unpack_from"),
        (ELF, ELF_MAGIC_ONLY, "unpack_from"),
        (ELF, ELF_WITH_AN_UNADDRESSABLE_SECTION_TABLE, "OverflowError"),
        (MACHO, MACHO_WITH_A_ZERO_SIZE_COMMAND, "has size 0"),
        (MACHO, MACHO_WITHOUT_A_SYMBOL_TABLE, "LC_SYMTAB is missing or shorter than 24 bytes"),
        (MACHO, MACHO_WITH_A_TRUNCATED_SYMBOL_TABLE, "LC_SYMTAB is missing or shorter than 24 bytes"),
        (MACHO, MACHO_WITH_DEFINED_SYMBOLS_PAST_THE_TABLE, "outside the 1 symbols"),
        (PE, pe(16), "no file-backed section"),
        (PE, pe(0), "declares no data directories"),
        (PE, pe(16, magic=0x10B), "not PE32+"),
        (WASM, WASM_WITH_A_SECTION_PAST_THE_END, "has size 16"),
        (WASM, WASM_WITH_AN_UNKNOWN_IMPORT_KIND, "9 is not a valid WasmImport"),
        (WASM, WASM_WITH_AN_ENDLESS_LEB128, "longer than 64 bits"),
        (WASM, WASM_WITH_AN_IMPORT_NAME_PAST_ITS_SECTION, "runs past its section's 3"),
        (WASM, wasm_exporting(*WASM_SIDE_MODULE_EXPORTS, memory=b""), "declares and imports no memory"),
        (
            WASM,
            WASM_HEADER + WASM_MEMORY_IMPORT + wasm_section(7, wasm_name("PyInit_salix")),
            "a read at 14 runs past its section's 14 bytes",
        ),
        (
            WASM,
            WASM_HEADER + wasm_section(2, wasm_name("env") + wasm_name("g") + b"\x03"),
            "3 bytes at 7 run past its section's 8 bytes",
        ),
    ],
    ids=[
        "truncated-elf",
        "elf-magic-only",
        "elf-unaddressable-section-table",
        "macho-zero-size-command",
        "macho-no-symtab",
        "macho-truncated-symtab",
        "macho-defined-symbols-past-the-table",
        "pe-rva-outside-sections",
        "pe-no-data-directories",
        "pe32",
        "wasm-section-past-the-end",
        "wasm-unknown-import-kind",
        "wasm-endless-leb128",
        "wasm-import-name-past-its-section",
        "wasm-without-memory",
        "wasm-export-without-a-kind",
        "wasm-global-import-without-its-type",
    ],
)
def test_a_payload_that_cannot_be_read_fails_that_wheel_cleanly(target, payload, reason, tmp_path):
    platform, payload_name = target
    completed = check(wheel(tmp_path, platform, payload_name, payload))

    assert completed.returncode == 1
    assert "Traceback" not in completed.stderr
    assert "could not be checked" in completed.stderr
    assert reason in completed.stderr
    assert completed.stdout.startswith("FAIL ")


def test_a_payload_of_the_wrong_shape_reports_only_its_shape(tmp_path):
    completed = check(wheel(tmp_path, *PE, TRUNCATED_ELF))

    assert completed.returncode == 1
    assert "tag win_amd64 implies" in completed.stderr
    assert "exports" not in completed.stderr
    assert "could not be checked" not in completed.stderr


def test_a_64_bit_side_module_does_not_pass_for_a_wasm32_tag(tmp_path):
    completed = check(wheel(tmp_path, *WASM, WASM_WITH_64_BIT_MEMORY))

    assert completed.returncode == 1
    assert "is Shape(container='wasm', machine='wasm64'), tag pyemscripten_2026_0_wasm32 implies" in completed.stderr


def test_a_side_module_exporting_its_init_beside_the_linkers_symbols_passes(tmp_path):
    completed = check(wheel(tmp_path, *WASM, wasm_exporting(*WASM_SIDE_MODULE_EXPORTS)))

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.startswith("ok ")


@pytest.mark.parametrize(
    "exports",
    [WASM_SIDE_MODULE_EXPORTS[:2], (*WASM_SIDE_MODULE_EXPORTS, "salix_internal")],
    ids=["without-its-init", "with-an-internal-symbol"],
)
def test_a_side_module_exporting_anything_else_fails(exports, tmp_path):
    completed = check(wheel(tmp_path, *WASM, wasm_exporting(*exports)))

    assert completed.returncode == 1
    assert "not PyInit_salix alone or with the linker's" in completed.stderr


def test_a_wheel_that_is_not_a_zip_fails_alone(tmp_path):
    broken = tmp_path / "salix-0.1.0a1-cp313-cp313-manylinux_2_17_x86_64.whl"
    broken.write_bytes(b"<html>not a wheel</html>")
    completed = check(broken)

    assert completed.returncode == 1
    assert "Traceback" not in completed.stderr
    assert "BadZipFile" in completed.stderr


def test_a_record_row_without_three_fields_fails_that_wheel_cleanly(tmp_path):
    path = tmp_path / "salix-0.1.0a1-cp313-cp313-manylinux_2_17_x86_64.whl"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(f"{DIST_INFO}/RECORD", f"{DIST_INFO}/RECORD,\n")
    completed = check(path)

    assert completed.returncode == 1
    assert "Traceback" not in completed.stderr
    assert "not enough values to unpack" in completed.stderr


def test_one_unreadable_wheel_does_not_hide_the_others(tmp_path):
    first = tmp_path / "first"
    second = tmp_path / "second"
    first.mkdir()
    second.mkdir()
    unreadable = wheel(first, *ELF, ELF_MAGIC_ONLY)
    misshapen = wheel(second, *PE, TRUNCATED_ELF)
    completed = check(unreadable, misshapen)

    assert completed.returncode == 1
    assert completed.stdout.splitlines() == [f"FAIL {unreadable.name}", f"FAIL {misshapen.name}"]
    assert "tag win_amd64 implies" in completed.stderr


@pytest.mark.parametrize(
    ("target", "payload"),
    [
        (ELF, elf_module(0x3E, ("GLIBC_2.2.5", "GLIBC_2.3"), X86_64_RELATIVE_AND_DTPMOD64)),
        (ELF, elf_module(0x3E, ("GLIBC_2.17",), X86_64_RELATIVE_AND_DTPMOD64)),
        (ELF_AARCH64, elf_module(0xB7, ("GLIBC_2.17",), (1028, 1031))),
    ],
    ids=["x86-64-glibc-2.3", "x86-64-glibc-2.17", "aarch64-tls-descriptors"],
)
def test_a_manylinux_module_within_its_tags_glibc_passes(target, payload, tmp_path):
    completed = check(wheel(tmp_path, *target, payload))

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.startswith("ok ")


@pytest.mark.parametrize("version", ["GLIBC_2.18", "GLIBC_2.34"])
def test_a_manylinux_module_needing_what_its_tags_glibc_lacks_fails(version, tmp_path):
    payload = elf_module(0x3E, ("GLIBC_2.2.5", version), X86_64_RELATIVE_AND_DTPMOD64)
    completed = check(wheel(tmp_path, *ELF, payload))

    assert completed.returncode == 1
    assert f"needs ['{version}'], which glibc 2.17 lacks" in completed.stderr
    assert "could not be checked" not in completed.stderr


@pytest.mark.parametrize("version", ["GLIBC_ABI_GNU2_TLS", "GLIBC_PRIVATE", "GLIBC_\u0662.\u0661\u0667"])
def test_a_manylinux_module_needing_an_unnumbered_glibc_version_fails(version, tmp_path):
    payload = elf_module(0x3E, ("GLIBC_2.2.5", version), X86_64_RELATIVE_AND_DTPMOD64)
    completed = check(wheel(tmp_path, *ELF, payload))

    assert completed.returncode == 1
    assert f"needs ['{version}'], unnumbered glibc version nodes that a glibc 2.17 floor cannot cover" in completed.stderr
    assert "lacks" not in completed.stderr


@pytest.mark.parametrize(
    ("overrides", "reason"),
    [
        ({"need_count": 1}, "2 needs of 1 declared"),
        ({"need_count": 0xFFFFFFFF}, "declares 4294967295 needs but chains 2"),
        ({"versions": ("GLIBC_2.17", "GLIBC_2.3"), "version_count": 1}, "declares 1 versions but chains more"),
        ({"version_count": 0xFFFF}, "65538 entries in 4 slots"),
        ({"first_version": 0}, "puts its first version 0 bytes in"),
        ({"last_next_need": 8}, "links 8 bytes ahead, into itself"),
        ({"needs_link": 3}, "is not a string in section 3"),
        ({"last_name": 10_000}, "name 10000 is not a string in section 2"),
        ({"relocation_entry_size": 0}, "misstate their entry size"),
        ({"symbol_entry_size": 0}, "misstate their entry size"),
        ({"section_header_size": 0}, "the section headers are 0 bytes each, not 64"),
        ({"duplicated_section": 3}, "overlap the section before them"),
        ({"empty_section_at": 1 << 40}, "run past the file's"),
    ],
    ids=[
        "need-count-under-the-chain",
        "need-count-over-the-chain",
        "version-count-under-the-chain",
        "version-count-over-the-section",
        "first-version-inside-its-need",
        "need-linked-into-itself",
        "needs-linked-to-themselves",
        "name-past-its-string-table",
        "relocations-without-an-entry-size",
        "symbols-without-an-entry-size",
        "section-headers-without-a-size",
        "version-needs-listed-twice",
        "an-empty-section-past-the-file",
    ],
)
def test_a_version_need_or_relocation_section_that_misstates_itself_fails_that_wheel_cleanly(
    overrides, reason, tmp_path
):
    payload = elf_module(0xB7, relocation_kinds=(1028, 1031), **{"versions": ("GLIBC_2.17",), **overrides})
    completed = check(wheel(tmp_path, *ELF_AARCH64, payload))

    assert completed.returncode == 1
    assert "could not be checked" in completed.stderr
    assert reason in completed.stderr


def test_a_version_need_chained_outside_its_section_fails_that_wheel_cleanly(tmp_path):
    payload = elf_module(
        0x3E, ("GLIBC_2.2.5",), X86_64_RELATIVE_AND_DTPMOD64, need_count=3, last_next_need=4096
    )
    completed = check(wheel(tmp_path, *ELF, payload))

    assert completed.returncode == 1
    assert "could not be checked" in completed.stderr
    assert "is outside its section" in completed.stderr


def test_a_manylinux_x86_64_module_using_tls_descriptors_fails(tmp_path):
    payload = elf_module(0x3E, ("GLIBC_2.2.5",), (8, 36))
    completed = check(wheel(tmp_path, *ELF, payload))

    assert completed.returncode == 1
    assert "reaches its thread-locals through ['R_X86_64_TLSDESC'], the gnu2 TLS dialect" in completed.stderr


@pytest.mark.parametrize(
    ("exporting", "listed"),
    [
        (wasm_exporting(*WASM_SIDE_MODULE_EXPORTS, kind=3), "'PyInit_salix (kind 3)'"),
        (
            WASM_HEADER
            + WASM_MEMORY_IMPORT
            + wasm_section(
                7,
                *(wasm_name(name) + b"\0\0" for name in WASM_SIDE_MODULE_EXPORTS),
                wasm_name("memory") + b"\x02\0",
            ),
            "'memory (kind 2)'",
        ),
    ],
    ids=["init-as-a-global", "memory-beside-the-init"],
)
def test_a_side_module_exporting_anything_but_functions_fails_by_name_and_kind(exporting, listed, tmp_path):
    completed = check(wheel(tmp_path, *WASM, exporting))

    assert completed.returncode == 1
    assert listed in completed.stderr
    assert "could not be checked" not in completed.stderr


def test_many_exports_naming_one_long_string_are_resolved_once(tmp_path):
    payload = elf_module(
        0x3E, ("GLIBC_2.2.5",), X86_64_RELATIVE_AND_DTPMOD64, symbol_count=40_000, name_length=1_000_000
    )
    completed = check(wheel(tmp_path, *ELF, payload))

    assert completed.returncode == 1
    assert "not PyInit_salix alone" in completed.stderr
    assert "could not be checked" not in completed.stderr


def test_a_wheel_that_outlasts_the_time_bound_fails_with_it(tmp_path):
    hung = wheel(tmp_path, *WASM, wasm_exporting(*WASM_SIDE_MODULE_EXPORTS))
    completed = run_tool(
        "-c",
        "import sys, time\n"
        f"sys.path.insert(0, {str(CHECK_WHEEL.parent)!r})\n"
        "import check_wheel\n"
        "check_wheel.CHECK_SECONDS = 0.2\n"
        "check_wheel.check = lambda path: iter(time.sleep(10) or ())\n"
        f"sys.argv = ['check_wheel.py', {str(hung)!r}]\n"
        "check_wheel.main()\n",
    )

    assert completed.returncode == 1
    assert completed.stdout.splitlines() == [f"FAIL {hung.name}"]
    assert "could not be checked within 0.2 s" in completed.stderr


def test_wheels_sharing_a_filename_each_get_their_own_verdict(tmp_path):
    good = tmp_path / "good"
    bad = tmp_path / "bad"
    good.mkdir()
    bad.mkdir()
    passing = wheel(good, *WASM, wasm_exporting(*WASM_SIDE_MODULE_EXPORTS))
    failing = wheel(bad, *WASM, wasm_exporting(*WASM_SIDE_MODULE_EXPORTS[:2]))
    completed = check(passing, failing)

    assert passing.name == failing.name
    assert completed.stdout.splitlines() == [f"ok   {passing.name}", f"FAIL {failing.name}"]
    assert str(failing) in completed.stderr
    assert str(passing) not in completed.stderr
