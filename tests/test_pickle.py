import copy
import pickle
import sys
from collections import OrderedDict
from collections.abc import Iterable
from dataclasses import InitVar

import pytest

from salix import Struct, set_field

PROTOCOLS = range(2, pickle.HIGHEST_PROTOCOL + 1)


class Frozen(Struct):
    x: object
    y: object = 2


class Mutable(Struct, frozen=False):
    x: object
    y: object = 2


class Nested(Struct):
    inner: Frozen
    label: str = "n"


class Empty(Struct):
    pass


class OwnInit(Struct):
    x: object = 7

    def __init__(self) -> None:
        pass


class Seeded(Struct):
    a: int
    seed: InitVar[int] = 1


class Dicted:
    pass


class WithDict(Struct, Dicted, frozen=False):
    x: object = 0


class Slotted:
    __slots__ = ("extra",)


class CoBase(Struct, Slotted, frozen=False):
    x: object = 0


class Output(OrderedDict, Struct, frozen=False):
    a: int = 0


class Refusing(Struct, frozen=False):
    x: int = 0
    __new__ = None


class Reducing(Struct):
    x: int

    def __reduce__(self) -> tuple[type, tuple[int]]:
        return (Reducing, (42,))


class Stateful(Struct, frozen=False):
    x: int

    def __getstate__(self) -> dict[str, int]:
        return {"doubled": self.x * 2}

    def __setstate__(self, state: dict[str, int]) -> None:
        set_field(self, "x", state["doubled"])


class Failure(Exception, Struct):
    code: int


def round_trip(value: object, protocol: int = pickle.HIGHEST_PROTOCOL) -> object:
    return pickle.loads(pickle.dumps(value, protocol))


@pytest.mark.parametrize("protocol", PROTOCOLS)
def test_a_frozen_struct_round_trips(protocol: int) -> None:
    first = object()
    original = Frozen((1, "a"), None)

    assert round_trip(original, protocol) == original
    assert round_trip(Frozen(first), protocol).y == 2
    assert type(round_trip(original, protocol)) is Frozen


@pytest.mark.parametrize("protocol", PROTOCOLS)
def test_a_mutable_struct_round_trips(protocol: int) -> None:
    assert round_trip(Mutable([1], {"k": None}), protocol) == Mutable([1], {"k": None})


def test_none_is_a_value_and_an_unset_field_stays_unset() -> None:
    unset = Frozen.__new__(Frozen)
    set_field(unset, "y", None)

    loaded = round_trip(unset)

    assert loaded.y is None
    assert repr(loaded) == "Frozen(x=<unset>, y=None)"


def test_a_nested_frozen_struct_round_trips() -> None:
    assert round_trip(Nested(Frozen(1))) == Nested(Frozen(1))


def test_a_frozen_cycle_built_through_set_field_round_trips() -> None:
    holder = Frozen(None)
    set_field(holder, "x", holder)

    loaded = round_trip(holder)

    assert loaded.x is loaded


def test_a_mutable_cycle_round_trips() -> None:
    holder = Mutable(None)
    holder.x = holder

    loaded = round_trip(holder)

    assert loaded.x is loaded


def test_an_own_init_struct_restores_what_it_holds_rather_than_rerunning_init() -> None:
    held = OwnInit()
    set_field(held, "x", 99)

    assert round_trip(held).x == 99


def test_an_init_var_struct_round_trips_its_fields() -> None:
    assert round_trip(Seeded(3, 9)) == Seeded(3)


def test_the_instance_dict_and_a_co_base_slot_round_trip() -> None:
    with_dict = WithDict(5)
    with_dict.note = "kept"
    co_base = CoBase(3)
    co_base.extra = 4

    assert vars(round_trip(with_dict)) == {"note": "kept"}
    assert round_trip(with_dict).x == 5
    assert round_trip(co_base).extra == 4


def test_an_ordered_dict_co_base_round_trips_its_items_through_its_own_reduce() -> None:
    output = Output()
    output["key"] = 1

    assert round_trip(output)["key"] == 1


@pytest.mark.skipif(
    sys.version_info < (3, 11), reason="OrderedDict.__reduce__ carries no slot state before 3.11"
)
def test_an_ordered_dict_co_base_restores_its_field_into_the_constructed_instance() -> None:
    output = Output()
    output.a = 3

    assert round_trip(output).a == 3


@pytest.mark.parametrize("protocol", range(pickle.HIGHEST_PROTOCOL + 1))
def test_an_interned_struct_loads_as_the_singleton(protocol: int) -> None:
    assert round_trip(Empty(), protocol) is Empty()


@pytest.mark.parametrize("protocol", [0, 1])
def test_protocols_below_two_are_refused_by_name(protocol: int) -> None:
    with pytest.raises(TypeError, match=r"struct 'Frozen' pickles with protocol 2 or higher"):
        pickle.dumps(Frozen(1), protocol)


def test_a_live_frozen_struct_refuses_new_state() -> None:
    live = Frozen(1)

    with pytest.raises(TypeError, match="cannot restore state into a live frozen 'Frozen'"):
        live.__setstate__((None, {"x": 2}))

    assert live.x == 1


def test_a_live_mutable_struct_takes_new_state_like_assignment() -> None:
    live = Mutable(1)
    live.__setstate__((None, {"x": 2}))

    assert live.x == 2


def test_malformed_state_leaves_a_fresh_struct_untouched() -> None:
    fresh = Frozen.__new__(Frozen)

    with pytest.raises(TypeError, match="'z', which is not a slot of the struct"):
        fresh.__setstate__((None, {"x": 1, "z": 2}))

    with pytest.raises(TypeError, match="state must be None, a dict"):
        fresh.__setstate__([1, 2])

    assert repr(fresh) == "Frozen(x=<unset>, y=<unset>)"


def test_a_cannot_create_struct_refuses_at_load() -> None:
    payload = pickle.dumps(Mutable(1), 2).replace(b"\nMutable\n", b"\nRefusing\n")

    with pytest.raises(TypeError):
        pickle.loads(payload)


def test_an_exception_struct_keeps_its_own_reduce_at_every_protocol() -> None:
    for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
        assert round_trip(Failure(3), protocol).code == 3


def test_copy_and_deepcopy_honor_a_body_reduce() -> None:
    assert round_trip(Reducing(1)).x == 42
    assert copy.copy(Reducing(1)).x == 42
    assert copy.deepcopy(Reducing(1)).x == 42


def test_copy_and_deepcopy_honor_a_body_state_pair() -> None:
    assert round_trip(Stateful(2)).x == 4
    assert copy.copy(Stateful(2)).x == 4
    assert copy.deepcopy(Stateful(2)).x == 4


def test_a_struct_without_hooks_copies_shallow_and_deep() -> None:
    original = Mutable([1])

    assert copy.copy(original).x is original.x
    assert copy.deepcopy(original).x is not original.x


class ReducingCoBase:
    def __reduce_ex__(self, protocol: int) -> tuple[type, tuple[str]]:
        return (str, ("co-base reduce",))


class WithReducingCoBase(Struct, ReducingCoBase, frozen=False):
    x: int = 1


class ClassReducingCoBase:
    @classmethod
    def __reduce_ex__(cls, protocol: int) -> tuple[type, tuple[str]]:
        return (str, (f"{cls.__name__} at {protocol}",))


class WithClassReducingCoBase(Struct, ClassReducingCoBase, frozen=False):
    x: int = 1


class RestoringCoBase:
    def __setstate__(self, state: object) -> None:
        set_field(self, "x", 42)


class WithRestoringCoBase(Struct, RestoringCoBase, frozen=False):
    x: int = 1


class ClassRestoringCoBase:
    restored: object = None

    @classmethod
    def __setstate__(cls, state: object) -> None:
        cls.restored = state


class WithClassRestoringCoBase(Struct, ClassRestoringCoBase, frozen=False):
    x: int = 1


class NoReduceEx(Struct, frozen=False):
    x: object
    __reduce_ex__ = None


class GetStateOnly(Struct, frozen=False):
    x: int

    def __getstate__(self) -> dict[str, int]:
        return {"x": self.x}


class Late(Struct, frozen=False):
    x: int


def test_a_co_base_reduce_ex_steers_pickle_copy_and_deepcopy():
    assert round_trip(WithReducingCoBase()) == "co-base reduce"
    assert copy.copy(WithReducingCoBase()) == "co-base reduce"
    assert copy.deepcopy(WithReducingCoBase()) == "co-base reduce"


def test_a_classmethod_co_base_reduce_ex_receives_only_the_protocol():
    assert round_trip(WithClassReducingCoBase(), 3) == "WithClassReducingCoBase at 3"
    assert copy.copy(WithClassReducingCoBase()) == "WithClassReducingCoBase at 4"


def test_a_co_base_setstate_restores_through_pickle_and_copy():
    assert round_trip(WithRestoringCoBase()).x == 42
    assert copy.copy(WithRestoringCoBase()).x == 42
    assert copy.deepcopy(WithRestoringCoBase()).x == 42


def test_a_classmethod_co_base_setstate_receives_only_the_state():
    round_trip(WithClassRestoringCoBase(5))

    assert WithClassRestoringCoBase.restored == (None, {"x": 5})


def test_a_none_reduce_ex_leaves_copy_on_the_struct_s_own_path():
    original = NoReduceEx([1])

    assert copy.copy(original).x is original.x
    assert copy.deepcopy(original).x == [1]
    assert copy.deepcopy(original).x is not original.x


def test_dict_state_on_a_struct_without_an_instance_dict_names_the_struct():
    message = "struct 'GetStateOnly' has no instance __dict__ to restore dict state into"

    with pytest.raises(AttributeError, match=message):
        round_trip(GetStateOnly(5))

    with pytest.raises(AttributeError, match=message):
        copy.copy(GetStateOnly(5))


def test_dict_state_on_a_live_frozen_struct_is_refused_as_live():
    with pytest.raises(TypeError, match="cannot restore state into a live frozen 'Frozen'"):
        Frozen(1).__setstate__({"x": 2})


@pytest.mark.xfail(strict=True, reason="copy reads a class's reduce hooks when the class is created")
def test_copy_honors_a_hook_assigned_after_the_class_statement():
    Late.__getstate__ = lambda self: (None, {"x": self.x + 100})

    assert round_trip(Late(5)).x == 105
    assert copy.copy(Late(5)).x == 105


class DictCoBase(dict, Struct, frozen=False):
    a: int = 0


class SetCoBase(set, Struct, frozen=False):
    a: int = 0


class ListCoBase(list, Struct, frozen=False):
    a: int = 0


class OrderedCoBase(OrderedDict, Struct, frozen=False):
    a: int = 0


def filled_builtin_co_bases() -> list[object]:
    as_dict = DictCoBase()
    as_dict["k"] = 1
    as_set = SetCoBase()
    as_set.add(1)
    as_list = ListCoBase()
    as_list.append(1)
    as_ordered = OrderedCoBase()
    as_ordered["k"] = 1

    for value in (as_dict, as_set, as_list, as_ordered):
        value.a = 3

    return [as_dict, as_set, as_list, as_ordered]


@pytest.mark.parametrize("duplicate", [copy.copy, copy.deepcopy, round_trip], ids=["copy", "deepcopy", "pickle"])
def test_a_builtin_co_base_keeps_its_items_through_every_duplication(duplicate):
    for original in filled_builtin_co_bases():
        duplicated = duplicate(original)

        assert type(duplicated) is type(original)
        assert sorted(duplicated) == sorted(original)


@pytest.mark.skipif(
    sys.version_info < (3, 11), reason="set and OrderedDict reduce without slot state before 3.11"
)
@pytest.mark.parametrize("duplicate", [copy.copy, copy.deepcopy, round_trip], ids=["copy", "deepcopy", "pickle"])
def test_a_builtin_co_base_keeps_its_field_through_every_duplication(duplicate):
    for original in filled_builtin_co_bases():
        assert duplicate(original).a == 3


class NoReduceExWithReduce(Struct, frozen=False):
    x: int
    __reduce_ex__ = None

    def __reduce__(self) -> tuple[type, tuple[str]]:
        return (str, ("via reduce",))


class NoReduceExOverDict(dict, Struct, frozen=False):
    a: int = 0
    __reduce_ex__ = None


class HiddenReduceEx(Struct, frozen=False):
    x: object

    def __getstate__(self) -> tuple[None, dict[str, object]]:
        return (None, {"x": self.x})

    def __getattribute__(self, name: str) -> object:
        if name == "__reduce_ex__":
            raise AttributeError(name)

        return object.__getattribute__(self, name)


def test_a_none_reduce_ex_falls_back_to_a_body_reduce_as_copy_dot_py_does():
    assert copy.copy(NoReduceExWithReduce(1)) == "via reduce"
    assert copy.deepcopy(NoReduceExWithReduce(1)) == "via reduce"


def test_a_none_reduce_ex_over_builtin_storage_falls_back_to_reduce_not_the_struct_s_copy():
    with pytest.raises(TypeError, match="cannot be pickled"):
        copy.copy(NoReduceExOverDict())


def test_a_reduce_ex_that_raises_attribute_error_counts_as_absent():
    original = HiddenReduceEx([1])

    assert copy.copy(original).x is original.x


class SlottedExtra:
    __slots__ = ("extra",)


class ExtraAttribute:
    extra = 5


class ShadowedSlot(Struct, ExtraAttribute, SlottedExtra, frozen=False):
    y: int = 0


def test_a_co_base_slot_behind_a_plain_class_attribute_restores():
    shadowed = ShadowedSlot()
    shadowed.extra = 9

    assert round_trip(shadowed).extra == 9


@pytest.mark.parametrize("make", [lambda: Mutable(1), WithDict, CoBase], ids=["plain", "instance dict", "co-base slot"])
def test_python_level_storage_stays_on_the_struct_s_own_copy(make, monkeypatch):
    reductions = []
    instance = make()
    monkeypatch.setattr(type(instance), "__reduce_ex__", lambda self, protocol: reductions.append(protocol), raising=False)

    copy.copy(instance)
    copy.deepcopy(instance)

    assert reductions == []


def test_a_c_defined_co_base_refuses_duplication_like_a_stock_subclass():
    element_tree = pytest.importorskip("_elementtree")

    class BuilderCoBase(element_tree.TreeBuilder, Struct, frozen=False):
        a: int = 0

    class StockBuilder(element_tree.TreeBuilder):
        pass

    for duplicate in (copy.copy, copy.deepcopy, round_trip):
        with pytest.raises(TypeError, match="cannot pickle 'StockBuilder' object"):
            duplicate(StockBuilder())

        with pytest.raises(TypeError, match="cannot pickle 'BuilderCoBase' object"):
            duplicate(BuilderCoBase())


class InitSetCoBase(set, Struct):
    a: int = 0

    def __init__(self, items: Iterable[object] = (), a: int = 0) -> None:
        super().__init__(items)
        set_field(self, "a", a)


class InitOrderedCoBase(OrderedDict, Struct):
    a: int = 0

    def __init__(self, items: Iterable[tuple[object, object]] = (), a: int = 0) -> None:
        super().__init__(items)
        set_field(self, "a", a)


class RequiredSetCoBase(set, Struct):
    a: int


class RequiredOrderedCoBase(OrderedDict, Struct):
    a: int


class StockSlottedSet(set):
    __slots__ = ("a",)


class StockSlottedOrdered(OrderedDict):
    __slots__ = ("a",)


def test_a_value_co_base_whose_new_construction_skips_is_refused():
    with pytest.raises(TypeError, match="a struct cannot extend float"):

        class FloatCoBase(float, Struct, frozen=False):
            a: int = 0


@pytest.mark.parametrize(
    "make",
    [lambda: InitSetCoBase([1], a=7), lambda: InitOrderedCoBase([("k", 1)], a=7)],
    ids=["set", "OrderedDict"],
)
@pytest.mark.parametrize("duplicate", [copy.copy, copy.deepcopy, round_trip], ids=["copy", "deepcopy", "pickle"])
def test_a_frozen_struct_refuses_state_once_its_co_base_reduce_has_called_the_class(make, duplicate):
    if sys.version_info >= (3, 11):
        with pytest.raises(TypeError, match="cannot restore state into a live frozen"):
            duplicate(make())
    else:
        assert duplicate(make()).a == 0


@pytest.mark.parametrize(
    ("ours", "stock", "items"),
    [
        (RequiredSetCoBase, StockSlottedSet, [1]),
        (RequiredOrderedCoBase, StockSlottedOrdered, [("k", 1)]),
    ],
    ids=["set", "OrderedDict"],
)
@pytest.mark.parametrize("duplicate", [copy.copy, copy.deepcopy, round_trip], ids=["copy", "deepcopy", "pickle"])
def test_a_class_calling_co_base_carries_field_state_like_a_stock_slotted_subclass(ours, stock, items, duplicate):
    original = ours(items)
    set_field(original, "a", 7)
    stock_original = stock(items)
    stock_original.a = 7

    assert getattr(duplicate(original), "a", None) == getattr(duplicate(stock_original), "a", None)
    assert getattr(duplicate(original), "a", None) == (7 if sys.version_info >= (3, 11) else None)
