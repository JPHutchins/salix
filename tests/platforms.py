import sys

import pytest

starts_a_subprocess = pytest.mark.skipif(
    sys.platform == "emscripten", reason="Pyodide cannot start a subprocess"
)

recurses_through_c_until_recursion_error = pytest.mark.skipif(
    sys.platform == "emscripten",
    reason="Pyodide aborts on deep C recursion before CPython raises RecursionError",
)
