import pytest

from salix import Struct


class Base(Struct):
    x: int
    y: int


def test_a_subclass_appends_its_own_fields():
    class Extended(Base):
        z: int

    assert Extended.__match_args__ == ("x", "y", "z")
    assert Extended(1, 2, 3)._struct_fields_ == ("x", "y", "z")


def test_an_inherited_field_keeps_its_position():
    class Extended(Base):
        z: int

    assert Extended(1, 2, 3).x == 1


def test_inherited_defaults_carry_over():
    class WithDefault(Struct):
        a: int
        b: int = 7

    class Extended(WithDefault):
        c: int = 8

    assert Extended(1)._struct_defaults_ == (7, 8)
    assert (Extended(1).b, Extended(1).c) == (7, 8)


def test_a_subclass_may_give_an_inherited_field_a_default():
    class Defaulted(Base):
        y: int = 5

    assert Defaulted(1).y == 5
    assert Defaulted.__match_args__ == ("x", "y")
    assert Defaulted(1)._struct_fields_ == ("x", "y")


def test_a_redefaulted_inherited_field_is_not_shadowed_by_its_class_variable():
    class Defaulted(Base):
        y: int = 5

    instance = Defaulted(1, 9)

    assert instance.y == 9
    assert repr(instance) == "Defaulted(x=1, y=9)"


def test_a_bare_binding_over_an_inherited_field_is_not_shadowed_either():
    """The same shadowing, reached without re-annotating: a class variable whose
    name is already a field would otherwise sit ahead of the base's descriptor
    in the MRO, and attribute access alone would disagree with everything else.
    """

    class Bound(Base):
        y = 42

    instance = Bound(1, 9)

    assert instance.y == 9
    assert repr(instance) == "Bound(x=1, y=9)"


def test_a_required_field_may_not_follow_a_defaulted_one():
    with pytest.raises(TypeError, match="non-default field 'b' follows a field with a default"):

        class Bad(Struct):
            a: int = 1
            b: int


def test_the_rule_holds_across_the_inheritance_boundary():
    class Defaulted(Struct):
        a: int = 1

    with pytest.raises(TypeError, match="non-default field 'b' follows a field with a default"):

        class Bad(Defaulted):
            b: int


def test_a_class_with_no_annotations_is_transparent():
    class Middle(Base):
        pass

    class Bottom(Middle):
        z: int

    assert Bottom(1, 2, 3)._struct_fields_ == ("x", "y", "z")


def test_a_deep_chain_accumulates_in_order():
    current = Base

    for name in ("p", "q", "r"):
        current = type(current)(f"Level_{name}", (current,), {"__annotations__": {name: int}})

    assert current(1, 2, 3, 4, 5)._struct_fields_ == ("x", "y", "p", "q", "r")


def test_the_base_itself_has_no_fields():
    assert Struct()._struct_fields_ == ()
    assert Struct()._struct_defaults_ == ()


def test_two_field_bearing_bases_are_rejected():
    """Each adds a slot, so the layouts cannot be combined."""

    class Other(Struct):
        z: int

    with pytest.raises(TypeError, match="lay-out conflict"):

        class Both(Base, Other):
            pass


def test_a_fieldless_second_base_is_allowed():
    class Marker:
        pass

    class Combined(Base, Marker):
        z: int

    assert Combined(1, 2, 3)._struct_fields_ == ("x", "y", "z")
    assert isinstance(Combined(1, 2, 3), Marker)
