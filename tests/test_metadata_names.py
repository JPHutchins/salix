import pytest

from salix import Struct

SALIX = ("_struct_fields_", "_struct_defaults_")
MSGSPEC = ("__struct_fields__", "__struct_defaults__")
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
def test_a_field_named_any_of_them_is_refused(name):
    """The reservation is real: a field that takes one of the four names is
    refused, because the class would answer with the metadata while the
    instance answered with the field. What #82 settled rather than left as a
    discovery.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)("Shadowed", (Struct,), {"__annotations__": {name: int, "x": int}})


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_plain_class_variable_of_that_name_is_refused_too(name):
    """The other half of the same shape, and it arrives by a different route:
    `drop_class_variables` strips only names the body annotated, so an
    unannotated assignment would have stayed in the class dict and the
    instance would have found it there.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)("Shadowed", (Struct,), {"__annotations__": {"x": int}, name: 999})


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_method_named_any_of_them_is_refused_too(name):
    """A body binding need not be a field: a method that takes one of the
    names reads two ways the same, so it is refused the same.
    """

    def method(self):
        return 1

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)(
            "Shadowed", (Struct,), {"__annotations__": {"x": int}, name: method}
        )


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_subclass_binding_one_is_refused(name):
    """The refusal keys on the class that writes the binding, so a subclass
    cannot shadow the metadata its base reports.
    """

    class Base(Struct):
        x: int

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)("Shadowed", (Base,), {name: 999})


@pytest.mark.parametrize(
    "name",
    (
        "_struct_fields",
        "_struct_fields__",
        "struct_fields_",
        "_struct_defaults",
        "_struct_defaults__",
    ),
)
def test_neighbouring_names_are_ordinary(name):
    """The check is exact: neighbours of the four reserved names build and
    read back as fields, and the metadata still answers.
    """

    built = type(Struct)("Built", (Struct,), {"__annotations__": {name: int, "x": int}})

    assert built._struct_fields_ == (name, "x")
    assert getattr(built(5, 6), name) == 5


def test_a_subclass_reports_its_own_fields_under_both_names():
    class Extended(Point):
        z: int = 3

    assert Extended._struct_fields_ == ("x", "y", "z")
    assert Extended.__struct_fields__ == ("x", "y", "z")
    assert Extended._struct_defaults_ == (2, 3)
    assert Extended.__struct_defaults__ == (2, 3)
