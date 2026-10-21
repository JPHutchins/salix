import pytest
from platforms import printed_by_a_fresh_interpreter, starts_a_subprocess

pytestmark = starts_a_subprocess


@pytest.mark.parametrize(
    ("first_use", "change", "printed"),
    [
        (True, "rebound", "'changed' [1] True"),
        (True, "mutated_in_place", "'changed' 'changed' True"),
        (False, "rebound", "'changed' 'changed' True"),
    ],
    ids=["rebound_after_first_use", "mutated_in_place_after_first_use", "rebound_before_first_use"],
)
def test_a_deepcopy_dispatch_rebound_after_the_first_struct_deepcopy_is_not_seen_but_one_mutated_in_place_is(
    first_use: bool, change: str, printed: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import salix

        class Holder(salix.Struct):
            items: list

        def changed(value, memo):
            return "changed"

        def rebound():
            copy._deepcopy_dispatch = {{**copy._deepcopy_dispatch, list: changed}}

        def mutated_in_place():
            copy._deepcopy_dispatch[list] = changed

        if {first_use}:
            copy.deepcopy(Holder([0]))
        {change}()
        source = [1]
        field = copy.deepcopy(Holder(source)).items
        print(repr(copy.deepcopy([1])), repr(field), field is not source)
    """) == printed


def test_a_dispatch_table_rebound_after_the_first_struct_deepcopy_is_still_seen():
    assert printed_by_a_fresh_interpreter("""
        import copy
        import salix

        class Point(salix.Struct):
            x: int

        def reduced(point):
            return (str, ("reduced",))

        copy.deepcopy(Point(1))
        copy.dispatch_table = {**copy.dispatch_table, Point: reduced}
        print(copy.deepcopy(Point(1)), Point(1).__deepcopy__({}))
    """) == "reduced reduced"


@pytest.mark.parametrize(
    ("first_use", "printed"),
    [(False, "str True"), (True, "Plain True")],
    ids=["replaced_before_first_use", "replaced_after_first_use"],
)
def test_a_copy_module_replaced_after_the_first_struct_deepcopy_is_not_seen(first_use: bool, printed: str):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import sys
        import types
        import salix

        class Plain:
            pass

        class Holder(salix.Struct):
            value: object

        if {first_use}:
            copy.deepcopy(Holder(Plain()))
        replacement = types.ModuleType("copy")
        replacement.__dict__.update(copy.__dict__)
        replacement.deepcopy = lambda value, memo=None: "replaced"
        sys.modules["copy"] = replacement
        original = Plain()
        copied = Holder(original).__deepcopy__({{}}).value
        print(type(copied).__name__, copied is not original)
    """) == printed


@pytest.mark.parametrize(
    ("first_use", "swap", "reduces_with_the_current_module"),
    [
        (True, "reloaded", "True"),
        (True, "replaced_in_sys_modules", "False"),
        (False, "replaced_in_sys_modules", "True"),
    ],
    ids=["reloaded_after_first_use", "replaced_after_first_use", "replaced_before_first_use"],
)
def test_a_copyreg_reloaded_after_the_first_pickle_is_seen_but_one_replaced_in_sys_modules_is_not(
    first_use: bool, swap: str, reduces_with_the_current_module: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copyreg
        import importlib.util
        import sys
        import salix

        class Point(salix.Struct):
            x: int

        def reloaded():
            importlib.reload(copyreg)

        def replaced_in_sys_modules():
            specification = importlib.util.find_spec("copyreg")
            fresh = importlib.util.module_from_spec(specification)
            sys.modules["copyreg"] = fresh
            specification.loader.exec_module(fresh)

        if {first_use}:
            Point(1).__reduce_ex__(2)
        {swap}()
        print(Point(1).__reduce_ex__(2)[0] is sys.modules["copyreg"].__newobj__)
    """) == reduces_with_the_current_module


@pytest.mark.parametrize(
    ("deepcopy_at_first_use", "printed"),
    [("real", "0 [1, 2]"), ("replaced", "1 [1, 2]")],
    ids=["stdlib_at_first_use", "replaced_at_first_use"],
)
def test_the_deepcopy_fast_path_is_decided_by_the_copy_dot_deepcopy_bound_at_the_first_struct_deepcopy(
    deepcopy_at_first_use: str, printed: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import salix
        from deepcopy_calls import deepcopy_calls_from_outside_copy

        real = copy.deepcopy

        def replaced(value, memo=None):
            return real(value, memo)

        class Plain:
            pass

        class Holder(salix.Struct):
            value: object

        copy.deepcopy = {deepcopy_at_first_use}
        copy.deepcopy(Holder(Plain()))
        copy.deepcopy = real

        class Defaulted(salix.Struct):
            items: list = [1, 2]

        calls = deepcopy_calls_from_outside_copy(Defaulted)
        Defaulted().items.append(3)
        print(calls, Defaulted().items)
    """) == printed


@pytest.mark.parametrize(
    ("deepcopy_at_first_use", "deepcopy_after_first_use", "recorded"),
    [("recording", "real", "True"), ("real", "recording", "False")],
    ids=["recording_at_first_use", "recording_after_first_use"],
)
def test_default_copies_call_the_copy_dot_deepcopy_bound_when_the_first_defaulted_class_was_built(
    deepcopy_at_first_use: str, deepcopy_after_first_use: str, recorded: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import salix

        real = copy.deepcopy
        recorded = []

        def recording(value, memo=None):
            recorded.append(value)
            return real(value, memo)

        copy.deepcopy = {deepcopy_at_first_use}

        class Nested(salix.Struct):
            items: list = [[1]]

        copy.deepcopy = {deepcopy_after_first_use}
        recorded.clear()
        built = Nested()
        print(bool(recorded), built.items, built.items[0] is not Nested().items[0])
    """) == f"{recorded} [[1]] True"
