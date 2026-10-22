import copy
import dataclasses
import inspect
import pickle
from dataclasses import KW_ONLY, InitVar
from typing import ClassVar

import pytest

import salix
from salix import Struct, field


def parameters_of(cls: type) -> list[str]:
    return [str(parameter) for parameter in inspect.signature(cls).parameters.values()]


def keyword_only_flags(cls: type) -> list[bool]:
    return [record.kw_only for record in salix.fields(cls)]


def stock_keyword_only_flags(cls: type) -> list[bool]:
    return [record.kw_only for record in dataclasses.fields(cls)]


class Flagged(Struct, kw_only=True):
    x: int
    y: int = 2


@dataclasses.dataclass(kw_only=True)
class StockFlagged:
    x: int
    y: int = 2


class Marked(Struct, frozen=False):
    x: int = 1
    _: KW_ONLY
    y: int
    z: int = 3


@dataclasses.dataclass
class StockMarked:
    x: int = 1
    _: KW_ONLY
    y: int
    z: int = 3


class Specified(Struct):
    x: int = 1
    y: int = field(kw_only=True)
    z: int = field(default=9)


@dataclasses.dataclass
class StockSpecified:
    x: int = 1
    y: int = dataclasses.field(kw_only=True)
    z: int = dataclasses.field(default=9)


@pytest.mark.parametrize(
    ("salix_class", "stock_class"),
    [(Flagged, StockFlagged), (Marked, StockMarked), (Specified, StockSpecified)],
    ids=["class-keyword", "marker", "field"],
)
def test_each_spelling_matches_its_stock_twin(salix_class: type, stock_class: type) -> None:
    assert parameters_of(salix_class) == parameters_of(stock_class)
    assert salix_class.__match_args__ == stock_class.__match_args__
    assert keyword_only_flags(salix_class) == stock_keyword_only_flags(stock_class)
    assert salix_class._struct_fields_ == tuple(f.name for f in dataclasses.fields(stock_class))


def test_a_keyword_only_field_is_bound_by_name_only() -> None:
    assert (Flagged(x=1).x, Flagged(x=1, y=3).y) == (1, 3)
    assert (Marked(5, y=2, z=4).x, Marked(y=2).z) == (5, 3)
    assert (Specified(4, 5, y=2).z, Specified(y=2).x) == (5, 1)

    with pytest.raises(TypeError, match=r"Flagged\(\) takes at most 0 positional arguments but 1 were given"):
        Flagged(1)

    with pytest.raises(TypeError, match=r"Marked\(\) takes at most 1 positional arguments but 2 were given"):
        Marked(1, 2)


def test_a_missing_keyword_only_field_is_named_as_one() -> None:
    with pytest.raises(TypeError, match="Flagged\\(\\) missing required keyword-only argument 'x'"):
        Flagged()

    with pytest.raises(TypeError, match="Marked\\(\\) missing required keyword-only argument 'y'"):
        Marked(1)


def test_a_required_keyword_only_field_may_follow_a_default() -> None:
    assert Marked(y=2) == Marked(1, y=2, z=3)
    assert repr(Marked(y=2)) == "Marked(x=1, y=2, z=3)"


def test_the_defaults_stay_aligned_to_the_trailing_fields() -> None:
    assert Marked._struct_fields_ == ("x", "y", "z")
    assert Marked.__struct_defaults__ == (1, salix.MISSING, 3)
    assert Flagged.__struct_defaults__ == (2,)
    assert Specified.__struct_defaults__ == (1, salix.MISSING, 9)


def test_the_ordering_rule_still_holds_among_the_positional_fields() -> None:
    with pytest.raises(TypeError, match="non-default field 'b' follows a field with a default"):

        class Misordered(Struct):
            a: int = 1
            _: KW_ONLY
            c: int
            b: int = field(kw_only=False)


def test_an_explicit_choice_overrides_the_class_keyword() -> None:
    class Mixed(Struct, kw_only=True):
        x: int
        y: int = field(kw_only=False, default=0)

    @dataclasses.dataclass(kw_only=True)
    class StockMixed:
        x: int
        y: int = dataclasses.field(kw_only=False, default=0)

    assert parameters_of(Mixed) == parameters_of(StockMixed)
    assert keyword_only_flags(Mixed) == stock_keyword_only_flags(StockMixed) == [True, False]


def test_a_keyword_only_base_puts_its_fields_after_a_positional_subclass_s() -> None:
    class Base(Struct, kw_only=True):
        a: int

    class Sub(Base):
        b: int

    @dataclasses.dataclass(kw_only=True)
    class StockBase:
        a: int

    @dataclasses.dataclass
    class StockSub(StockBase):
        b: int

    assert parameters_of(Sub) == parameters_of(StockSub)
    assert Sub._struct_fields_ == ("a", "b")
    assert repr(Sub(2, a=1)).endswith("Sub(a=1, b=2)")
    assert Sub.__match_args__ == StockSub.__match_args__ == ("b",)


def test_the_class_keyword_is_not_inherited() -> None:
    class Base(Struct, kw_only=True):
        a: int = 1

    class Sub(Base):
        b: int

    assert keyword_only_flags(Sub) == [True, False]
    assert Sub(2).b == 2


def test_a_redeclared_field_takes_the_subclass_s_choice() -> None:
    class Base(Struct):
        a: int
        b: int

    class Sub(Base):
        _: KW_ONLY
        a: int

    @dataclasses.dataclass
    class StockBase:
        a: int
        b: int

    @dataclasses.dataclass
    class StockSub(StockBase):
        _: KW_ONLY
        a: int

    assert parameters_of(Sub) == parameters_of(StockSub)
    assert keyword_only_flags(Sub) == stock_keyword_only_flags(StockSub) == [True, False]


def test_field_without_a_default_drops_an_inherited_one() -> None:
    class Base(Struct):
        x: int = 1

    class Sub(Base):
        x: int = field(kw_only=True)

    @dataclasses.dataclass
    class StockBase:
        x: int = 1

    @dataclasses.dataclass
    class StockSub(StockBase):
        x: int = dataclasses.field(kw_only=True)

    assert salix.fields(Sub)[0].default is salix.MISSING
    assert dataclasses.fields(StockSub)[0].default is dataclasses.MISSING

    with pytest.raises(TypeError, match="missing required keyword-only argument 'x'"):
        Sub()


def test_a_second_marker_is_refused_as_stock_refuses_it() -> None:
    with pytest.raises(TypeError) as salix_error:

        class Twice(Struct):
            a: int
            _: KW_ONLY
            b: int
            __: KW_ONLY
            c: int

    with pytest.raises(TypeError) as stock_error:

        @dataclasses.dataclass
        class StockTwice:
            a: int
            _: KW_ONLY
            b: int
            __: KW_ONLY
            c: int

    assert str(salix_error.value) == str(stock_error.value)


def test_the_marker_is_read_from_a_string_annotation() -> None:
    namespace: dict[str, object] = {}
    exec(
        "from __future__ import annotations\n"
        "from dataclasses import KW_ONLY\n"
        "from salix import Struct\n"
        "class Deferred(Struct):\n"
        "    a: int\n"
        "    _: KW_ONLY\n"
        "    b: int\n",
        namespace,
    )
    deferred = namespace["Deferred"]

    assert isinstance(deferred, type)
    assert keyword_only_flags(deferred) == [False, True]
    assert str(inspect.signature(deferred)) == "(a: 'int', *, b: 'int')"


def test_a_keyword_only_init_var_reaches_post_init() -> None:
    class Scaled(Struct, frozen=False):
        x: int
        _: KW_ONLY
        scale: InitVar[int] = 1

        def __post_init__(self, scale: int) -> None:
            self.x = self.x * scale

    @dataclasses.dataclass
    class StockScaled:
        x: int
        _: KW_ONLY
        scale: InitVar[int] = 1

        def __post_init__(self, scale: int) -> None:
            self.x = self.x * scale

    assert parameters_of(Scaled) == parameters_of(StockScaled)
    assert Scaled(2, scale=3).x == StockScaled(2, scale=3).x == 6
    assert Scaled.__match_args__ == StockScaled.__match_args__ == ("x",)


def test_field_on_an_init_var_leaves_its_default_as_the_class_attribute() -> None:
    class Defaulted(Struct):
        x: int
        scale: InitVar[int] = field(default=2, kw_only=True)

        def __post_init__(self, scale: int) -> None:
            pass

    class Required(Struct):
        x: int
        scale: InitVar[int] = field(kw_only=True)

        def __post_init__(self, scale: int) -> None:
            pass

    assert Defaulted.__dict__["scale"] == 2
    assert "scale" not in Required.__dict__

    with pytest.raises(TypeError, match="missing required keyword-only argument 'scale'"):
        Required(1)


def test_field_takes_only_default_and_kw_only() -> None:
    with pytest.raises(TypeError, match="salix does not support 'default_factory'"):
        field(default_factory=list)  # type: ignore[call-overload]

    with pytest.raises(TypeError, match="keyword arguments only"):
        field(1)  # type: ignore[call-overload]


def test_field_is_refused_where_it_cannot_describe_a_field() -> None:
    with pytest.raises(TypeError, match="'limit' is annotated ClassVar, and field\\(\\) describes a field"):

        class Constant(Struct):
            limit: ClassVar[int] = field(default=1)

    with pytest.raises(TypeError, match="'y' is bound to field\\(\\), so it needs an annotation"):

        class Unannotated(Struct):
            x: int
            y = field(default=1)


def test_a_kw_only_marker_bound_to_field_keeps_the_value_as_stock_does() -> None:
    bound = field(default=5)

    class Bound(Struct):
        a: int
        _: KW_ONLY = bound
        b: int

    @dataclasses.dataclass
    class StockBound:
        a: int
        _: KW_ONLY = dataclasses.field(default=5)
        b: int

    def kinds(cls: type) -> list[tuple[str, inspect._ParameterKind]]:
        return [(parameter.name, parameter.kind) for parameter in inspect.signature(cls).parameters.values()]

    assert kinds(Bound) == kinds(StockBound)
    assert vars(Bound)["_"] is bound
    assert isinstance(vars(StockBound)["_"], dataclasses.Field)


def test_field_rebinding_an_inherited_field_needs_its_annotation() -> None:
    class Base(Struct):
        x: int

    with pytest.raises(TypeError, match=r"'x' is a field that 'Sub' inherits.*without an annotation"):

        class Sub(Base):
            x = field(kw_only=True)


def test_an_exception_struct_refuses_a_keyword_only_field() -> None:
    with pytest.raises(TypeError, match="an exception struct cannot take keyword-only field 'detail'"):

        class Failure(Struct, Exception):
            code: int
            _: KW_ONLY
            detail: str


@pytest.mark.parametrize(
    ("instance", "changes"),
    [(Flagged(x=1), {"y": 5}), (Marked(5, y=2), {"y": 7}), (Specified(4, y=2), {"y": 6})],
    ids=["class-keyword", "marker", "field"],
)
def test_a_keyword_only_struct_round_trips_through_every_duplicate(
    instance: Struct, changes: dict[str, int]
) -> None:
    replaced = salix.replace(instance, **changes)

    assert [getattr(replaced, name) for name in changes] == list(changes.values())
    assert copy.copy(instance) == instance
    assert copy.deepcopy(instance) == instance
    assert pickle.loads(pickle.dumps(instance)) == instance
    assert salix.from_mapping(type(instance), salix.to_dict(instance)) == instance



def test_a_metaclass_handoff_keeps_the_class_keyword() -> None:
    class Meta(type(Struct)):
        def __new__(mcs, name: str, bases: tuple[type, ...], namespace: dict[str, object], **keywords: object) -> type:
            return super().__new__(mcs, name, bases, namespace, **keywords)

    class HandedOff(Struct, metaclass=Meta, kw_only=True):
        x: int

    assert keyword_only_flags(HandedOff) == [True]
    assert str(inspect.signature(HandedOff)) == "(*, x: int)"
