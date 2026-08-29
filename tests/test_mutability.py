import sys

import pytest
from values import EVERY, identify

from salix import Struct


class Frozen(Struct):
    x: int
    y: int = 2


class Mutable(Struct, frozen=False):
    x: int
    y: int = 2


def test_a_struct_is_frozen_unless_it_says_otherwise():
    with pytest.raises(AttributeError, match="does not support attribute assignment"):
        Frozen(1).x = 9

    with pytest.raises(AttributeError, match="does not support attribute deletion"):
        del Frozen(1).x


def test_frozen_true_is_accepted_and_is_the_default_anyway():
    class Explicit(Struct, frozen=True):
        x: int

    with pytest.raises(AttributeError, match="does not support attribute assignment"):
        Explicit(1).x = 9


def test_a_mutable_struct_accepts_a_write():
    instance = Mutable(1)
    instance.x = 9

    assert instance.x == 9


@pytest.mark.parametrize("value", EVERY, ids=identify)
def test_a_field_may_be_reassigned_to_any_value(value):
    instance = Mutable(1)
    instance.x = value

    assert instance.x is value


def test_a_mutable_struct_accepts_a_delete():
    instance = Mutable(1)
    del instance.x

    with pytest.raises(AttributeError):
        _ = instance.x


def test_a_deleted_field_reads_as_unset_in_repr():
    """The one way to reach that branch without writing the NULL from C."""

    instance = Mutable(1, 2)
    del instance.x

    assert repr(instance) == "Mutable(x=<unset>, y=2)"


def test_a_mutable_struct_still_rejects_a_name_that_is_not_a_field():
    with pytest.raises(AttributeError):
        Mutable(1).z = 9


def test_writes_are_visible_to_equality_and_repr():
    instance = Mutable(1)
    instance.x = 5

    assert instance == Mutable(5)
    assert repr(instance) == "Mutable(x=5, y=2)"


def test_a_mutable_struct_is_unhashable():
    assert Mutable.__hash__ is None

    with pytest.raises(TypeError, match="unhashable"):
        hash(Mutable(1))


def test_the_dunder_agrees_with_the_slot():
    """Assigning tp_setattro directly would leave these two disagreeing."""

    assert Mutable.__setattr__ is object.__setattr__
    assert Mutable.__delattr__ is object.__delattr__
    assert Frozen.__setattr__ is not object.__setattr__


def test_a_frozen_struct_is_still_hashable_and_structural():
    assert hash(Frozen(1)) == hash((1, 2))
    assert Frozen(1) == Frozen(1, 2)


def test_mutability_is_inherited():
    class Child(Mutable):
        z: int = 3

    instance = Child(1, 2, 3)
    instance.z = 9

    assert instance.z == 9
    assert Child.__hash__ is None


def test_frozenness_is_inherited():
    class Child(Frozen):
        z: int = 3

    with pytest.raises(AttributeError, match="does not support attribute assignment"):
        Child(1, 2, 3).z = 9


def test_a_mutable_struct_may_not_inherit_from_a_frozen_one():
    with pytest.raises(TypeError, match="mutable struct cannot inherit from a frozen one"):

        class Child(Frozen, frozen=False):
            pass


def test_a_frozen_struct_may_strengthen_a_mutable_one():
    """A fieldless frozen base imposes nothing, and frozen=True is a
    strengthening the caller asks for — the promise flows in from the caller
    rather than the widest base."""

    class Child(Mutable, frozen=True):
        pass

    instance = Child(1)

    with pytest.raises(AttributeError, match="does not support attribute assignment"):
        instance.x = 9

    assert instance == Child(1)
    assert hash(instance) == hash(Child(1))


def test_frozen_true_over_a_mutable_base_holds_beside_a_permissive_co_base():
    """The block is enforced by the mixin's tp_setattro, which the MRO reaches
    before any co-base, in either order, for writes and deletes."""

    class Permissive:
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    class Ahead(Mutable, Permissive, frozen=True):
        pass

    class Behind(Permissive, Mutable, frozen=True):
        pass

    for Child in (Ahead, Behind):
        with pytest.raises(AttributeError, match="does not support attribute"):
            Child(1).x = 9

        with pytest.raises(AttributeError, match="does not support attribute"):
            del Child(1).x


def test_a_frozen_child_of_a_frozen_base_holds_beside_a_permissive_co_base():
    """No option transition fires the rebind, so a co-base whose own
    __setattr__ slot would answer is met pre-creation: the namespace is born
    with the block's wrappers, and CPython's slot inheritance lands on them
    in either order."""

    class Permissive:
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    class FrozenBase(Struct, frozen=True):
        x: int

    class Ahead(Permissive, FrozenBase):
        pass

    class Behind(FrozenBase, Permissive):
        pass

    for Child in (Ahead, Behind):
        assert "__setattr__" in Child.__dict__

        with pytest.raises(AttributeError, match="does not support attribute"):
            Child(1).x = 9

        with pytest.raises(AttributeError, match="does not support attribute"):
            del Child(1).x


def test_a_frozen_setattr_escape_beside_a_permissive_co_base_keeps_answering():
    """The body's own half is the escape; the pre-creation rebind skips it
    and injects the other half, which stays refused, in either order. An
    escape written by a base is shadowed by the child's injected half, the
    same way the post-hoc repair shadowed it before."""

    class Permissive:
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

        def __delattr__(self, name: str) -> None:
            object.__delattr__(self, name)

    class FrozenBase(Struct, frozen=True):
        x: int

    class EscapedAhead(Permissive, FrozenBase):
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    class EscapedBehind(FrozenBase, Permissive):
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    for Child in (EscapedAhead, EscapedBehind):
        if sys.version_info >= (3, 13):
            instance = Child(1)
            instance.x = 9

            assert instance.x == 9

            with pytest.raises(AttributeError, match="does not support attribute"):
                del instance.x
        else:
            # The parent build refuses the escape the same way on 3.10-3.12:
            # the mixed wrapper/function slot derivation lands on a slot the
            # body's own half cannot answer. Parity, not a promise.
            with pytest.raises(TypeError):
                Child(1).x = 9

            with pytest.raises(AttributeError):
                del Child(1).x

    class Escaping(FrozenBase):
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    class InheritedAhead(Permissive, Escaping):
        pass

    with pytest.raises(AttributeError, match="does not support attribute"):
        InheritedAhead(1).x = 9


def test_a_frozen_delattr_escape_beside_a_permissive_co_base_keeps_answering():
    class Permissive:
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

        def __delattr__(self, name: str) -> None:
            object.__delattr__(self, name)

    class FrozenBase(Struct, frozen=True):
        x: int

    class EscapedAhead(Permissive, FrozenBase):
        def __delattr__(self, name: str) -> None:
            object.__delattr__(self, name)

    class EscapedBehind(FrozenBase, Permissive):
        def __delattr__(self, name: str) -> None:
            object.__delattr__(self, name)

    for Child in (EscapedAhead, EscapedBehind):
        if sys.version_info >= (3, 13):
            del Child(1).x

            with pytest.raises(AttributeError, match="does not support attribute"):
                Child(1).x = 9
        else:
            with pytest.raises(TypeError):
                del Child(1).x

            with pytest.raises(AttributeError):
                Child(1).x = 9


def test_a_fieldless_frozen_base_promises_nothing_to_a_mutable_class():
    """A fieldless frozen base made no promise, so frozen=False over one is
    free, in either order."""

    class FrozenFieldless(Struct):
        pass

    class Ahead(FrozenFieldless, Mutable, frozen=False):
        pass

    class Behind(Mutable, FrozenFieldless, frozen=False):
        pass

    for Child in (Ahead, Behind):
        instance = Child(1)
        instance.x = 9

        assert instance.x == 9


def test_a_mutable_struct_lets_a_co_base_hook_observe_writes():
    observed = []

    class Observing:
        def __setattr__(self, name: str, value: object) -> None:
            observed.append((name, value))
            object.__setattr__(self, name, value)

    class Child(Observing, Mutable):
        pass

    child = Child(1)
    child.x = 9

    assert ("x", 9) in observed
    assert child.x == 9


def test_a_mutable_struct_lets_its_own_body_delattr_observe_deletion():
    deleted = []

    class Child(Mutable):
        def __delattr__(self, name: str) -> None:
            deleted.append(name)
            object.__delattr__(self, name)

    child = Child(1)
    del child.x

    assert "x" in deleted


def test_a_frozen_structs_body_delattr_keeps_answering():
    """The escape hatch works for either half: a body __delattr__ is skipped
    by the rebind per name, so deletes reach the hook while writes stay
    refused. What the hook does with the deletion is its own business."""

    deleted = []

    class Single(Struct):
        x: int

        def __delattr__(self, name: str) -> None:
            deleted.append(name)

    class Strengthened(Mutable, frozen=True):
        def __delattr__(self, name: str) -> None:
            deleted.append(name)

    for Child in (Single, Strengthened):
        if sys.version_info >= (3, 13) or Child is Single:
            with pytest.raises(AttributeError):
                Child(1).x = 9
        else:
            with pytest.raises(TypeError):
                Child(1).x = 9

        del Child(1).x

    assert deleted == ["x", "x"]


def test_a_frozen_class_with_an_escape_hatch_is_unhashable():
    """The body __setattr__ hatch admits writes, so the hash pairs with the
    mutability it admits rather than the frozen record."""

    class Child(Mutable, frozen=True):
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    child = Child(1)
    child.x = 9

    assert child.x == 9

    with pytest.raises(TypeError, match="unhashable"):
        hash(child)


def test_a_fielded_frozen_base_refuses_a_mutable_child_in_either_order():
    class FrozenOther(Struct):
        other: object

    for order in ((Frozen, FrozenOther), (FrozenOther, Frozen)):
        with pytest.raises(TypeError, match="mutable struct cannot inherit from a frozen one"):
            type(Struct)("Child", order, {}, frozen=False)


def test_a_base_with_no_fields_imposes_nothing():
    """Which is what lets a first subclass of Struct ask to be mutable at all."""

    class Fieldless(Struct):
        pass

    class Child(Fieldless, frozen=False):
        x: int

    instance = Child(1)
    instance.x = 9

    assert instance.x == 9


def test_a_fieldless_base_still_carries_its_mutability():
    """It imposes nothing, but a child that says nothing inherits it."""

    class Fieldless(Struct, frozen=False):
        pass

    class Child(Fieldless):
        x: int

    instance = Child(1)
    instance.x = 9

    assert instance.x == 9
    assert Child.__hash__ is None


def test_a_child_of_a_fieldless_mutable_base_may_freeze_itself():
    class Fieldless(Struct, frozen=False):
        pass

    class Child(Fieldless, frozen=True):
        x: int

    with pytest.raises(AttributeError, match="does not support attribute assignment"):
        Child(1).x = 9

    assert hash(Child(1)) == hash((1,))


def test_the_orig_class_write_is_accepted_and_discarded():
    class Ok(Struct):
        value: int

    ok = Ok(1)

    ok.__orig_class__ = "typing bookkeeping"

    assert not hasattr(ok, "__orig_class__")
