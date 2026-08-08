"""Methods on a struct, in the shapes people actually write them.

A struct's body is an ordinary class body, and the methods it defines are kept
-- except a name that collides with a field, which is not kept and not simply
lost either: it becomes that field's default. These are the codec-shaped methods
that motivate reaching for a struct in the first place -- `to_bytes`,
`to_string`, a `from_bytes` classmethod -- plus the descriptors that sit
alongside them, and the dunders that salix would otherwise generate.

Methods are the subject. `__slots__` is salix's whatever the body says, and
`__match_args__` is unless the class opts out -- which means salix writes none
rather than that the class has none, since `Struct`'s own empty tuple is still
in the MRO. `TestBindingsSalixOwns` pins all of that.
"""

import dataclasses
import functools
import struct as struct_module
import sys
import weakref
from typing import NamedTuple

import pytest

from salix import Struct, set_field

HEADER = struct_module.Struct("<HBB")


class Frame(Struct):
    """A wire frame, of the kind a firmware protocol hands around."""

    length: int
    kind: int
    flags: int = 0

    def to_bytes(self) -> bytes:
        return HEADER.pack(self.length, self.kind, self.flags)

    def to_string(self) -> str:
        return f"{self.kind:02x}:{self.length:04x}:{self.flags:02x}"

    @classmethod
    def from_bytes(cls, payload: bytes) -> "Frame":
        return cls(*HEADER.unpack(payload))

    @classmethod
    def empty(cls) -> "Frame":
        return cls(0, 0)

    @staticmethod
    def header_size() -> int:
        return HEADER.size

    @property
    def is_empty(self) -> bool:
        return self.length == 0

    def __len__(self) -> int:
        return self.length


class TestMethods:
    def test_an_instance_method_reads_the_fields(self):
        """The expected bytes written out rather than re-derived from `to_bytes`'
        own expression, so a wrong field order fails here instead of shifting
        both sides together. An inert method returning the same constant would
        pass; `test_a_round_trip_returns_an_equal_struct` is what needs the
        fields.
        """

        assert Frame(258, 7, 1).to_bytes() == b"\x02\x01\x07\x01"

    def test_a_second_instance_method_is_not_displaced_by_the_first(self):
        assert Frame(258, 7, 1).to_string() == "07:0102:01"

    def test_a_classmethod_constructs_through_the_generated_constructor(self):
        """Fed the same independent bytes `to_bytes` is checked against, so the
        two directions are pinned separately rather than against each other.
        """

        frame = Frame.from_bytes(b"\x02\x01\x07\x01")

        assert (frame.length, frame.kind, frame.flags) == (258, 7, 1)

    def test_a_round_trip_returns_an_equal_struct(self):
        """Composing pack with unpack holds by the struct module's own inverse
        property, so this pins the classmethod reaching `__eq__` with a real
        instance and nothing about the wire format. What pins the format is the
        pair that check `to_bytes` and `from_bytes` against independent bytes.
        """

        original = Frame(4096, 3, 2)

        assert Frame.from_bytes(original.to_bytes()) == original

    def test_a_classmethod_taking_no_arguments_works_too(self):
        assert Frame.empty() == Frame(0, 0, 0)

    def test_a_staticmethod_needs_no_instance(self):
        """`<HBB` is four bytes; comparing against HEADER.size would compare the
        method's own return value with itself.
        """

        assert Frame.header_size() == 4
        assert Frame(1, 1).header_size() == 4

    def test_a_property_reads_the_fields(self):
        assert Frame(0, 0).is_empty
        assert not Frame(1, 0).is_empty

    def test_a_dunder_the_body_defines_is_kept(self):
        frame = Frame(258, 7)

        assert (len(frame), frame.kind) == (258, 7)

    def test_a_body_repr_and_init_displace_the_generated_ones(self):
        """Two different mechanisms reaching the same place. `__repr__` is a
        dunder the mixin binds and the body's rebinding wins in the MRO;
        `__init__` is not a dunder salix writes at all -- the constructor is a
        vectorcall, and `defines_own_init` drops it for `PyType_GenericNew` when
        the body defines one.

        `set_field` rather than `self.x = ...` because the struct is frozen by
        default, and the frozen wall does not open for its own `__init__` --
        test_custom_init.py pins both halves of that.
        """

        class Custom(Struct):
            x: int

            def __repr__(self) -> str:
                return "custom repr"

            def __init__(self, x: int) -> None:
                set_field(self, "x", x * 10)

        instance = Custom(1)

        assert repr(instance) == "custom repr"
        assert instance.x == 10
        assert instance == Custom(1)
        assert hash(instance) == hash((10,))

    @staticmethod
    def loose_equality():
        class Loose(Struct):
            x: int

            def __eq__(self, other: object) -> bool:
                return isinstance(other, Loose)

            def __hash__(self) -> int:
                return 0

        return Loose

    def test_a_body_eq_and_hash_displace_the_generated_ones(self):
        """The header says the body keeps the dunders salix would have written,
        and these are the two a body is most likely to want back.
        """

        Loose = self.loose_equality()
        first, second = Loose(1), Loose(2)

        assert first == second
        assert hash(first) == hash(Loose(99)) == 0
        assert len({first, second}) == 1

    def test_a_body_eq_carries_ne_with_it(self):
        """The mixin is a base, so its structural __ne__ is what the lookup
        found beside a body __eq__ that answered a different question, and
        `a == b` and `a != b` were both true. #58.
        """

        Loose = self.loose_equality()

        assert (Loose(1) != Loose(2)) is False

    def test_a_subclass_of_it_derives_ne_the_same_way(self):
        """A subclass writing nothing about equality inherits both halves.

        A regression guard rather than a proof of where the binding goes: the
        assertion holds whether the derived __ne__ sits in Loose or in Child,
        since either inverts the same inherited __eq__. What pins the location
        is the class dict, asserted below.
        """

        Loose = self.loose_equality()

        class Child(Loose):
            y: int = 0

        assert (Child(1) == Child(2), Child(1) != Child(2)) == (True, False)
        assert "__ne__" in vars(Loose)
        assert "__ne__" not in vars(Child)

    def test_a_body_eq_carries_it_through_an_eq_option_change(self):
        """The order the two rebinds run in, which nothing structural enforces.

        `rebind` skips a name already in the namespace, so a comparison rebind
        running first would put the mixin's structural __ne__ in beside the
        body's __eq__ and bind_not_equal would then skip it -- #58 through the
        other door. Only a class that changes the eq option reaches that rebind
        at all, which is why the tests above cannot see the ordering.

        The two halves are not equal witnesses. OptedBackIn is the one that
        observes the order; OptedOut answers correctly whichever way round the
        two run, because `rebind(..., false)` installs object's __ne__ for
        eq=False anyway. It is here to cover the eq=False direction at all.

        Its answer assertion is what pins that direction -- without the binding
        the MRO reaches the mixin's structural __ne__ and returns True. The
        `vars()` assertions below add only *where* the binding sits, which the
        answers cannot show.
        """

        class OptedOut(Struct, eq=False):
            x: int

            def __eq__(self, other: object) -> bool:
                return isinstance(other, OptedOut)

            def __hash__(self) -> int:
                return 0

        class OptedBackIn(OptedOut, eq=True):
            y: int = 0

            def __eq__(self, other: object) -> bool:
                return isinstance(other, OptedBackIn)

            def __hash__(self) -> int:
                return 0

        assert (OptedOut(1) == OptedOut(2), OptedOut(1) != OptedOut(2)) == (True, False)
        assert (OptedBackIn(1) == OptedBackIn(2), OptedBackIn(1) != OptedBackIn(2)) == (True, False)
        assert "__ne__" in vars(OptedOut)
        assert "__ne__" in vars(OptedBackIn)

    def test_a_body_ne_is_kept_over_the_derived_one(self):
        """Deriving is what a body that says nothing about != asks for."""

        class Both(Struct):
            x: int

            def __eq__(self, other: object) -> bool:
                return isinstance(other, Both)

            def __ne__(self, other: object) -> str:  # type: ignore[override]
                return "the body's"

            def __hash__(self) -> int:
                return 0

        assert (Both(1) != Both(2)) == "the body's"

    def test_a_struct_that_leaves_equality_to_salix_is_unaffected(self):
        """The dict is what says so, not the answers.

        object.__ne__ re-dispatches through the mixin's own tp_richcompare, so
        a structural struct answers identically whether the binding is there or
        not -- an unconditional bind_not_equal passes every assertion below the
        first. The gate is only observable in vars().
        """

        class Structural(Struct):
            x: int

        assert "__ne__" not in vars(Structural)
        assert (Structural(1) != Structural(2), Structural(1) != Structural(1)) == (True, False)

    def test_a_co_base_s_ne_is_shadowed_and_that_is_where_47_lives(self):
        """Pinned as it is rather than as plain Python has it.

        Binding into the dict puts the derived __ne__ ahead of a co-base's,
        which plain Python would let win -- there the derived one is the end of
        the MRO. The mixin's structural __ne__ already displaced a co-base's
        before this, so what changed is which answer wins; #47 is the issue for
        a non-struct base's comparison dunders being shadowed.
        """

        class Taggable:
            def __ne__(self, other: object) -> str:  # type: ignore[override]
                return "the co-base's"

        class Mixed(Struct, Taggable):
            x: int

            def __eq__(self, other: object) -> bool:
                return isinstance(other, Mixed)

            def __hash__(self) -> int:
                return 0

        assert (Mixed(1) != Mixed(2)) is False

    def test_every_other_construction_derives_ne_from_eq(self):
        """The parity claim the tests above rest on, asserted rather than
        described -- this file has been wrong about a sibling's behaviour more
        than once.
        """

        @dataclasses.dataclass(eq=False)
        class Data:
            x: int

            def __eq__(self, other: object) -> bool:
                return isinstance(other, Data)

            def __hash__(self) -> int:
                return 0

        class Plain:
            def __init__(self, x: int) -> None:
                self.x = x

            def __eq__(self, other: object) -> bool:
                return isinstance(other, Plain)

            def __hash__(self) -> int:
                return 0

        assert (Data(1) != Data(2)) is False
        assert (Plain(1) != Plain(2)) is False

    def test_the_generated_dunders_survive_alongside_them(self):
        frame = Frame(258, 7, 1)

        assert repr(frame) == "Frame(length=258, kind=7, flags=1)"
        assert frame == Frame(258, 7, 1)
        assert hash(frame) == hash((258, 7, 1))

    def test_the_docstring_the_body_wrote_is_the_class_docstring(self):
        assert Frame.__doc__ is not None
        assert "wire frame" in Frame.__doc__


class TestInheritedMethods:
    """`Tagged` is the one subclass these share: declared once, so a change to
    Frame's fields lands in one place.
    """

    @staticmethod
    def tagged_subclass():
        class Tagged(Frame):
            tag: int = 0

        return Tagged

    def test_a_subclass_inherits_them(self):
        tagged = self.tagged_subclass()(258, 7, 1, 9)

        assert tagged.tag == 9
        assert tagged.header_size() == 4
        assert tagged.is_empty is False
        assert tagged.to_string() == "07:0102:01"

    def test_a_subclass_may_override_one(self):
        class Loud(Frame):
            def to_string(self) -> str:
                return super().to_string().upper()

        assert Loud(258, 10, 1).to_string() == "0A:0102:01"

    def test_a_classmethod_on_a_subclass_builds_the_subclass(self):
        Tagged = self.tagged_subclass()
        built = Tagged.empty()

        assert isinstance(built, Tagged)
        assert built.tag == 0


class TestCaching:
    """Where a computed value can live, given no instance dict.

    Filling a declared field from `__post_init__` with `set_field` is the other
    answer and the one salix supports; test_post_init.py pins it, including that
    the hook runs exactly once per construction.
    """

    @staticmethod
    def counted_cache():
        """A fresh struct and the call log of its cached method. Fresh per test,
        because the cache holds its keys -- the instances -- alive, so entries
        one test made would still answer for the next.
        """

        calls: list[int] = []

        class Computed(Struct):
            x: int

            @functools.cache  # noqa: B019 -- the lifetime cost is the point
            def slow(self) -> int:
                calls.append(1)
                return self.x * 100

        return Computed, calls

    def test_functools_cached_property_is_not_supported(self):
        """A struct's fields are its slots and it has no instance dict, so there
        is nowhere for cached_property to put the value.

        The siblings are asserted below rather than described here, because
        every parity claim this file made in prose has turned out to be wrong at
        least once.
        """

        class Cached(Struct):
            x: int

            @functools.cached_property
            def expensive(self) -> int:
                return self.x * 100

        with pytest.raises(TypeError, match="No '__dict__' attribute"):
            _ = Cached(2).expensive

    def test_a_slotted_dataclass_fails_the_same_way(self):
        @dataclasses.dataclass(slots=True)
        class Cached:
            x: int

            @functools.cached_property
            def expensive(self) -> int:
                return self.x * 100

        with pytest.raises(TypeError, match="No '__dict__' attribute"):
            _ = Cached(2).expensive

    def test_a_namedtuple_fails_too_for_its_own_reason_below_3_13(self):
        """Below 3.13 the NamedTuple machinery never calls `__set_name__`, so
        the descriptor complains about that rather than about the missing dict.
        """

        class Cached(NamedTuple):
            x: int

            @functools.cached_property
            def expensive(self) -> int:
                return self.x * 100

        expected = (
            "No '__dict__' attribute"
            if sys.version_info >= (3, 13)
            else "without calling __set_name__"
        )

        with pytest.raises(TypeError, match=expected):
            _ = Cached(2).expensive

    def test_only_a_dataclass_carrying_a_dict_succeeds(self):
        @dataclasses.dataclass
        class Cached:
            x: int

            @functools.cached_property
            def expensive(self) -> int:
                return self.x * 100

        assert Cached(2).expensive == 200

    def test_functools_cache_on_a_method_still_works(self):
        """It caches on the function rather than the instance, so the absent
        __dict__ is not in its way -- at the cost of holding the struct alive.
        """

        Computed, calls = self.counted_cache()
        instance = Computed(2)

        assert instance.slow() == 200
        assert instance.slow() == 200
        assert len(calls) == 1

    def test_that_cache_is_shared_by_every_equal_instance(self):
        """The entry is keyed by `self`, and a struct hashes by field values, so
        two structs that compare equal are one key. Right for a pure function of
        the fields, and wrong the moment the method reads anything else.
        """

        Computed, calls = self.counted_cache()
        first, second = Computed(2), Computed(2)

        assert first is not second
        assert first.slow() == 200
        assert second.slow() == 200
        assert len(calls) == 1


class TestBindingsSalixOwns:
    """Not every body binding is the body's to keep, and neither of these
    collides with a field name. `__slots__` is taken unconditionally, so an
    entry that is not a field is refused rather than dropped -- #12.
    `__match_args__` is taken only while the class wants one.
    """

    def test_a_body_slot_that_is_not_a_field_is_refused(self):
        with pytest.raises(TypeError, match="'extra', which is not a field"):

            class Slotted(Struct):
                x: int
                __slots__ = ("extra",)

    def test_a_bare_string_slots_is_refused_too(self):
        """`__slots__ = "extra"` is one name to `type.__new__`, not five
        characters, so it has to be read the same way here.
        """

        with pytest.raises(TypeError, match="'extra', which is not a field"):

            class Stringy(Struct):
                x: int
                __slots__ = "extra"

    def test_a_body_slots_naming_only_fields_is_accepted(self):
        """Refused when an entry would be *lost*, not merely when `__slots__` is
        present. salix writes the field tuple over it, so a body that names the
        fields loses nothing -- and the transformed namespace re-enters class
        creation when a delegating metaclass wins, carrying salix's own
        `__slots__` with it. `test_struct_identity.py` covers that path.
        """

        class Agreeing(Struct):
            x: int
            __slots__ = ("x",)

        assert Agreeing.__slots__ == ("x",)
        assert Agreeing(1).x == 1

    def test_weakref_may_be_named_because_salix_writes_it(self):
        """`held` rather than a temporary: `weakref.ref(Weak(1))()` answers None,
        because the argument is decref'd when the call returns and nothing else
        holds it. It passes under pytest anyway -- assertion rewriting binds the
        intermediate to a local -- and fails under `--assert=plain`, so the
        temporary form would have been green for a reason unrelated to weakref.
        """

        class Weak(Struct, weakref=True):
            x: int
            __slots__ = ("__weakref__", "x")

        held = Weak(1)

        assert weakref.ref(held)() is held

    def test_naming_weakref_without_asking_for_it_is_refused(self):
        """salix writes `__weakref__` only for a class that passes
        `weakref=True`, so without it the entry is one that would be dropped --
        and dropping it leaves a class that cannot be weak-referenced at all.
        """

        with pytest.raises(TypeError, match="carries no weakref slot to name"):

            class Unasked(Struct):
                x: int
                __slots__ = ("__weakref__", "x")

    def test_an_inherited_weakref_slot_exempts_it_too(self):
        """The other half of the exemption: the class need not ask for the slot
        if a base already has one, because then salix drops nothing.
        """

        class Referenceable(Struct, weakref=True):
            pass

        class Child(Referenceable):
            x: int = 0
            __slots__ = ("__weakref__",)

        held = Child(1)

        assert weakref.ref(held)() is held

    def test_the_base_with_the_slot_need_not_be_the_layout_base(self):
        """`has_weakref_slot` asks the widest struct base, which is the one
        CPython gives `tp_base`. The slot comes from *any* base, so asking only
        that one refused a class that is weak-referenceable without the entry --
        and the same class one base-order over was accepted.
        """

        class Referenceable(Struct, weakref=True):
            pass

        class Wider(Struct):
            a: int
            b: int

        class Both(Wider, Referenceable):
            __slots__ = ("__weakref__",)

        held = Both(1, 2)

        assert weakref.ref(held)() is held

    def test_a_slot_naming_an_inherited_field_is_accepted(self):
        """Refused when an entry would be lost, and an inherited field's slot is
        already on the base -- salix writes only this class's new fields, so
        naming an inherited one loses nothing.
        """

        class Base(Struct):
            x: int

        class Child(Base):
            y: int = 0
            __slots__ = ("x",)

        assert Child(1).x == 1

    def test_a_non_string_slot_entry_is_refused(self):
        """Membership would otherwise be decided by `__eq__`, so an object that
        compares equal to a field name would pass the check and then be dropped
        by `type.__new__` -- and one whose `__eq__` raises would replace the
        refusal with its own error.
        """

        with pytest.raises(TypeError, match="__slots__ items must be strings"):

            class Numbered(Struct):
                x: int
                __slots__ = (1,)

    def test_a_body_match_args_is_replaced_by_the_fields(self):
        class Matched(Struct):
            x: int
            __match_args__ = ("nope",)

        assert Matched.__match_args__ == ("x",)

    def test_opting_out_of_match_args_leaves_the_body_its_own(self):
        """`match_args=False` means salix writes none into this namespace, so
        the body keeps what it wrote.

        Not that the class has none: `Struct` itself carries a generated `()`,
        which every subclass sees through the MRO, so a struct that opts out and
        writes nothing still reports `()`. `dataclass(match_args=False)` has no
        attribute at all. Both asserted below.
        """

        class Matched(Struct, match_args=False):
            x: int
            __match_args__ = ("nope",)
            __slots__ = ("x",)

        class Silent(Struct, match_args=False):
            x: int

        assert Matched.__match_args__ == ("nope",)
        assert Matched.__slots__ == ("x",)
        assert Silent.__match_args__ == ()
        assert Struct.__match_args__ == ()


class TestNameCollisions:
    """A method named after a field is not discarded -- it becomes that field's
    default, which is worse. `append_declared` reads the class-body value bound
    to the field name, and a `def` is a class-body value bound to a name.

    So the class builds, `Collide.x` is the slot descriptor, and `Collide()`
    hands back an instance whose int field holds a function. #54 is the issue.
    """

    @staticmethod
    def colliding_method():
        class Collide(Struct):
            x: int

            def x(self) -> str:
                return "method"

        return Collide

    def test_the_method_is_gone_from_the_class(self):
        Collide = self.colliding_method()

        assert type(Collide.x).__name__ == "member_descriptor"
        assert Collide(1).x == 1

    def test_but_it_became_the_default_and_the_class_is_constructible(self):
        """The half `Collide(1).x == 1` cannot see. Constructing with no
        argument is accepted, because the field now has a default.
        """

        Collide = self.colliding_method()
        (default,) = Collide._struct_defaults_

        assert callable(default)
        assert Collide().x is default

    def test_a_property_collides_the_same_way(self):
        class CollideProp(Struct):
            y: int

            @property
            def y(self) -> str:
                return "property"

        (default,) = CollideProp._struct_defaults_

        assert isinstance(default, property)
        assert CollideProp().y is default
        assert CollideProp(1).y == 1
