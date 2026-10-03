import copy
import inspect
import sys
import types
from dataclasses import InitVar, dataclass
from typing import ClassVar

import pytest

import salix
from salix import Struct, set_field

META = type(Struct)


def parameters_of(cls: type) -> list[str]:
    return [str(parameter) for parameter in inspect.signature(cls).parameters.values()]


@dataclass
class StockSeeded:
    a: int
    flag: InitVar[int]
    b: int = 2
    scale: InitVar[int] = 10

    def __post_init__(self, flag: int, scale: int) -> None:
        self.b = self.b + flag * scale


class Seeded(Struct, frozen=False):
    a: int
    flag: InitVar[int]
    b: int = 2
    scale: InitVar[int] = 10

    def __post_init__(self, flag: int, scale: int) -> None:
        self.b = self.b + flag * scale


def test_an_init_var_is_a_parameter_and_not_a_field() -> None:
    assert Seeded._struct_fields_ == ("a", "b")
    assert parameters_of(Seeded) == parameters_of(StockSeeded)
    assert Seeded.__match_args__ == StockSeeded.__match_args__ == ("a", "flag", "b", "scale")


def test_init_vars_bind_by_position_and_keyword_between_the_fields() -> None:
    assert Seeded(1, 5, 3, 7).b == StockSeeded(1, 5, 3, 7).b == 38
    assert Seeded(a=1, flag=5).b == StockSeeded(a=1, flag=5).b == 52
    assert Seeded(1, scale=0, flag=4).b == 2


def test_an_init_var_is_stored_nowhere() -> None:
    seeded = Seeded(1, 5)

    assert repr(seeded) == "Seeded(a=1, b=52)"
    assert seeded == Seeded(1, 0, 52, 0)
    assert seeded.scale == Seeded.scale == StockSeeded.scale == 10
    assert "flag" not in vars(Seeded)
    assert "flag" in Seeded.__annotations__

    with pytest.raises(AttributeError):
        _ = seeded.flag


def test_a_missing_init_var_is_a_missing_argument() -> None:
    with pytest.raises(TypeError, match="missing required argument 'flag'"):
        Seeded(1)


def test_the_first_missing_argument_is_named_in_parameter_order() -> None:
    class FlagFirst(Struct):
        flag: InitVar[int]
        a: int

    with pytest.raises(TypeError, match="missing required argument 'flag'"):
        FlagFirst()

    with pytest.raises(TypeError, match="missing required argument 'flag'"):
        salix.from_mapping(FlagFirst, {})

    with pytest.raises(TypeError, match="missing required argument 'a'"):
        FlagFirst(flag=1)


@pytest.mark.parametrize(
    ("arguments", "keywords", "message"),
    [
        ((1, 5), {"flag": 6}, "multiple values for argument 'flag'"),
        ((1, 5), {"seed": 6}, "unexpected keyword argument 'seed'"),
        ((1, 5, 3, 7, 9), {}, "takes at most 4 positional arguments but 5 were given"),
    ],
    ids=["twice", "unknown", "too-many"],
)
def test_init_var_binding_errors_name_the_argument(
    arguments: tuple[int, ...],
    keywords: dict[str, int],
    message: str,
) -> None:
    with pytest.raises(TypeError, match=message):
        Seeded(*arguments, **keywords)


def test_init_vars_without_a_post_init_are_taken_and_dropped() -> None:
    class Unused(Struct):
        a: int
        flag: InitVar[int] = 3

    assert Unused._struct_fields_ == ("a",)
    assert Unused(1, 4).a == 1
    assert Unused(1, 4) == Unused(1)


def test_a_post_init_of_the_wrong_arity_fails_at_construction() -> None:
    class WrongArity(Struct):
        a: int
        flag: InitVar[int] = 3

        def __post_init__(self) -> None:
            pass

    with pytest.raises(TypeError, match="positional argument"):
        WrongArity(1)


def test_from_mapping_takes_init_vars_by_name() -> None:
    assert salix.from_mapping(Seeded, {"a": 1, "flag": 2, "scale": 3}).b == 8
    assert salix.from_mapping(Seeded, types.MappingProxyType({"a": 1, "flag": 2})).b == 22

    with pytest.raises(TypeError, match="missing required argument 'flag'"):
        salix.from_mapping(Seeded, {"a": 1})


def test_replace_requires_an_undefaulted_init_var() -> None:
    error = TypeError if sys.version_info >= (3, 13) else ValueError

    with pytest.raises(error, match="InitVar 'flag' must be specified with replace"):
        salix.replace(Seeded(1, 5), a=2)

    assert salix.replace(Seeded(1, 0), flag=1).b == 12
    assert salix.replace(Seeded(1, 0), a=3, flag=1, scale=0).a == 3


def test_replace_on_a_frozen_struct_still_requires_the_init_var() -> None:
    class Frozen(Struct):
        a: int
        flag: InitVar[int]

    error = TypeError if sys.version_info >= (3, 13) else ValueError

    with pytest.raises(error, match="InitVar 'flag' must be specified with replace"):
        salix.replace(Frozen(1, 2))


def test_replace_on_a_struct_with_its_own_init_passes_init_vars_to_that_init() -> None:
    class Custom(Struct):
        a: int
        flag: InitVar[int] = 0

        def __init__(self, a: int, flag: int = 0) -> None:
            set_field(self, "a", a + flag)

    assert salix.replace(Custom(1), flag=5).a == 6


def test_copies_do_not_rerun_post_init() -> None:
    seeded = Seeded(1, 5)

    assert copy.copy(seeded).b == copy.deepcopy(seeded).b == 52


def test_a_frozen_struct_derives_a_field_from_an_init_var_with_set_field() -> None:
    class Scaled(Struct):
        value: int
        factor: InitVar[int] = 1

        def __post_init__(self, factor: int) -> None:
            set_field(self, "value", self.value * factor)

    assert Scaled(3, 4).value == 12


def test_an_init_var_default_reaches_post_init_as_a_fresh_copy() -> None:
    seen: list[list[int]] = []

    class Collecting(Struct):
        items: InitVar[list[int]] = [1]  # noqa: RUF012

        def __post_init__(self, items: list[int]) -> None:
            seen.append(items)

    Collecting()
    Collecting()

    assert seen == [[1], [1]]
    assert seen[0] is not seen[1]


def test_a_bare_init_var_is_an_init_var() -> None:
    class Bare(Struct):
        flag: InitVar

    assert Bare._struct_fields_ == ()
    assert Bare(flag=1) == Bare(flag=2)


@pytest.mark.parametrize("text", ["InitVar[int]", "InitVar", "dataclasses.InitVar[int]"])
def test_the_source_text_form_is_an_init_var(text: str) -> None:
    Textual = META("Textual", (Struct,), {"__annotations__": {"v": text, "a": "int"}})

    assert Textual._struct_fields_ == ("a",)
    assert list(inspect.signature(Textual).parameters) == ["v", "a"]


class TestInheritance:
    def test_a_subclass_inherits_init_vars_in_parameter_order(self) -> None:
        @dataclass
        class StockParent:
            a: int
            p: InitVar[int] = 1

        @dataclass
        class StockChild(StockParent):
            c: int = 0
            q: InitVar[int] = 2

            def __post_init__(self, p: int, q: int) -> None:
                self.c = p * 10 + q

        class Parent(Struct, frozen=False):
            a: int
            p: InitVar[int] = 1

        class Child(Parent):
            c: int = 0
            q: InitVar[int] = 2

            def __post_init__(self, p: int, q: int) -> None:
                self.c = p * 10 + q

        assert parameters_of(Child) == parameters_of(StockChild)
        assert Child(1, 5, 6, 7).c == StockChild(1, 5, 6, 7).c == 57

    def test_an_inherited_post_init_takes_the_inherited_init_vars(self) -> None:
        class Parent(Struct, frozen=False):
            a: int
            p: InitVar[int] = 1

            def __post_init__(self, p: int) -> None:
                self.a = self.a + p

        class Child(Parent):
            c: int = 0

        assert Child(1, 5, 6) == Child(6, 0, 6)

    def test_an_init_var_over_an_inherited_field_takes_the_field_away(self) -> None:
        @dataclass
        class StockBase:
            x: int = 1

        @dataclass
        class StockOver(StockBase):
            x: InitVar[int] = 7

        class Base(Struct):
            x: int = 1

        class Over(Base):
            x: InitVar[int] = 7

        assert Over._struct_fields_ == ()
        assert parameters_of(Over) == parameters_of(StockOver)
        assert Over(3).x == StockOver(3).x == 7

    def test_a_field_over_an_inherited_init_var_keeps_its_position(self) -> None:
        class Parent(Struct):
            a: int
            p: InitVar[int] = 1
            b: int = 2

        class Child(Parent):
            p: int = 9

        assert Child._struct_fields_ == ("a", "p", "b")
        assert list(inspect.signature(Child).parameters) == ["a", "p", "b"]
        assert (Child(1, 5).a, Child(1, 5).p, Child(1, 5).b) == (1, 5, 2)
        assert Child(1).p == 9

    def test_a_class_var_over_an_inherited_init_var_takes_the_init_var_away(self) -> None:
        class Parent(Struct):
            a: int = 0
            p: InitVar[int] = 1

        class Child(Parent):
            p: ClassVar[int] = 5

        assert Child._struct_fields_ == ("a",)
        assert list(inspect.signature(Child).parameters) == ["a"]
        assert Child.__match_args__ == ("a",)
        assert Child.p == 5

        with pytest.raises(TypeError, match="without an assigned value"):

            class Bare(Parent):
                p: ClassVar[int]

    def test_an_init_var_re_added_after_a_class_var_removed_it_keeps_its_position(self) -> None:
        @dataclass
        class StockA:
            x: int = 1
            y: int = 2

        @dataclass
        class StockB(StockA):
            x: ClassVar[int] = 5

        @dataclass
        class StockD(StockB):
            x: InitVar[int] = 9

        class A(Struct):
            x: int = 1
            y: int = 2

        class B(A):
            x: ClassVar[int] = 5

        class D(B):
            x: InitVar[int] = 9

        assert parameters_of(D) == parameters_of(StockD)
        assert D._struct_fields_ == ("y",)

    def test_a_struct_base_with_only_init_vars_is_the_one_inherited_from(self) -> None:
        class Mixin(Struct):
            pass

        class OnlyInitVars(Struct):
            seed: InitVar[int]

        class Combined(Mixin, OnlyInitVars):
            pass

        assert list(inspect.signature(Combined).parameters) == ["seed"]

    def test_init_vars_a_second_struct_base_would_drop_are_refused(self) -> None:
        class First(Struct):
            first: InitVar[int]

        class Second(Struct):
            second: InitVar[int]

        with pytest.raises(TypeError, match=r"InitVars of .*Second would be dropped"):

            class Combined(First, Second):
                pass

    def test_a_fielded_base_beside_an_init_var_base_is_refused_in_either_order(self) -> None:
        class Seeds(Struct):
            seed: InitVar[int] = 1

        class Fielded(Struct):
            field: int = 5

        with pytest.raises(TypeError, match=r"fields and InitVars of .*Fielded would be dropped"):

            class SeedsFirst(Seeds, Fielded):
                pass

        with pytest.raises(TypeError, match=r"fields and InitVars of .*Seeds would be dropped"):

            class FieldedFirst(Fielded, Seeds):
                pass

    def test_a_diamond_whose_second_base_adds_nothing_builds(self) -> None:
        @dataclass
        class StockCommon:
            a: int = 0

        @dataclass
        class StockSeeded(StockCommon):
            seed: InitVar[int] = 1

        @dataclass
        class StockPlain(StockCommon):
            pass

        @dataclass
        class StockCombined(StockSeeded, StockPlain):
            pass

        seen: list[int] = []

        class Common(Struct):
            a: int = 0

        class Seeded(Common):
            seed: InitVar[int] = 1

            def __post_init__(self, seed: int) -> None:
                seen.append(seed)

        class Plain(Common):
            pass

        class Combined(Seeded, Plain):
            pass

        assert parameters_of(Combined) == parameters_of(StockCombined)
        assert Combined._struct_fields_ == ("a",)
        assert Combined(4, 5).a == 4
        assert seen == [5]

    def test_a_diamond_over_a_mutable_default_builds(self) -> None:
        class Common(Struct):
            items: list[int] = [1]  # noqa: RUF012

        class Seeded(Common):
            seed: InitVar[int] = 1

        class Plain(Common):
            pass

        class Combined(Seeded, Plain):
            pass

        assert Combined().items == [1]

    def test_a_diamond_whose_chosen_base_takes_a_parameter_away_is_refused(self) -> None:
        class Common(Struct):
            a: int = 0

        class Seeded(Common):
            a: ClassVar[int] = 5
            seed: InitVar[int] = 1

        class Plain(Common):
            pass

        with pytest.raises(TypeError, match=r"InitVars of .*Plain would be dropped"):

            class Combined(Seeded, Plain):
                pass

    def test_a_diamond_whose_second_base_reannotates_a_field_is_refused(self) -> None:
        class Common(Struct):
            a: int = 0

        class Seeded(Common):
            seed: InitVar[int] = 1

        class Reannotating(Common):
            a: float = 0

        with pytest.raises(TypeError, match=r"InitVars of .*Reannotating would be dropped"):

            class Combined(Seeded, Reannotating):
                pass

    def test_a_diamond_whose_second_base_redefaults_a_field_is_refused(self) -> None:
        class Common(Struct):
            a: int = 0

        class Seeded(Common):
            seed: InitVar[int] = 1

        class Redefaulting(Common):
            a: int = 5

        with pytest.raises(TypeError, match=r"InitVars of .*Redefaulting would be dropped"):

            class Combined(Seeded, Redefaulting):
                pass

    def test_a_delegate_that_drops_a_field_beside_an_init_var_is_refused(self) -> None:
        class FieldDropping(META):
            def __new__(
                metacls: type,
                name: str,
                bases: tuple[type, ...],
                namespace: dict[str, object],
                **keywords: object,
            ) -> type:
                annotations = namespace.get("__annotations__", {})
                kept = {key: value for key, value in annotations.items() if key != "b"}
                slots = tuple(slot for slot in namespace.get("__slots__", ()) if slot != "b")

                return super().__new__(
                    metacls,
                    name,
                    bases,
                    {**namespace, "__annotations__": kept, "__slots__": slots},
                    **keywords,
                )

        class Base(Struct, metaclass=FieldDropping):
            a: int

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"b": int, "flag": InitVar[int]}})

    def test_the_handoff_to_a_derived_metatype_keeps_the_init_vars(self) -> None:
        class Delegating(META):
            pass

        class Base(Struct, metaclass=Delegating):
            a: int

        Built = META(
            "Built",
            (Base,),
            {
                "__annotations__": {"flag": InitVar[int]},
                "__post_init__": lambda self, flag: set_field(self, "a", self.a + flag),
            },
        )

        assert type(Built) is Delegating
        assert Built(1, 2).a == 3

    def test_a_delegate_that_drops_an_init_var_is_refused(self) -> None:
        class Dropping(META):
            def __new__(
                metacls: type,
                name: str,
                bases: tuple[type, ...],
                namespace: dict[str, object],
                **keywords: object,
            ) -> type:
                annotations = namespace.get("__annotations__", {})
                kept = {key: value for key, value in annotations.items() if key != "flag"}

                return super().__new__(
                    metacls, name, bases, {**namespace, "__annotations__": kept}, **keywords
                )

        class Base(Struct, metaclass=Dropping):
            a: int

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"b": int, "flag": InitVar[int]}})


def test_parameter_order_spans_fields_and_init_vars() -> None:
    with pytest.raises(TypeError, match="non-default InitVar 'flag' follows a field with a default"):

        class FlagAfterDefault(Struct):
            a: int = 1
            flag: InitVar[int]

    with pytest.raises(TypeError, match="non-default field 'b' follows an InitVar with a default"):

        class FieldAfterDefault(Struct):
            flag: InitVar[int] = 1
            b: int


def test_a_double_underscore_init_var_is_refused() -> None:
    with pytest.raises(TypeError, match="'__hash__' cannot be an InitVar"):

        class Shadowing(Struct):
            __hash__: InitVar[int] = 0


def test_an_exception_struct_refuses_an_init_var() -> None:
    with pytest.raises(TypeError, match="an exception struct cannot take InitVar 'code'"):

        class Failure(Exception, Struct):
            code: InitVar[int]


def test_a_nested_init_var_is_refused() -> None:
    with pytest.raises(TypeError, match=r"write InitVar\[\.\.\.\] as the whole annotation"):
        META("Nested", (Struct,), {"__annotations__": {"v": list[InitVar[int]]}})
