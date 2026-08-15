from typing import ClassVar

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
@pytest.mark.parametrize("binding", (999, lambda self: 1))
def test_a_body_binding_of_any_of_them_is_refused(name, binding):
    """The other half of the same shape, and it arrives by a different route:
    `drop_class_variables` strips only names the body annotated, so an
    unannotated assignment would have stayed in the class dict and the
    instance would have found it there. Any value is refused the same -- the
    probe checks the name, not what is bound to it.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)(
            "Shadowed", (Struct,), {"__annotations__": {"x": int}, name: binding}
        )


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_subclass_binding_one_is_refused(name):
    """The refusal keys on the class that writes the binding, so a struct
    subclass cannot shadow the metadata its base reports through its own
    body; a binding carried by a plain base is out of scope by the ruling.
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
        "struct_defaults_",
    ),
)
def test_neighbouring_names_are_ordinary(name):
    """The check is exact: a neighbour that differs in its underscores and
    does not trip CPython's slot-name mangling builds and reads back as a
    field, and the metadata still answers.
    """

    built = type(Struct)("Built", (Struct,), {"__annotations__": {name: int, "x": int}})

    assert built._struct_fields_ == (name, "x")
    assert getattr(built(5, 6), name) == 5


def test_an_unannotated_binding_of_an_ordinary_name_stays_in_the_class_dict():
    """`drop_class_variables` strips only names the body annotated: an
    unannotated assignment stays in the class dict and the instance finds
    it there. Restated with an ordinary name now that the reserved ones are
    refused, so the mechanism keeps a pin of its own.
    """

    class Shadowed(Struct):
        x: int
        stray = 999

    assert vars(Shadowed)["stray"] == 999
    assert Shadowed(1).stray == 999


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_class_statement_taking_a_reserved_name_is_refused(name):
    """The refusal holds on the syntax users write, not only the metaclass
    call: a plain body binding of any of the four spellings is refused.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        exec(
            f"class Shadowed(Struct):\n"
            f"    x: int\n"
            f"    {name} = 999",
            {"Struct": Struct},
        )


def test_a_class_statement_mangles_the_closest_dunder_neighbour_into_a_field():
    """The closest neighbour of a reserved dunder takes CPython's mangling in
    a real class body: `__struct_fields` becomes `_Shadowed__struct_fields`
    and builds as an ordinary field there. The type() path mangles the slot
    set the same way and the build dies with a slot-offset RuntimeError,
    which is why the near-miss list stops short of it.
    """

    class Shadowed(Struct):
        x: int
        __struct_fields: int

    assert Shadowed._struct_fields_ == ("x", "_Shadowed__struct_fields")
    assert Shadowed(5, 6)._Shadowed__struct_fields == 6


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_reserved_name_annotated_classvar_gets_the_reserved_refusal(name):
    """The ClassVar advice ("write it below the fields, without an
    annotation") leads straight into the reservation's binding refusal, so
    the reserved message fires instead.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)(
            "Shadowed", (Struct,), {"__annotations__": {name: ClassVar[int], "x": int}}
        )


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_reserved_name_with_a_default_gets_the_reserved_refusal(name):
    """A defaulted reserved field followed by a non-default one would
    otherwise surface a reorder error first, and reordering only surfaces
    the reservation; the reserved message fires instead.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)(
            "Shadowed", (Struct,), {"__annotations__": {name: int, "x": int}, name: 7}
        )


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_reserved_name_with_a_shared_mutable_default_gets_the_reserved_refusal(name):
    """A shared-mutable default would otherwise surface its own refusal
    first, and its advice cannot help a name that cannot be a field at all;
    the reserved message fires instead.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)(
            "Shadowed",
            (Struct,),
            {"__annotations__": {name: tuple, "x": int}, name: ([1],)},
        )


@pytest.mark.parametrize("name", SALIX + MSGSPEC)
def test_a_reserved_name_in_slots_gets_the_reserved_refusal(name):
    """The __slots__ advice ("declare it as a field") leads straight into
    the reservation, so the reserved message fires instead.
    """

    with pytest.raises(TypeError, match="is reserved for salix's metadata"):
        type(Struct)("Shadowed", (Struct,), {"__slots__": (name,), "x": int})


def test_the_name_quartet_agrees_with_the_other_file_that_owns_it():
    """test_struct_identity.py owns the quartet for its msgspec-alias pins;
    the two copies must agree or one side silently stops covering names.
    """

    import test_struct_identity

    assert SALIX + MSGSPEC == test_struct_identity.METADATA_NAMES


def test_a_subclass_reports_its_own_fields_under_both_names():
    class Extended(Point):
        z: int = 3

    assert Extended._struct_fields_ == ("x", "y", "z")
    assert Extended.__struct_fields__ == ("x", "y", "z")
    assert Extended._struct_defaults_ == (2, 3)
    assert Extended.__struct_defaults__ == (2, 3)
