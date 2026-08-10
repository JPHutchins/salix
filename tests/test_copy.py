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


def test_a_co_base_copy_is_deferred_to():
    class HasCopy:
        def __copy__(self) -> "HasCopy":
            copied = HasCopy()
            copied.was_copied = True

            return copied

    class RealStruct(Struct, HasCopy, frozen=False):
        x: int

    copied = copy.copy(RealStruct(1))

    assert type(copied) is HasCopy
    assert copied.was_copied is True


def test_a_classmethod_co_base_copy_fails_like_copy_dot_py():
    class CM:
        @classmethod
        def __copy__(cls) -> str:
            return "cm-copy"

    class RealStruct(Struct, CM, frozen=False):
        x: int

    # copy.py binds the classmethod, then calls the bound method with the
    # instance — two arguments to a one-argument classmethod.
    with pytest.raises(TypeError, match="positional"):
        copy.copy(RealStruct(1))


def test_a_two_argument_classmethod_co_base_copy_receives_the_leaf_class():
    class CM:
        @classmethod
        def __copy__(cls, x: int) -> str:  # noqa: PLE0302 -- the arity is the case under test
            return f"got class {cls.__name__}"

    class RealStruct(Struct, CM, frozen=False):
        x: int

    class PlainCM(CM):
        pass

    # copy.py binds the classmethod to the concrete class; the struct must
    # receive its own class, not the co-base that defines the method.
    assert copy.copy(PlainCM()) == "got class PlainCM"
    assert copy.copy(RealStruct(7)) == "got class RealStruct"


def test_a_property_co_base_copy_fails_like_copy_dot_py():
    class Prop:
        @property
        def __copy__(self) -> str:
            return "prop-copy"

    class RealStruct(Struct, Prop, frozen=False):
        x: int

    with pytest.raises(TypeError, match="property"):
        copy.copy(RealStruct(1))


def test_a_member_descriptor_co_base_copy_fails_like_copy_dot_py():
    class Slotted:
        __slots__ = ("__copy__",)

    class RealStruct(Struct, Slotted, frozen=False):
        x: int

    with pytest.raises(TypeError, match="member_descriptor"):
        copy.copy(RealStruct(1))


def test_a_dispatch_table_copier_is_honored_for_a_real_struct():
    def special_copy(instance):
        return (type(instance), (999, "seven"), None)

    copy.dispatch_table[Point] = special_copy

    try:
        copied = copy.copy(Point(1, "two"))

        assert copied == Point(999, "seven")
    finally:
        del copy.dispatch_table[Point]


def test_an_impostor_delegates_to_a_co_base_copy():
    class HasCopy:
        def __copy__(self) -> "HasCopy":
            copied = HasCopy()
            copied.was_copied = True

            return copied

    class ImpostorWithCopy(Struct.__mro__[1], list, HasCopy):
        pass

    copied = copy.copy(ImpostorWithCopy())

    assert type(copied) is HasCopy
    assert copied.was_copied is True


def test_a_dispatch_table_copier_is_honored_for_an_impostor():
    def special_copy(instance):
        return (list, ([1, 2, 3],))

    copy.dispatch_table[Impostor] = special_copy

    try:
        copied = copy.copy(Impostor())

        # The copier's reduce tuple won: without the dispatch_table consult
        # the delegate would have fallen back to __reduce_ex__ and produced
        # an Impostor.
        assert type(copied) is list
        assert copied == [1, 2, 3]
    finally:
        del copy.dispatch_table[Impostor]


def test_a_non_struct_base_slot_is_carried_into_the_copy():
    class WithSlots:
        __slots__ = ("a",)

    class S(Struct, WithSlots, frozen=False):
        x: int

    instance = S(1)
    instance.a = "base-slot-value"
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.x == 1
    assert copied.a == "base-slot-value"

    bare = copy.copy(S(2))

    with pytest.raises(AttributeError):
        _ = bare.a


def test_a_self_reference_in_a_field_still_points_at_the_source():
    class Loop(Struct, frozen=False):
        other: object = None

    instance = Loop()
    instance.other = instance
    copied = copy.copy(instance)

    assert copied is not instance
    assert copied.other is instance
