import pytest
from platforms import (
    adds_to_copy_atomic_types_in_place,
    printed_by_a_fresh_interpreter,
    rebinds_copy_atomic_types,
    starts_a_subprocess,
)

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
    ("first_use", "change"),
    [(True, "rebound"), (True, "mutated_in_place"), (False, "rebound")],
    ids=["rebound_after_first_use", "mutated_in_place_after_first_use", "rebound_before_first_use"],
)
def test_a_default_copy_sees_a_dispatch_table_change_as_copy_deepcopy_does_before_or_after_first_use(
    first_use: bool, change: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import salix

        class Warm(salix.Struct):
            items: list = [[0]]

        def reduced(value):
            return (frozenset, (list(value),))

        def rebound():
            copy.dispatch_table = {{**copy.dispatch_table, set: reduced}}

        def mutated_in_place():
            copy.dispatch_table[set] = reduced

        if {first_use}:
            Warm()
        {change}()
        declared = {{1, 2}}

        class Holder(salix.Struct):
            v: set = declared

        print(type(Holder().v).__name__, type(copy.deepcopy(declared)).__name__)
    """) == "frozenset frozenset"


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
    """, imports_test_helpers=True) == printed


@pytest.mark.parametrize(
    ("deepcopy_at_first_use", "deepcopy_after_first_use", "construction_recorded"),
    [("recording", "real", "True"), ("real", "recording", "False")],
    ids=["recording_at_first_use", "recording_after_first_use"],
)
def test_default_copies_call_the_copy_dot_deepcopy_bound_when_the_first_defaulted_class_was_built(
    deepcopy_at_first_use: str, deepcopy_after_first_use: str, construction_recorded: str
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
    """) == f"{construction_recorded} [[1]] True"


@rebinds_copy_atomic_types
@pytest.mark.parametrize(
    ("declared", "printed"),
    [
        ("{1, 2}", "True True 0 True"),
        ("{'a': 1}", "True True 0 True"),
        ("set()", "False False 0 True"),
        ("{}", "False False 0 True"),
    ],
    ids=["set", "dict", "empty_set", "empty_dict"],
)
def test_a_container_registered_as_atomic_before_first_use_is_shared_without_a_deepcopy_unless_empty(
    declared: str, printed: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import salix
        from deepcopy_calls import result_and_deepcopy_calls_from_outside_copy

        declared = {declared}
        copy._atomic_types = copy._atomic_types | {{type(declared)}}

        class Holder(salix.Struct):
            v: object = declared

        (first, second), calls = result_and_deepcopy_calls_from_outside_copy(lambda: (Holder(), Holder()))
        print(first.v is second.v, first.v is declared, calls, copy.deepcopy(declared) is declared)
    """, imports_test_helpers=True) == printed


@rebinds_copy_atomic_types
def test_a_mutable_struct_registered_as_atomic_before_first_use_is_shared_without_a_deepcopy():
    assert printed_by_a_fresh_interpreter("""
        import copy
        import salix
        from deepcopy_calls import result_and_deepcopy_calls_from_outside_copy

        class Box(salix.Struct, frozen=False):
            items: list

        declared = Box([1])
        copy._atomic_types = copy._atomic_types | {Box}

        class Holder(salix.Struct):
            v: Box = declared

        (first, second), calls = result_and_deepcopy_calls_from_outside_copy(lambda: (Holder(), Holder()))
        print(first.v is second.v, first.v is declared, calls, copy.deepcopy(declared) is declared)
    """, imports_test_helpers=True) == "True True 0 True"


@rebinds_copy_atomic_types
@pytest.mark.parametrize(
    ("first_use", "registration", "printed"),
    [
        (False, "rebound", "True True"),
        (True, "rebound", "False True"),
        pytest.param(True, "added_in_place", "True True", marks=adds_to_copy_atomic_types_in_place),
    ],
    ids=["rebound_before_first_use", "rebound_after_first_use", "added_in_place_after_first_use"],
)
def test_an_atomic_registration_rebound_after_the_first_default_copy_is_not_seen_but_one_added_in_place_is(
    first_use: bool, registration: str, printed: str
):
    assert printed_by_a_fresh_interpreter(f"""
        import copy
        import salix

        class Warm(salix.Struct):
            items: list = [[0]]

        def rebound():
            copy._atomic_types = copy._atomic_types | {{set}}

        def added_in_place():
            copy._atomic_types.add(set)

        if {first_use}:
            Warm()
        {registration}()
        declared = {{1, 2}}

        class Holder(salix.Struct):
            v: object = declared

        print(Holder().v is Holder().v, copy.deepcopy(declared) is declared)
    """) == printed
