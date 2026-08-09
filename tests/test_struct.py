import pytest

from salix import Struct


class Point2D(Struct):
    x: float
    y: float


class WithDefaults(Struct):
    a: int
    b: int = 2
    c: int = 3


def test_basic_construct_and_access():
    p = Point2D(1.0, 2.0)
    assert p.x == 1.0
    assert p.y == 2.0
    assert Point2D(x=1.0, y=2.0).x == 1.0
    assert Point2D(1.0, y=2.0).y == 2.0


def test_slots_and_no_dict():
    p = Point2D(1.0, 2.0)
    assert not hasattr(p, "__dict__")
    assert Point2D.__slots__ == ("x", "y")


def test_non_ascii_field_name():
    class MenuItem(Struct):
        café: int

    item = MenuItem(café=3)
    assert item.café == 3
    assert item._struct_fields_ == ("café",)


def test_match_args():
    assert Point2D.__match_args__ == ("x", "y")
    match Point2D(1.0, 2.0):
        case Point2D(x, y):
            assert x == 1.0 and y == 2.0
        case _:
            pytest.fail("pattern did not match")


def test_annotations():
    assert Point2D.__annotations__ == {"x": float, "y": float}


def test_struct_fields_introspection():
    assert Point2D(1.0, 2.0)._struct_fields_ == ("x", "y")
    assert WithDefaults(1)._struct_defaults_ == (2, 3)


def test_introspection_answers_on_the_class_too():
    assert Point2D._struct_fields_ == ("x", "y")
    assert WithDefaults._struct_defaults_ == (2, 3)
    assert Struct._struct_fields_ == ()
    assert Struct._struct_defaults_ == ()


def test_defaults():
    assert WithDefaults(1) == WithDefaults(1, 2, 3)
    assert WithDefaults(1, b=20).b == 20
    assert WithDefaults(1, 0, 0).c == 0


def test_immutable():
    p = Point2D(1.0, 2.0)
    with pytest.raises(TypeError):
        p.x = 9.0
    with pytest.raises(TypeError):
        del p.x


def test_eq_structural():
    assert Point2D(1.0, 2.0) == Point2D(1.0, 2.0)
    assert Point2D(1.0, 2.0) != Point2D(1.0, 9.0)

    class Other2D(Struct):
        x: float
        y: float

    assert Point2D(1.0, 2.0) == Other2D(1.0, 2.0)

    class Point1D(Struct):
        x: float

    assert Point1D(1.0) != Point2D(1.0, 2.0)


def test_hash():
    assert hash(Point2D(1.0, 2.0)) == hash(Point2D(1.0, 2.0))
    assert hash(Point2D(1.0, 2.0)) == hash((1.0, 2.0))
    assert len({Point2D(1.0, 2.0), Point2D(1.0, 2.0)}) == 1


def test_repr():
    assert repr(Point2D(1.0, 2.0)) == "Point2D(x=1.0, y=2.0)"
    assert repr(WithDefaults(1)) == "WithDefaults(a=1, b=2, c=3)"


@pytest.mark.parametrize("bad", [
    lambda: Point2D(1.0),
    lambda: Point2D(1.0, 2.0, 3.0),
    lambda: Point2D(1.0, 2.0, z=3.0),
    lambda: Point2D(1.0, x=2.0),
])
def test_errors(bad):
    with pytest.raises(TypeError):
        bad()


def test_inheritance_extends_fields():
    class Point3D(Point2D):
        z: float

    p = Point3D(1.0, 2.0, 3.0)
    assert (p.x, p.y, p.z) == (1.0, 2.0, 3.0)
    assert Point3D.__match_args__ == ("x", "y", "z")
    assert p._struct_fields_ == ("x", "y", "z")


def test_empty_struct():
    class Empty(Struct):
        pass

    assert Empty() == Empty()
    assert repr(Empty()) == "Empty()"


def test_metaclass_identity():
    assert type(Point2D) is type(Struct)
    assert isinstance(Point2D(1.0, 2.0), Struct)


def test_module_of_the_exported_names():
    assert Struct.__module__ == "salix"
    assert Point2D.__module__ == __name__
