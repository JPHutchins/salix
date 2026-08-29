import gc
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


def test_a_dispatch_table_entry_still_precedes_the_singleton():
    import copy

    class Other(Struct):
        pass

    class Registered(Struct):
        pass

    copy.dispatch_table[Registered] = lambda _: (Other, ())

    try:
        assert type(copy.copy(Registered())) is Other
    finally:
        del copy.dispatch_table[Registered]


def test_a_co_base_copy_method_still_precedes_the_singleton():
    import copy

    class WithCopy:
        def __copy__(self) -> object:
            return "co-base copy"

    class CoBased(Struct, WithCopy):
        pass

    assert copy.copy(CoBased()) == "co-base copy"


def test_post_init_observes_the_settle_bindings():
    observed = []

    class Bound(Struct):
        def __hash__(self) -> int:
            return 12345

        def __post_init__(self) -> None:
            observed.append(hash(self))

    assert hash(Bound()) == 12345
    assert observed == [12345]


def test_a_class_with_its_own_new_is_not_interned():
    class WithNew(Struct):
        def __new__(cls, *args, **kwargs):
            return super().__new__(cls)

    assert WithNew() is not WithNew()


def test_a_co_base_carrying_a_slot_is_not_interned():
    class Slotted:
        __slots__ = ("z",)

    class WithSlot(Struct, Slotted):
        pass

    assert WithSlot() is not WithSlot()


def test_a_diverting_setattro_co_base_is_not_interned():
    class Diverting:
        def __setattr__(self, name: str, value: object) -> None:
            object.__setattr__(self, name, value)

    class WithDivert(Struct, Diverting):
        pass

    assert WithDivert() is not WithDivert()


def test_built_and_dropped_interned_classes_free_completely_under_stress():
    """The lifetime stress shape: build a qualifying class, touch its
    singleton, drop every reference, collect -- and the pair frees together.
    A dangling clear (the class decrefing an already-freed singleton) crashes
    here, and a leak keeps a weakref alive. 3.10/3.11 are where the claimed
    ordering lives; the harness runs everywhere CI does.

    The positive control is the interned-path assertion itself: every
    iteration proves the singleton was built and interned, so the classes
    dropped are exactly the pair the claim is about.
    """

    surviving = 0

    for i in range(200):
        built = type(Struct)(f"Dropped{i}", (Struct,), {})
        singleton = built()
        assert singleton is built()
        class_ref = weakref.ref(built)
        del built, singleton
        gc.collect(0)
        surviving += class_ref() is not None

    assert surviving == 0
