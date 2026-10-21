import copy
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

starts_a_subprocess = pytest.mark.skipif(
    sys.platform == "emscripten", reason="Pyodide cannot start a subprocess"
)

starts_a_thread = pytest.mark.skipif(sys.platform == "emscripten", reason="Pyodide cannot start a thread")

recurses_through_c_until_recursion_error = pytest.mark.skipif(
    sys.platform == "emscripten",
    reason="Pyodide aborts on deep C recursion before CPython raises RecursionError",
)

COPY_ATOMIC_TYPES = getattr(copy, "_atomic_types", None)

rebinds_copy_atomic_types = pytest.mark.skipif(
    COPY_ATOMIC_TYPES is None, reason="copy._atomic_types exists from 3.14"
)

adds_to_copy_atomic_types_in_place = pytest.mark.skipif(
    not isinstance(COPY_ATOMIC_TYPES, set), reason="copy._atomic_types is a mutable set on 3.14 only"
)

TESTS_DIRECTORY = str(Path(__file__).resolve().parent)


def printed_by_a_fresh_interpreter(probe: str, *, imports_test_helpers: bool = False) -> str:
    import salix

    package_parent = os.path.dirname(os.path.dirname(salix.__file__))
    try:
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                "-S",
                "-c",
                f"__import__('sys').path.insert(0, {package_parent!r})\n"
                + (f"__import__('sys').path.append({TESTS_DIRECTORY!r})\n" if imports_test_helpers else "")
                + textwrap.dedent(probe),
            ],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    except subprocess.TimeoutExpired as hung:
        pytest.fail(f"the probe hung for {hung.timeout} s\nstdout: {decoded(hung.stdout)}\nstderr: {decoded(hung.stderr)}")

    assert result.returncode == 0, f"exit {result.returncode}\nstdout: {result.stdout}\nstderr: {result.stderr}"

    return result.stdout.strip()


def run_tool(*command: str | os.PathLike[str], cwd: os.PathLike[str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, *map(str, command)],
        cwd=cwd,
        capture_output=True,
        encoding="utf-8",
        env={**os.environ, "PYTHONIOENCODING": "utf-8"},
        check=False,
        timeout=60,
    )


def decoded(output: str | bytes | None) -> str:
    match output:
        case bytes():
            return output.decode(errors="replace")
        case str():
            return output
        case None:
            return ""
