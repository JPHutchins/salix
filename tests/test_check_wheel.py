import base64
import hashlib
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

CHECK_WHEEL = Path(__file__).resolve().parent.parent / "tools" / "check_wheel.py"

pytestmark = pytest.mark.skipif(not CHECK_WHEEL.exists(), reason="tools/ is not beside these tests")

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


def check(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECK_WHEEL), str(path)],
        capture_output=True,
        text=True,
        check=False,
        timeout=60,
    )


TRUNCATED_ELF = b"\x7fELF" + bytes(14) + struct.pack("<H", 0x3E) + bytes(4)

MACHO_WITH_A_ZERO_SIZE_COMMAND = (
    b"\xcf\xfa\xed\xfe" + struct.pack("<IIIIIII", 0x01000007, 3, 6, 1, 8, 0, 0) + struct.pack("<II", 0x19, 0)
)

MACHO_WITHOUT_A_SYMBOL_TABLE = (
    b"\xcf\xfa\xed\xfe" + struct.pack("<IIIIIII", 0x01000007, 3, 6, 1, 8, 0, 0) + struct.pack("<II", 0x19, 8)
)

PE_EXPORTS_OUTSIDE_EVERY_SECTION = (
    b"MZ"
    + bytes(0x3A)
    + struct.pack("<I", 0x40)
    + b"PE\0\0"
    + struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, 240, 0)
    + struct.pack("<H", 0x20B)
    + bytes(110)
    + struct.pack("<II", 0x1000, 0x40)
    + bytes(120)
)

PE32_NOT_PLUS = (
    b"MZ"
    + bytes(0x3A)
    + struct.pack("<I", 0x40)
    + b"PE\0\0"
    + struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, 224, 0)
    + struct.pack("<H", 0x10B)
    + bytes(222)
)


@pytest.mark.parametrize(
    ("platform", "payload_name", "payload", "reason"),
    [
        ("manylinux_2_17_x86_64", "salix/__init__.cpython-313-x86_64-linux-gnu.so", TRUNCATED_ELF, "unpack_from"),
        ("macosx_10_13_x86_64", "salix/__init__.cpython-313-darwin.so", MACHO_WITH_A_ZERO_SIZE_COMMAND, "has size 0"),
        ("macosx_10_13_x86_64", "salix/__init__.cpython-313-darwin.so", MACHO_WITHOUT_A_SYMBOL_TABLE, "LC_SYMTAB"),
        ("win_amd64", "salix/__init__.cp313-win_amd64.pyd", PE_EXPORTS_OUTSIDE_EVERY_SECTION, "no file-backed section"),
        ("win_amd64", "salix/__init__.cp313-win_amd64.pyd", PE32_NOT_PLUS, "not PE32+"),
    ],
    ids=["truncated-elf", "macho-zero-size-command", "macho-no-symtab", "pe-rva-outside-sections", "pe32"],
)
def test_a_payload_whose_exports_cannot_be_read_fails_that_wheel_cleanly(platform, payload_name, payload, reason, tmp_path):
    completed = check(wheel(tmp_path, platform, payload_name, payload))

    assert completed.returncode == 1
    assert "Traceback" not in completed.stderr
    assert "exports could not be read" in completed.stderr
    assert reason in completed.stderr
    assert completed.stdout.startswith("FAIL ")


def test_a_payload_of_the_wrong_shape_reports_only_its_shape(tmp_path):
    completed = check(
        wheel(tmp_path, "win_amd64", "salix/__init__.cp313-win_amd64.pyd", TRUNCATED_ELF)
    )

    assert completed.returncode == 1
    assert "tag win_amd64 implies" in completed.stderr
    assert "exports" not in completed.stderr
