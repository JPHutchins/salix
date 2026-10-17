import copy
import copyreg
import sys
import threading
from collections.abc import Callable
from types import FrameType

import pytest
from platforms import recurses_through_c_until_recursion_error, starts_a_thread
from values import COPIED_WHEN_EMPTY, NON_EMPTY

from salix import Struct, fields, from_mapping, set_field, to_dict


class Point(Struct):
    x: int
    y: int


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

    name = "".join(["value_", "one"])  # noqa: FLY002

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
    gets.
    """

    def test_a_list_default_is_not_shared(self):
        class Holder(Struct, frozen=False):
            xs: list = []  # noqa: RUF012

        first, second = Holder(), Holder()
        first.xs.append(1)

        assert second.xs == []

    def test_frozen_does_not_make_the_copy_unnecessary(self):
        """Frozen stops rebinding, not mutation -- the same as a frozen
        dataclass -- so the default has to be copied even here. Immutability of
        the struct is not immutability of what its field points at.
        """

        class Holder(Struct):
            xs: list = []  # noqa: RUF012

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
        values.py names, taken from that list rather than named again --
        adding a type to values.py brings a case here with it.

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

        uncached_text = "".join(["not ", "interned"])  # noqa: FLY002

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
            xs: list = []  # noqa: RUF012

        supplied = [1]

        assert Holder(supplied).xs is supplied

    def test_a_non_empty_container_is_deep_copied(self):
        """#172: a shallow copy would share the inner list, so the default is
        deep-copied per instance instead of refused.
        """

        class Nested(Struct):
            xs: list = [[1]]  # noqa: RUF012

        first, second = Nested(), Nested()
        first.xs[0].append(2)

        assert second.xs == [[1]]

    @pytest.mark.parametrize(
        "factory",
        [kind for kind in COPIED_WHEN_EMPTY if kind is not list],
        ids=lambda factory: factory.__name__,
    )
    def test_every_non_empty_builtin_is_deep_copied(self, factory):
        """#172: the refusal is gone; the default is deep-copied per instance,
        derived from the same list the copying is, so a type added there
        arrives here copied as well -- the halves of the rule still agree.
        """

        value = NON_EMPTY[factory]

        class Holder(Struct):
            v: object = value

        first, second = Holder(), Holder()

        assert first.v is not second.v
        assert first.v == value

    @pytest.mark.parametrize(
        "value",
        [
            pytest.param(__import__("array").array("i", [1, 2]), id="array"),
            pytest.param(__import__("collections").deque([1, 2]), id="deque"),
        ],
    )
    def test_a_mutable_container_outside_the_four_is_shared_and_not_refused(self, value):
        """Mutability alone copies nothing. Every one of these declares
        `__hash__` is None -- it says it does not hash before being asked --
        which is what #51's rule leaves alone, so they are neither copied nor
        refused.

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
        "factory",
        [
            *(
                pytest.param(subclass_of(kind), id=f"a_{kind.__name__}_subclass")
                for kind in COPIED_WHEN_EMPTY
            ),
            pytest.param(__import__("collections").defaultdict, id="a_defaultdict"),
        ],
        ids=lambda factory: getattr(factory, "__name__", factory.id),
    )
    def test_a_subclass_of_one_of_the_four_is_copied_too(self, factory):
        """#163: the boundary widened from the four exact types to their
        subclasses, so a defaultdict default is not shared across instances."""

        class Holder(Struct):
            v: object = factory()

        first, second = Holder(), Holder()

        assert first.v is not second.v
        assert first.v == second.v

    @pytest.mark.parametrize(
        "seed",
        [
            *(pytest.param(subclass_of(kind)(NON_EMPTY[kind]), id=f"a_{kind.__name__}_subclass") for kind in COPIED_WHEN_EMPTY),
            pytest.param(__import__("collections").defaultdict(list, {"k": [1]}), id="a_defaultdict"),
        ],
    )
    def test_a_non_empty_subclass_of_one_of_the_four_is_deep_copied(self, seed):
        """#172: the refusal is gone -- the default is deep-copied per
        instance, subclasses included."""

        class Holder(Struct):
            v: object = seed

        first, second = Holder(), Holder()

        assert first.v is not second.v
        assert first.v == seed

        if isinstance(seed, dict) and isinstance(seed["k"], list):
            first.v["k"].append(9)
        elif isinstance(seed, set):
            first.v.add(9)
        elif isinstance(seed, dict):
            first.v[9] = 9
        else:
            first.v.append(9)

        assert second.v == seed

    @pytest.mark.parametrize(
        "value, inner",
        [
            pytest.param(([],), (0,), id="a_tuple_of_a_list"),
            pytest.param(((1, []),), (0, 1), id="a_tuple_two_deep"),
            pytest.param((frozenset(), []), (1,), id="a_pair_holding_one_of_each"),
        ],
    )
    def test_a_shallowly_immutable_container_of_something_mutable_is_deep_copied(self, value, inner):
        """#167: where the old rule refused, the default is now accepted and
        deep-copied per instance -- `xs: object = ([],)` no longer hands every
        instance the same inner list. The copy is deep: mutating an inner
        element is invisible to the other instance. Values a deepcopy cannot
        carry (a writable memoryview) fall back to sharing, pinned by the
        test below.
        """

        class Holder(Struct):
            v: object = value

        first, second = Holder(), Holder()

        assert first.v is not second.v

        first_inner = first.v
        second_inner = second.v

        for step in inner:
            first_inner = first_inner[step]
            second_inner = second_inner[step]

        first_inner.append(9)

        assert 9 not in second_inner

    def test_a_value_a_deepcopy_cannot_carry_is_shared(self):
        """The deep path falls back to the old sharing for values deepcopy
        refuses, rather than failing the construction."""

        value = memoryview(bytearray(b"abc"))

        class Holder(Struct):
            v: object = value

        assert Holder().v is Holder().v

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
        """The hash probe runs once when the class builds its stored default,
        so a class author's `__hash__` answers once per class statement.
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

    def test_an_author_hash_exception_propagates_past_later_fields(self):
        """A later field's probe used to clear the pending exception; the
        author's error must reach the class statement in either field order."""

        class Angry:
            def __hash__(self) -> int:
                raise RuntimeError("boom")

        with pytest.raises(RuntimeError, match="boom"):

            class AngryFirst(Struct):
                a: object = Angry()
                b: object = ([],)

        with pytest.raises(RuntimeError, match="boom"):

            class AngrySecond(Struct):
                a: object = ([],)
                b: object = Angry()

    def test_a_hash_that_raises_later_propagates_at_construction(self):
        """The class-statement probe succeeded once, so the constructor's
        re-probe must surface the author's exception, not a SystemError."""

        class SometimesAngry:
            def __init__(self) -> None:
                self.calls = 0

            def __hash__(self) -> int:
                self.calls += 1

                if self.calls > 1:
                    raise RuntimeError("boom")

                return 7

        value = SometimesAngry()

        class Holder(Struct):
            v: object = value

        with pytest.raises(RuntimeError, match="boom"):
            Holder()

    def test_a_deepcopy_that_returns_its_argument_shares_the_declared_object(self):
        """copy.deepcopy hands the argument back as the copy for a
        __deepcopy__ returning self; salix cannot distinguish it from a real
        copy, so the declared object is what instances share. Pinned as the
        accepted outcome of the protocol, not a severing guarantee."""

        class Selfie(list):
            def __deepcopy__(self, memo):
                return self

        seed = Selfie([1])

        class NonEmpty(Struct):
            v: object = seed

        assert NonEmpty().v is seed

        class UnhashableSelfie:
            def __hash__(self) -> int:
                raise TypeError("nope")

            def __deepcopy__(self, memo):
                return self

        value = UnhashableSelfie()

        class Probed(Struct):
            v: object = value

        assert Probed().v is value

    def test_a_caching_new_takes_the_base_copy_instead_of_aliasing(self):
        """A caching __new__ hands the same instance back; the constructor
        copy must not return it."""

        class Cached(list):
            def __new__(cls, *args):
                cached = getattr(cls, "_one", None)

                if cached is None:
                    cached = cls._one = super().__new__(cls)

                return cached

        declared = Cached()

        class Holder(Struct):
            v: object = declared

        assert Holder().v is not declared
        assert Holder().v is not Cached._one

    def test_a_subclass_re_probes_an_inherited_default_once_per_class(self):
        """The probe runs when the singleton is built, once per class
        statement -- the inherited default is re-copied for the subclass's own
        singleton. Instance construction re-probes the declared default, so
        the count here precedes any instantiation."""


        calls = []

        class Counted:
            def __hash__(self) -> int:
                calls.append(1)
                return 7

        class Base(Struct):
            v: object = Counted()

        class Child(Base):
            pass

        class Grandchild(Child):
            pass

        assert calls == [1, 1, 1]
        assert Grandchild().v is Base().v

    @recurses_through_c_until_recursion_error
    def test_a_value_that_holds_itself_is_shared_rather_than_refused(self):
        """A frozen struct pointing at itself, built through `set_field`, runs
        the hash out of stack instead of declining it -- so the probe never
        reaches an answer. Sharing is what it got before there was a probe, and
        a frozen struct is safe to share whatever it points at, itself included.
        """

        class Loop(Struct):
            v: object = None

        loop = Loop(None)
        set_field(loop, "v", loop)

        class Holder(Struct):
            w: object = loop

        assert Holder().w is loop

    @recurses_through_c_until_recursion_error
    def test_a_mutable_whose_own_hash_recurses_is_shared_too(self):
        """The price of sharing what the probe cannot classify, pinned rather
        than left to be discovered: this one really is the aliasing bug, and it
        gets through because a stack overflow is indistinguishable from the
        self-referential struct above.

        It is where the rule already stood -- a deque, an array and a
        defaultdict are shared while holding mutables for the same reason, by
        saying up front that they do not hash.
        """

        class RecursiveHash:
            def __init__(self) -> None:
                self.items: list[str] = []

            def __hash__(self) -> int:
                return hash(self)

        value = RecursiveHash()

        class Holder(Struct):
            v: object = value

        assert Holder().v is value

        value.items.append("seen by every instance")

        assert Holder().v.items == ["seen by every instance"]

    def test_a_hash_that_fails_for_its_own_reasons_propagates_unchanged(self):
        """The copy-time probe reads TypeError and ValueError as the instance
        declining. Anything else is not that, and salix says so with the
        author's own exception rather than claiming the value holds something
        mutable -- raised at the class statement, which is when the
        singleton build copies the default.
        """

        class Angry:
            def __hash__(self) -> int:
                raise RuntimeError("boom")

        with pytest.raises(RuntimeError, match="boom"):

            class Holder(Struct):
                v: object = Angry()

    def test_a_writable_memoryview_answers_ValueError_and_is_still_caught(self):
        """The trap: the probe is a hash that raises, and a writable memoryview
        raises ValueError where a tuple of lists raises TypeError. Both are read
        as the instance declining; a deepcopy then refuses the memoryview and
        the default is shared, so the construction still succeeds.
        """

        with pytest.raises(ValueError, match="cannot hash writable"):
            hash(memoryview(bytearray(b"abc")))

        class Holder(Struct):
            v: object = memoryview(bytearray(b"abc"))

        assert Holder().v is Holder().v

    def test_a_body_init_does_not_exempt_the_declared_default(self):
        """Its constructor never reads the default, so nothing is shared. The
        declaration is still a promise the class makes through
        _struct_defaults_, and the stored copy is severed from the declared
        object by the deep copy.
        """

        declared = ["a", "b"]

        class Holder(Struct, frozen=False):
            xs: list = declared

        class WithBodyInit(Struct, frozen=False):
            xs: list = declared

            def __init__(self) -> None:
                self.xs = []

        assert Holder._struct_defaults_[0] == declared
        assert Holder._struct_defaults_[0] is not declared
        assert WithBodyInit._struct_defaults_[0] == declared
        assert WithBodyInit._struct_defaults_[0] is not declared

    def test_a_subclass_of_a_mutable_builtin_is_copied(self):
        """#163: the boundary widened from the four exact types to their
        subclasses. A defaultdict's constructor is not the iterable one, so the
        copy falls back to the base copy and the factory is dropped -- the
        sharing bug is what the rule stops."""

        from collections import defaultdict

        class Holder(Struct):
            d: object = defaultdict(list)

        assert Holder().d is not Holder().d

    def test_an_inherited_default_is_copied_as_well(self):
        class Base(Struct, frozen=False):
            xs: list = []  # noqa: RUF012

        class Child(Base):
            y: int = 0

        first, second = Child(), Child()
        first.xs.append(1)

        assert second.xs == []

    @pytest.mark.parametrize(
        "value",
        [
            pytest.param([1, "two", 3.0, None, b"four"], id="a_list_of_atoms"),
            pytest.param({"a": 1, 2: "b", None: 3.0}, id="a_dict_of_atoms"),
            pytest.param({1, "two", 3.0}, id="a_set_of_atoms"),
            pytest.param([int, Point], id="a_list_of_classes"),
            pytest.param([1, (2, [3])], id="a_list_holding_a_tuple"),
            pytest.param({"k": [1]}, id="a_dict_holding_a_list"),
        ],
    )
    def test_a_default_copy_equals_what_copy_deepcopy_returns(self, value):
        class Holder(Struct):
            v: object = value

        first, second = Holder(), Holder()

        assert first.v is not second.v
        assert type(first.v) is type(value)
        assert first.v == copy.deepcopy(value)

    def test_an_inner_list_of_a_dict_default_is_not_shared(self):
        class Holder(Struct):
            v: dict = {"k": [1]}  # noqa: RUF012

        first, second = Holder(), Holder()
        first.v["k"].append(2)

        assert second.v == {"k": [1]}

    def test_a_set_default_iterates_in_the_order_copy_deepcopy_gives(self):
        class Holder(Struct):
            v: set = {0}  # noqa: RUF012

        stored = Holder._struct_defaults_[0]
        stored |= set(range(40))
        stored -= set(range(0, 40, 3))
        stored |= {64, 100, 128}

        assert list(Holder().v) == list(copy.deepcopy(stored))

    @pytest.mark.parametrize("kind", [list, dict], ids=lambda kind: kind.__name__)
    def test_a_registered_dispatch_copier_copies_the_default(self, kind, monkeypatch):
        class Holder(Struct):
            v: object = kind([("a", 1)]) if kind is dict else kind([1])

        monkeypatch.setitem(copy._deepcopy_dispatch, kind, lambda value, memo: kind())

        assert Holder().v == kind()

    @pytest.mark.parametrize(
        ("default", "calls"),
        [
            pytest.param([1, "a", None, 2.5], 0, id="atom_list"),
            pytest.param({"a": 1, "b": 2}, 0, id="atom_dict"),
            pytest.param({1, "b"}, 0, id="atom_set"),
            pytest.param([[1]], 1, id="nested_list"),
            pytest.param({"a": (1, "b")}, 1, id="tuple_in_dict"),
        ],
    )
    def test_a_default_of_atoms_is_copied_without_calling_deepcopy(self, default, calls):
        """Replacing copy.deepcopy turns the fast path off, so this counts calls
        to the real one with a profiler instead."""

        class Holder(Struct, frozen=False):
            v: object = default

        Holder()

        assert deepcopy_calls_from_outside_copy(Holder) == calls

    def test_a_registered_reductor_copies_a_set_default(self, monkeypatch):
        class Holder(Struct):
            v: set = {1, 2}  # noqa: RUF012

        monkeypatch.setitem(copyreg.dispatch_table, set, lambda value: (frozenset, (list(value),)))

        assert type(Holder().v) is frozenset

    def test_a_struct_default_holding_a_list_is_copied_deeply(self):
        class Inner(Struct):
            values: list = [1, 2, 3]  # noqa: RUF012

        class Holder(Struct):
            inner: Inner = Inner()

        first, second = Holder(), Holder()

        assert first.inner == second.inner == Inner()
        assert first.inner is not second.inner
        assert first.inner.values is not second.inner.values

    @pytest.mark.parametrize("frozen", [True, False], ids=["frozen", "mutable"])
    def test_a_struct_default_with_its_own_deepcopy_is_copied_by_it(self, frozen):
        calls: list[object] = []

        class Hooked(Struct, frozen=frozen):
            values: list

            def __deepcopy__(self, memo: dict) -> "Hooked":
                calls.append(self)
                return Hooked(list(self.values))

        class Holder(Struct):
            inner: Hooked = Hooked([1])

        calls.clear()
        Holder()

        assert len(calls) == 1

    def test_a_struct_default_whose_getattribute_sees_the_lookup_is_copied_through_it(self):
        looked_up: list[str] = []

        class Watched(Struct):
            values: list

            def __getattribute__(self, name: str) -> object:
                looked_up.append(name)
                return object.__getattribute__(self, name)

        class Holder(Struct):
            inner: Watched = Watched([1])

        looked_up.clear()
        Holder()

        assert "__deepcopy__" in looked_up

    def test_a_struct_default_with_an_instance_hook_is_copied_by_it(self):
        class Dicted:
            pass

        class WithDict(Struct, Dicted):
            values: list

        class Holder(Struct):
            inner: WithDict = WithDict([1])

        vars(Holder._struct_defaults_[0])["__deepcopy__"] = lambda memo: "from the instance"

        assert Holder().inner == "from the instance"

    def test_a_struct_default_with_a_registered_copier_is_copied_by_it(self, monkeypatch):
        class Inner(Struct):
            values: list

        class Holder(Struct):
            inner: Inner = Inner([1])

        monkeypatch.setitem(copy._deepcopy_dispatch, Inner, lambda value, memo: "registered")

        assert Holder().inner == "registered"

    def test_a_hashable_mutable_inside_a_set_default_is_not_shared(self):
        class Token:
            pass

        class Holder(Struct):
            v: set = {Token()}  # noqa: RUF012

        first, second = Holder(), Holder()

        assert next(iter(first.v)) is not next(iter(second.v))

    def test_an_author_hash_before_a_list_in_a_struct_default_still_propagates(self):
        class Angry:
            def __hash__(self) -> int:
                raise RuntimeError("boom")

        class Inner(Struct):
            first: object
            second: object

        with pytest.raises(RuntimeError, match="boom"):

            class Holder(Struct):
                inner: Inner = Inner(Angry(), [1])

    def test_a_struct_default_whose_fields_share_a_list_keeps_them_shared(self):
        class Pair(Struct):
            first: list
            second: list

        shared = [1]

        class Holder(Struct):
            pair: Pair = Pair(shared, shared)

        constructed = Holder()

        assert constructed.pair.first is constructed.pair.second
        assert constructed.pair.first is not Holder().pair.first

    def test_a_struct_default_that_reduces_itself_is_copied_through_the_reduction(self):
        class Reducing(Struct):
            values: list

            def __reduce__(self) -> tuple:
                return (Reducing, ([*self.values, 99],))

        class Holder(Struct):
            inner: Reducing = Reducing([1])

        assert Holder().inner == copy.deepcopy(Holder._struct_defaults_[0])

    def test_a_struct_default_with_a_registered_reductor_is_copied_by_it(self, monkeypatch):
        class Inner(Struct):
            values: list

        class Holder(Struct):
            inner: Inner = Inner([1])

        monkeypatch.setitem(copyreg.dispatch_table, Inner, lambda value: (Inner, ([7],)))

        assert Holder().inner == Inner([7])

    def test_a_struct_default_with_a_co_base_deepcopy_is_copied_by_it(self):
        class CoBase:
            __slots__ = ()

            def __deepcopy__(self, memo: dict) -> object:
                return WithCoBase([*self.values, 5])

        class WithCoBase(Struct, CoBase):
            values: list

        class Holder(Struct):
            inner: WithCoBase = WithCoBase([1])

        assert Holder().inner == copy.deepcopy(Holder._struct_defaults_[0])

    @pytest.mark.parametrize(
        "options",
        [{}, {"eq": False}],
        ids=["unhashable", "hashable_by_identity"],
    )
    def test_a_mutable_struct_default_is_copied_per_instance(self, options):
        class Inner(Struct, frozen=False, **options):
            x: int = 0
            values: list = []  # noqa: RUF012

        declared = Inner()

        class Holder(Struct, frozen=False):
            inner: Inner = declared

        first, second = Holder(), Holder()
        first.inner.x = 5
        first.inner.values.append(1)

        assert second.inner.x == 0
        assert second.inner.values == []
        assert first.inner is not declared
        assert second.inner is not declared
        assert Holder._struct_defaults_[0] is not declared

    def test_a_supplied_mutable_struct_is_stored_as_given(self):
        class Inner(Struct, frozen=False):
            x: int = 0

        class Holder(Struct, frozen=False):
            inner: Inner = Inner()

        given = Inner(3)

        assert Holder(given).inner is given
        assert Holder(inner=given).inner is given

    def test_a_mutable_struct_default_nested_in_a_mutable_struct_default_is_not_shared(self):
        class Run(Struct, frozen=False):
            dir: str = "outputs"

        class Job(Struct, frozen=False):
            run: Run = Run()

        class Config(Struct, frozen=False):
            job: Job = Job()

        first, second = Config(), Config()
        first.job.run.dir = "elsewhere"

        assert second.job.run.dir == "outputs"
        assert first.job is not second.job
        assert first.job.run is not second.job.run

    def test_a_mutable_struct_default_a_deepcopy_cannot_carry_is_shared(self):
        attempts: list[object] = []

        class Locked(Struct, frozen=False):
            lock: object

            def __deepcopy__(self, memo):
                attempts.append(self)
                raise TypeError("a lock cannot be deep-copied")

        declared = Locked(threading.Lock())

        class Holder(Struct, frozen=False):
            inner: Locked = declared

        attempts_before_construction = len(attempts)
        first, second = Holder(), Holder()

        assert first.inner is second.inner is declared
        assert Holder._struct_defaults_[0] is declared
        assert len(attempts) == attempts_before_construction + 2
        assert all(attempt is declared for attempt in attempts)

    def test_two_fields_declared_with_one_object_get_separate_copies(self):
        class Settings(Struct, frozen=False):
            verbose: bool = False

        shared_settings = Settings()
        shared_items = [1]

        class Aliased(Struct, frozen=False):
            first_settings: Settings = shared_settings
            second_settings: Settings = shared_settings
            first_items: list = shared_items
            second_items: list = shared_items

        stored = Aliased._struct_defaults_
        first, second = Aliased(), Aliased()

        assert stored[0] is not stored[1]
        assert stored[2] is not stored[3]
        assert first.first_settings is not first.second_settings
        assert first.first_items is not first.second_items
        assert first.first_settings is not second.first_settings
        assert first.first_items is not second.first_items
        assert all(
            settings is not shared_settings
            for settings in (stored[0], stored[1], first.first_settings, first.second_settings)
        )
        assert all(
            items is not shared_items
            for items in (stored[2], stored[3], first.first_items, first.second_items)
        )

    def test_a_struct_default_that_declares_no_hash_is_shared_with_the_mutable_struct_it_holds(self):
        class Inner(Struct, frozen=False):
            x: int = 0

        class Outer(Struct):
            inner: Inner = Inner()
            __hash__ = None

        class Holder(Struct, frozen=False):
            outer: Outer = Outer()

        assert Holder().outer is Holder().outer

    def test_a_list_beside_an_identity_hashed_mutable_struct_copies_both(self):
        class Inner(Struct, frozen=False, eq=False):
            x: int = 0

        class Outer(Struct):
            items: list = []  # noqa: RUF012
            inner: Inner = Inner()

        class Holder(Struct, frozen=False):
            outer: Outer = Outer()

        first, second = Holder(), Holder()

        assert first.outer.inner is not second.outer.inner
        assert first.outer.items is not second.outer.items

    def test_an_identity_hashed_struct_default_is_shared_with_the_list_it_holds(self):
        class Outer(Struct, eq=False):
            items: list = []  # noqa: RUF012

        class Holder(Struct, frozen=False):
            outer: Outer = Outer()

        first, second = Holder(), Holder()

        assert first.outer is second.outer
        assert first.outer.items is second.outer.items

    @pytest.mark.parametrize(("eq", "shared"), [(True, False), (False, True)], ids=["eq", "identity"])
    def test_a_mutable_struct_nested_in_a_tuple_default_follows_its_hash(self, eq, shared):
        class Inner(Struct, frozen=False, eq=eq):
            x: int = 0

        class Holder(Struct, frozen=False):
            pair: tuple = (1, Inner())

        assert (Holder().pair[1] is Holder().pair[1]) is shared

    @pytest.mark.parametrize(("eq", "shared"), [(True, False), (False, True)], ids=["eq", "identity"])
    def test_a_mutable_struct_nested_in_a_frozen_struct_default_follows_its_hash(self, eq, shared):
        class Inner(Struct, frozen=False, eq=eq):
            x: int = 0

        class Outer(Struct):
            inner: Inner = Inner()

        class Holder(Struct, frozen=False):
            outer: Outer = Outer()

        assert (Holder().outer.inner is Holder().outer.inner) is shared

    def test_a_frozen_struct_default_of_atoms_stays_shared(self):
        class Inner(Struct):
            x: int = 0

        class Holder(Struct, frozen=False):
            inner: Inner = Inner()

        assert Holder().inner is Holder().inner


def deepcopy_calls_from_outside_copy(operation: Callable[[], object]) -> int:
    calls = 0

    def profile(frame: FrameType, event: str, argument: object) -> None:
        nonlocal calls
        called_from_outside_copy = (
            frame.f_back is None or frame.f_back.f_code.co_filename != copy.__file__
        )
        if event == "call" and frame.f_code is copy.deepcopy.__code__ and called_from_outside_copy:
            calls += 1

    sys.setprofile(profile)
    try:
        operation()
    finally:
        sys.setprofile(None)

    return calls


def tried_while_its_class_is_created(window: str, attempt) -> list[object]:
    outcomes: list[object] = []

    def record(cls: type) -> None:
        try:
            outcomes.append(attempt(cls))
        except (TypeError, AttributeError) as error:
            outcomes.append(error)

    if window == "init_subclass":

        class Base(Struct, frozen=False):
            def __init_subclass__(cls, **keywords: object) -> None:
                super().__init_subclass__(**keywords)
                record(cls)

        class Fielded(Base):
            count: int = 1

    else:

        class Hook:
            def __set_name__(self, owner: type, name: str) -> None:
                record(owner)

        class Fielded(Struct, frozen=False):
            count: int = 1
            hook = Hook()

    assert Fielded(count=5).count == 5

    return outcomes


def subclassed_by_a_class_statement(cls: type) -> type:
    class Kid(cls):
        pass

    return Kid


@pytest.mark.parametrize("window", ["init_subclass", "set_name"])
@pytest.mark.parametrize(
    ("attempt", "refusal"),
    [
        pytest.param(lambda cls: cls(), "create an instance of", id="call"),
        pytest.param(lambda cls: cls(count=5), "create an instance of", id="call-with-a-field"),
        pytest.param(lambda cls: from_mapping(cls, {"count": 5}), "create an instance of", id="from_mapping"),
        pytest.param(lambda cls: type("Kid", (cls,), {}), "subclass", id="type"),
        pytest.param(subclassed_by_a_class_statement, "subclass", id="class-statement"),
    ],
)
def test_a_class_still_being_created_refuses_instances_and_subclasses(window, attempt, refusal):
    outcomes = tried_while_its_class_is_created(window, attempt)

    assert len(outcomes) == 1, outcomes
    assert isinstance(outcomes[0], TypeError)
    assert str(outcomes[0]) == f"cannot {refusal} 'Fielded' until its class has finished being created"


@pytest.mark.parametrize("window", ["init_subclass", "set_name"])
@pytest.mark.parametrize("on_the_instance", [False, True], ids=["class", "instance"])
@pytest.mark.parametrize(
    "name",
    [
        "_struct_fields_",
        "__struct_fields__",
        "_struct_defaults_",
        "__struct_defaults__",
        "_struct_annotations_",
        "__struct_annotations__",
        "_struct_metadata_",
        "__struct_metadata__",
    ],
)
def test_a_class_still_being_created_has_no_field_tables_to_read(window, on_the_instance, name):
    outcomes = tried_while_its_class_is_created(
        window, lambda cls: getattr(cls.__new__(cls) if on_the_instance else cls, name)
    )

    assert len(outcomes) == 1, outcomes
    assert isinstance(outcomes[0], AttributeError)
    assert str(outcomes[0]) == "the fields of 'Fielded' cannot be read until its class has finished being created"


@pytest.mark.parametrize(
    ("read", "expected"),
    [
        (lambda self: type(self).__struct_fields__, ()),
        (fields, ()),
        (to_dict, {}),
    ],
    ids=["__struct_fields__", "fields", "to_dict"],
)
def test_the_post_init_of_a_frozen_fieldless_singleton_reads_its_finished_class(read, expected):
    seen = []

    class Tag(Struct, frozen=True):
        def __post_init__(self):
            seen.append(read(self))

    assert Tag() is Tag()
    assert seen == [expected]


def outcome_on_another_thread(attempt):
    outcomes = []

    def record():
        try:
            outcomes.append(attempt())
        except (TypeError, AttributeError) as error:
            outcomes.append(error)

    thread = threading.Thread(target=record)
    thread.start()
    thread.join()

    assert len(outcomes) == 1, outcomes
    return outcomes[0]


def refused_as_unfinished(outcome):
    return (
        isinstance(outcome, (TypeError, AttributeError))
        and "until its class has finished being created" in str(outcome)
    )


@starts_a_thread
@pytest.mark.parametrize("copies_the_namespace", [False, True], ids=["namespace-passed-on", "namespace-copied"])
def test_a_class_handed_to_a_metaclass_new_is_its_builders_alone_until_the_build_returns(copies_the_namespace):
    inside = {}

    class Peeking(type(Struct)):
        def __new__(mcs, name, bases, namespace, **keywords):
            cls = super().__new__(mcs, name, bases, dict(namespace) if copies_the_namespace else namespace, **keywords)

            if name == "Fielded":
                inside["builder"] = tuple(field.name for field in fields(cls))
                inside["another thread"] = outcome_on_another_thread(lambda: fields(cls))

            return cls

    class Base(Struct, metaclass=Peeking):
        pass

    built = type(Struct)("Fielded", (Base,), {"__annotations__": {"count": int}, "count": 1})

    assert inside["builder"] == ("count",)
    assert refused_as_unfinished(inside["another thread"]), inside["another thread"]
    assert outcome_on_another_thread(lambda: built().count) == 1


@starts_a_thread
def test_a_struct_class_built_inside_a_metaclass_new_is_created_once_the_outer_build_returns():
    helpers = []

    class Building(type(Struct)):
        def __new__(mcs, name, bases, namespace, **keywords):
            if name == "Outer":
                helpers.append(type(Struct)("Helper", (Struct,), {"__annotations__": {"x": int}}))

            return super().__new__(mcs, name, bases, namespace, **keywords)

    class Base(Struct, metaclass=Building):
        pass

    type(Struct)("Outer", (Base,), {"__annotations__": {"count": int}})

    assert len(helpers) == 1, helpers
    assert outcome_on_another_thread(lambda: helpers[0](x=3).x) == 3


@starts_a_thread
def test_another_thread_cannot_call_a_singleton_class_until_its_singleton_is_stored():
    during_post_init = []

    class Tag(Struct, frozen=True):
        def __post_init__(self):
            if not during_post_init:
                during_post_init.append(outcome_on_another_thread(type(self)))

    assert refused_as_unfinished(during_post_init[0]), during_post_init
    assert Tag() is Tag()


@starts_a_thread
def test_a_struct_class_built_inside_a_failing_metaclass_new_is_still_created():
    helpers = []

    class GivingUp(type(Struct)):
        def __new__(mcs, name, bases, namespace, **keywords):
            if name == "Outer":
                helpers.append(type(Struct)("Helper", (Struct,), {"__annotations__": {"x": int}}))
                raise LookupError("the metaclass gave up")

            return super().__new__(mcs, name, bases, namespace, **keywords)

    class Base(Struct, metaclass=GivingUp):
        pass

    with pytest.raises(LookupError):
        type(Struct)("Outer", (Base,), {"__annotations__": {"count": int}})

    assert len(helpers) == 1, helpers
    assert outcome_on_another_thread(lambda: helpers[0](x=3).x) == 3


def test_a_class_whose_build_failed_is_refused_even_on_the_thread_that_built_it():
    escaped = []

    class Escaping(Struct, frozen=True):
        def __init_subclass__(cls, **keywords):
            super().__init_subclass__(**keywords)
            escaped.append(cls)

    with pytest.raises(LookupError):

        class Failing(Escaping):
            def __post_init__(self):
                raise LookupError("the singleton refused")

    assert len(escaped) == 1, escaped

    with pytest.raises(TypeError, match="until its class has finished being created"):
        fields(escaped[0])
