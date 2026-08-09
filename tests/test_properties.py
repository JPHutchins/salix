from hypothesis import given
from hypothesis import strategies as st
from values import HASHABLE, Coordinate, Frozen, Inner

from salix import Struct


class Pair(Struct):
    x: object
    y: object


IDENTIFIERS = st.from_regex(r"\A[a-z][a-z0-9_]{0,10}\Z")

# No NaN: it is not equal to itself, which is a property of float rather than
# of anything here. Structs are in the leaves as well as the branches, so a
# field holding another struct is generated rather than only hand-written.
LEAVES = (
    st.sampled_from(HASHABLE)
    | st.none()
    | st.booleans()
    | st.integers()
    | st.floats(allow_nan=False)
    | st.text()
    | st.binary()
)
VALUES = st.recursive(
    LEAVES,
    lambda children: st.tuples(children)
    | st.frozensets(children)
    | st.builds(Inner, children)
    | st.builds(Coordinate, st.floats(allow_nan=False), st.floats(allow_nan=False))
    | st.builds(Frozen, st.text()),
    max_leaves=5,
)
# Built rather than sampled: hypothesis hashes a sampled_from pool to dedupe it,
# and a writable memoryview raises ValueError there rather than TypeError.
UNHASHABLE_VALUES = (
    st.lists(LEAVES)
    | st.dictionaries(LEAVES, LEAVES)
    | st.sets(LEAVES)
    | st.builds(bytearray, st.binary())
    | st.builds(lambda payload: memoryview(bytearray(payload)), st.binary())
)


@given(VALUES, VALUES)
def test_a_field_reads_back_as_the_object_that_was_passed(first, second):
    pair = Pair(first, second)

    assert pair.x is first
    assert pair.y is second


@given(VALUES, VALUES)
def test_equal_values_make_equal_structs(first, second):
    assert Pair(first, second) == Pair(first, second)


@given(VALUES, VALUES)
def test_a_struct_hashes_as_the_tuple_of_its_values(first, second):
    assert hash(Pair(first, second)) == hash((first, second))


@given(VALUES, VALUES)
def test_equality_implies_equal_hashes(first, second):
    left, right = Pair(first, second), Pair(first, second)

    # SIM201: the claim is about __eq__, and != is a different operator.
    assert not (left == right) or hash(left) == hash(right)  # noqa: SIM201


@given(st.lists(IDENTIFIERS, min_size=0, max_size=6, unique=True))
def test_field_names_survive_a_round_trip(names):
    struct_class = type(Struct)(
        "Generated", (Struct,), {"__annotations__": {name: object for name in names}}
    )
    values = tuple(range(len(names)))
    instance = struct_class(*values)

    assert instance._struct_fields_ == tuple(names)
    assert instance.__match_args__ == tuple(names)
    assert tuple(getattr(instance, name) for name in names) == values


@given(st.lists(IDENTIFIERS, min_size=1, max_size=5, unique=True), st.data())
def test_every_field_is_reachable_by_keyword(names, data):
    struct_class = type(Struct)(
        "Generated", (Struct,), {"__annotations__": {name: object for name in names}}
    )
    arguments = {name: data.draw(st.integers()) for name in names}
    instance = struct_class(**arguments)

    assert {name: getattr(instance, name) for name in names} == arguments


@given(UNHASHABLE_VALUES, UNHASHABLE_VALUES)
def test_an_unhashable_field_still_round_trips_and_compares(first, second):
    pair = Pair(first, second)

    assert pair.x is first
    assert pair.y is second
    assert pair == Pair(first, second)
