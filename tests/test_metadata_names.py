import pytest

from salix import Struct

SALIX = ("_struct_fields_", "_struct_defaults_")
MSGSPEC = ("__struct_fields__", "__struct_defaults__")
FIELD_NAMES = (SALIX[0], MSGSPEC[0])
EXPECTED = (
    ("_struct_fields_", ("x", "y")),
    ("__struct_fields__", ("x", "y")),
    ("_struct_defaults_", (2,)),
    ("__struct_defaults__", (2,)),
)
MIXIN = Struct.__mro__[1]
META = type(Struct)


class Point(Struct):
    x: int
    y: int = 2


@pytest.mark.parametrize(("name", "expected"), EXPECTED)
def test_every_spelling_reports_the_value_itself_on_the_class(name, expected):
    """The value rather than the other spelling. On the class both spellings
    are wired to the same C getter, so comparing them to each other holds by
    construction and would survive a fields getter that returned the defaults
    for both.
    """

    assert getattr(Point, name) == expected


@pytest.mark.parametrize(("name", "expected"), EXPECTED)
def test_every_spelling_reports_the_value_itself_on_an_instance(name, expected):
    """Worth stating separately: the mixin wires the four names to four
    different functions, so a crossed `which` enum shows up here and nowhere
    else.
    """

    assert getattr(Point(1), name) == expected


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_neither_spelling_may_be_assigned(name):
    """Both are getsets with no setter, on the class and on the instance."""

    with pytest.raises(AttributeError):
        setattr(Point, name, ("z",))

    with pytest.raises(TypeError, match="does not support attribute assignment"):
        setattr(Point(1), name, ("z",))


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_salix_writes_no_spelling_into_the_classes_it_builds(name):
    """A getset on the metaclass answers for the class and one on the mixin
    answers for the instance; salix puts neither into the class it builds.

    An earlier version of this said that arrangement stops a class shadowing
    one spelling. It does not, and the test below is what that actually does.
    """

    assert name not in vars(Point)
    assert name in vars(META)
    assert name in vars(MIXIN)


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_body_that_takes_one_of_these_names_is_read_two_ways(name):
    """Reserving the names did not make them unusable, and a class that uses
    one anyway answers differently depending on where it is read.

    The metaclass getset is a data descriptor, so it wins on the class. On an
    instance the class's own binding is what the lookup reaches first -- the
    slot descriptor for a field, or the value for a plain assignment -- so the
    metadata is what the class says and the body is what the instance says.

    This is not new with the sunder: `__struct_fields__` as a field name has
    always done it. What is new is that two more names now behave this way,
    which is the cost of reserving them. Filed as #82; pinned here so that a
    decision about it is a decision rather than a discovery.
    """

    shadowed = type(Struct)(
        "Shadowed", (Struct,), {"__annotations__": {name: int, "x": int}}
    )
    metadata = (name, "x") if name in FIELD_NAMES else ()

    assert getattr(shadowed, name) == metadata
    assert getattr(shadowed(5, 6), name) == 5


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_plain_class_variable_of_that_name_is_read_two_ways_too(name):
    """The other half of the same shape, and it arrives by a different route:
    `drop_class_variables` strips only names the body annotated, so an
    unannotated assignment stays in the class dict and the instance finds it
    there.
    """

    shadowed = type(Struct)(
        "Shadowed", (Struct,), {"__annotations__": {"x": int}, name: 999}
    )
    metadata = ("x",) if name in FIELD_NAMES else ()

    assert vars(shadowed)[name] == 999
    assert getattr(shadowed, name) == metadata
    assert getattr(shadowed(1), name) == 999


def test_a_subclass_reports_its_own_fields_under_both_names():
    class Extended(Point):
        z: int = 3

    assert Extended._struct_fields_ == ("x", "y", "z")
    assert Extended.__struct_fields__ == ("x", "y", "z")
    assert Extended._struct_defaults_ == (2, 3)
    assert Extended.__struct_defaults__ == (2, 3)
