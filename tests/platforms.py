import os
import subprocess
import sys
import textwrap

import pytest

import salix

starts_a_subprocess = pytest.mark.skipif(
    sys.platform == "emscripten", reason="Pyodide cannot start a subprocess"
)

starts_a_thread = pytest.mark.skipif(sys.platform == "emscripten", reason="Pyodide cannot start a thread")

recurses_through_c_until_recursion_error = pytest.mark.skipif(
    sys.platform == "emscripten",
    reason="Pyodide aborts on deep C recursion before CPython raises RecursionError",
)


def printed_by_a_fresh_interpreter(probe: str) -> str:
    package_parent = os.path.dirname(os.path.dirname(salix.__file__))
    result = subprocess.run(
        [
            sys.executable,
            "-I",
            "-S",
            "-c",
            f"__import__('sys').path.insert(0, {package_parent!r})\n{textwrap.dedent(probe)}",
        ],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )

    assert result.returncode == 0, result.stderr

    return result.stdout.strip()
