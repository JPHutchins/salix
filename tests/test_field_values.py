"""A field is untyped storage: whatever goes in comes back out, unchanged.

Everything here runs against the whole value set rather than a stand-in int,
because the C never inspects a value except to compare, hash or repr it -- and
those three are exactly where an assumption about the type would bite.
"""

import pytest
from values import (
    COPIED_WHEN_EMPTY,
    EVERY,
    HASHABLE,
    UNHASHABLE,
    Inner,
    Outer,
    identify,
    refused_as_default,
)

from salix import Struct


class Pair(Struct):
    first: object
    second: object


class Defaulted(Struct):
    required: object
    optional: object = object()


@pytest.mark.parametrize("value", EVERY, ids=identify)
def test_a_value_survives_a_round_trip_unchanged(value):
    assert Pair(value, None).first is value
    assert Pair(None, value).second is value
    assert Pair(first=value, second=value).first is value


@pytest.mark.parametrize("value", EVERY, ids=identify)
def test_a_value_may_be_a_default(value):
    if type(value) in COPIED_WHEN_EMPTY and len(value) > 0:
        with pytest.raises(TypeError, match="non-empty"):

            class Refused(Struct):
                field: object = value

        return

    if refused_as_default(value):
        with pytest.raises(TypeError, match="cannot be hashed"):

            class SharesContents(Struct):
                field: object = value

        return

    class Local(Struct):
        field: object = value

    (stored,) = Local._struct_defaults_

    assert Local().field == value
    assert stored == value

    if type(value) in COPIED_WHEN_EMPTY:
        # Two copies, not one: the class keeps its own, severed from whatever
        # the body named, and each instance keeps one severed from the class's.
        assert stored is not value
        assert Local().field is not stored
        assert Local().field is not Local().field
    else:
        assert stored is value
        assert Local().field is value


@pytest.mark.parametrize("value", EVERY, ids=identify)
def test_repr_embeds_the_field_reprs(value):
    assert repr(Pair(value, value)) == f"Pair(first={value!r}, second={value!r})"


@pytest.mark.parametrize("value", EVERY, ids=identify)
def test_a_struct_equals_itself_whatever_it_holds(value):
    pair = Pair(value, value)

    assert pair == pair  # noqa: PLR0124 -- reflexivity is the assertion


@pytest.mark.parametrize("value", HASHABLE, ids=identify)
def test_a_hashable_field_makes_a_hashable_struct(value):
    assert hash(Pair(value, value)) == hash((value, value))


def refuses_to_hash_with(value: object) -> type[BaseException]:
    try:
        hash(value)
    except BaseException as error:  # noqa: BLE001 -- the type is what is being derived
        return type(error)

    raise AssertionError(f"{value!r} is hashable, so it does not belong in UNHASHABLE")


@pytest.mark.parametrize("value", UNHASHABLE, ids=identify)
def test_a_struct_hashes_no_better_than_the_fields_it_holds(value):
    """Not always TypeError: a writable memoryview raises ValueError."""

    with pytest.raises(refuses_to_hash_with(value)):
        hash(Pair(value, None))


@pytest.mark.parametrize("value", HASHABLE, ids=identify)
def test_structural_equality_holds_for_every_hashable_value(value):
    assert Pair(value, value) == Pair(value, value)


def test_a_struct_nests_inside_a_struct():
    inner = Inner(1)
    outer = Outer(inner, "tag")

    assert outer.inner is inner
    assert repr(outer) == "Outer(inner=Inner(value=1), tag='tag')"
    assert hash(outer) == hash((inner, "tag"))


def test_nested_structs_compare_structurally_all_the_way_down():
    assert Outer(Inner(1), "tag") == Outer(Inner(1), "tag")
    assert Outer(Inner(1), "tag") != Outer(Inner(2), "tag")


def test_nesting_can_go_arbitrarily_deep():
    deep = Inner("bottom")

    for _ in range(50):
        deep = Inner(deep)

    assert Inner(deep) == Inner(deep)
    assert repr(deep).count("Inner(") == 51


def test_a_struct_is_usable_as_a_dict_key():
    table = {Inner(1): "one", Inner("one"): "text"}

    assert table[Inner(1)] == "one"
    assert table[Inner("one")] == "text"


def test_equal_but_distinct_values_still_compare_equal():
    """Equality goes through the values, not through identity."""

    left, right = Pair([1, 2], (3,)), Pair([1, 2], (3,))

    assert left.first is not right.first
    assert left == right
