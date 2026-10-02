import importlib.util
import runpy
from collections import Counter
from itertools import chain
from operator import attrgetter
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
BUILD_CONFIG = ROOT / "build_config.py"

pytestmark = [
    pytest.mark.skipif(not BUILD_CONFIG.exists(), reason="build_config.py is not beside these tests"),
    pytest.mark.skipif(
        importlib.util.find_spec("tomllib") is None and importlib.util.find_spec("tomli") is None,
        reason="build_config.py reads pyproject.toml with tomllib or tomli",
    ),
]


def test_every_c_file_is_a_build_or_test_source() -> None:
    assert Counter(
        path.relative_to(ROOT).as_posix()
        for directory in ("src", "tests/c")
        for path in (ROOT / directory).rglob("*.c")
    ) == Counter(
        chain.from_iterable(
            attrgetter("sources", "test_sources")(runpy.run_path(str(BUILD_CONFIG))["BUILD"])
        )
    )
