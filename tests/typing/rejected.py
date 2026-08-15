from salix import Struct, set_field


class Point(Struct):
    x: int
    y: str


class Mutable(Struct, frozen=False):
    value: int


class Ordered(Struct, order=True):
    rank: int


class NoMatchArgs(Struct, match_args=False):
    rank: int


def a_frozen_field_may_not_be_assigned() -> None:
    Point(1, "two").x = 2  # type: ignore[misc]


def a_frozen_field_may_not_be_deleted() -> None:
    """pyright's alone: mypy does not model `del` against a read-only attribute."""

    del Point(1, "two").x  # pyright: ignore[reportAttributeAccessIssue]


def an_argument_of_the_wrong_type_is_rejected() -> None:
    Point("one", 2)  # type: ignore[arg-type]


def a_missing_argument_is_rejected() -> None:
    Point(1)  # type: ignore[call-arg]


def an_extra_argument_is_rejected() -> None:
    Point(1, "two", 3)  # type: ignore[call-arg]


def an_unknown_keyword_is_rejected() -> None:
    Point(x=1, y="two", z=3)  # type: ignore[call-arg]


def a_field_that_does_not_exist_is_rejected() -> None:
    Point(1, "two").z  # type: ignore[attr-defined]


def a_mutable_field_still_has_a_type() -> None:
    Mutable(1).value = "text"  # type: ignore[assignment]


def a_struct_without_order_has_no_comparisons() -> None:
    Point(1, "two") < Point(1, "two")  # type: ignore[operator]


def ordering_against_an_unrelated_struct_is_rejected() -> None:
    Ordered(1) < Point(1, "two")  # type: ignore[operator]


def an_unknown_class_keyword_is_rejected() -> None:
    """All three, since the stub stopped naming a metaclass: a class keyword
    goes to the metaclass when there is one, and mypy stops checking it against
    __init_subclass__ there.
    """

    class Typo(  # type: ignore[call-arg]
        Struct,
        frozn=False,  # pyright: ignore[reportCallIssue]
    ):
        value: int


def match_args_false_leaves_no_positional_pattern() -> None:
    match NoMatchArgs(1):
        case NoMatchArgs(rank):  # type: ignore[misc]
            print(rank)


def set_field_will_not_take_a_non_struct() -> None:
    set_field(1, "x", 9)  # type: ignore[arg-type]


def set_field_will_not_take_a_name_that_is_not_a_string() -> None:
    set_field(Point(1, "two"), 5, 9)  # type: ignore[arg-type]


def set_field_will_not_take_keywords() -> None:
    set_field(instance=Point(1, "two"), name="x", value=9)  # type: ignore[call-arg]


def the_field_names_may_not_be_assigned_on_the_class() -> None:
    Point._struct_fields_ = ("z",)  # type: ignore[misc]


def the_defaults_may_not_be_assigned_on_the_class() -> None:
    Point._struct_defaults_ = (9,)  # type: ignore[misc]


def the_field_names_may_not_be_assigned_on_an_instance() -> None:
    Point(1, "two")._struct_fields_ = ("z",)  # type: ignore[misc]


def the_defaults_may_not_be_assigned_on_an_instance() -> None:
    Point(1, "two")._struct_defaults_ = (9,)  # type: ignore[misc]


def msgspec_names_for_them_may_not_be_assigned_either() -> None:
    Point.__struct_fields__ = ("z",)  # type: ignore[misc]
    Point.__struct_defaults__ = (9,)  # type: ignore[misc]
    Point(1, "two").__struct_fields__ = ("z",)  # type: ignore[misc]
    Point(1, "two").__struct_defaults__ = (9,)  # type: ignore[misc]


class AFieldMayNotTakeOneOfTheMetadataNames(Struct):
    """Declaring the four names Final is what makes a checker refuse them as
    field names, and the runtime refuses them now too: the reservation is
    real in both, so this class cannot be built either way.

    Stated in this file so that the refusal is checked rather than assumed.
    """

    _struct_fields_: int  # type: ignore[assignment,misc]
    _struct_defaults_: int  # type: ignore[assignment,misc]
    __struct_fields__: int  # type: ignore[assignment,misc]
    __struct_defaults__: int  # type: ignore[assignment,misc]
