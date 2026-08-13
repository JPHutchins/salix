"""The README's ClassVar and caching tables, run rather than read."""

import functools
import sys
import typing

import pytest

from salix import Struct, set_field


@pytest.mark.parametrize(
    "annotation",
    [
        typing.ClassVar[int],
        typing.Optional[typing.Annotated[typing.ClassVar[int], "m"]],  # noqa: UP045
    ],
)
def test_the_object_path_refuses_class_var_forms(annotation):
    with pytest.raises(TypeError, match="ClassVar"):
        type("Probe", (Struct,), {"__annotations__": {"x": annotation}})


@pytest.mark.parametrize(
    "annotation",
    ["ClassVar", "Annotated[int, ClassVar]", "Optional[Annotated[ClassVar[int], m]]"],
)
def test_the_text_path_refuses_class_var_forms(annotation):
    with pytest.raises(TypeError, match="ClassVar"):
        type("Probe", (Struct,), {"__annotations__": {"x": annotation}})


def test_the_text_path_accepts_an_alias_and_the_field_swallows_a_positional():
    cls = type("Probe", (Struct,), {"__annotations__": {"x": "CV[int]"}})

    assert cls._struct_fields_ == ("x",)
    assert cls(7).x == 7


def test_the_object_path_accepts_a_type_named_class_var():
    class ClassVar:
        pass

    cls = type("Probe", (Struct,), {"__annotations__": {"x": ClassVar}})

    assert cls._struct_fields_ == ("x",)


def test_the_object_path_accepts_annotated_metadata():
    cls = type(
        "Probe",
        (Struct,),
        {"__annotations__": {"x": typing.Annotated[int, typing.ClassVar]}},
    )

    assert cls._struct_fields_ == ("x",)


@pytest.mark.skipif(sys.version_info < (3, 14), reason="3.14 evaluates resolvable annotations")
def test_an_unresolvable_annotation_arrives_as_an_object_and_is_accepted():
    import annotationlib

    class WithNope(Struct):
        x: Nope  # noqa: F821

    annotation = annotationlib.get_annotations(
        WithNope, format=annotationlib.Format.FORWARDREF
    )["x"]

    assert not isinstance(annotation, str)
    assert WithNope._struct_fields_ == ("x",)


def test_value_equal_instances_share_one_cache_entry():
    calls = []

    class Cached(Struct):
        x: int

        @functools.cache  # noqa: B019 -- the retained instances are the point
        def slow(self) -> int:
            calls.append(self.x)

            return self.x * 2

    assert Cached(1).slow() == 2
    assert Cached(1).slow() == 2

    assert calls == [1]


def test_mutating_a_cached_instance_misses_and_leaks_the_entry():
    class Cached(Struct):
        x: int

        @functools.cache  # noqa: B019 -- the leaked entry is the point
        def slow(self) -> int:
            return self.x * 2

    instance = Cached(1)

    assert instance.slow() == 2

    set_field(instance, "x", 5)

    assert instance.slow() == 10
    assert Cached.slow.cache_info().currsize == 2


def test_an_unhashable_struct_refuses_the_cache():
    class Mutable(Struct, frozen=False):
        x: int

        @functools.cache  # noqa: B019 -- the refusal is the point
        def slow(self) -> int:
            return self.x

    with pytest.raises(TypeError, match="unhashable"):
        Mutable(1).slow()


def test_a_body_defined_eq_is_unhashable_and_the_cache_refuses():
    class BodyEq(Struct):  # noqa: PLW1641 -- the unhashability is the point
        x: int

        def __eq__(self, other: object) -> bool:
            return True

        @functools.cache  # noqa: B019 -- the refusal is the point
        def slow(self) -> int:
            return self.x

    with pytest.raises(TypeError, match="unhashable"):
        BodyEq(1).slow()


def test_eq_false_hashes_by_identity_and_the_cache_returns_stale_values():
    class Identity(Struct, eq=False):
        x: int

        @functools.cache  # noqa: B019 -- the stale hit is the point
        def slow(self) -> int:
            return self.x * 2

    instance = Identity(1)
    other = Identity(1)

    assert instance.slow() == 2
    assert hash(instance) != hash(other)

    set_field(instance, "x", 5)

    assert instance.slow() == 2


def test_eq_false_mutable_structs_are_hashable():
    class MutableIdentity(Struct, eq=False, frozen=False):
        x: int

    first = MutableIdentity(1)
    second = MutableIdentity(1)

    assert hash(first) != hash(second)


def test_a_body_defined_eq_still_makes_an_eq_false_struct_unhashable():
    class BodyEq(Struct, eq=False):  # noqa: PLW1641 -- the unhashability is the point
        x: int

        def __eq__(self, other: object) -> bool:
            return True

    with pytest.raises(TypeError, match="unhashable"):
        hash(BodyEq(1))


def test_a_body_hash_none_makes_the_struct_unhashable_under_both_eq_options():
    class NoHash(Struct):
        x: int
        __hash__ = None  # type: ignore[assignment]

    with pytest.raises(TypeError, match="unhashable"):
        hash(NoHash(1))

    class NoHashEqFalse(Struct, eq=False):
        x: int
        __hash__ = None  # type: ignore[assignment]

    with pytest.raises(TypeError, match="unhashable"):
        hash(NoHashEqFalse(1))


def test_a_body_hash_wins_over_the_identity_hash_under_eq_false():
    class SevenHash(Struct, eq=False):
        x: int

        def __hash__(self) -> int:
            return 7

    assert hash(SevenHash(1)) == 7


def test_equality_inherited_from_a_struct_base_makes_the_subclass_unhashable():
    class EqStruct(Struct):  # noqa: PLW1641 -- the inherited unhashability is the point
        x: int

        def __eq__(self, other: object) -> bool:
            return True

    class Child(EqStruct):
        y: int

    with pytest.raises(TypeError, match="unhashable"):
        hash(Child(1, 2))


def test_an_unhashable_field_value_makes_the_struct_unhashable():
    class Holding(Struct):
        items: list

    with pytest.raises(TypeError, match="unhashable"):
        hash(Holding([1]))


def test_cached_property_works_when_a_co_base_carries_a_dict():
    class Dicted:
        pass

    class WithDict(Struct, Dicted, frozen=False):
        x: int

        @functools.cached_property
        def double(self) -> int:
            return self.x * 2

    instance = WithDict(3)

    assert instance.double == 6
    assert instance.__dict__ == {"double": 6}


def test_a_defined_init_displaces_post_init():
    class WithInit(Struct):
        x: int
        double: int = 0

        def __init__(self, x: int) -> None:
            set_field(self, "x", x)

        def __post_init__(self) -> None:
            set_field(self, "double", self.x * 2)

    assert WithInit(4).double == 0


def test_an_inherited_init_displaces_post_init():
    class InitBase:
        def __init__(self, x: int) -> None:
            set_field(self, "x", x)

    class Inherits(Struct, InitBase):
        x: int
        double: int = 0

        def __post_init__(self) -> None:
            set_field(self, "double", self.x * 2)

    assert Inherits(4).double == 0
