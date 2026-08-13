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

    def __deepcopy__(self, memo: dict) -> "Shadowing":
        return Shadowing(self.x + 1000)


class Dicted:
    pass


class WithDict(Struct, Dicted, frozen=False):
    x: int


class Impostor(Struct.__mro__[1], list):
    pass


def test_deepcopy_returns_a_distinct_equal_instance():
    point = Point(1, "two")
    copied = copy.deepcopy(point)

    assert copied is not point
    assert copied == point
    assert type(copied) is Point


def test_deepcopy_is_deep():
    holder = ["shared"]
    instance = Mutable(holder)
    copied = copy.deepcopy(instance)

    assert copied is not instance
    assert copied.x is not holder
    assert copied.x == holder


def test_an_unset_field_stays_unset_in_the_deep_copy():
    instance = Mutable(1)
    del instance.x
    copied = copy.deepcopy(instance)

    with pytest.raises(AttributeError):
        _ = copied.x
    assert "x=<unset>" in repr(copied)


def test_a_constructor_hook_does_not_run_on_the_deep_copy(hooked):
    hooked_class, calls = hooked
    source = hooked_class(1)
    copied = copy.deepcopy(source)

    assert copied == source
    assert calls == [1]


def test_a_body_deepcopy_shadows_the_mixin_one():
    copied = copy.deepcopy(Shadowing(1))

    assert copied == Shadowing(1001)


def test_a_mixin_subclass_that_is_not_a_struct_deepcopies_like_its_other_base():
    instance = Impostor([1, 2])
    instance.extra = ["world"]
    copied = copy.deepcopy(instance)

    assert type(copied) is Impostor
    assert copied == [1, 2]
    assert copied.extra == ["world"]
    assert copied.extra is not instance.extra


def test_the_instance_dict_is_copied_deeply():
    instance = WithDict(1)
    instance.extra = ["world"]
    copied = copy.deepcopy(instance)

    assert copied is not instance
    assert copied.x == 1
    assert copied.extra == ["world"]
    assert copied.__dict__ is not instance.__dict__
    assert copied.__dict__["extra"] is not instance.__dict__["extra"]


def test_a_field_named_like_a_mixin_deepcopy_method_is_refused_at_class_creation():
    with pytest.raises(TypeError, match="__deepcopy__"):
        class Colliding(Struct):
            __deepcopy__: int


def test_a_subclass_deepcopies_every_inherited_field():
    class Point3D(Point):
        z: float = 3.0

    copied = copy.deepcopy(Point3D(1, "two"))

    assert copied == Point3D(1, "two")
    assert copied.z == 3.0


def test_a_weakref_slot_is_not_carried_into_the_deep_copy():
    class Weak(Struct, weakref=True):
        x: int

    instance = Weak(1)
    source_ref = weakref.ref(instance)
    copied = copy.deepcopy(instance)
    copied_ref = weakref.ref(copied)

    assert copied == Weak(1)
    assert source_ref() is instance
    assert copied_ref() is copied


def test_a_nested_struct_is_deep_copied():
    class Inner(Struct):
        xs: list

    class Outer(Struct):
        inner: Inner

    inner = Inner([1, 2])
    outer = Outer(inner)
    copied = copy.deepcopy(outer)

    assert copied is not outer
    assert copied.inner is not inner
    assert copied.inner.xs is not inner.xs
    assert copied.inner.xs == [1, 2]


def test_a_value_shared_by_two_fields_is_copied_once():
    class Duo(Struct, frozen=False):
        first: object
        second: object

    holder = ["shared"]
    instance = Duo(holder, holder)
    copied = copy.deepcopy(instance)

    assert copied.first is copied.second
    assert copied.first is not holder


def test_a_co_base_deepcopy_is_deferred_to():
    class HasDeepcopy:
        def __deepcopy__(self, memo: dict) -> "HasDeepcopy":
            copied = HasDeepcopy()
            copied.was_copied = True

            return copied

    class RealStruct(Struct, HasDeepcopy, frozen=False):
        x: int

    copied = copy.deepcopy(RealStruct(1))

    assert type(copied) is HasDeepcopy
    assert copied.was_copied is True


def test_a_one_argument_classmethod_co_base_deepcopy_fails_like_copy_dot_py():
    class CM:
        @classmethod
        def __deepcopy__(cls) -> str:  # noqa: PLE0302 -- the arity is the case under test
            return "cm-deepcopy"

    class RealStruct(Struct, CM, frozen=False):
        x: int

    # copy.py binds the classmethod, then calls the bound method with the
    # memo -- two arguments to a one-argument classmethod.
    with pytest.raises(TypeError, match="positional"):
        copy.deepcopy(RealStruct(1))


def test_a_two_argument_classmethod_co_base_deepcopy_receives_the_leaf_class_and_memo():
    class CM:
        @classmethod
        def __deepcopy__(cls, memo: dict) -> str:
            return f"got class {cls.__name__} and memo {id(memo)}"

    class RealStruct(Struct, CM, frozen=False):
        x: int

    memo = {}
    result = copy.deepcopy(RealStruct(7), memo)

    assert result == f"got class RealStruct and memo {id(memo)}"


def test_a_property_co_base_deepcopy_fails_like_copy_dot_py():
    class Prop:
        @property
        def __deepcopy__(self) -> str:  # noqa: PLE0302 -- the arity is the case under test
            return "prop-deepcopy"

    class RealStruct(Struct, Prop, frozen=False):
        x: int

    # copy.py resolves the property on the instance, then calls the resolved
    # value with the memo.
    with pytest.raises(TypeError, match=r"is not callable"):
        copy.deepcopy(RealStruct(1))


def test_a_member_descriptor_co_base_deepcopy_falls_through_like_copy_dot_py():
    class Slotted:
        __slots__ = ("__deepcopy__",)

    class RealStruct(Struct, Slotted, frozen=False):
        x: int

    # copy.py's instance lookup reads the unset slot as AttributeError, so
    # the method counts as absent and the struct is deep-copied raw.
    copied = copy.deepcopy(RealStruct(1))

    assert type(copied) is RealStruct
    assert copied.x == 1


def test_a_none_co_base_deepcopy_is_treated_as_absent():
    class NoneDeepcopy:
        __deepcopy__ = None

    class RealStruct(Struct, NoneDeepcopy, frozen=False):
        x: int

    copied = copy.deepcopy(RealStruct(1))

    assert type(copied) is RealStruct
    assert copied.x == 1


def test_a_dispatch_table_copier_is_honored_for_a_real_struct():
    def special_deepcopy(instance):
        return (type(instance), (999, "seven"), None)

    copy.dispatch_table[Point] = special_deepcopy

    try:
        copied = copy.deepcopy(Point(1, "two"))

        assert copied == Point(999, "seven")
    finally:
        del copy.dispatch_table[Point]


def test_a_dispatch_table_copier_is_honored_for_an_impostor():
    def special_deepcopy(instance):
        return (list, ([1, 2, 3],))

    copy.dispatch_table[Impostor] = special_deepcopy

    try:
        copied = copy.deepcopy(Impostor())

        assert type(copied) is list
        assert copied == [1, 2, 3]
    finally:
        del copy.dispatch_table[Impostor]


def test_an_impostor_delegates_to_a_co_base_deepcopy():
    class HasDeepcopy:
        def __deepcopy__(self, memo: dict) -> "HasDeepcopy":
            copied = HasDeepcopy()
            copied.was_copied = True

            return copied

    class ImpostorWithDeepcopy(Struct.__mro__[1], list, HasDeepcopy):
        pass

    copied = copy.deepcopy(ImpostorWithDeepcopy())

    assert type(copied) is HasDeepcopy
    assert copied.was_copied is True


def test_an_impostor_with_reduce_ex_none_falls_back_to_reduce():
    class Special(Struct.__mro__[1], list):
        __reduce_ex__ = None

        def __reduce__(self):
            return (list, ([1, 2, 3],))

    copied = copy.deepcopy(Special())

    assert type(copied) is list
    assert copied == [1, 2, 3]


def test_an_uncopyable_impostor_raises_copy_dot_error():
    class Uncopyable(Struct.__mro__[1], list):
        __reduce_ex__ = None
        __reduce__ = None

    # copy.py's message is `% cls`, which renders the class repr.
    with pytest.raises(
        copy.Error, match=r"un\(deep\)copyable object of type <class '.*Uncopyable'>"
    ):
        copy.deepcopy(Uncopyable())


def test_a_falsy_reduce_ex_is_called_like_copy_dot_py():
    class FalsyReduceEx(Struct.__mro__[1], list):
        __reduce_ex__ = 0

        def __reduce__(self):
            return (list, ([7, 8],))

    # copy.py gates __reduce_ex__ on `is not None`, so a falsy non-None is
    # called (and fails); the truthiness gate is only on __reduce__.
    with pytest.raises(TypeError):
        copy.deepcopy(FalsyReduceEx())


def test_a_non_struct_base_slot_is_deep_copied():
    class WithSlots:
        __slots__ = ("a",)

    class S(Struct, WithSlots, frozen=False):
        x: int

    instance = S(1)
    instance.a = ["base-slot-value"]
    copied = copy.deepcopy(instance)

    assert copied is not instance
    assert copied.x == 1
    assert copied.a == ["base-slot-value"]
    assert copied.a is not instance.a

    bare = copy.deepcopy(S(2))

    with pytest.raises(AttributeError):
        _ = bare.a


def test_a_self_reference_in_a_field_resolves_to_the_copy():
    class Loop(Struct, frozen=False):
        other: object = None

    instance = Loop()
    instance.other = instance
    copied = copy.deepcopy(instance)

    assert copied is not instance
    assert copied.other is copied


def test_a_self_reference_in_the_instance_dict_resolves_to_the_copy():
    class Loop(Struct, Dicted, frozen=False):
        other: object = None

    instance = Loop()
    instance.extra = instance
    copied = copy.deepcopy(instance)

    assert copied is not instance
    assert copied.extra is copied


def test_a_cycle_between_two_structs_resolves_to_the_copies():
    class Node(Struct, frozen=False):
        peer: object = None

    first = Node()
    second = Node()
    first.peer = second
    second.peer = first
    copied_first = copy.deepcopy(first)

    assert copied_first.peer is not second
    assert copied_first.peer.peer is copied_first


def test_a_user_memo_can_presupply_the_copy():
    instance = Point(1, "two")
    stand_in = Point(9, "nine")
    memo = {id(instance): stand_in}
    copied = copy.deepcopy(instance, memo)

    assert copied is stand_in

    # The dunder honors a presupplied entry the same way copy.deepcopy's
    # entry check does, so a direct protocol call matches its aliasing.
    assert instance.__deepcopy__(memo) is stand_in


def test_a_falsy_dispatch_table_entry_is_skipped_on_deepcopy():
    copy.dispatch_table[Point] = 0

    try:
        copied = copy.deepcopy(Point(1, "two"))

        # deepcopy gates the dispatch_table branch on truthiness (copy.py's
        # `if reductor:`), where copy gates it on identity, so the falsy
        # entry is skipped and the raw path runs instead of calling 0.
        assert copied == Point(1, "two")
    finally:
        del copy.dispatch_table[Point]


def test_a_non_dict_memo_is_refused():
    with pytest.raises(TypeError, match="must be a dict"):
        Point(1, "two").__deepcopy__(None)


def test_a_non_dict_memo_is_refused_for_an_impostor_too():
    with pytest.raises(TypeError, match="must be a dict"):
        Impostor([1]).__deepcopy__(None)


def test_a_rebound_mixin_deepcopy_does_not_reenter_forever():
    class Pin(Struct):
        x: int
        __deepcopy__ = Struct.__deepcopy__

    # The body's rebind of the mixin's own method is found by getattr and
    # called; the deferral must not then re-find it in the class's dict.
    copied = copy.deepcopy(Pin(1))

    assert type(copied) is Pin
    assert copied == Pin(1)


def test_a_direct_protocol_call_registers_the_deferred_result():
    class HasDeepcopy:
        def __deepcopy__(self, memo: dict) -> "HasDeepcopy":
            copied = HasDeepcopy()
            copied.was_copied = True

            return copied

    class RealStruct(Struct, HasDeepcopy, frozen=False):
        x: int

    instance = RealStruct(1)
    memo = {}
    result = instance.__deepcopy__(memo)

    assert memo[id(instance)] is result


def test_a_failed_deepcopy_leaves_no_partial_shell_in_the_memo():
    class Uncopyable:
        def __deepcopy__(self, memo: dict):
            raise ValueError("no")

    class Holder(Struct):
        first: object
        second: object = None

    instance = Holder(Uncopyable())
    memo = {}

    with pytest.raises(ValueError, match="no"):
        copy.deepcopy(instance, memo)

    assert id(instance) not in memo
