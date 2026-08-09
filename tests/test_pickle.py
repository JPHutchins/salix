import copy
import copyreg
import pickle
from collections.abc import Callable

import pytest

from salix import Struct, set_field

# The classes live at module level because pickle resolves a class by qualified
# name, and an instance of a test-local class cannot be unpickled.

init_calls: list[int] = []
post_init_calls: list[int] = []
body_new_calls: list[str] = []
reduce_calls: list[str] = []


class Plain(Struct):
    x: int
    y: int = 0


class Mutable(Struct, frozen=False):
    x: int
    y: int = 0


class WithInit(Struct, frozen=False):
    x: int
    y: int = 0

    def __init__(self, both: int) -> None:
        init_calls.append(both)
        self.x = both
        self.y = both


class FrozenWithInit(Struct):
    x: int
    y: int

    def __init__(self) -> None:
        set_field(self, "x", 1)


class Child(Plain):
    z: int = 3


class Empty(Struct):
    pass


class Node(Struct):
    child: object = None


class WithPostInit(Struct, frozen=False):
    x: int

    def __post_init__(self) -> None:
        post_init_calls.append(self.x)


class MutableDefault(Struct, frozen=False):
    xs: list = []  # noqa: RUF012 -- the copy is the feature under test


class Dicted:
    pass


class WithDict(Struct, Dicted, frozen=False):
    x: int


class WithBodyNew(Struct):
    x: int

    def __new__(cls):
        body_new_calls.append("with_body_new")
        return super().__new__(cls)


class ChildOfBodyNew(WithBodyNew):
    y: int = 9


class RecordingNew(Struct):
    x: int = 0

    def __new__(cls, both: int = 0):
        body_new_calls.append(("recording_new", both))
        obj = super().__new__(cls)
        set_field(obj, "x", both)
        return obj


class ArgNew(Struct):
    x: int

    def __new__(cls, both: int):
        body_new_calls.append(("arg_new", both))
        obj = super().__new__(cls)
        set_field(obj, "x", both)
        return obj

    def __getnewargs__(self):
        return (self.x,)


class SingletonNew(Struct):
    x: int = 1
    _instance = None

    def __new__(cls):
        if cls._instance is None:
            obj = super().__new__(cls)
            set_field(obj, "x", 42)
            cls._instance = obj
        return cls._instance


class RejectingNew(Struct):
    x: int = 0
    reject = False

    def __new__(cls):
        if cls.reject:
            raise TypeError("genuine rejection")
        return super().__new__(cls)


class DeclinesForReconstruction(Struct):
    x: int = 5

    def __new__(cls, v=None):
        if v is not None:
            raise TypeError("declined for reconstruction")
        obj = super().__new__(cls)
        set_field(obj, "x", 5)
        return obj

    def __getnewargs__(self):
        return (self.x,)


class WithGetNewArgs(Struct):
    x: int

    def __getnewargs__(self):
        return (self.x,)


class WithGetNewArgsEx(Struct):
    x: int

    def __getnewargs_ex__(self):
        return ((self.x,), {"x": self.x})


class WithGetNewArgsExKeywordOnly(Struct):
    x: int

    def __getnewargs_ex__(self):
        return ((), {"x": self.x})


class CoBaseNew:
    def __new__(cls, v: int = 0):
        body_new_calls.append("co_base_new")
        return super().__new__(cls)


class BodyBaseNew(Struct):
    x: int = 0

    def __new__(cls, v: int = 0):
        body_new_calls.append("body_base_new")
        return super().__new__(cls)


class SubWithCoBaseNew(CoBaseNew, BodyBaseNew):
    y: int = 1


class ReduceCopyBase(Struct):
    x: int = 0

    def __copy__(self):
        return "user_copy"

    def __reduce__(self):
        return (type(self), (), self.__getstate__())


class SubOfReduceCopyBase(ReduceCopyBase):
    y: int = 1


class GenuineWithGNA(Struct):
    x: int = 0

    def __new__(cls, v: int = 0):
        raise TypeError("genuine rejection")

    def __getnewargs__(self):
        return (self.x,)


class ValueErrorNew(Struct):
    x: int = 5

    def __new__(cls, v: int = 0):
        if v:
            raise ValueError("genuine rejection")
        return super().__new__(cls)

    def __getnewargs__(self):
        return (self.x,)


class DeclinesWithReduce(Struct):
    x: int = 5

    def __new__(cls, v: int = 0):
        if v:
            raise TypeError("declined for reconstruction")
        return super().__new__(cls)

    def __getnewargs__(self):
        return (self.x,)

    def __reduce__(self):
        return (copyreg.__newobj__, (type(self), self.x), self.__getstate__())


class DeclinesWithReduceNoGNA(Struct):
    x: int = 5

    def __new__(cls, v: int = 0):
        if v:
            raise TypeError("declined for reconstruction")
        return super().__new__(cls)

    def __reduce__(self):
        return (copyreg.__newobj__, (type(self), self.x), self.__getstate__())


class WithReduce(Struct):
    x: int = 0

    def __reduce__(self):
        reduce_calls.append("custom")
        return (type(self), (), self.__getstate__())


FACTORIES: dict[str, Callable[[], Struct]] = {
    "plain": lambda: Plain(1, 2),
    "mutable": lambda: Mutable(1, 2),
    "with_init": lambda: WithInit(7),
    "frozen_with_init": FrozenWithInit,
    "child": lambda: Child(1, 2),
    "empty": Empty,
    "with_post_init": lambda: WithPostInit(1),
    "mutable_default": MutableDefault,
}


@pytest.fixture(autouse=True)
def _reset_call_records() -> None:
    init_calls.clear()
    post_init_calls.clear()
    body_new_calls.clear()
    reduce_calls.clear()


@pytest.mark.parametrize("protocol", [2, 5])
@pytest.mark.parametrize("factory", FACTORIES.values(), ids=FACTORIES.keys())
def test_round_trip_preserves_equality_and_type(factory, protocol):
    instance = factory()
    restored = pickle.loads(pickle.dumps(instance, protocol=protocol))

    assert restored == instance
    assert type(restored) is type(instance)


def test_a_written_non_default_value_survives_the_round_trip():
    """Written first, because tp_new fills defaults during loads whatever the
    pickle said -- asserting the default back would pass on a round trip that
    restored nothing at all."""

    frozen = Plain(1, 2)
    set_field(frozen, "x", 99)

    mutable = Mutable(1, 2)
    mutable.x = 99

    assert pickle.loads(pickle.dumps(frozen)).x == 99
    assert pickle.loads(pickle.dumps(mutable)).x == 99


@pytest.mark.parametrize("factory", FACTORIES.values(), ids=FACTORIES.keys())
def test_copy_and_deepcopy_for_every_base_shape(factory):
    instance = factory()
    copied = copy.copy(instance)
    deep = copy.deepcopy(instance)

    assert copied == instance
    assert deep == instance
    assert copied is not instance
    assert deep is not instance


def test_copy_shares_a_mutable_field_and_deepcopy_does_not():
    instance = MutableDefault()
    instance.xs.append(1)
    copied = copy.copy(instance)
    deep = copy.deepcopy(instance)

    assert copied.xs is instance.xs
    assert deep.xs is not instance.xs
    assert deep.xs == instance.xs


def test_a_restored_frozen_struct_still_refuses_writes():
    restored = pickle.loads(pickle.dumps(Plain(1, 2)))

    with pytest.raises(TypeError, match="does not support attribute assignment"):
        restored.x = 9


def test_post_init_runs_once_at_construction_and_not_on_restore():
    post_init_calls.clear()
    instance = WithPostInit(1)

    assert post_init_calls == [1]

    pickle.loads(pickle.dumps(instance))
    copy.copy(instance)
    copy.deepcopy(instance)

    assert post_init_calls == [1]


def test_a_custom_init_signature_is_preserved():
    with pytest.raises(TypeError, match="takes 2 positional arguments"):
        WithInit(1, 2)


def test_the_round_trip_never_calls_init():
    init_calls.clear()
    instance = WithInit(7)

    assert init_calls == [7]

    pickle.loads(pickle.dumps(instance))
    copy.copy(instance)
    copy.deepcopy(instance)

    assert init_calls == [7]


def test_none_round_trips_as_none_not_absence():
    instance = Mutable(1, 2)
    instance.y = None

    assert pickle.loads(pickle.dumps(instance)).y is None


def test_an_unset_field_round_trips_as_unset():
    instance = Mutable(1, 2)
    del instance.x
    restored = pickle.loads(pickle.dumps(instance))

    with pytest.raises(AttributeError):
        _ = restored.x

    assert repr(restored) == "Mutable(x=<unset>, y=2)"


def test_an_unwritten_required_field_round_trips_as_unset():
    instance = FrozenWithInit()

    assert instance.__getstate__() == ({"x": 1}, ("y",), None)

    restored = pickle.loads(pickle.dumps(instance))

    with pytest.raises(AttributeError):
        _ = restored.y

    assert repr(restored) == "FrozenWithInit(x=1, y=<unset>)"


def test_a_self_referential_struct_round_trips_to_itself():
    node = Node()
    set_field(node, "child", node)

    restored = pickle.loads(pickle.dumps(node))

    assert restored.child is restored


def test_deepcopy_of_a_self_referential_struct_keeps_the_cycle():
    node = Node()
    set_field(node, "child", node)

    deep = copy.deepcopy(node)

    assert deep.child is deep


def test_zero_field_and_root_instances_round_trip_copy_and_deepcopy():
    for instance in (Empty(), Struct()):
        restored = pickle.loads(pickle.dumps(instance))

        assert type(restored) is type(instance)
        assert restored == instance
        assert copy.copy(instance) is not instance
        assert copy.deepcopy(instance) is not instance


def test_the_state_is_the_values_the_unset_names_and_the_dict():
    assert Plain(1, 2).__getstate__() == ({"x": 1, "y": 2}, (), None)

    instance = Mutable(1, 2)
    del instance.y

    assert instance.__getstate__() == ({"x": 1}, ("y",), None)


def test_an_unknown_name_in_the_state_is_refused():
    with pytest.raises(AttributeError, match="has no field"):
        Plain(1, 2).__setstate__(({"x": 1, "z": 2}, (), None))

    with pytest.raises(AttributeError, match="has no field"):
        Plain(1, 2).__setstate__(({}, ("z",), None))


def test_a_non_tuple_state_is_refused():
    with pytest.raises(TypeError):
        Plain(1, 2).__setstate__({"x": 1})


def test_a_malformed_state_is_refused():
    with pytest.raises(TypeError):
        Plain(1, 2).__setstate__((1, 2))


def test_getstate_takes_no_arguments():
    with pytest.raises(TypeError):
        Plain(1, 2).__getstate__(1)


def test_a_struct_holding_a_lambda_is_not_picklable():
    instance = Mutable(1, 2)
    instance.y = lambda: 0

    with pytest.raises((AttributeError, pickle.PicklingError)):
        pickle.dumps(instance)


def test_new_fills_defaults_and_leaves_required_unset():
    instance = Plain.__new__(Plain)

    assert instance.y == 0

    with pytest.raises(AttributeError):
        _ = instance.x


def test_new_works_for_a_subclass():
    instance = Child.__new__(Child)

    assert instance.y == 0
    assert instance.z == 3

    with pytest.raises(AttributeError):
        _ = instance.x


def test_new_refuses_a_non_subtype():
    with pytest.raises(TypeError):
        Plain.__new__(object)


def test_new_refuses_extra_arguments():
    with pytest.raises(TypeError):
        Plain.__new__(Plain, 1)


def test_new_refuses_keyword_arguments():
    with pytest.raises(TypeError, match="takes no keyword arguments"):
        Plain.__new__(Plain, x=5)


def test_object_new_of_a_struct_class_is_refused():
    with pytest.raises(TypeError, match="is not safe"):
        object.__new__(Plain)


def test_a_bad_state_is_refused_atomically():
    instance = Plain(10, 20)

    with pytest.raises(AttributeError, match="has no field"):
        instance.__setstate__(({"x": 1, "z": 2}, (), None))

    assert instance.x == 10
    assert instance.y == 20

    with pytest.raises(AttributeError, match="has no field"):
        instance.__setstate__(({}, ("y", "z"), None))

    assert instance.x == 10
    assert instance.y == 20


def test_a_field_listed_as_both_set_and_unset_is_refused():
    with pytest.raises(TypeError, match="both set and unset"):
        Plain(1, 2).__setstate__(({"x": 1}, ("x",), None))


def test_a_non_str_field_name_in_the_state_is_refused():
    with pytest.raises(TypeError, match="must be str"):
        Plain(1, 2).__setstate__(({1: "bad"}, (), None))

    with pytest.raises(TypeError, match="must be str"):
        Plain(1, 2).__setstate__(({}, (1,), None))


def test_the_instance_dict_round_trips_through_pickle_and_copy():
    instance = WithDict(1)
    instance.extra = "world"

    assert instance.__getstate__() == ({"x": 1}, (), {"extra": "world"})

    restored = pickle.loads(pickle.dumps(instance))
    assert restored.extra == "world"
    assert copy.copy(instance).extra == "world"
    assert copy.deepcopy(instance).extra == "world"


def test_setstate_restores_the_instance_dict_from_the_third_element():
    instance = WithDict(1)
    instance.__setstate__(({"x": 9}, (), {"extra": "world", 1: "entry"}))

    assert instance.x == 9
    assert instance.__dict__ == {"extra": "world", 1: "entry"}


def test_a_null_instance_dict_slot_round_trips():
    instance = WithDict(1)

    assert instance.__getstate__()[2] is None

    pickle.dumps(instance)

    assert instance.__getstate__()[2] is None

    restored = pickle.loads(pickle.dumps(instance))
    assert restored == instance
    assert restored.__dict__ == {}

    instance.__setstate__(({}, (), None))
    assert instance.__dict__ == {}


def test_a_dict_key_naming_a_field_is_preserved_in_the_instance_dict():
    instance = WithDict(1)
    instance.extra = "world"
    instance.__dict__["x"] = "shadow"

    assert instance.__getstate__() == ({"x": 1}, (), {"x": "shadow", "extra": "world"})

    restored = pickle.loads(pickle.dumps(instance))
    assert restored.x == 1
    assert restored.__dict__ == {"x": "shadow", "extra": "world"}
    assert copy.copy(instance).x == 1
    assert copy.deepcopy(instance).x == 1


def test_a_shadowing_dict_key_does_not_make_an_unset_field_refuse_the_state():
    instance = WithDict.__new__(WithDict)
    instance.__dict__["x"] = "shadow"
    instance.__dict__["extra"] = "world"

    assert instance.__getstate__() == ({}, ("x",), {"x": "shadow", "extra": "world"})

    restored = pickle.loads(pickle.dumps(instance))

    with pytest.raises(AttributeError):
        _ = restored.x

    assert restored.__dict__ == {"x": "shadow", "extra": "world"}


def test_setstate_replaces_the_instance_dict():
    instance = WithDict(1)
    instance.extra = "world"

    instance.__setstate__(instance.__getstate__())

    assert instance.__dict__ == {"extra": "world"}

    instance.__dict__["stale"] = 1
    instance.__setstate__(({"x": 5}, (), {"extra": "fresh"}))

    assert instance.x == 5
    assert instance.__dict__ == {"extra": "fresh"}


def test_type_call_arguments_on_a_bodyless_struct_are_refused():
    class TypeCallingMeta(type(Struct)):
        def __call__(cls, *args, **kwargs):
            return type.__call__(cls, *args, **kwargs)

    class Bodyless(Struct, metaclass=TypeCallingMeta):
        x: int
        y: int = 0

    assert Bodyless().y == 0

    with pytest.raises(TypeError, match="takes no arguments"):
        Bodyless(1, 2)


def test_type_call_arguments_are_still_passed_to_a_body_init():
    class TypeCallingMeta(type(Struct)):
        def __call__(cls, *args, **kwargs):
            return type.__call__(cls, *args, **kwargs)

    class WithBodyInit(Struct, metaclass=TypeCallingMeta, frozen=False):
        x: int

        def __init__(self, both: int) -> None:
            self.x = both

    instance = WithBodyInit(7)

    assert instance.x == 7


def test_a_body_new_on_a_base_struct_survives_on_its_subclass():
    instance = ChildOfBodyNew.__new__(ChildOfBodyNew)

    assert body_new_calls == ["with_body_new"]

    restored = pickle.loads(pickle.dumps(instance))

    assert body_new_calls == ["with_body_new", "with_body_new"]
    assert restored == instance


def test_a_recording_body_new_is_honored_by_construction_copy_and_pickle():
    instance = RecordingNew(7)

    assert body_new_calls == [("recording_new", 7)]

    copied = copy.copy(instance)
    restored = pickle.loads(pickle.dumps(instance))

    assert body_new_calls == [
        ("recording_new", 7),
        ("recording_new", 0),
        ("recording_new", 0),
    ]
    assert copied == instance
    assert restored == instance
    assert copied.x == 7
    assert restored.x == 7


def test_a_body_new_requiring_an_arg_receives_it_through_getnewargs():
    instance = ArgNew(7)

    assert body_new_calls == [("arg_new", 7)]

    copied = copy.copy(instance)
    restored = pickle.loads(pickle.dumps(instance))

    assert copied == instance
    assert restored == instance
    assert body_new_calls == [("arg_new", 7), ("arg_new", 7), ("arg_new", 7)]


def test_a_singleton_body_new_returns_the_same_instance_across_all_three():
    first = SingletonNew()
    copied = copy.copy(first)
    restored = pickle.loads(pickle.dumps(first))

    assert copied is first
    assert restored is first


def test_a_body_new_raising_typeerror_genuinely_propagates():
    instance = RejectingNew()
    RejectingNew.reject = True

    with pytest.raises(TypeError, match="genuine rejection"):
        RejectingNew()
    with pytest.raises(TypeError, match="genuine rejection"):
        copy.copy(instance)
    with pytest.raises(TypeError, match="genuine rejection"):
        pickle.loads(pickle.dumps(instance))

    RejectingNew.reject = False


def test_a_body_new_raising_typeerror_on_reconstruction_propagates():
    instance = DeclinesForReconstruction()

    with pytest.raises(TypeError, match="declined for reconstruction"):
        copy.copy(instance)
    with pytest.raises(TypeError, match="declined for reconstruction"):
        pickle.loads(pickle.dumps(instance))


def test_a_class_declaring_getnewargs_round_trips_copy_and_pickle():
    instance = WithGetNewArgs(5)

    assert copy.copy(instance) == instance
    assert pickle.loads(pickle.dumps(instance)) == instance

    with pytest.raises(TypeError, match="takes no keyword arguments"):
        WithGetNewArgs.__new__(WithGetNewArgs, 5, x=5)


def test_a_direct_posonly_new_call_on_a_getnewargs_struct_is_reconstruction_capable():
    instance = WithGetNewArgs(5)

    assert copy.copy(instance) == instance


def test_a_bodyless_struct_keeps_the_plain_new_path():
    instance = Plain(1, 2)

    assert copy.copy(instance) == instance
    assert pickle.loads(pickle.dumps(instance)) == instance

    with pytest.raises(TypeError):
        Plain.__new__(Plain, 1)


def test_setstate_with_the_own_dict_as_third_element_is_a_no_op():
    instance = WithDict(1)
    instance.extra = "world"

    instance.__setstate__(({"x": 9}, (), instance.__dict__))

    assert instance.x == 9
    assert instance.__dict__ == {"extra": "world"}


def test_setstate_dict_restore_is_atomic_on_a_failed_merge():
    class Evil:
        n = 0

        def __init__(self, name):
            self.name = name

        def __hash__(self):
            return 42 if self.name != "tmp" else 7

        def __eq__(self, other):
            Evil.n += 1
            if Evil.n > 1:
                raise RuntimeError(f"eq failed on call {Evil.n}")
            return self is other

    e1 = Evil("a")
    e2 = Evil("b")
    tmp = Evil("tmp")
    state_dict = {e1: 1, e2: 2, tmp: 3}
    del state_dict[tmp]

    instance = WithDict(1)
    instance.extra = "old"
    instance.__dict__["keep"] = "this"

    with pytest.raises(RuntimeError, match="eq failed"):
        instance.__setstate__(({"x": 2}, (), state_dict))

    assert instance.__dict__ == {"extra": "old", "keep": "this"}
    assert instance.x == 2


@pytest.mark.parametrize("protocol", [2, 3])
def test_protocols_2_and_3_round_trip_a_getnewargs_ex_struct(protocol):
    instance = WithGetNewArgsEx(5)
    restored = pickle.loads(pickle.dumps(instance, protocol=protocol))

    assert restored == instance
    assert restored.x == 5


@pytest.mark.parametrize("protocol", [2, 3])
def test_protocols_2_and_3_round_trip_a_keyword_only_getnewargs_ex_struct(protocol):
    instance = WithGetNewArgsExKeywordOnly(5)
    restored = pickle.loads(pickle.dumps(instance, protocol=protocol))

    assert restored == instance
    assert restored.x == 5


def test_a_direct_keyword_new_call_on_a_bodyless_struct_is_refused():
    with pytest.raises(TypeError, match="takes no keyword arguments"):
        Plain.__new__(Plain, x=1)


def test_a_direct_keyword_new_call_on_a_getnewargs_struct_is_refused():
    with pytest.raises(TypeError, match="takes no keyword arguments"):
        WithGetNewArgs.__new__(WithGetNewArgs, x=1)


def test_a_genuine_body_typeerror_propagates_with_getnewargs_under_construction():
    with pytest.raises(TypeError, match="genuine rejection"):
        GenuineWithGNA(5)


def test_a_body_new_raising_a_non_typeerror_propagates_on_reconstruction():
    instance = ValueErrorNew()
    with pytest.raises(ValueError, match="genuine rejection"):
        copy.copy(instance)
    with pytest.raises(ValueError, match="genuine rejection"):
        pickle.loads(pickle.dumps(instance))


def test_a_reduced_body_new_typeerror_propagates_on_every_reconstruction_path():
    instance = DeclinesWithReduce()

    for func in (copy.copy, copy.deepcopy):
        with pytest.raises(TypeError, match="declined for reconstruction"):
            func(instance)

    for protocol in (2, 3, 4, 5):
        with pytest.raises(TypeError, match="declined for reconstruction"):
            pickle.loads(pickle.dumps(instance, protocol=protocol))


def test_a_reduced_body_new_without_getnewargs_propagates_consistently():
    instance = DeclinesWithReduceNoGNA()

    for func in (copy.copy, copy.deepcopy):
        with pytest.raises(TypeError, match="declined for reconstruction"):
            func(instance)

    for protocol in (2, 3, 4, 5):
        with pytest.raises(TypeError, match="declined for reconstruction"):
            pickle.loads(pickle.dumps(instance, protocol=protocol))


def test_a_custom_reduce_controls_copy_and_deepcopy():
    instance = WithReduce()

    copied = copy.copy(instance)
    deep = copy.deepcopy(instance)

    assert reduce_calls == ["custom", "custom"]
    assert copied == instance
    assert deep == instance


def test_setstate_swaps_the_instance_dict_for_atomicity():
    instance = WithDict(1)
    instance.extra = "world"

    instance.__setstate__(({"x": 2}, (), {"new": "val"}))

    assert instance.__dict__ == {"new": "val"}


def test_setstate_with_none_third_element_clears_the_instance_dict():
    instance = WithDict(1)
    instance.extra = "world"

    instance.__setstate__(({}, (), None))

    assert instance.__dict__ == {}


def test_a_co_base_earlier_in_the_mro_supplies_the_body_new():
    body_new_calls.clear()
    instance = SubWithCoBaseNew.__new__(SubWithCoBaseNew)

    assert body_new_calls == ["co_base_new", "body_base_new"]
    assert instance.y == 1


def test_a_subclass_inherits_a_user_copy_instead_of_being_shadowed():
    instance = SubOfReduceCopyBase()

    assert copy.copy(instance) == "user_copy"


def test_a_bodyless_struct_gaining_getnewargs_after_creation_still_copies():
    instance = Plain(1, 2)

    assert copy.copy(instance) == instance

    try:
        Plain.__getnewargs__ = lambda self: (self.x, self.y)

        assert copy.copy(instance) == instance
        assert copy.deepcopy(instance) == instance
    finally:
        del Plain.__getnewargs__


def test_a_getattr_returning_none_does_not_break_copy():
    class GetattrNone(Struct):
        x: int

        def __getattr__(self, name):
            return None

    instance = GetattrNone(5)

    assert copy.copy(instance) == instance
    assert copy.deepcopy(instance) == instance


def test_a_declaring_getnewargs_getattr_returning_none_copies_without_error():
    class DeclaringGetattrNone(Struct):
        x: int

        def __getnewargs__(self):
            return (self.x,)

        def __getattr__(self, name):
            return None

    instance = DeclaringGetattrNone(5)

    assert copy.copy(instance) == instance
    assert copy.deepcopy(instance) == instance


def test_copyreg_dispatch_table_is_honored_by_copy_and_deepcopy():
    class Registered(Struct):
        x: int

    def custom_copier(obj):
        return (lambda v: ("copied", v), (obj.x,), None)

    try:
        copyreg.dispatch_table[Registered] = custom_copier
        assert copy.copy(Registered(5)) == ("copied", 5)
        assert copy.deepcopy(Registered(5)) == ("copied", 5)
    finally:
        del copyreg.dispatch_table[Registered]


def test_a_reduce_owning_class_keeps_copy_through_the_reduce():
    class ReduceOwned(Struct):
        x: int = 0

        def __reduce__(self):
            return (type(self), (), self.__getstate__())

    assert "__copy__" not in ReduceOwned.__dict__
    assert hasattr(ReduceOwned, "__copy__") is False

    assert copy.copy(ReduceOwned(5)) == ReduceOwned(5)


def test_a_getattr_returning_a_value_is_never_called_by_deepcopy():
    calls = []

    class GetattrValue(Struct):
        x: int

        def __getattr__(self, name):
            calls.append(name)
            return 123

    instance = GetattrValue(5)

    deep = copy.deepcopy(instance)

    assert deep == instance
    assert calls == []


def test_setstate_refuses_a_duplicate_unset_name():
    class P(Struct):
        alpha: int
        beta: int

    instance = P(1, 2)

    with pytest.raises(TypeError, match="listed more than once as unset"):
        instance.__setstate__(({"alpha": 5}, ("beta", "beta"), None))

    assert instance.alpha == 1
    assert instance.beta == 2


def test_setstate_replaces_the_instance_dict_for_atomicity():
    """The dict-restore builds the replacement up front and swaps it in only on
    success, so a failing merge leaves the instance untouched. The swap replaces
    the dict object; an external reference to the previous dict goes stale,
    which is the accepted cost of atomicity. The None branch clears the existing
    dict in place.
    """

    instance = WithDict(1)
    instance.extra = "world"
    reference = instance.__dict__

    instance.__setstate__(({"x": 2}, (), {"new": "val"}))

    assert instance.__dict__ == {"new": "val"}
    assert reference == {"extra": "world"}

    instance.__setstate__(({}, (), None))

    assert instance.__dict__ == {}

@pytest.mark.parametrize("protocol", [0, 1])
def test_protocols_0_and_1_are_deliberately_refused(protocol):
    """The old protocols restore through copyreg._reconstructor, whose
    object.__new__(cls) a struct refuses; a pickle that cannot be written by
    them is a limitation that is deliberate rather than silent."""

    with pytest.raises(TypeError):
        pickle.dumps(Plain(1, 2), protocol=protocol)


def test_deepcopy_of_a_string_reduce_returns_the_object_unchanged():
    """stdlib copy.deepcopy handles a string reduce result by returning the
    object itself; Struct_deepcopy must match, so deepcopy and copy agree.
    """

    class StrReduce(Struct):
        x: int

        def __reduce__(self):
            return "salix.StrReduce"

    instance = StrReduce(1)

    import copy

    assert copy.deepcopy(instance) is instance
    assert copy.copy(instance) is instance


def test_deepcopy_falls_back_to_reduce_when_reduce_ex_is_none():
    """A class that sets __reduce_ex__ = None routes reduction through
    __reduce__, matching stdlib copy.deepcopy's getattr(x, "__reduce_ex__",
    None) treating None as absence. copy.copy works because it has no
    __copy__; deepcopy must agree.
    """

    class ReduceExNone(Struct):
        x: int

        __reduce_ex__ = None

        def __reduce__(self):
            return (ReduceExNone, (self.x,))

    instance = ReduceExNone(3)

    assert copy.deepcopy(instance) == instance
    assert copy.copy(instance) == instance
