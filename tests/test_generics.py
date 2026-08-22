from typing import Generic, TypeVar

from salix import Struct

T = TypeVar("T")
U = TypeVar("U")


def test_a_classic_generic_struct_constructs_and_matches():
    class Ok(Struct, Generic[T]):
        value: T

    ok = Ok(3)

    assert ok.value == 3
    assert Ok.__struct_fields__ == ("value",)
    assert Ok.__match_args__ == ("value",)
    assert Ok(3) == Ok(3)
    assert hash(Ok(3)) == hash((3,))

    match Ok("x"):
        case Ok(v):
            assert v == "x"
        case _:
            raise AssertionError


def test_a_classic_generic_struct_can_be_inherited():
    class Ok(Struct, Generic[T]):
        value: T

    class Tagged(Ok[T]):
        tag: str = ""

    assert Tagged.__struct_fields__ == ("value", "tag")


def test_a_classic_generic_struct_can_be_subscripted_and_constructed():
    class Ok(Struct, Generic[T]):
        value: T

    assert Ok[int](3).value == 3

    class Tagged(Ok[T]):
        tag: str = ""

    assert Tagged[int](3, "t").value == 3
    assert Tagged[int](3, "t").tag == "t"


def test_multiple_type_variables_bind_in_order():
    class Pair(Struct, Generic[T, U]):
        first: T
        second: U

    pair = Pair(1, "two")

    assert pair.first == 1
    assert pair.second == "two"


def test_a_generic_struct_takes_defaults_and_mutability():
    class Mutable(Struct, Generic[T], frozen=False):
        value: T = 0

    assert Mutable().value == 0
    assert Mutable("x").value == "x"


def test_an_interned_zero_field_generic_stays_interned():
    class Nul(Struct, Generic[T]):
        pass

    assert Nul() is Nul()


def test_the_annotations_metadata_holds_the_type_variable():
    class Ok(Struct, Generic[T]):
        value: T

    assert Ok._struct_annotations_ == (T,)


def test_replace_and_copy_work_on_generic_instances():
    import copy

    class Ok(Struct, Generic[T]):
        value: T

    assert Ok(3).__replace__(value=4).value == 4
    assert copy.copy(Ok(3)).value == 3
