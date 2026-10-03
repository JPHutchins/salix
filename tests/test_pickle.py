import copy
import pickle
import sys
from collections import OrderedDict
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
