from dataclasses import dataclass

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
    with pytest.raises(AttributeError):
        p.x = 9.0
    with pytest.raises(AttributeError):
        del p.x


def test_eq_compares_instances_of_one_class():
    assert Point2D(1.0, 2.0) == Point2D(1.0, 2.0)
    assert Point2D(1.0, 2.0) != Point2D(1.0, 9.0)

    class Other2D(Struct):
        x: float
        y: float

    assert Point2D(1.0, 2.0) != Other2D(1.0, 2.0)

    class Point1D(Struct):
        x: float

    assert Point1D(1.0) != Point2D(1.0, 2.0)


def test_two_field_less_classes_are_unequal_like_stock_dataclasses():
    @dataclass(frozen=True)
    class StockTop:
        pass

    @dataclass(frozen=True)
    class StockDead:
        pass

    class Top(Struct):
        pass

    class Dead(Struct):
        pass

    assert (StockTop() == StockDead(), Top() == Dead()) == (False, False)
    assert (len({StockTop(), StockDead()}), len({Top(), Dead()})) == (2, 2)
    assert Top() == Top()


def test_a_subclass_instance_is_unequal_to_its_base_s_like_stock_dataclasses():
    @dataclass(frozen=True)
    class StockBase:
        x: int

    @dataclass(frozen=True)
    class StockChild(StockBase):
        pass

    class Base(Struct):
        x: int

    class Child(Base):
        pass

    assert (StockBase(1) == StockChild(1), Base(1) == Child(1)) == (False, False)
    assert (StockChild(1) == StockBase(1), Child(1) == Base(1)) == (False, False)


def test_a_struct_is_unequal_to_a_tuple_of_its_values():
    assert Point2D(1.0, 2.0) != (1.0, 2.0)


def test_repr_names_the_qualified_class_like_stock_dataclasses():
    def define():
        @dataclass
        class StockPoint:
            x: int

        class Point(Struct):
            x: int

        return StockPoint, Point

    StockPoint, Point = define()

    assert repr(Point(1)) == repr(StockPoint(1)).replace("StockPoint", "Point")

    StockPoint.__qualname__ = "Renamed"
    Point.__qualname__ = "Renamed"

    assert repr(Point(1)) == repr(StockPoint(1)) == "Renamed(x=1)"


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
    assert repr(Empty()) == f"{Empty.__qualname__}()"


def test_metaclass_identity():
    assert type(Point2D) is type(Struct)
    assert isinstance(Point2D(1.0, 2.0), Struct)


def test_module_of_the_exported_names():
    assert Struct.__module__ == "salix"
    assert Point2D.__module__ == __name__
