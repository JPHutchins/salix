import os
import subprocess
import sys
import textwrap

import pytest
from platforms import starts_a_subprocess

import salix

pytestmark = starts_a_subprocess


def printed_by_a_fresh_interpreter(probe: str) -> str:
    package_parent = os.path.dirname(os.path.dirname(salix.__file__))
    result = subprocess.run(
        [
            sys.executable,
            "-I",
            "-c",
            f"import sys\nsys.path.insert(0, {package_parent!r})\n{textwrap.dedent(probe)}",
        ],
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return result.stdout.strip()


@pytest.mark.parametrize(
    ("change", "struct_field"),
    [
        ("copy._deepcopy_dispatch = {**copy._deepcopy_dispatch, list: changed}", "[1]"),
        ("copy._deepcopy_dispatch[list] = changed", "changed"),
    ],
    ids=["rebound", "mutated_in_place"],
)
def test_a_deepcopy_dispatch_rebound_after_the_first_struct_deepcopy_is_not_seen_but_one_mutated_in_place_is(
    change: str, struct_field: str
):
    printed = printed_by_a_fresh_interpreter(f"""
        import copy
        import salix

        class Holder(salix.Struct):
            items: list

        def changed(value, memo):
            return "changed"

        copy.deepcopy(Holder([1]))
        {change}
        print(copy.deepcopy([1]), copy.deepcopy(Holder([1])).items)
    """)

    assert printed == f"changed {struct_field}"


def test_a_dispatch_table_rebound_after_the_first_struct_deepcopy_is_still_seen():
    printed = printed_by_a_fresh_interpreter("""
        import copy
        import salix

        class Point(salix.Struct):
            x: int

        def reduced(point):
            return (str, ("reduced",))

        copy.deepcopy(Point(1))
        copy.dispatch_table = {**copy.dispatch_table, Point: reduced}
        print(copy.deepcopy(Point(1)), Point(1).__deepcopy__({}))
    """)

    assert printed == "reduced reduced"


@pytest.mark.parametrize(
    ("first_deepcopy", "field_type"),
    [("", "str"), ("copy.deepcopy(Holder(Plain()))", "Plain")],
    ids=["replaced_before_first_use", "replaced_after_first_use"],
)
def test_a_copy_module_replaced_after_the_first_struct_deepcopy_is_not_seen(
    first_deepcopy: str, field_type: str
):
    printed = printed_by_a_fresh_interpreter(f"""
        import copy
        import types
        import salix

        class Plain:
            pass

        class Holder(salix.Struct):
            value: object

        {first_deepcopy}
        replacement = types.ModuleType("copy")
        replacement.__dict__.update(copy.__dict__)
        replacement.deepcopy = lambda value, memo=None: "replaced"
        sys.modules["copy"] = replacement
        print(type(Holder(Plain()).__deepcopy__({{}}).value).__name__)
    """)

    assert printed == field_type


@pytest.mark.parametrize(
    ("swap", "reduces_with_the_current_module"),
    [
        ("importlib.reload(copyreg)", "True"),
        (
            "specification = importlib.util.find_spec('copyreg')\n"
            "        fresh = importlib.util.module_from_spec(specification)\n"
            "        specification.loader.exec_module(fresh)\n"
            "        sys.modules['copyreg'] = fresh",
            "False",
        ),
    ],
    ids=["reloaded", "replaced_in_sys_modules"],
)
def test_a_copyreg_reloaded_after_the_first_pickle_is_seen_but_one_replaced_in_sys_modules_is_not(
    swap: str, reduces_with_the_current_module: str
):
    printed = printed_by_a_fresh_interpreter(f"""
        import copyreg
        import importlib
        import importlib.util
        import pickle
        import salix

        class Point(salix.Struct):
            x: int

        Point(1).__reduce_ex__(2)
        {swap}
        reduced_with = Point(1).__reduce_ex__(2)[0]
        restored = pickle.loads(pickle.dumps(Point(1), 2))
        print(reduced_with is sys.modules["copyreg"].__newobj__, restored == Point(1))
    """)

    assert printed == f"{reduces_with_the_current_module} True"


@pytest.mark.parametrize(
    ("deepcopy_at_first_use", "deepcopy_calls"),
    [("real", "0"), ("lambda value, memo=None: real(value, memo)", "1")],
    ids=["stdlib_at_first_use", "replaced_at_first_use"],
)
def test_the_deepcopy_fast_path_is_decided_by_the_copy_dot_deepcopy_bound_at_the_first_struct_deepcopy(
    deepcopy_at_first_use: str, deepcopy_calls: str
):
    printed = printed_by_a_fresh_interpreter(f"""
        import copy
        import salix

        real = copy.deepcopy

        class Plain:
            pass

        class Holder(salix.Struct):
            value: object

        copy.deepcopy = {deepcopy_at_first_use}
        copy.deepcopy(Holder(Plain()))
        copy.deepcopy = real

        class Defaulted(salix.Struct):
            items: list = [1, 2]

        calls = 0

        def profile(frame, event, argument):
            global calls
            called_from_outside_copy = frame.f_back is None or frame.f_back.f_code.co_filename != copy.__file__
            if event == "call" and frame.f_code is real.__code__ and called_from_outside_copy:
                calls += 1

        sys.setprofile(profile)
        Defaulted()
        sys.setprofile(None)
        print(calls)
    """)

    assert printed == deepcopy_calls
