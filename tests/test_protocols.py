from typing import ClassVar, Protocol

import pytest

from salix import Struct, set_field


class Point(Struct):
    x: int
    y: int


class SameShape(Struct):
    x: int
    y: int


class Nested(Struct):
    items: list


def test_equality_is_structural_not_nominal():
    assert Point(1, 2) == SameShape(1, 2)


def test_differing_field_names_are_never_equal():
    class Renamed(Struct):
        x: int
        z: int

    assert Point(1, 2) != Renamed(1, 2)


def test_differing_arity_is_never_equal():
    class Shorter(Struct):
        x: int

    assert Point(1, 2) != Shorter(1)


def test_comparison_with_a_non_struct_defers():
    assert Point(1, 2).__eq__(object()) is NotImplemented
    assert Point(1, 2) != object()


def test_ordering_is_unsupported():
    with pytest.raises(TypeError, match="not supported between instances"):
        _ = Point(1, 2) < Point(1, 3)


def test_a_raising_comparison_propagates():
    class Hostile:
        def __eq__(self, other):
            raise RuntimeError("no")

        __hash__ = None

    with pytest.raises(RuntimeError, match="no"):
        _ = Point(Hostile(), 1) == Point(Hostile(), 1)


def test_hash_matches_the_tuple_of_values():
    assert hash(Point(1, 2)) == hash((1, 2))


def test_equal_structs_share_a_hash_bucket():
    assert len({Point(1, 2), SameShape(1, 2)}) == 1


def test_an_unhashable_field_makes_the_struct_unhashable():
    with pytest.raises(TypeError, match="unhashable"):
        hash(Nested([1]))


def test_hash_of_a_struct_that_contains_itself():
    class Node(Struct):
        child: object = None

    node = Node()
    set_field(node, "child", node)

    with pytest.raises(RecursionError):
        hash(node)


def test_repr_round_trips_through_eval():
    point = Point(1, 2)

    assert eval(repr(point), {"Point": Point}) == point


def test_repr_of_a_struct_that_contains_itself():
    node = Nested([])
    node.items.append(node)

    assert repr(node) == "Nested(items=[...])"


def test_match_args_drives_positional_patterns():
    match Point(1, 2):
        case Point(x, y):
            assert (x, y) == (1, 2)
        case _:
            pytest.fail("positional pattern did not match")


def test_keyword_patterns_match_too():
    match Point(1, 2):
        case Point(y=2):
            pass
        case _:
            pytest.fail("keyword pattern did not match")


def test_instances_carry_no_dict_or_weakref_slot():
    point = Point(1, 2)

    assert not hasattr(point, "__dict__")

    with pytest.raises(TypeError):
        import weakref

        weakref.ref(point)


class Command(Protocol):
    COMMAND: ClassVar[bytes]


class PayloadCommand(Struct):
    COMMAND: ClassVar[bytes] = b"launch"
    payload: int


def test_a_classvar_protocol_member_is_a_class_variable_at_runtime():
    assert PayloadCommand(1).COMMAND == b"launch"
    assert PayloadCommand.COMMAND == b"launch"
    assert PayloadCommand._struct_fields_ == ("payload",)


def test_a_struct_class_cannot_inherit_a_protocol():
    with pytest.raises(TypeError):
        type(Struct)("Inheriting", (Struct, Command), {"__annotations__": {"payload": int}})
