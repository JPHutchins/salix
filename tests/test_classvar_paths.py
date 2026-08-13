"""The README's ClassVar and caching tables, run rather than read."""

import functools
import sys
import typing

import pytest

from salix import Struct, set_field


def build(namespace):
    """The class, or the TypeError that refused it."""

    try:
        return type("Probe", (Struct,), namespace), None
    except TypeError as exc:
        return None, exc


def test_an_alias_is_refused_on_the_object_path():
    cls, error = build({"__annotations__": {"x": typing.ClassVar[int]}})

    assert cls is None
    assert error is not None


def test_an_alias_is_accepted_on_the_text_path():
    cls, error = build({"__annotations__": {"x": "CV[int]"}})

    assert error is None
    assert cls._struct_fields_ == ("x",)


def test_a_type_named_class_var_is_accepted_on_the_object_path():
    class ClassVar:
        pass

    cls, error = build({"__annotations__": {"x": ClassVar}})

    assert error is None
    assert cls._struct_fields_ == ("x",)


def test_a_type_named_class_var_is_refused_on_the_text_path():
    cls, error = build({"__annotations__": {"x": "ClassVar"}})

    assert cls is None
    assert error is not None


def test_annotated_metadata_is_accepted_on_the_object_path():
    cls, error = build({"__annotations__": {"x": typing.Annotated[int, typing.ClassVar]}})

    assert error is None
    assert cls._struct_fields_ == ("x",)


def test_annotated_metadata_is_refused_on_the_text_path():
    cls, error = build({"__annotations__": {"x": "Annotated[int, ClassVar]"}})

    assert cls is None
    assert error is not None


def test_optional_annotated_is_refused_on_both_paths():
    cls, error = build(
        {
            "__annotations__": {
                "x": typing.Optional[typing.Annotated[typing.ClassVar[int], "m"]]  # noqa: UP045
            }
        }
    )

    assert cls is None
    assert error is not None

    cls, error = build({"__annotations__": {"x": "Optional[Annotated[ClassVar[int], m]]"}})

    assert cls is None
    assert error is not None


@pytest.mark.skipif(sys.version_info < (3, 14), reason="3.14 evaluates resolvable annotations")
def test_an_unresolvable_annotation_arrives_as_an_object_and_is_accepted():
    class WithNope(Struct):
        x: Nope  # noqa: F821

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
