import sys
from dataclasses import dataclass
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


def test_equality_is_nominal_like_stock_dataclasses():
    @dataclass
    class StockPoint:
        x: int
        y: int

    @dataclass
    class StockSameShape:
        x: int
        y: int

    assert StockPoint(1, 2) != StockSameShape(1, 2)
    assert Point(1, 2) != SameShape(1, 2)


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


def test_structs_of_two_classes_keep_two_set_entries():
    assert hash(Point(1, 2)) == hash(SameShape(1, 2))
    assert len({Point(1, 2), SameShape(1, 2)}) == 2


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


class Builder(Protocol):
    def build(self) -> object: ...


class BuilderStructMeta(type(Struct), type(Builder)):
    pass


class Built(Struct, Builder, metaclass=BuilderStructMeta):
    bar: str = "foo"

    def build(self) -> object:
        return self.bar


class BuiltSubclass(Built):
    extra: int = 0


class ConcreteBuilder(Builder):
    pass


class BuiltOverConcrete(Struct, ConcreteBuilder, metaclass=BuilderStructMeta):
    bar: str = "foo"


class AuthoredBuilder(Protocol):
    def __init__(self, bar: str) -> None:
        self.authored = bar


class AuthoredStructMeta(type(Struct), type(AuthoredBuilder)):
    pass


def test_a_protocol_base_keeps_the_generated_constructor():
    assert Built(bar="baz").bar == "baz"
    assert Built("baz").build() == "baz"
    assert Built().bar == "foo"


def test_a_subclass_of_a_protocol_struct_keeps_the_generated_constructor():
    assert BuiltSubclass(bar="baz", extra=1) == BuiltSubclass("baz", 1)
    assert (BuiltSubclass(bar="baz", extra=1).bar, BuiltSubclass(bar="baz", extra=1).extra) == ("baz", 1)


def test_a_concrete_subclass_of_a_protocol_in_the_bases_keeps_the_generated_constructor():
    assert BuiltOverConcrete(bar="baz").bar == "baz"


@pytest.mark.skipif(
    sys.version_info < (3, 11), reason="typing replaces every protocol __init__ with its placeholder before 3.11"
)
def test_an_authored_protocol_init_still_owns_the_construction():
    class AuthoredBuilt(Struct, AuthoredBuilder, frozen=False, metaclass=AuthoredStructMeta):
        bar: str = "foo"
        authored: str = "unset"

    assert AuthoredBuilt("baz").authored == "baz"
    assert AuthoredBuilt("baz").bar == "foo"
