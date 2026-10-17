import sys

import pytest

from salix import Struct, to_dict


class Point(Struct):
    x: int
    y: str = "seven"


class Empty(Struct):
    pass


class Partial(Struct, frozen=False):
    x: int
    y: int

    def __init__(self, x: int) -> None:
        self.x = x


class Dicted:
    pass


class WithDict(Struct, Dicted, frozen=False):
    x: int


def test_each_set_field_maps_to_its_value_in_declaration_order():
    assert list(to_dict(Point(1, "two")).items()) == [("x", 1), ("y", "two")]


def test_inherited_fields_come_first():
    class Located(Point):
        z: float = 0.5

    assert list(to_dict(Located(1))) == ["x", "y", "z"]


def test_the_values_are_the_instance_values_not_copies():
    class Holder(Struct):
        items: list[int]

    items = [1]

    assert to_dict(Holder(items))["items"] is items


def test_an_unset_field_is_skipped():
    assert to_dict(Partial(1)) == {"x": 1}
    assert to_dict(Point.__new__(Point)) == {}


@pytest.mark.parametrize("set_count", [1, 6, 11])
def test_a_sparse_instance_gives_a_dict_sized_for_its_set_fields(set_count):
    Wide = type("Wide", (Struct,), {"__annotations__": {f"field_{i}": object for i in range(30)}})
    Narrow = type("Narrow", (Struct,), {"__annotations__": {f"field_{i}": object for i in range(set_count)}})
    values = {f"field_{i}": i for i in range(set_count)}
    sparse = Wide.__new__(Wide)
    sparse.__setstate__((None, values))

    built = to_dict(sparse)
    _, slots = sparse.__reduce_ex__(4)[2]

    assert built == values
    assert sys.getsizeof(built) == sys.getsizeof(to_dict(Narrow(**values)))
    assert sys.getsizeof(slots) == sys.getsizeof(built)


def test_a_field_set_to_none_is_kept():
    assert to_dict(Point(1, None)) == {"x": 1, "y": None}


def test_a_fieldless_struct_gives_an_empty_dict():
    assert to_dict(Empty()) == {}


def test_the_instance_dict_is_not_merged():
    instance = WithDict(1)
    instance.extra = "a"

    assert to_dict(instance) == {"x": 1}


def test_equal_fields_give_equal_dicts_whatever_the_instance_dicts_hold():
    first = WithDict(1)
    second = WithDict(1)
    first.extra = "a"
    second.extra = "b"

    assert first == second
    assert to_dict(first) == to_dict(second)


def test_an_unset_field_and_a_none_field_compare_equal_but_give_different_dicts():
    unset = Partial(1)
    none = Partial(1)
    none.y = None

    assert unset == none
    assert to_dict(unset) == {"x": 1}
    assert to_dict(none) == {"x": 1, "y": None}


def test_each_call_returns_a_new_dict():
    instance = Point(1)
    first = to_dict(instance)
    first["x"] = 9

    assert to_dict(instance) == {"x": 1, "y": "seven"}
    assert to_dict(instance) is not to_dict(instance)


def test_a_class_is_refused():
    with pytest.raises(TypeError, match=r"^to_dict\(\) expects a struct instance, not the class Point$"):
        to_dict(Point)


def test_a_non_struct_is_refused():
    with pytest.raises(TypeError, match=r"^to_dict\(\) expects a struct instance, not an instance of int$"):
        to_dict(1)


def test_an_instance_made_while_its_class_is_created_is_refused():
    refusals: list[str] = []

    class Base(Struct):
        def __init_subclass__(cls, **keywords: object) -> None:
            super().__init_subclass__(**keywords)

            try:
                to_dict(cls.__new__(cls))
            except TypeError as refusal:
                refusals.append(str(refusal))

    class Late(Base):
        x: int = 1

    assert refusals == ["cannot read the fields of 'Late' until its class has finished being created"]
    assert to_dict(Late()) == {"x": 1}
