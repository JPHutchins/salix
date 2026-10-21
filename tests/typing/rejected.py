import sys
from dataclasses import KW_ONLY, InitVar, dataclass

from salix import Struct, field, fields, replace, set_field, to_dict


class Point(Struct):
    x: int
    y: str


class Mutable(Struct, frozen=False):
    value: int


class Seeded(Struct):
    a: int
    seed: InitVar[int]


class Ordered(Struct, order=True):
    rank: int


class NoMatchArgs(Struct, match_args=False):
    rank: int


class KeywordOnly(Struct, kw_only=True):
    x: int


class AfterTheMarker(Struct):
    x: int
    _: KW_ONLY
    y: int


class OneKeywordOnlyField(Struct):
    x: int = 1
    y: int = field(kw_only=True)


@dataclass(frozen=True)
class Plain:
    x: int


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


def a_keyword_only_field_passed_by_position_is_rejected() -> None:
    KeywordOnly(1)  # type: ignore[call-arg]


def a_field_after_the_marker_passed_by_position_is_rejected() -> None:
    AfterTheMarker(1, 2)  # type: ignore[call-arg]


def a_keyword_only_field_specifier_passed_by_position_is_rejected() -> None:
    OneKeywordOnlyField(1, 2)  # type: ignore[call-arg]


def a_field_option_salix_does_not_take_is_rejected() -> None:
    field(default_factory=list)  # type: ignore[call-overload]


def a_missing_init_var_is_rejected() -> None:
    Seeded(1)  # type: ignore[call-arg]


def an_init_var_of_the_wrong_type_is_rejected() -> None:
    Seeded(1, "two")  # type: ignore[arg-type]


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


def to_dict_will_not_take_a_non_struct() -> None:
    to_dict(1)  # type: ignore[arg-type]


def to_dict_will_not_take_a_struct_class() -> None:
    to_dict(Point)  # type: ignore[arg-type]


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


def fields_refuses_a_value_that_is_not_a_struct() -> None:
    fields(1)  # type: ignore[arg-type]


def a_field_record_is_read_only() -> None:
    fields(Point)[0].name = "renamed"  # type: ignore[misc]


def replace_refuses_a_value_that_is_not_a_struct() -> None:
    replace(Plain(1), x=2)  # type: ignore[arg-type]


def replace_takes_no_positional_changes() -> None:
    replace(Point(1, "two"), 3)  # type: ignore[call-arg]


if sys.version_info >= (3, 13):

    def replace_refuses_a_name_that_is_not_a_field() -> None:
        replace(Point(1, "two"), z=1)  # type: ignore[call-arg]

    def replace_refuses_a_value_of_the_wrong_type() -> None:
        replace(Point(1, "two"), x="one")  # type: ignore[arg-type]

    def the_replace_method_refuses_a_name_that_is_not_a_field() -> None:
        Point(1, "two").__replace__(z=1)  # type: ignore[call-arg]
