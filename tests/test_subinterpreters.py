import gc
import os
import subprocess
import sys

import pytest

import salix
from salix import Struct

sub = pytest.importorskip("_xxsubinterpreters")

IMPORT_CODE = f"import sys\nsys.path[:] = {sys.path!r}\nimport salix"


def run_in_subinterpreter(code):
    interp = sub.create()

    try:
        sub.run_string(interp, code)
    finally:
        sub.destroy(interp)


def test_a_bare_import_exits_cleanly():
    """The module state is cleared in m_free when the module deallocates, and
    a bare interpreter reaches that deallocation -- the suite does not,
    because the classes it built keep the module alive. The crash that a
    wrong m_free argument produced only fired at that shutdown, so only this
    shape sees it."""

    env = os.environ.copy()
    env["PYTHONPATH"] = os.path.dirname(os.path.dirname(salix.__file__)) + os.pathsep + env.get(
        "PYTHONPATH", ""
    )
    result = subprocess.run(
        [sys.executable, "-c", "import salix"],
        env=env,
        capture_output=True,
        timeout=60,
        check=False,
    )

    assert result.returncode == 0


def test_settling_a_multi_base_class_leaks_no_module_reference():
    """The settle reaches its cache through sys.modules; that lookup must not
    hold a reference beyond the build, or every multi-base class pins the
    module alive."""

    before = sys.getrefcount(salix)

    class ByOrder(Struct):
        pass

    class ByEq(Struct):
        a: int

    class Both(ByOrder, ByEq, eq=False):
        b: int

    del Both, ByOrder, ByEq
    gc.collect()

    assert sys.getrefcount(salix) == before


def test_a_second_subinterpreter_import_is_refused_where_declared():
    """On 3.12 the module declares Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED,
    so the corrupting second import never happens. 3.13+ removed the
    Python-side subinterpreter API entirely, so nothing runs there."""

    if sys.version_info < (3, 12):
        pytest.skip("the refusing slot does not exist before 3.12")

    if sys.version_info >= (3, 13):
        pytest.skip("3.13 removed the Python-side subinterpreter API")

    with pytest.raises(sub.RunFailedError, match="does not support"):
        run_in_subinterpreter(IMPORT_CODE)


def test_a_second_subinterpreter_import_does_not_corrupt_this_interpreter():
    """The binding cache lives in per-interpreter module state, so a second
    subinterpreter's import -- which re-runs the module exec, fills its own
    cache, and frees it again at teardown -- leaves this interpreter's
    settle decisions intact: this eq=False multi-base record still hashes
    the way the record says."""

    if sys.version_info >= (3, 12):
        pytest.skip("the import is refused there; this path is 3.10/3.11")

    run_in_subinterpreter(IMPORT_CODE)

    class ByOrder(Struct):
        pass

    class ByEq(Struct):
        a: int

    class Both(ByOrder, ByEq, eq=False):
        b: int

    instance = Both(1, 2)

    assert Both.__hash__ is object.__hash__
    assert isinstance(hash(instance), int)
