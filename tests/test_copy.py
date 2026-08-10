import copy
import weakref

import pytest

from salix import Struct


class Point(Struct):
    x: int
    y: str = "seven"


class Mutable(Struct, frozen=False):
    x: int


post_init_calls: list[int] = []


class WithPostInit(Struct, frozen=False):
    x: int

    def __post_init__(self) -> None:
        post_init_calls.append(self.x)


init_calls: list[int] = []


class WithInit(Struct, frozen=False):
    x: int

    def __init__(self, x: int) -> None:
        init_calls.append(x)
        self.x = x


class Shadowing(Struct):
    x: int

    def __copy__(self) -> "Shadowing":
        return Shadowing(self.x + 1000)


class Dicted:
    pass


class WithDict(Struct, Dicted, frozen=False):
    x: int


class Impostor(Struct.__mro__[1], list):
    pass


def test_copy_returns_a_distinct_equal_instance():
    point = Point(1, "two")
    copied = copy.copy(point)

    assert copied is not point
    assert copied == point
    assert type(copied) is Point


def test_copy_is_shallow():
    holder = ["shared"]
    instance = Mutable(holder)
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.x is holder


def test_an_unset_field_stays_unset_in_the_copy():
    instance = Mutable(1)
    del instance.x
    copied = copy.copy(instance)

    with pytest.raises(AttributeError):
        _ = copied.x
    assert "x=<unset>" in repr(copied)


def test_post_init_does_not_run_on_the_copy():
    post_init_calls.clear()
    source = WithPostInit(1)
    copied = copy.copy(source)

    assert copied == source
    assert post_init_calls == [1]


def test_a_custom_init_does_not_run_on_the_copy():
    init_calls.clear()
    copied = copy.copy(WithInit(1))

    assert copied.x == 1
    assert init_calls == [1]


def test_a_body_copy_shadows_the_mixin_one():
    copied = copy.copy(Shadowing(1))

    assert copied == Shadowing(1001)


def test_copy_on_a_mixin_subclass_that_is_not_a_struct_is_refused():
    with pytest.raises(AttributeError, match="__copy__ is defined on structs"):
        copy.copy(Impostor())


def test_the_instance_dict_is_copied_shallowly():
    instance = WithDict(1)
    instance.extra = "world"
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.x == 1
    assert copied.extra == "world"
    assert copied.__dict__ is not instance.__dict__
    assert copied.__dict__["extra"] is instance.__dict__["extra"]


def test_a_frozen_struct_copies():
    copied = copy.copy(Point(1, "two"))

    assert copied == Point(1, "two")


def test_a_subclass_copies_every_inherited_field():
    class Point3D(Point):
        z: float = 3.0

    copied = copy.copy(Point3D(1, "two"))

    assert copied == Point3D(1, "two")
    assert copied.z == 3.0


def test_a_weakref_slot_is_not_carried_into_the_copy():
    class Weak(Struct, weakref=True):
        x: int

    instance = Weak(1)
    ref = weakref.ref(instance)
    copied = copy.copy(instance)

    assert copied == Weak(1)
    assert ref() is instance


def test_a_self_reference_in_a_field_still_points_at_the_source():
    class Loop(Struct, frozen=False):
        other: object = None

    instance = Loop()
    instance.other = instance
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.other is instance
