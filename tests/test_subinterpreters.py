import sys

import pytest

from salix import Struct

sub = pytest.importorskip("_xxsubinterpreters")

IMPORT_CODE = f"import sys\nsys.path[:] = {sys.path!r}\nimport salix"


def run_in_subinterpreter(code):
    interp = sub.create()

    try:
        sub.run_string(interp, code)
    finally:
        sub.destroy(interp)


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
