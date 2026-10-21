import os
import subprocess
import sys
import textwrap

import pytest

starts_a_subprocess = pytest.mark.skipif(
    sys.platform == "emscripten", reason="Pyodide cannot start a subprocess"
)

starts_a_thread = pytest.mark.skipif(sys.platform == "emscripten", reason="Pyodide cannot start a thread")

recurses_through_c_until_recursion_error = pytest.mark.skipif(
    sys.platform == "emscripten",
    reason="Pyodide aborts on deep C recursion before CPython raises RecursionError",
)


def printed_by_a_fresh_interpreter(probe: str) -> str:
    import salix

    package_parent = os.path.dirname(os.path.dirname(salix.__file__))
    try:
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
    except subprocess.TimeoutExpired as hung:
        pytest.fail(f"the probe hung for {hung.timeout} s\nstdout: {decoded(hung.stdout)}\nstderr: {decoded(hung.stderr)}")

    assert result.returncode == 0, f"exit {result.returncode}\nstdout: {result.stdout}\nstderr: {result.stderr}"

    return result.stdout.strip()


def decoded(output: str | bytes | None) -> str:
    match output:
        case bytes():
            return output.decode(errors="replace")
        case str():
            return output
        case None:
            return ""
