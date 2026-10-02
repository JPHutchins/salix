import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
BUILD_CONFIG = ROOT / "build_config.py"
COMPILED_ONLY_INTO_THE_C_TESTS = frozenset({"src/testing.c"})

pytestmark = [
    pytest.mark.skipif(not BUILD_CONFIG.exists(), reason="build_config.py is not beside these tests"),
    pytest.mark.skipif(sys.version_info < (3, 11), reason="build_config.py needs tomllib"),
]


def test_every_c_file_under_src_is_a_build_source() -> None:
    assert {
        path.relative_to(ROOT).as_posix() for path in (ROOT / "src").rglob("*.c")
    } - COMPILED_ONLY_INTO_THE_C_TESTS == set(
        subprocess.run(
            [sys.executable, str(BUILD_CONFIG), "sources"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split()
    )
