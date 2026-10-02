import importlib.util
import runpy
from collections import Counter
from pathlib import Path
from typing import Any

import pytest

ROOT = Path(__file__).resolve().parent.parent
BUILD_CONFIG = ROOT / "build_config.py"
TEST_SUPPORT_SOURCE = "src/testing.c"

pytestmark = [
    pytest.mark.skipif(not BUILD_CONFIG.exists(), reason="build_config.py is not beside these tests"),
    pytest.mark.skipif(
        importlib.util.find_spec("tomllib") is None and importlib.util.find_spec("tomli") is None,
        reason="build_config.py reads pyproject.toml with tomllib or tomli",
    ),
]


def c_files(directory: str) -> Counter[str]:
    return Counter(path.relative_to(ROOT).as_posix() for path in (ROOT / directory).rglob("*.c"))


@pytest.fixture
def build() -> Any:
    return runpy.run_path(str(BUILD_CONFIG))["BUILD"]


def test_every_c_file_under_src_is_a_build_source(build: Any) -> None:
    assert Counter(build.sources) == c_files("src") - Counter([TEST_SUPPORT_SOURCE])


def test_every_c_file_under_tests_c_is_a_test_source(build: Any) -> None:
    assert Counter(build.test_sources) == c_files("tests/c") + Counter([TEST_SUPPORT_SOURCE])
