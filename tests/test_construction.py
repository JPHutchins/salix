"""Argument binding: what reaches a slot, and what is rejected."""

import pytest
from values import COPIED_WHEN_EMPTY, NON_EMPTY

from salix import Struct


class Point(Struct):
    x: int
    y: int


class Mutable(Struct, frozen=False):
    a: int


class Defaulted(Struct):
    a: int
    b: int = 2
    c: int = 3


class Empty(Struct):
    pass


class Renamed(Struct):
    value_one: int
    value_two: int


def subclass_of(kind: type) -> type:
    """A subclass of one of the four, which is not one of the four."""

    return type(f"A{kind.__name__.capitalize()}", (kind,), {})


def test_positional_and_keyword_reach_the_same_slots():
    assert Point(1, 2) == Point(x=1, y=2) == Point(1, y=2)


def test_defaults_fill_what_is_not_supplied():
    assert (Defaulted(1).b, Defaulted(1).c) == (2, 3)
    assert Defaulted(1, 9).b == 9
    assert Defaulted(1, c=9).c == 9


def test_a_value_is_stored_not_copied():
    value = [1]

    assert Point(value, 0).x is value


def test_none_is_a_value_rather_than_an_absence():
    assert Point(None, None).x is None
    assert repr(Point(None, None)) == "Point(x=None, y=None)"


def test_too_many_positional_arguments():
    with pytest.raises(TypeError, match="takes at most 2 positional arguments but 3 were given"):
        Point(1, 2, 3)


def test_missing_a_required_argument():
    with pytest.raises(TypeError, match="missing required argument 'y'"):
        Point(1)


def test_unexpected_keyword_argument():
    with pytest.raises(TypeError, match="unexpected keyword argument 'z'"):
        Point(1, 2, z=3)


def test_the_same_field_given_twice():
    with pytest.raises(TypeError, match="multiple values for argument 'x'"):
        Point(1, x=2)


def test_a_keyword_name_built_at_runtime_still_resolves():
    """The fast path compares interned names by identity; this misses it."""

    name = "".join(["value_", "one"])  # noqa: FLY002 -- a literal would be interned

    assert name is not "value_one"  # noqa: F632
    assert Renamed(**{name: 1}, value_two=2).value_one == 1


def test_an_empty_struct_accepts_nothing():
    assert Empty() == Empty()

    with pytest.raises(TypeError, match="takes at most 0 positional arguments"):
        Empty(1)

    with pytest.raises(TypeError, match="unexpected keyword argument"):
        Empty(nope=1)


def test_the_class_is_callable_through_the_slow_path_too():
    """tp_call routes to the same vectorcall, so apply() must agree with a call."""

    arguments = (1, 2)

    assert Point(*arguments) == Point.__call__(*arguments)


class TestMutableDefaults:
    """`xs: list = []` reads as an empty list per instance, and that is what it
    gets. Exactly four builtins are copied at construction, and everything else
    is shared -- which is cheaper and indistinguishable for a value that cannot
    be mutated, and simply sharing for one that can. `array.array`, `deque`, a
    writable `memoryview` and the subclasses of the four are all in the second
    group; #51 argues for hashability as the test that would replace the list.
    """

    def test_a_list_default_is_not_shared(self):
        class Holder(Struct, frozen=False):
            xs: list = []  # noqa: RUF012 -- the copy is the feature under test

        first, second = Holder(), Holder()
        first.xs.append(1)

        assert second.xs == []

    def test_frozen_does_not_make_the_copy_unnecessary(self):
        """Frozen stops rebinding, not mutation -- the same as a frozen
        dataclass -- so the default has to be copied even here. Immutability of
        the struct is not immutability of what its field points at.
        """

        class Holder(Struct):
            xs: list = []  # noqa: RUF012 -- the copy is the feature under test

        first, second = Holder(), Holder()
        first.xs.append(1)

        assert second.xs == []

    @pytest.mark.parametrize(
        "factory",
        [kind for kind in COPIED_WHEN_EMPTY if kind is not list],
        ids=lambda factory: factory.__name__,
    )
    def test_the_other_mutable_builtins_are_copied_too(self, factory):
        """`list` has its own test above; these are the rest of the four that
        `struct_copies_default` names, taken from that list rather than named
        again -- adding a type to values.py brings a case here with it.

        The type is asserted as well as the distinctness, because each of these
        is copied by its own constructor and a wrong one would answer with a
        distinct object of the wrong type -- which the `is not` alone accepts.
        """

        class Holder(Struct):
            v: object = factory()

        assert Holder().v is not Holder().v
        assert type(Holder().v) is factory
        assert Holder().v == factory()

    def test_an_immutable_default_is_shared(self):
        """Sharing holds. Two of the three assertions say little on their own,
        and the vacuity that took an int out of this was never the int's -- it
        is every immutable's. Measured: `str(s)`, `tuple(t)`, `copy.copy` and a
        full slice all hand the same object back, so the text and pair
        assertions pass whether salix shares the default or copies everything.

        The nested struct is the one that discriminates, and for the reason
        that makes the other two vacuous: `copy.copy(Inner(1))` raises (#13), so
        a salix that copied every default could not get past this field at all.

        The str is still built at runtime rather than written as a literal, and
        that buys less than the earlier wording claimed. It does not make the
        assertion discriminating. It means a salix that *rebuilt* the string
        rather than storing the declared one would be caught here, where an
        interned literal would hide it.

        What actually pins the copying is the test above: a mutable default is
        a distinct object per instance, and that assertion can fail.
        """

        class Inner(Struct):
            a: int

        uncached_text = "".join(["not ", "interned"])  # noqa: FLY002 -- see above

        class Holder(Struct):
            text: str = uncached_text
            pair: tuple = (1, 2)
            nested: Inner = Inner(1)

        first, second = Holder(), Holder()

        assert first.text is second.text
        assert first.pair is second.pair
        assert first.nested is second.nested

    def test_the_class_keeps_its_own_copy_of_the_declared_default(self):
        """The emptiness the refusal checked has to stay true. A module-level
        container declared as the default is still the module's to append to,
        and the class would otherwise be holding the same object -- so every
        instance would get a shallow copy of something the refusal rejected.

        msgspec severs the same alias by replacing the default with a Factory.
        """

        shared = []

        class Holder(Struct, frozen=False):
            xs: list = shared

        shared.append([2])

        assert Holder._struct_defaults_[0] is not shared
        assert Holder().xs == []

    def test_construction_and_inheritance_agree_after_that_mutation(self):
        """One copies and one refuses, so they have to be looking at the same
        thing -- the stored default, which the module-level alias no longer
        reaches. `_struct_defaults_` still hands it out and #51 is where that
        route is argued; what this pins is that the two agree.
        """

        shared = []

        class Holder(Struct, frozen=False):
            xs: list = shared

        shared.append([2])

        class Inheriting(Holder):
            pass

        assert Inheriting().xs == []

    def test_a_supplied_value_is_still_stored_rather_than_copied(self):
        """Only the default is the class's to hand out repeatedly."""

        class Holder(Struct):
            xs: list = []  # noqa: RUF012 -- the copy is the feature under test

        supplied = [1]

        assert Holder(supplied).xs is supplied

    def test_a_non_empty_container_is_refused_rather_than_shallow_copied(self):
        """Copying it could only be shallow, so every instance would get its own
        outer list around the same inner one -- the same bug one level down.
        """

        with pytest.raises(TypeError, match="non-empty list"):

            class Nested(Struct):
                xs: list = [[1]]  # noqa: RUF012 -- the refusal is the assertion

    @pytest.mark.parametrize(
        "factory",
        [kind for kind in COPIED_WHEN_EMPTY if kind is not list],
        ids=lambda factory: factory.__name__,
    )
    def test_every_non_empty_builtin_is_refused(self, factory):
        """Derived from the same list the copying is, so a type added there
        arrives here refused as well as copied -- the two halves of the rule
        that have to agree.
        """

        value = NON_EMPTY[factory]

        with pytest.raises(TypeError, match=f"non-empty {factory.__name__}"):

            class Holder(Struct):
                v: object = value

    @pytest.mark.parametrize(
        "value",
        [
            pytest.param(__import__("array").array("i", [1, 2]), id="array"),
            pytest.param(__import__("collections").deque([1, 2]), id="deque"),
            pytest.param(__import__("collections").defaultdict(list), id="defaultdict"),
            pytest.param(Mutable(1), id="a_mutable_struct"),
            *(
                pytest.param(subclass_of(kind)(NON_EMPTY[kind]), id=f"a_{kind.__name__}_subclass")
                for kind in COPIED_WHEN_EMPTY
            ),
        ],
    )
    def test_a_mutable_container_outside_the_four_is_shared_and_not_refused(self, value):
        """The boundary is the four exact types, not mutability, so these are
        neither copied nor refused. Every one of these declares `__hash__` is
        None -- it says it does not hash before being asked -- which is what
        #51's rule leaves alone; salix's own `frozen=False` struct included,
        since `eq` without `frozen` sets `__hash__` to None.

        The four subclasses are the sharpest of them: a *non-empty* subclass of
        a type the refusal covers is shared outright, which is the aliasing bug
        this file is otherwise about. It is the price of copying by exact type,
        since `PyDict_Copy` of a defaultdict is a dict, and it is deliberate
        rather than missed.

        `hash(value)` rather than `isinstance(value, Hashable)`: the ABC asks
        whether `__hash__` is non-None, and a writable memoryview has one that
        raises when called. Only the call is the test.
        """

        with pytest.raises((TypeError, ValueError)):
            hash(value)

        class Holder(Struct, frozen=False):
            v: object = value

        assert Holder().v is Holder().v
        assert Holder._struct_defaults_[0] is value

    @pytest.mark.parametrize(
        "value",
        [
            pytest.param(([],), id="a_tuple_of_a_list"),
            pytest.param(((1, []),), id="a_tuple_two_deep"),
            pytest.param(memoryview(bytearray(b"abc")), id="a_writable_memoryview"),
            pytest.param((frozenset(), []), id="a_pair_holding_one_of_each"),
        ],
    )
    def test_a_shallowly_immutable_container_of_something_mutable_is_refused(self, value):
        """#51's addition to the rule. A tuple is not one of the four, so it was
        shared outright -- and `xs: object = ([],)` handed every instance the
        same inner list, which is the aliasing bug the four-type rule exists to
        stop, one level down and outside its reach.

        The type says it hashes and the instance then refuses, which is exactly
        what a container of something mutable does and what nothing immutable
        does.
        """

        with pytest.raises(TypeError, match="whose value will not"):

            class Holder(Struct):
                v: object = value

    @pytest.mark.parametrize(
        "value",
        [
            pytest.param((1, 2), id="a_tuple_of_ints"),
            pytest.param("text", id="a_string"),
            pytest.param(frozenset({1}), id="a_frozenset"),
            pytest.param(memoryview(b"abc"), id="a_read_only_memoryview"),
            pytest.param(Point(1, 2), id="a_frozen_struct"),
        ],
    )
    def test_an_immutable_container_is_still_shared(self, value):
        """The control. Every one of these hashes, so none of them can be
        holding anything mutable, and sharing is right: there is nothing an
        instance could do to one that another instance would see.
        """

        class Holder(Struct):
            v: object = value

        assert Holder().v is value
        assert Holder().v is Holder().v

    def test_the_probe_hashes_a_default_once(self):
        """`build_defaults` checks the declared value and then the stored copy,
        which for anything outside the four copied types is the same object. The
        hash probe runs on the stored one only, so a class author's `__hash__`
        is called once rather than twice.
        """

        calls = []

        class Counted:
            def __hash__(self) -> int:
                calls.append(1)
                return 7

        value = Counted()

        class Holder(Struct):
            v: object = value

        assert calls == [1]
        assert Holder().v is value

    def test_a_hash_that_fails_for_its_own_reasons_propagates_unchanged(self):
        """The probe reads TypeError and ValueError as the instance declining.
        Anything else is not that, and salix says so with the author's own
        exception rather than claiming the value holds something mutable.

        A deliberate consequence: such a class used to build and share the
        value, and now does not.
        """

        class Angry:
            def __hash__(self) -> int:
                raise RuntimeError("boom")

        with pytest.raises(RuntimeError, match="boom"):

            class Holder(Struct):
                v: object = Angry()

    def test_a_writable_memoryview_answers_ValueError_and_is_still_caught(self):
        """The trap: the probe is a hash that raises, and a writable memoryview
        raises ValueError where a tuple of lists raises TypeError. Catching only
        TypeError would let it through and turn class creation into a ValueError
        from nowhere.
        """

        with pytest.raises(ValueError, match="cannot hash writable"):
            hash(memoryview(bytearray(b"abc")))

        with pytest.raises(TypeError, match="whose value will not"):

            class Holder(Struct):
                v: object = memoryview(bytearray(b"abc"))

    def test_a_body_init_does_not_exempt_the_declared_default(self):
        """Its constructor never reads the default, so nothing is shared -- but
        the declaration is still a promise the class makes through
        _struct_defaults_, and dataclasses refuses it under init=False for the
        same reason. The message names both hooks because __post_init__ is not
        one of them here: a body __init__ displaces the generated constructor,
        and run_post_init goes with it.
        """

        with pytest.raises(TypeError, match=r"non-empty list.*your own __init__"):

            class Holder(Struct, frozen=False):
                xs: list = ["a", "b"]  # noqa: RUF012 -- the refusal is the assertion

                def __init__(self) -> None:
                    self.xs = []

    def test_a_subclass_of_a_mutable_builtin_is_shared_not_copied(self):
        """The copy has to preserve the type and PyDict_Copy of a defaultdict is
        a dict, so subclasses are left alone -- as msgspec leaves them.
        """

        from collections import defaultdict

        class Holder(Struct):
            d: object = defaultdict(list)

        assert Holder().d is Holder().d

    def test_an_inherited_default_is_copied_as_well(self):
        class Base(Struct, frozen=False):
            xs: list = []  # noqa: RUF012 -- the copy is the feature under test

        class Child(Base):
            y: int = 0

        first, second = Child(), Child()
        first.xs.append(1)

        assert second.xs == []
