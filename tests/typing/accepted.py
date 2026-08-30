from typing import Any, ClassVar, Generic, Literal, Protocol, TypeVar

from typing_extensions import assert_type

from salix import Struct, from_mapping, replace, set_field


class Point(Struct):
    x: int
    y: str


class Defaulted(Struct):
    a: int
    b: str = "b"


class Constants(Struct):
    limit: ClassVar[int] = 10
    name: str


class Mutable(Struct, frozen=False):
    value: int


T = TypeVar("T")


class Ok(Struct, Generic[T]):
    value: T


class Explicit(Struct, frozen=True):
    value: int


class Ordered(Struct, order=True):
    rank: int


class Options(Struct, eq=True, repr=False, match_args=False, weakref=True):
    value: int


def the_constructor_is_synthesised_from_the_annotations() -> None:
    assert_type(Point(1, "two").x, int)
    assert_type(Point(1, "two").y, str)
    assert_type(Point(x=1, y="two").x, int)


def a_default_makes_its_argument_optional() -> None:
    assert_type(Defaulted(1).b, str)
    assert_type(Defaulted(1, "other").b, str)


def a_class_var_is_a_class_attribute_and_not_a_constructor_argument() -> None:
    assert_type(Constants.limit, int)
    assert_type(Constants("x").limit, int)
    assert_type(Constants("x").name, str)


def a_mutable_struct_accepts_a_write() -> None:
    instance = Mutable(1)
    instance.value = 2


def inheritance_extends_the_signature() -> None:
    class Point3D(Point):
        z: float

    assert_type(Point3D(1, "two", 3.0).z, float)


def introspection_is_typed_on_both_the_class_and_the_instance() -> None:
    assert_type(Point._struct_fields_, tuple[str, ...])
    assert_type(Point(1, "two")._struct_fields_, tuple[str, ...])
    assert_type(Point.__struct_fields__, tuple[str, ...])
    assert_type(Point(1, "two").__struct_fields__, tuple[str, ...])
    assert_type(Point._struct_annotations_, tuple[Any, ...])
    assert_type(Point.__struct_annotations__, tuple[Any, ...])
    assert_type(Point._struct_metadata_, tuple[tuple[Any, ...], ...])
    assert_type(Point.__struct_metadata__, tuple[tuple[Any, ...], ...])
    # Narrower than the stub declares: the transform synthesises the literal
    # names, which is what makes a positional pattern check.
    assert_type(Point.__match_args__, tuple[Literal["x"], Literal["y"]])


def a_struct_is_a_struct() -> None:
    def take(struct: Struct) -> Struct:
        return struct

    take(Point(1, "two"))
    take(Explicit(1))


def order_synthesises_the_comparisons() -> None:
    assert_type(Ordered(1) < Ordered(2), bool)
    assert_type(Ordered(1) <= Ordered(2), bool)
    assert_type(Ordered(1) > Ordered(2), bool)
    assert_type(Ordered(1) >= Ordered(2), bool)
    assert_type(sorted([Ordered(1), Ordered(2)]), list[Ordered])


def the_options_a_checker_cannot_see_are_still_accepted() -> None:
    """eq, repr, match_args and weakref have no type-level meaning; declaring
    them on __init_subclass__ is what keeps the class definition from erroring.
    """

    assert_type(Options(1).value, int)


def a_struct_hierarchy_declares_its_own_class_keyword() -> None:
    class Base(Struct):
        def __init_subclass__(
            cls, plugin: str | None = None, **keywords: Any
        ) -> None:
            super().__init_subclass__(**keywords)

    class Child(Base, plugin="x"):
        value: int

    assert_type(Child(1).value, int)


def set_field_takes_a_struct_a_name_and_any_value() -> None:
    assert_type(set_field(Point(1, "two"), "x", 9), None)
    set_field(Point(1, "two"), "y", object())


def subscripting_a_generic_struct_narrows_the_field_type() -> None:
    assert_type(Ok(3).value, int)
    assert_type(Ok[str]("x").value, str)


def replace_returns_the_concrete_struct_type() -> None:
    assert_type(replace(Point(1, "two"), x=9), Point)
    replace(Point(1, "two"), y=object())


def from_mapping_returns_the_concrete_struct_type() -> None:
    assert_type(from_mapping(Point, {"x": 1, "y": "two"}), Point)
    from_mapping(Point, {"x": object()})


def __copy___returns_the_concrete_struct_type() -> None:
    assert_type(Point(1, "two").__copy__(), Point)


def __deepcopy___returns_the_concrete_struct_type() -> None:
    assert_type(Point(1, "two").__deepcopy__({}), Point)


class CommandProtocol(Protocol):
    COMMAND: ClassVar[bytes]


class CarriesCommand(Struct):
    COMMAND: ClassVar[bytes] = b"carried"
    payload: int


def a_struct_satisfies_a_classvar_protocol_structurally() -> None:
    def reads(command: CommandProtocol) -> bytes:
        return command.COMMAND

    assert_type(reads(CarriesCommand(1)), bytes)
