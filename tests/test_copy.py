import copy
import weakref

import pytest

from salix import Struct


class Point(Struct):
    x: int
    y: str = "seven"


class Mutable(Struct, frozen=False):
    x: int


@pytest.fixture(params=["post_init", "init"])
def hooked(request):
    calls: list[int] = []

    if request.param == "post_init":

        class Hooked(Struct, frozen=False):
            x: int

            def __post_init__(self) -> None:
                calls.append(self.x)

    else:

        class Hooked(Struct, frozen=False):
            x: int

            def __init__(self, x: int) -> None:
                calls.append(x)
                self.x = x

    return Hooked, calls


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


def test_a_constructor_hook_does_not_run_on_the_copy(hooked):
    hooked_class, calls = hooked
    source = hooked_class(1)
    copied = copy.copy(source)

    assert copied == source
    assert calls == [1]


def test_a_body_copy_shadows_the_mixin_one():
    copied = copy.copy(Shadowing(1))

    assert copied == Shadowing(1001)


def test_a_mixin_subclass_that_is_not_a_struct_copies_like_its_other_base():
    instance = Impostor([1, 2])
    instance.extra = "world"
    copied = copy.copy(instance)

    assert type(copied) is Impostor
    assert copied == [1, 2]
    assert copied.extra == "world"


def test_the_instance_dict_is_copied_shallowly():
    instance = WithDict(1)
    instance.extra = "world"
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.x == 1
    assert copied.extra == "world"
    assert copied.__dict__ is not instance.__dict__
    assert copied.__dict__["extra"] is instance.__dict__["extra"]


def test_a_field_named_like_a_mixin_copy_method_is_refused_at_class_creation():
    with pytest.raises(TypeError, match="__copy__"):
        class Colliding(Struct):
            __copy__: int

    with pytest.raises(TypeError, match="__deepcopy__"):
        class Colliding(Struct):
            __deepcopy__: int


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
    source_ref = weakref.ref(instance)
    copied = copy.copy(instance)
    copied_ref = weakref.ref(copied)

    assert copied == Weak(1)
    assert source_ref() is instance
    assert copied_ref() is copied


def test_a_self_reference_in_a_field_still_points_at_the_source():
    class Loop(Struct, frozen=False):
        other: object = None

    instance = Loop()
    instance.other = instance
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.other is instance
