import weakref

import pytest

from salix import Struct


class Empty(Struct):
    pass


class OtherEmpty(Struct):
    pass


class Weaky(Struct, weakref=True):
    pass


class Mutable(Struct, frozen=False):
    pass


def test_a_zero_field_frozen_struct_is_its_own_singleton():
    assert Empty() is Empty()
    assert isinstance(Empty(), Empty)
    assert repr(Empty()) == "Empty()"


def test_the_singleton_hashes_like_the_empty_tuple():
    assert hash(Empty()) == hash(())


def test_each_subclass_gets_its_own_singleton():
    assert Empty() is not OtherEmpty()


def test_weakref_true_stays_allocated():
    held = Weaky()

    assert Weaky() is not held
    assert weakref.ref(held)() is held


def test_a_mutable_zero_field_struct_stays_allocated():
    assert Mutable() is not Mutable()


def test_arguments_are_still_refused():
    with pytest.raises(TypeError, match="takes at most 0 positional"):
        Empty(1)


def test_post_init_runs_once_for_the_singleton():
    calls = []

    class Counted(Struct):
        def __post_init__(self) -> None:
            calls.append(self)

    Counted()
    Counted()

    assert len(calls) == 1
    assert Counted() is calls[0]


def test_copy_and_deepcopy_preserve_the_singleton():
    import copy

    assert copy.copy(Empty()) is Empty()
    assert copy.deepcopy(Empty()) is Empty()


def test_a_class_with_its_own_init_is_not_interned():
    class WithInit(Struct):
        def __init__(self) -> None:
            pass

    assert WithInit() is not WithInit()


def test_a_failing_post_init_fails_the_class_statement():
    with pytest.raises(RuntimeError, match="boom"):
        class Exploding(Struct):
            def __post_init__(self) -> None:
                raise RuntimeError("boom")


def test_the_singleton_is_built_before_the_class_name_binds():
    with pytest.raises(NameError):
        class Named(Struct):
            def __post_init__(self) -> None:
                return Named
