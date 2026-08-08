"""A field holds an arbitrary object, so the suite should pass arbitrary objects.

Split by hashability, because that is the one property of a field value the
struct itself is sensitive to.
"""

import array
import dataclasses
import datetime
import decimal
import enum
import fractions
import typing

from salix import Struct

# Exactly these four, not subclasses of them: the copy has to preserve the type,
# and a PyDict_Copy of a defaultdict is a dict. Empty ones are copied per
# instance; a non-empty one is refused, because copying it could only be shallow
# and its contents would still be shared. `struct_copies_default` in
# src/construct.h is what this mirrors, and it is the only list of the four that
# the suite keeps.
COPIED_WHEN_EMPTY = (list, dict, set, bytearray)

# A non-empty instance of each, for the half of the rule that refuses rather
# than copies. The types come from the tuple above; only the contents are
# spelled here, because no one seed constructs all four -- dict wants pairs and
# bytearray wants small ints. The assertion is what keeps the two from drifting.
NON_EMPTY = {list: [1], dict: {"k": 1}, set: {1}, bytearray: bytearray(b"x")}

# A seed is an exact instance of its own key, and it is non-empty; `b"x"` is a
# bytes rather than a bytearray and would not be refused at all.
assert set(NON_EMPTY) == set(COPIED_WHEN_EMPTY)
assert all(type(value) is kind and len(value) > 0 for kind, value in NON_EMPTY.items())


class Colour(enum.Enum):
    RED = 1
    GREEN = 2


class Flags(enum.IntFlag):
    NONE = 0
    ONE = 1


class Coordinate(typing.NamedTuple):
    latitude: float
    longitude: float


@dataclasses.dataclass(frozen=True)
class Frozen:
    label: str


@dataclasses.dataclass
class Mutable:
    label: str

    __hash__ = None  # type: ignore[assignment]


class Opaque:
    """Identity equality and identity hash, like most objects."""


class ByValue:
    def __init__(self, key: object) -> None:
        self.key = key

    def __eq__(self, other: object) -> bool:
        return isinstance(other, ByValue) and self.key == other.key

    def __hash__(self) -> int:
        return hash(self.key)

    def __repr__(self) -> str:
        return f"ByValue({self.key!r})"


class Inner(Struct):
    value: object


class Outer(Struct):
    inner: object
    tag: object


def _hashable() -> tuple[object, ...]:
    return (
        None,
        True,
        False,
        0,
        -1,
        2**128,
        3.5,
        -0.0,
        float("inf"),
        complex(1, 2),
        "",
        "text",
        "\N{SNOWMAN}\N{ROCKET}",
        b"bytes",
        (),
        (1, ("nested",)),
        frozenset({1, 2}),
        range(3),
        Colour.RED,
        Flags.ONE,
        Coordinate(1.0, 2.0),
        Frozen("label"),
        Opaque(),
        ByValue("key"),
        decimal.Decimal("1.5"),
        fractions.Fraction(1, 3),
        datetime.date(2026, 7, 27),
        memoryview(b"bytes"),
        Inner(1),
        Inner(Inner("deep")),
        int,
        len,
        Ellipsis,
        NotImplemented,
    )


def _unhashable() -> tuple[object, ...]:
    return (
        [],
        [1, 2],
        {},
        {"key": "value"},
        set(),
        {1, 2},
        bytearray(),
        bytearray(b"bytes"),
        array.array("i", [1, 2]),
        memoryview(bytearray(b"bytes")),
        Mutable("label"),
    )


HASHABLE = _hashable()
UNHASHABLE = _unhashable()
EVERY = HASHABLE + UNHASHABLE

# Both halves of the rule are reached by parametrizing over these, so each of
# the four needs an empty instance and a non-empty one. set and bytearray had
# only the non-empty one, which left the copy -- and with it the severing that
# class creation does -- asserted for list and dict alone.
assert all(
    {not value for value in UNHASHABLE if type(value) is kind} == {True, False}
    for kind in COPIED_WHEN_EMPTY
)


def _shares_mutable_contents(value: object) -> bool:
    """The type says it hashes and the instance then refuses, which is how a
    container of something mutable answers. `refuse_shared_mutable_contents` in
    src/fields.c is what this mirrors. ValueError as well as TypeError: a
    writable memoryview raises the first.
    """

    if type(value).__hash__ is None:
        return False

    try:
        hash(value)
    except (TypeError, ValueError):
        return True

    return False


# What salix refuses as a class-body default for holding something mutable that
# every instance would otherwise share. Selected from EVERY rather than built
# fresh, because the callers below test identity against the parametrized value.
REFUSED_AS_DEFAULT = tuple(value for value in EVERY if _shares_mutable_contents(value))

# The membership, stated rather than left to the rule, so widening or narrowing
# the rule has to come here and say so. A writable memoryview is the only value
# in the set whose type claims a hash that the instance then refuses; everything
# else unhashable here declares __hash__ = None before being asked.
assert [type(value).__name__ for value in REFUSED_AS_DEFAULT] == ["memoryview"]


def refused_as_default(value: object) -> bool:
    return any(value is refused for refused in REFUSED_AS_DEFAULT)


def identify(value: object) -> str:
    """A stable, readable parametrize id for values whose repr is unwieldy."""

    return f"{type(value).__name__}-{id(value) % 9973:04d}"
