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
