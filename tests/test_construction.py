import pytest
from values import COPIED_WHEN_EMPTY, NON_EMPTY

from salix import Struct, set_field


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

    def test_a_non_empty_container_is_deep_copied(self):
        """#172: a shallow copy would share the inner list, so the default is
        deep-copied per instance instead of refused.
        """

        class Nested(Struct):
            xs: list = [[1]]  # noqa: RUF012 -- the copy is the assertion

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
            pytest.param(Mutable(1), id="a_mutable_struct"),
        ],
    )
    def test_a_mutable_container_outside_the_four_is_shared_and_not_refused(self, value):
        """The boundary is the four types and their subclasses, not mutability,
        so these are neither copied nor refused. Every one of these declares
        `__hash__` is None -- it says it does not hash before being asked --
        which is what #51's rule leaves alone; salix's own `frozen=False`
        struct included, since `eq` without `frozen` sets `__hash__` to None.

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

    def test_a_deepcopy_that_returns_its_argument_falls_back_to_sharing(self):
        """A __deepcopy__ returning self is the refusal by protocol; the
        stored default stays the declared object rather than masquerading as
        a severed copy. Both the size-gated path and the probe path share."""

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
            xs: list = []  # noqa: RUF012 -- the copy is the feature under test

        class Child(Base):
            y: int = 0

        first, second = Child(), Child()
        first.xs.append(1)

        assert second.xs == []
