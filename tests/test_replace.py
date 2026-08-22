import copy
import sys

import pytest

from salix import Struct, replace


class Point(Struct):
    x: int
    y: str = "seven"


class Mutable(Struct, frozen=False):
    x: int
    y: str = "seven"


class WithInit(Struct, frozen=False):
    x: int

    def __init__(self, x: int) -> None:
        self.x = x * 2


class Dicted:
    pass


class WithDict(Struct, Dicted, frozen=False):
    x: int


class Empty(Struct):
    pass


def test_replace_changes_one_field_and_copies_the_rest():
    original = Point(1)
    replaced = original.__replace__(x=2)

    assert replaced is not original
    assert replaced == Point(2, "seven")
    assert original == Point(1, "seven")


def test_the_module_function_matches_the_dunder():
    original = Point(1)
    replaced = replace(original, x=2)

    assert replaced == Point(2, "seven")


def test_an_unknown_change_is_refused_like_the_constructor():
    with pytest.raises(TypeError, match="got an unexpected keyword argument 'z'"):
        Point(1).__replace__(z=2)


def test_a_zero_change_replace_returns_the_instance():
    original = Point(1)

    assert original.__replace__() is original


def test_a_second_positional_argument_is_refused():
    with pytest.raises(TypeError, match="takes exactly one positional argument"):
        Point(1).__replace__(1, x=2)


def test_post_init_runs_again_with_the_changed_value():
    calls = []

    class Counted(Struct):
        x: int

        def __post_init__(self) -> None:
            calls.append(self.x)

    original = Counted(1)
    replaced = replace(original, x=2)

    assert calls == [1, 2]
    assert replaced == Counted(2)


def test_a_body_init_is_the_construction_path():
    original = WithInit(3)

    assert original.x == 6
    assert replace(original, x=4).x == 8
    assert original.x == 6


def test_a_body_replace_override_is_honored_by_the_module_function():
    class Overriding(Struct):
        x: int

        def __replace__(self, /, **changes: object) -> object:
            return "body replace"

    assert replace(Overriding(1), x=2) == "body replace"


def test_a_mutable_struct_can_be_replaced():
    original = Mutable(1)
    replaced = replace(original, x=2)

    assert replaced is not original
    assert replaced == Mutable(2, "seven")
    assert original == Mutable(1, "seven")


def test_the_singleton_survives_a_zero_change_replace():
    assert Empty().__replace__() is Empty()

    with pytest.raises(TypeError, match="got an unexpected keyword argument"):
        Empty().__replace__(x=1)


def test_the_instance_dict_is_copied_shallowly():
    original = WithDict(1)
    original.extra = "world"
    replaced = replace(original, x=2)

    assert replaced is not original
    assert replaced.x == 2
    assert replaced.extra == "world"
    assert replaced.__dict__ is not original.__dict__


def test_a_non_struct_is_refused():
    with pytest.raises(TypeError, match="replace\\(\\) expects a struct, not int"):
        replace(5)


def test_an_impostor_is_not_replaceable():
    class Impostor(Struct.__mro__[1], list):
        pass

    with pytest.raises(TypeError, match="Impostor object is not replaceable"):
        Impostor().__replace__(x=1)


@pytest.mark.skipif(sys.version_info < (3, 13), reason="copy.replace is 3.13+")
def test_copy_replace_uses_the_dunder():
    replaced = copy.replace(Point(1), x=9)

    assert replaced == Point(9, "seven")


class WithDictInit(Struct, Dicted, frozen=False):
    x: int

    def __init__(self, x: int) -> None:
        self.x = x


def test_a_slow_path_replace_carries_the_instance_dict():
    original = WithDictInit(1)
    original.extra = "world"
    replaced = replace(original, x=2)

    assert replaced.x == 2
    assert replaced.extra == "world"
    assert replaced.__dict__ is not original.__dict__


def test_a_slow_path_replace_carries_co_base_members():
    class Slotted:
        __slots__ = ("z",)

    class WithSlot(Struct, Slotted, frozen=False):
        x: int

        def __init__(self, x: int) -> None:
            self.x = x

    original = WithSlot(1)
    original.z = 5
    replaced = replace(original, x=2)

    assert replaced.z == 5


def test_a_positional_only_init_propagates_its_own_type_error():
    class PositionalOnly(Struct, frozen=False):
        x: int

        def __init__(self, x: int, /) -> None:
            self.x = x

    with pytest.raises(TypeError):
        replace(PositionalOnly(1), x=2)


def test_a_body_new_is_discarded_by_replace_as_by_construction():
    calls = []

    class WithNew(Struct, frozen=False):
        x: int

        def __new__(cls, *args: object, **kwargs: object) -> object:
            calls.append(1)
            return super().__new__(cls)

    original = WithNew(1)
    replaced = replace(original, x=2)

    assert calls == []
    assert replaced == WithNew(2)


def test_an_instance_dict_entry_does_not_shadow_the_dunder():
    original = WithDict(1)
    original.__dict__["__replace__"] = "shadow"
    replaced = replace(original, x=2)

    assert replaced == WithDict(2)


def test_a_slow_path_replace_keeps_the_constructors_dict_entries():
    class DictInit(Struct, Dicted, frozen=False):
        x: int

        def __init__(self, x: int) -> None:
            self.x = x
            self.extra = "fresh"

    original = DictInit(1)
    original.stale = "stale"
    replaced = replace(original, x=2)

    assert replaced.extra == "fresh"
    assert replaced.stale == "stale"


def test_an_unknown_change_is_refused_before_a_swallowing_init_sees_it():
    class Swallowing(Struct, frozen=False):
        x: int

        def __init__(self, x: int, **rest: object) -> None:
            self.x = x

    with pytest.raises(TypeError, match="got an unexpected keyword argument 'z'"):
        replace(Swallowing(1), z=2)


def test_a_subset_signature_init_propagates_its_own_type_error():
    class Subset(Struct, frozen=False):
        x: int
        y: int = 0

        def __init__(self, x: int) -> None:
            self.x = x

    with pytest.raises(TypeError, match="unexpected keyword argument 'y'"):
        replace(Subset(1), x=2)


def test_an_init_none_class_fails_construction_before_replace_can_run():
    class NoneInit(Struct):
        x: int = 7
        __init__ = None

    with pytest.raises(TypeError, match="'NoneType' object is not callable"):
        NoneInit(3)


def test_a_zero_change_replace_of_a_mutable_struct_is_a_copy():
    original = Mutable(1)
    replaced = replace(original)

    assert replaced is not original
    assert replaced == original


def test_post_init_runs_before_the_source_dict_is_installed():
    seen = []

    class Watched(Struct, Dicted, frozen=False):
        x: int

        def __post_init__(self) -> None:
            seen.append(self.__dict__)

    original = Watched(1)
    original.extra = "world"
    replaced = replace(original, x=2)

    assert seen[-1] == {}
    assert replaced.extra == "world"


def test_a_metaclass_call_returning_a_foreign_struct_is_refused():
    class RogueMeta(type(Struct)):
        def __call__(cls, *args: object, **kwargs: object) -> object:
            return alien if cls is Other else other

    class Other(Struct, metaclass=RogueMeta, frozen=False):
        x: int = 0

        def __init__(self, x: int = 0) -> None:
            self.x = x

    class Alien(Struct, metaclass=RogueMeta, frozen=False):
        x: int = 0

        def __init__(self, x: int = 0) -> None:
            self.x = x

    other = type.__call__(Other)
    alien = type.__call__(Alien)

    disguised = Other(1)

    assert type(disguised) is Alien

    with pytest.raises(SystemError, match="returned a different type"):
        replace(disguised, x=2)
