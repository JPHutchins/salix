import collections
import dataclasses
import gc
import re
import weakref
from collections.abc import Mapping

import pytest

from salix import Struct, from_mapping, set_field


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
        xs: list = []  # noqa: RUF012

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


class LyingItems(Mapping[str, object]):
    def __init__(self, items_result: object) -> None:
        self._items_result = items_result

    def __getitem__(self, key: str) -> object:
        raise KeyError(key)

    def __iter__(self):
        return iter(())

    def __len__(self) -> int:
        return 1

    def items(self):
        return self._items_result


def test_an_items_pair_that_is_not_a_two_tuple_is_refused():
    with pytest.raises(TypeError, match="items\\(\\) must yield \\(str, value\\) pairs"):
        from_mapping(Point, LyingItems([("x", 1, "extra"), 42]))


def test_an_items_result_that_is_not_a_sequence_is_refused():
    with pytest.raises(TypeError, match="items\\(\\) must return a sequence"):
        from_mapping(Point, LyingItems(5))


def test_an_items_exception_propagates():
    class RaisingItems(Mapping[str, object]):
        def __getitem__(self, key: str) -> object:
            raise KeyError(key)

        def __iter__(self):
            return iter(())

        def __len__(self) -> int:
            return 0

        @property
        def items(self) -> object:
            raise RuntimeError("boom")

    with pytest.raises(RuntimeError, match="boom"):
        from_mapping(Point, RaisingItems())


def test_a_non_string_key_is_refused_like_the_constructor():
    with pytest.raises(TypeError, match="keywords must be strings"):
        from_mapping(Point, {1: "one"})  # type: ignore[arg-type]

    with pytest.raises(TypeError, match="keywords must be strings"):
        from_mapping(Point, LyingItems([(1, "one")]))


def test_a_body_init_receives_a_non_dict_mapping_through_its_items():
    built = from_mapping(WithInit, PlainMapping({"x": 3}))

    assert built.x == 6


def test_an_items_result_that_is_a_tuple_works():
    class TupleItems(Mapping[str, object]):
        def __getitem__(self, key: str) -> object:
            raise KeyError(key)

        def __iter__(self):
            return iter(())

        def __len__(self) -> int:
            return 2

        def items(self):
            return (("x", 1), ("y", "two"))

    built = from_mapping(Point, TupleItems())

    assert built == Point(1, "two")


def test_duplicate_items_keys_are_refused_like_the_constructor():
    class DuplicateItems(Mapping[str, object]):
        def __getitem__(self, key: str) -> object:
            raise KeyError(key)

        def __iter__(self):
            return iter(())

        def __len__(self) -> int:
            return 2

        def items(self):
            return [("x", 1), ("x", 2)]

    with pytest.raises(TypeError, match="multiple values for argument 'x'"):
        from_mapping(Point, DuplicateItems())


def test_a_dict_mixing_non_string_and_unknown_keys_raises_strings_first():
    with pytest.raises(TypeError, match="keywords must be strings"):
        from_mapping(Point, {"z": 1, 2: "two"})  # type: ignore[arg-type]


def test_the_field_bind_path_bypasses_a_metaclass_call():
    calls = []

    class CountingMeta(type(Struct)):
        def __call__(cls, *args: object, **kwargs: object) -> object:
            calls.append(1)
            return super().__call__(*args, **kwargs)

    class Counted(Struct, metaclass=CountingMeta):
        x: int

    Counted(1)
    assert len(calls) == 1

    from_mapping(Counted, {"x": 2})

    assert len(calls) == 1


def test_an_own_init_class_refuses_a_non_string_key_like_the_constructor():
    with pytest.raises(TypeError, match="keywords must be strings"):
        from_mapping(WithInit, {1: "one"})  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("co_base", "owner"),
    [(dict, "dict"), (collections.OrderedDict, "collections.OrderedDict"), (set, "set")],
    ids=["dict", "OrderedDict", "set"],
)
def test_from_mapping_refuses_a_struct_whose_init_comes_from_a_builtin_container(co_base, owner):
    class Over(co_base, Struct, frozen=False):
        a: int = 0

    with pytest.raises(TypeError, match=rf"takes its __init__ from {re.escape(owner)}, a built-in that salix does not pass"):
        from_mapping(Over, {"a": 7})


def test_from_mapping_through_a_cooperative_python_init_binds_the_field():
    class Cooperative:
        def __init__(self, *, a: int = 0) -> None:
            set_field(self, "a", a)

    class Over(Cooperative, Struct, frozen=False):
        a: int = 0

    assert from_mapping(Over, {"a": 7}).a == 7


def test_an_init_var_struct_over_a_container_is_refused_because_from_mapping_calls_the_class():
    class WithInitVar(dict, Struct, frozen=False):
        a: int = 0
        flag: dataclasses.InitVar[int] = 1

    with pytest.raises(TypeError, match="takes its __init__ from dict,"):
        from_mapping(WithInitVar, {"a": 7})


def test_from_mapping_refuses_a_built_in_init_before_reading_the_values():
    reads = []

    class Watched(Mapping[str, object]):
        def __getitem__(self, key: str) -> object:
            return 7

        def __iter__(self):
            return iter(["a"])

        def __len__(self) -> int:
            return 1

        def items(self):
            reads.append("items")
            return super().items()

    class Over(dict, Struct, frozen=False):
        a: int = 0

    with pytest.raises(TypeError, match="takes its __init__ from dict,"):
        from_mapping(Over, Watched())

    assert reads == []


def test_a_base_swapped_out_and_freed_leaves_no_trace_in_the_refusal():
    class PlainDict(dict):
        pass

    def build() -> tuple[type, weakref.ref[type]]:
        class Assigned(dict):
            __init__ = dict.__init__

        class Over(Assigned, Struct, frozen=False):
            a: int = 0

        return Over, weakref.ref(Assigned)

    over, assigned = build()
    over.__bases__ = (PlainDict, Struct)
    gc.collect()

    assert assigned() is None

    with pytest.raises(TypeError, match="'Over' takes its __init__ from dict,"):
        from_mapping(over, {"a": 7})
