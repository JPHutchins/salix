from collections.abc import Mapping

import pytest

from salix import Struct, from_mapping


class Point(Struct):
    x: int
    y: str = "seven"


class Empty(Struct):
    pass


class Mutable(Struct, frozen=False):
    x: int


class WithInit(Struct, frozen=False):
    x: int

    def __init__(self, x: int) -> None:
        self.x = x * 2


class PlainMapping(Mapping[str, object]):
    def __init__(self, pairs: dict[str, object]) -> None:
        self._pairs = pairs

    def __getitem__(self, key: str) -> object:
        return self._pairs[key]

    def __iter__(self):
        return iter(self._pairs)

    def __len__(self) -> int:
        return len(self._pairs)


def test_keys_in_any_order_land_in_field_order():
    built = from_mapping(Point, {"y": "two", "x": 1})

    assert built == Point(1, "two")


def test_absent_keys_take_their_defaults():
    built = from_mapping(Point, {"x": 1})

    assert built == Point(1, "seven")


def test_an_absent_mutable_default_is_copied_per_instance():
    class Holder(Struct):
        required: int
        xs: list = []  # noqa: RUF012 -- the copy is what is being pinned

    (stored,) = Holder._struct_defaults_
    built = from_mapping(Holder, {"required": 1})

    assert built.xs == []
    assert built.xs is not stored


def test_an_unknown_key_is_refused_like_the_constructor():
    with pytest.raises(TypeError, match="got an unexpected keyword argument 'z'"):
        from_mapping(Point, {"x": 1, "z": 2})


def test_a_missing_required_field_is_refused_like_the_constructor():
    with pytest.raises(TypeError, match="missing required argument 'x'"):
        from_mapping(Point, {})


def test_a_non_dict_mapping_works():
    built = from_mapping(Point, PlainMapping({"x": 1, "y": "two"}))

    assert built == Point(1, "two")


def test_a_non_mapping_is_refused():
    with pytest.raises(TypeError, match="values must be a mapping, not list"):
        from_mapping(Point, [("x", 1)])  # type: ignore[arg-type]


def test_a_non_struct_class_is_refused():
    with pytest.raises(TypeError, match="expects a struct class, not int"):
        from_mapping(5, {})  # type: ignore[arg-type]


def test_the_singleton_survives_an_empty_mapping():
    assert from_mapping(Empty, {}) is Empty()

    with pytest.raises(TypeError, match="got an unexpected keyword argument"):
        from_mapping(Empty, {"x": 1})


def test_a_mutable_zero_field_struct_stays_allocated():
    assert from_mapping(Mutable, {"x": 1}) is not from_mapping(Mutable, {"x": 1})


def test_post_init_runs():
    calls = []

    class Counted(Struct):
        x: int

        def __post_init__(self) -> None:
            calls.append(self.x)

    built = from_mapping(Counted, {"x": 3})

    assert calls == [3]
    assert built == Counted(3)


def test_a_body_init_is_the_construction_path():
    built = from_mapping(WithInit, {"x": 3})

    assert built.x == 6
