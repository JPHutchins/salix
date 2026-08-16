import weakref

import pytest

from salix import Struct


class Fieldless(Struct):
    pass


class FieldlessWeak(Struct, weakref=True):
    pass


class WithFields(Struct):
    a: int
    b: int


class WithOneField(Struct):
    """One field, because that is the width a weakref slot costs. Below 3.12
    this and FieldlessWeak measure the same, which is the tie that says whether
    salix is reading layout off sizes or off fields.
    """

    a: int


def test_a_fieldless_base_in_front_does_not_hide_the_fields_behind_it():
    class Both(Fieldless, WithFields):
        c: int

    instance = Both(1, 2, 3)

    assert Both._struct_fields_ == ("a", "b", "c")
    assert (instance.a, instance.b, instance.c) == (1, 2, 3)
    assert repr(instance) == "Both(a=1, b=2, c=3)"


def test_a_weakref_base_in_front_does_not_hide_them_either():
    """Sizes are the wrong question. Below 3.12 a weakref slot widens the type,
    so a fieldless base carrying one measures exactly as wide as WithOneField
    and would win a comparison on size -- while CPython, which discounts
    __weakref__ when it settles on __base__, gave the layout to the other.
    """

    class Both(FieldlessWeak, WithOneField):
        c: int

    instance = Both(1, 2)

    assert Both._struct_fields_ == ("a", "c")
    assert (instance.a, instance.c) == (1, 2)


@pytest.mark.parametrize(
    "bases",
    [
        (Fieldless, WithFields),
        (WithFields, Fieldless),
        (FieldlessWeak, WithOneField),
        (WithOneField, FieldlessWeak),
        (Fieldless, FieldlessWeak, WithOneField),
        (FieldlessWeak, Fieldless, WithFields),
    ],
    ids=lambda bases: "-".join(base.__name__ for base in bases),
)
def test_the_layout_base_is_the_one_cpython_chose(bases):
    """The offsets salix hands out are the base's slots plus the new ones, so
    the base it reads them from has to be the base the new slots were placed
    after -- and that base is CPython's choice, not salix's. Inheriting exactly
    the fields of __base__ is that agreement stated in terms a test can read.
    """

    child = type(Struct)("Child", bases, {"__annotations__": {"c": int}})

    assert child._struct_fields_ == (*child.__base__._struct_fields_, "c")


def test_the_order_of_the_bases_does_not_change_the_fields():
    class Forwards(Fieldless, WithFields):
        c: int

    class Backwards(WithFields, Fieldless):
        c: int

    assert Forwards._struct_fields_ == Backwards._struct_fields_


def test_the_frozen_keyword_is_answered_by_any_base_with_fields():
    """The symptom that mattered: a fieldless base in front waived the
    constraint, so a mutable subclass of a frozen struct could exist and be
    handed to anything holding a reference of the frozen base's type.
    """

    with pytest.raises(TypeError, match="mutable struct cannot inherit"):

        class Mutable(Fieldless, WithFields, frozen=False):
            c: int


def test_a_weakref_base_in_front_does_not_waive_it_either():
    with pytest.raises(TypeError, match="mutable struct cannot inherit"):

        class Mutable(FieldlessWeak, WithOneField, frozen=False):
            c: int


def test_a_fieldless_base_alone_still_waives_it():
    """Nothing behind it made a promise about a field, so there is none to break."""

    class Mutable(Fieldless, frozen=False):
        c: int

    instance = Mutable(1)
    instance.c = 2

    assert instance.c == 2


def test_options_still_come_from_the_struct_base():
    class Unordered(Struct, repr=False):
        pass

    class Child(Unordered):
        x: int = 0

    assert "object at" in repr(Child())


class TestWhichBaseAnswers:
    """Which base answers which question, for the two this fix separates.

    The *layout* base is the widest -- where CPython put the slots, so where
    their offsets come from, and it is the whole of what this fix changes about
    layout. Options are still read off the first struct base, as they were
    before.

    `frozen` is the exception, because it is a promise rather than a preference:
    any base with fields that made it binds the whole class, and the rebind is
    forced when some other struct base is mutable, since that base bound
    object's __setattr__ on its own transition and the MRO reaches it first.

    Reading the options off one base while the MRO answers from another is a
    wider problem than this fix and predates it; the tests here pin the shapes
    that work, not that one.
    """

    def test_a_mutable_base_in_front_does_not_unfreeze_the_class(self):
        """The one that has to be forced. Otherwise C refuses a mutable subclass
        and hashes by value while accepting every write -- a dict key that
        changes its own hash.
        """

        class MutableFieldless(Struct, frozen=False):
            pass

        class Both(MutableFieldless, WithFields):
            c: int

        with pytest.raises(TypeError, match="does not support attribute assignment"):
            Both(1, 2, 3).a = 99

        with pytest.raises(TypeError, match="mutable struct cannot inherit"):

            class Mutable(Both, frozen=False):
                d: int = 0

    def test_a_frozen_fieldless_base_may_still_strengthen_a_mutable_one(self):
        """The other direction, and reading frozen off the widest base broke it:
        the class was refused because the widest base was the mutable one.
        """

        class FrozenFieldless(Struct):
            pass

        class MutableFields(Struct, frozen=False):
            a: int

        class Strengthened(FrozenFieldless, MutableFields, frozen=True):
            c: int

        with pytest.raises(TypeError, match="does not support attribute assignment"):
            Strengthened(1, 2).a = 99

    def test_a_repr_less_base_in_front_takes_the_repr_with_it(self):
        """Not a defect -- the class records `repr=False` and behaves that way.
        What matters is that the two agree; reading the option off the widest
        base while the MRO answered from this one is what did not.
        """

        class Unrepresented(Struct, repr=False):
            pass

        class Both(Unrepresented, WithFields):
            c: int

        assert "object at" in repr(Both(1, 2, 3))

    def test_and_a_subclass_can_still_ask_for_it_back(self):
        class Unrepresented(Struct, repr=False):
            pass

        class Both(Unrepresented, WithFields):
            c: int

        class Child(Both, repr=True):
            d: int = 0

        assert repr(Child(1, 2, 3)) == "Child(a=1, b=2, c=3, d=0)"

    def test_an_identity_equal_base_in_front_takes_equality_with_it(self):
        """And takes the hash with it, which is the half that has to follow."""

        class ByIdentity(Struct, eq=False):
            pass

        class Both(ByIdentity, WithFields):
            c: int

        first, second = Both(1, 2, 3), Both(1, 2, 3)

        assert first != second
        assert hash(first) == hash(first)
        assert first in {first}

    def test_a_honoured_body_ordering_dunder_survives_the_eq_repair(self):
        """A later base's __eq__ triggers the comparison repair; the first
        base's own body __lt__ is honoured, so the repair rebinds per name
        and leaves it answering.
        """

        class ByOrder(Struct):
            def __lt__(self, other: object) -> object:
                return "by-order"

        class ByEq(Struct):  # noqa: PLW1641 -- the body pair is the trigger
            b: int

            def __eq__(self, other: object) -> bool:
                return True

        class Both(ByOrder, ByEq):
            c: int

        first, second = Both(1, 2), Both(1, 2)

        assert first.__lt__(second) == "by-order"


    def test_a_body_eq_on_a_base_that_is_not_the_layout_one_still_carries_its_hash(self):
        """Equal objects hashing differently is the shape that broke here.

        A body `__eq__` wins the MRO from any struct base, but the deferral
        about its `__hash__` was read off the layout base only -- so a fieldless
        base's body pair compared while salix's value hash answered beside it.
        """

        class ByBody(Struct):
            def __eq__(self, other: object) -> bool:
                return True

            def __hash__(self) -> int:
                return 7

        class Both(ByBody, WithFields):
            c: int

        first, second = Both(1, 2, 3), Both(9, 9, 9)

        assert first == second
        assert hash(first) == hash(second) == 7

    def test_a_weakref_slot_on_a_base_that_is_not_the_layout_one_is_recorded(self):
        """The option has to match tp_weaklistoffset, or a subclass asks for a
        second __weakref__ that CPython refuses.
        """

        class Weak(FieldlessWeak, WithFields):
            c: int

        class Child(Weak):
            d: int = 0

        assert weakref.ref(Weak(1, 2, 3)) is not None
        assert weakref.ref(Child(1, 2, 3)) is not None

    def test_a_keyword_matching_the_layout_base_is_still_honoured(self):
        """The shape that catches the rebind comparing against the wrong base.

        `apply_options` decides on a transition, so it has to be handed the
        options the class actually inherits. Handed the layout base's instead,
        a keyword asking for what the *layout* base already says looks like no
        change at all -- nothing is rebound, the MRO keeps the first base's
        binding, and the keyword is dropped without a word.
        """

        class Unrepresented(Struct, repr=False):
            pass

        class ByIdentity(Struct, eq=False):
            pass

        class Represented(Unrepresented, WithFields, repr=True):
            c: int

        class Equal(ByIdentity, WithFields, eq=True):
            c: int

        assert repr(Represented(1, 2, 3)) == "Represented(a=1, b=2, c=3)"
        assert Equal(1, 2, 3) == Equal(1, 2, 3)

    def test_a_base_that_agrees_changes_nothing(self):
        """The negative control: two struct bases are not on their own a reason
        to bind anything, so a class whose bases agree is left as it was.
        """

        class AlsoFrozen(Struct):
            pass

        class Both(AlsoFrozen, WithFields):
            c: int

        assert "__setattr__" not in vars(Both)
        assert "__repr__" not in vars(Both)
        assert "__eq__" not in vars(Both)

    def test_and_a_plain_subclass_still_inherits_a_body_pair(self):
        class ByBody(Struct):
            a: int

            def __eq__(self, other: object) -> bool:
                return True

            def __hash__(self) -> int:
                return 7

        class Child(ByBody):
            c: int

        assert (Child(1, 2) == Child(9, 9)) is True
        assert hash(Child(1, 2)) == 7
