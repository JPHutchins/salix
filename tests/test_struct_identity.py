import collections.abc
import weakref

import pytest

import salix
from salix import Struct

MIXIN = Struct.__mro__[1]
META = type(Struct)


class Forwarding(META):
    def __new__(metacls, name, bases, namespace, **keywords):
        return super().__new__(metacls, name, bases, namespace, **keywords)


class Plain(META):
    def __new__(metacls, name, bases, namespace):
        return super().__new__(metacls, name, bases, namespace)

# Both spellings of both, in one place: two literals drifted apart is a test
# that silently stops covering a name.
METADATA_NAMES = (
    "_struct_fields_",
    "_struct_defaults_",
    "__struct_fields__",
    "__struct_defaults__",
)


class Point(Struct):
    x: int


class Ordered(Struct, order=True):
    x: int


@pytest.fixture
def impostor():
    """A mixin subclass. The co-base supplies the tp_new the mixin withholds,
    which is what makes one of these constructible at all.
    """

    return type("Impostor", (MIXIN, list), {"__slots__": ("z",)})()


class TestAMixinSubclass:
    def test_a_struct_does_not_compare_equal_to_one(self, impostor):
        assert Point(1) != impostor
        assert impostor != Point(1)
        assert (Point(1) == impostor) is False

    def test_it_does_not_order_against_a_struct(self, impostor):
        with pytest.raises(TypeError):
            _ = Ordered(1) < impostor

        with pytest.raises(TypeError):
            _ = impostor < Ordered(1)

    def test_it_reprs_as_the_object_it_is(self, impostor):
        assert "Impostor object at" in repr(impostor)

    def test_it_is_unhashable(self, impostor):
        """#66: it is compared by its co-base, because rich_compare answers
        NotImplemented and the reflected operation gets there. An identity hash
        beside that made it equal to a value it could not be looked up beside,
        so hashing refuses instead and the pair is consistent again.
        """

        with pytest.raises(TypeError, match="unhashable type: 'Impostor'"):
            hash(impostor)

    def test_it_is_an_instance_of_Hashable_all_the_same(self, impostor):
        """The ABC asks whether tp_hash is non-NULL rather than calling it, and
        the mixin's is -- the slot every struct hashes through. So the operator
        and `isinstance` disagree, which is what a writable memoryview does too.
        """

        assert isinstance(impostor, collections.abc.Hashable)

        view = memoryview(bytearray(b"ab"))

        assert isinstance(view, collections.abc.Hashable)

        with pytest.raises(ValueError, match="cannot hash writable memoryview"):
            hash(view)

    def test_equality_between_two_of_them_is_intransitive(self):
        """A consequence of the fallback policy rather than a decision of its
        own, and #66's fix does not change it: `rich_compare` answers
        NotImplemented for a non-struct, so two content-equal impostors fall
        back to identity, while each compares equal to the plain value they
        wrap through the co-base's reflected `__eq__`.
        """

        pair = type("Tupleish", (MIXIN, tuple), {"__slots__": ()})
        left, right = pair((1, 2)), pair((1, 2))

        assert left == (1, 2)
        assert right == (1, 2)
        assert left != right

    def test_the_co_base_setter_is_overridden_rather_than_deferred_to(self):
        """Setattr falls back to object's, which for a co-base that defines its
        own `__setattr__` means overriding it rather than falling back: the
        write lands and the co-base never sees it. Same trade as repr.
        """

        seen = []

        class Recording(list):
            def __setattr__(self, name, value):
                seen.append(name)
                object.__setattr__(self, name, value)

        impostor = type("Sub", (MIXIN, Recording), {"__slots__": ("z",)})()
        impostor.z = 1

        assert impostor.z == 1
        assert seen == []

        Recording().z = 1

        assert seen == ["z"]

    def test_it_cannot_be_equal_to_something_it_hashes_differently_from(self):
        """The contract the fallback used to break, over a co-base that is
        itself a value type: `==` said yes and the dict said no.
        """

        equal_to_a_tuple = type("Tupleish", (MIXIN, tuple), {"__slots__": ()})((1, 2))

        assert equal_to_a_tuple == (1, 2)

        with pytest.raises(TypeError, match="unhashable type"):
            {(1, 2): "from the tuple"}[equal_to_a_tuple]

    def test_it_is_not_frozen(self, impostor):
        impostor.z = 1

        assert impostor.z == 1

    @pytest.mark.parametrize("attribute", METADATA_NAMES)
    def test_it_has_no_metadata_to_report(self, impostor, attribute):
        """An AttributeError rather than a TypeError, because that is what the
        two facilities for asking whether an attribute is there catch.
        """

        with pytest.raises(AttributeError, match=f"{attribute} is defined on structs"):
            getattr(impostor, attribute)

        assert not hasattr(impostor, attribute)
        assert getattr(impostor, attribute, "absent") == "absent"

    def test_set_field_still_refuses_it(self, impostor):
        with pytest.raises(TypeError, match="expects a struct"):
            salix.set_field(impostor, "z", 1)


class TestTheMetaclassWithoutTheMixin:
    def test_the_module_does_not_offer_it(self):
        """#15: reaching the metaclass takes `type(Struct)`, which is what the
        rest of this class does. What is gone is the invitation to.

        The second assertion is the relationship rather than the spelling: the
        name a repr shows is not a name the module answers to, whatever that
        name is later changed to.
        """

        assert not hasattr(salix, "StructMeta")
        assert not hasattr(salix, META.__name__)

    def test_a_class_body_naming_it_is_refused(self):
        with pytest.raises(TypeError, match="a struct class inherits salix"):

            class Bare(metaclass=META):
                x: int

    def test_calling_it_with_no_bases_is_refused(self):
        with pytest.raises(TypeError, match="a struct class inherits salix"):
            META("Bare", (), {})

    def test_a_co_base_that_is_not_a_struct_does_not_smuggle_one_through(self):
        with pytest.raises(TypeError, match="a struct class inherits salix"):
            META("Bare", (list,), {})

    def test_calling_it_with_a_struct_base_still_works(self):
        built = META("Extended", (Point,), {"__annotations__": {"y": int}})

        assert built._struct_fields_ == ("x", "y")
        assert built(1, 2) == built(1, 2)

    @pytest.mark.parametrize("attribute", METADATA_NAMES)
    def test_its_own_metadata_getters_are_never_handed_the_metaclass(self, attribute):
        """They read fields that live past the end of a PyTypeObject, so what
        they are handed has to be an instance of the metaclass -- sized by its
        basicsize -- and never the metaclass itself, which is an instance of
        `type` and shorter by the difference the third assertion names.

        Two things keep it that way, and neither is salix's: `type(META)` is
        `type`, so an attribute lookup on META finds the descriptor and returns
        it unread, and the descriptor refuses any other object outright.
        """

        descriptor = META.__dict__[attribute]

        assert type(META) is type
        assert getattr(META, attribute) is descriptor
        assert META.__basicsize__ > type.__basicsize__

        for wrong in (META, type, int, object()):
            with pytest.raises(TypeError, match="doesn't apply to"):
                descriptor.__get__(wrong)


def metaclass_subclass_that_builds_with_type_new():
    class Substituting(META):
        def __new__(mcls, *args, **keywords):
            return type.__new__(mcls, *args, **keywords)

    return Substituting


class TestTheUninstalledClassCannotBeBuilt:
    """`is_struct` asks the metaclass, so every instance of it has to have
    a field table. What guarantees it is CPython's own `tp_new_wrapper`: the
    most derived non-heap base of the requested type is the metaclass, whose
    `tp_new` is not `type`'s, so `type.__new__` refuses to build one.

    That guard is load-bearing and it is CPython's, not salix's, so these pin it
    on every supported version rather than trusting it. What a class that got
    past it would do to the guarded slots is not asserted here, because the
    guard cannot be turned off to find out -- only that the invariant every one
    of those slots reads holds.
    """

    @pytest.mark.parametrize(
        "metatype",
        [
            pytest.param(lambda: META, id="the-metaclass"),
            pytest.param(lambda: type("Inheriting", (META,), {}), id="a-plain-subclass"),
            pytest.param(
                metaclass_subclass_that_builds_with_type_new,
                id="one-that-substitutes-type-new",
            ),
        ],
    )
    def test_type_new_refuses_every_route_to_the_metatype(self, metatype):
        """One assertion, three ways in: the refusal follows the metatype rather
        than the call site.
        """

        with pytest.raises(TypeError, match="is not safe"):
            type.__new__(metatype(), "S", (Point,), {})

    def test_the_substituting_new_is_refused_through_its_own_call_too(self):
        """The route the parametrization above cannot express: not
        `type.__new__` directly, but the metaclass call that reaches it.
        """

        Substituting = metaclass_subclass_that_builds_with_type_new()

        with pytest.raises(TypeError, match="is not safe"):
            Substituting("S", (Point,), {})

    def test_the_supported_route_still_installs(self):
        """types.new_class goes the long way round and comes out a real struct,
        so the refusals above are not refusing everything.
        """

        import types

        built = types.new_class("S", (Point,), {"metaclass": META})

        assert built._struct_fields_ == ("x",)
        assert built(1) == built(1)


class TestAMetaclassSubclass:
    def test_one_that_delegates_produces_a_struct(self):
        class Delegating(META):
            pass

        class Delegated(Struct, metaclass=Delegating):
            a: int

        assert Delegated(1)._struct_fields_ == ("a",)
        assert type(Delegated) is Delegating

    def test_a_keyword_option_survives_the_handoff_to_a_derived_metatype(self):
        """type.__new__ would hand off to the derived metatype, which re-entered
        here with no keywords at all and planned the class without them.
        """

        class Delegating(META):
            pass

        class Base(Struct, metaclass=Delegating):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, order=True)

        assert built(1, 2) < built(1, 3)
        assert type(built) is Delegating

    def test_weakref_survives_the_handoff_to_a_derived_metatype(self):
        class Base(Struct, metaclass=Forwarding):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=True)
        held = built(1, 2)

        assert weakref.ref(held)() is held

    def test_a_keyword_only_delegate_that_names_weakref_can_add_the_slot(self):
        class KwOnly(META):
            def __new__(metacls, name, bases, namespace, *, weakref=False):
                return super().__new__(metacls, name, bases, namespace, weakref=weakref)

        class Base(Struct, metaclass=KwOnly):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=True)
        held = built(1, 2)

        assert weakref.ref(held)() is held

    def test_a_keyword_only_delegate_that_names_only_weakref_gets_weakref_alone(self):
        """The one option the settle cannot repair rides by itself; the rest
        takes the keyword-less path and the settle restores it.
        """

        class KwOnly(META):
            def __new__(metacls, name, bases, namespace, *, weakref=False):
                return super().__new__(metacls, name, bases, namespace, weakref=weakref)

        class Base(Struct, metaclass=KwOnly):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=True, eq=False)
        held = built(1, 2)

        assert weakref.ref(held)() is held
        assert built(1, 2) != built(1, 3)

    def test_a_delegate_without_keywords_still_gets_none_handed_over(self):
        """A delegate whose __new__ takes no **keywords would be handed the
        options and raise; it gets the keyword-less hand-off it declared, and
        the settle restores what the keyword meant.
        """

        class Base(Struct, metaclass=Plain):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, order=True)

        assert built(1, 2) < built(1, 3)

    def test_a_delegate_that_cannot_take_the_options_cannot_add_the_weakref_slot(self):
        """The keywords cannot ride a __new__ that does not take them, so the
        refusal says so instead of blaming the entry salix itself wrote.
        """

        class Base(Struct, metaclass=Plain):
            x: int

        with pytest.raises(TypeError, match="cannot cross"):
            META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=True)

    def test_a_cobase_weakref_slot_needs_no_duplicate_entry(self):
        class Slotted:
            __slots__ = ("__weakref__",)

        class WithSlot(Struct, Slotted, weakref=True):
            x: int

        held = WithSlot(1)

        assert weakref.ref(held)() is held

    def test_a_keyword_only_delegate_still_gets_the_class_statement_keywords(self):
        """The class statement hands its keywords to the metaclass's own
        __new__, so a delegate that names them as keyword-only parameters
        builds without any hand-off from salix.
        """

        class KwOnly(META):
            def __new__(metacls, name, bases, namespace, *, weakref=False):
                return super().__new__(metacls, name, bases, namespace, weakref=weakref)

        class Base(Struct, metaclass=KwOnly):
            x: int

        class Built(Base, weakref=True):
            y: int = 0

        held = Built(1)

        assert weakref.ref(held)() is held

    def test_a_staticmethod_delegate_accepts_the_handoff_keywords(self):
        class Staticmethoded(META):
            @staticmethod
            def __new__(metacls, name, bases, namespace, **keywords):
                return META.__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Staticmethoded):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=True)
        held = built(1, 2)

        assert weakref.ref(held)() is held

    def test_a_keyword_rides_only_a_chain_that_takes_it_end_to_end(self):
        """A chain whose intermediate __new__ takes no **keywords would raise
        on the options, so the hand-off stays keyword-less and the settle
        restores what the keyword meant.
        """

        class Mid(META):
            def __new__(metacls, name, bases, namespace):
                return super().__new__(metacls, name, bases, namespace)

        class Low(Mid):
            def __new__(metacls, name, bases, namespace, **keywords):
                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Low):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, order=True)

        assert built(1, 2) < built(1, 3)

    def test_two_unrelated_metatypes_still_raise_the_conflict(self):
        """Picking the winner ourselves must not swallow the case that has no
        winner: type_new is handed the requested metatype and says so.
        """

        class Left(META):
            pass

        class Right(META):
            pass

        class FromLeft(Struct, metaclass=Left):
            x: int

        class FromRight(Struct, metaclass=Right):
            y: int

        with pytest.raises(TypeError, match="metaclass conflict"):
            META("Both", (FromLeft, FromRight), {})

    def test_a_default_survives_the_handoff_to_a_derived_metatype(self):
        """The re-entered call read the namespace after drop_class_variables had
        taken the field names out of it, so every default declared in the body
        was gone by the time it planned the fields.
        """

        class Delegating(META):
            pass

        class Base(Struct, metaclass=Delegating):
            x: int

        namespace = {"__annotations__": {"y": int}, "y": 42}
        built = META("Built", (Base,), namespace)

        assert built(1).y == 42

    def test_a_delegate_returning_a_foreign_class_is_refused(self):
        """The class the delegate hands back is settled only when it is the
        class this call describes; a class the delegate found lying around
        keeps the refusal, so the caller never receives one whose fields are
        not what was asked for.
        """

        calls = []

        class Wrong(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                calls.append(1)

                if len(calls) == 1:
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                return Base

        class Base(Struct, metaclass=Wrong):
            x: int

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"y": int}})

    def test_a_delegate_that_renames_the_class_is_refused(self):
        """The fields and the base can both match and the class still not be
        the one this call describes, so the name is part of the check.
        """

        class Renaming(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                return super().__new__(
                    metacls,
                    "Seeded" if name == "Seeded" else "Fake",
                    bases,
                    namespace,
                    **keywords
                )

        class Seeded(Struct, metaclass=Renaming):
            x: int

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Seeded,), {"__annotations__": {"y": int}})

    def test_the_class_statement_reaches_none_of_that(self):
        """The hand-off only happens for an explicit metaclass call over a base
        whose metatype wins. Written out, because the refusal above would be a
        serious regression if it reached the ordinary spelling.
        """

        class Base(Struct, metaclass=Forwarding):
            x: int

        class Statement(Base, order=True):
            y: int = 42

        assert Statement._struct_fields_ == ("x", "y")
        assert Statement(1).y == 42
        assert Statement(1) < Statement(1, 43)

    def test_both_survive_a_delegate_that_writes_its_own_new(self):
        """A Python __new__ -- even one that only forwards -- makes type_new
        hand the build to it with no keywords and a namespace
        drop_class_variables has already taken the defaults out of. The class
        the delegate built is the one this call planned, so the call's own
        options and defaults are applied to it.
        """

        class Base(Struct, metaclass=Forwarding):
            x: int

        namespace = {"__annotations__": {"y": int}, "y": 42}
        built = META("Built", (Base,), namespace, order=True)

        assert built(1).y == 42
        assert built(1) < built(1, 43)

    def test_a_delegate_can_turn_frozen_off(self):
        class Base(Struct, metaclass=Forwarding):
            pass

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, frozen=False)
        instance = built(1)
        instance.y = 2

        assert instance.y == 2
        assert built.__hash__ is None

        with pytest.raises(TypeError, match="unhashable"):
            hash(instance)

    def test_a_delegate_can_turn_eq_off(self):
        class Base(Struct, metaclass=Forwarding):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, eq=False)

        assert built(1, 2) != built(1, 2)

        with pytest.raises(TypeError, match="not supported between instances"):
            _ = built(1, 2) < built(1, 2)

    def test_a_delegate_can_turn_repr_off(self):
        class Base(Struct, metaclass=Forwarding):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, repr=False)
        instance = built(1, 2)

        assert repr(instance) == object.__repr__(instance)

    def test_a_delegate_can_turn_match_args_off(self):
        class Base(Struct, metaclass=Forwarding):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, match_args=False)

        assert "__match_args__" not in built.__dict__

    def test_a_body_match_args_survives_the_delegate_when_turned_off(self):
        """The fresh build only ever adds __match_args__ when wanted, so a
        body-defined one survives match_args=False; the settle restores it
        where the re-entered build has overwritten it.
        """

        class Base(Struct, metaclass=Forwarding):
            x: int

        namespace = {"__annotations__": {"y": int}, "__match_args__": ("first",)}
        built = META("Built", (Base,), namespace, match_args=False)

        assert built.__match_args__ == ("first",)

    def test_a_weakref_base_keeps_its_slot_when_the_delegate_turns_weakref_off(self):
        """weakref=False is metadata; the fresh path also inherits the base's
        slot, so the settle accepts the class as-is.
        """

        class Base(Struct, metaclass=Forwarding, weakref=True):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=False)
        instance = built(1, 2)

        assert weakref.ref(instance)() is instance

    def test_a_delegate_can_add_the_weakref_slot_the_call_planned(self):
        """The keywords ride the hand-off, so the re-entered build plans the
        same slot and the direct spelling builds like the subclass spelling.
        """

        class Base(Struct, metaclass=Forwarding):
            x: int

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, weakref=True)
        held = built(1, 2)

        assert weakref.ref(held)() is held

    def test_a_delegate_that_strips_the_dunders_is_settled(self):
        """A delegate that removes the dunders from the namespace leaves the
        built class with the mixin's C slots, which ignore dict writes; the
        settle rebinds through the type so the slots follow.
        """

        class Stripping(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                for dunder in (
                    "__eq__",
                    "__ne__",
                    "__lt__",
                    "__le__",
                    "__gt__",
                    "__ge__",
                    "__repr__",
                    "__setattr__",
                    "__delattr__",
                    "__hash__",
                ):
                    namespace.pop(dunder, None)

                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Stripping):
            pass

        built = META("Built", (Base,), {"__annotations__": {"y": int}}, frozen=False, eq=False, repr=False)
        instance = built(1)
        instance.y = 2

        assert instance.y == 2
        assert built(1) != built(1)
        assert repr(instance) == object.__repr__(instance)
        assert hash(instance) == object.__hash__(instance)

    def test_a_delegate_that_strips_a_body_dunder_restores_it(self):
        """The fresh build keeps a body-defined dunder; a delegate that strips
        it leaves the built class without it, and the settle restores the
        body's own definition.
        """

        class Stripping(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                namespace.pop("__eq__", None)
                namespace.pop("__hash__", None)

                return super().__new__(metacls, name, bases, namespace, **keywords)

        def by_x(self, other):
            return self.x == other.x

        def hash_x(self):
            return hash(self.x)

        class Base(Struct, metaclass=Stripping):
            x: int

        namespace = {
            "__annotations__": {"y": int},
            "__eq__": by_x,
            "__hash__": hash_x,
        }
        built = META("Built", (Base,), namespace)

        assert built(1, 7) == built(1, 8)
        assert hash(built(1, 7)) == hash(1)

    def test_a_delegate_that_injects_a_dunder_is_refused(self):
        """A dunder the delegate adds while the plan binds nothing for it
        would behave differently from the fresh build, so the settle refuses
        rather than silently keep it.
        """

        class Injecting(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                if name == "Base":
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                namespace["__eq__"] = lambda self, other: True

                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Injecting):
            x: int

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"y": int}})

    def test_a_delegate_returning_a_weakref_slotted_class_is_refused(self):
        """A lying-around class built with weakref=True carries a slot the
        fresh build of this call would not, so the settle refuses it.
        """

        calls = []

        class Substituting(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                calls.append(1)

                if len(calls) <= 2:
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                return Built

        class Base(Struct, metaclass=Substituting):
            x: int

        class Built(Base, weakref=True):
            y: int = 99

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"y": int}})

    def test_a_delegate_that_strips_init_is_settled(self):
        """install_fields' construction decision is re-applied: the body's
        __init__ is restored so tp_new routes through it (a body __init__
        displaces the constructor that would run __post_init__, exactly as
        a fresh build behaves).
        """

        class Stripping(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                if name == "Base":
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                namespace.pop("__init__", None)

                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Stripping):
            x: int

        namespace = {
            "__annotations__": {"y": int},
            "__init__": lambda self, y: salix.set_field(self, "y", y + 100),
        }
        built = META("Built", (Base,), namespace)

        assert built(1).y == 101

    def test_a_delegate_that_strips_post_init_is_settled(self):
        """install_fields' post_init decision is re-applied: the body's hook
        is restored and runs.
        """

        class Stripping(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                if name == "Base":
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                namespace.pop("__post_init__", None)

                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Stripping):
            x: int

        namespace = {
            "__annotations__": {"y": int},
            "__post_init__": lambda self: salix.set_field(self, "y", self.y * 2),
        }
        built = META("Built", (Base,), namespace)

        assert built(0, 4).y == 8

    def test_a_delegate_that_injects_ne_when_the_body_answers_eq_is_overwritten(self):
        """The answered arm binds object's __ne__ whenever the body did not
        define one, overwriting whatever the delegate injected.
        """

        class Injecting(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                if name == "Base":
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                namespace["__ne__"] = lambda self, other: True

                return super().__new__(metacls, name, bases, namespace, **keywords)

        def by_x(self, other):
            return self.x == other.x

        class Base(Struct, metaclass=Injecting):
            x: int

        namespace = {"__annotations__": {"y": int}, "__eq__": by_x}
        built = META("Built", (Base,), namespace)

        assert (built(1, 7) != built(1, 8)) is False

    def test_a_delegate_that_strips_new_is_settled(self):
        """The fresh build keeps a body __new__ in the class dict (dispatch
        never consults it), so a delegate that strips it is restored.
        """

        class Stripping(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                if name == "Base":
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                namespace.pop("__new__", None)

                return super().__new__(metacls, name, bases, namespace, **keywords)

        def body_new(cls, *args):
            return super(cls, cls).__new__(cls, *args)

        class Base(Struct, metaclass=Stripping):
            x: int

        namespace = {"__annotations__": {"y": int}, "__new__": body_new}
        built = META("Built", (Base,), namespace)

        assert "__new__" in built.__dict__

    def test_a_delegate_that_injects_new_is_refused(self):
        """A __new__ the delegate adds while the body defined none would
        change construction, so the settle refuses.
        """

        class Injecting(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                if name == "Base":
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                namespace["__new__"] = lambda cls, *args: None

                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Injecting):
            x: int

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"y": int}})

    def test_a_delegate_that_adds_a_non_struct_base_is_refused(self):
        """The identity gate compares the bases tuple, not just the layout
        base; an extra base would route construction through its __init__.
        """

        calls = []

        class Substituting(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                calls.append(1)

                if len(calls) <= 2:
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                return Built

        class Base(Struct, metaclass=Substituting):
            x: int

        class InitBase:
            pass

        class Built(Base, InitBase):
            y: int = 99

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), {"__annotations__": {"y": int}})

    def test_a_delegate_returning_same_count_defaults_is_corrected_not_accepted(self):
        """A lying-around class can match the plan's name, fields, and default
        count while carrying different default values; the settle installs the
        call's own defaults rather than accepting the class silently.
        """

        calls = []

        class Substituting(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                calls.append(1)

                if len(calls) <= 2:
                    return super().__new__(metacls, name, bases, namespace, **keywords)

                return Built

        class Base(Struct, metaclass=Substituting):
            x: int

        class Built(Base):
            y: int = 99

        namespace = {"__annotations__": {"y": int}, "y": 42}
        built = META("Built", (Base,), namespace)

        assert built(1).y == 42

    def test_a_body_qualname_does_not_upset_the_settle(self):
        """The name check reads ht_name; a body-set __qualname__ is legal and
        the fresh path accepts it.
        """

        class Base(Struct, metaclass=Forwarding):
            x: int

        namespace = {"__annotations__": {"y": int}, "__qualname__": "Wrapped.Built"}
        built = META("Built", (Base,), namespace)

        assert built.__qualname__ == "Wrapped.Built"
        assert built(1, 2).y == 2

    @pytest.mark.parametrize("produced", ["not a type", bytearray(8192), 0])
    def test_one_that_returns_a_non_type_is_refused(self, produced):
        """type.__new__ hands off to the winning metatype, so the object
        create_class gets back is whatever this __new__ chose to return.

        A __new__ that builds its substitute with an inline `type(name, bases,
        ns)` never gets here: the bases carry the same metaclass, so `type` re-
        enters it and CPython raises RecursionError before anything is returned.
        That is metaclass semantics rather than salix's -- the same __new__ on a
        plain `type` subclass recurses identically -- so the refusal covers what
        a __new__ returns, not every way one can fail to return.
        """

        calls = []

        class Wrong(META):
            def __new__(mcls, *args, **keywords):
                calls.append(1)

                if len(calls) == 1:
                    return super().__new__(mcls, *args, **keywords)

                return produced

        class Seeded(Struct, metaclass=Wrong):
            a: int

        with pytest.raises(TypeError, match=r"Wrong\.__new__ returned .*is not a struct class"):
            META("Z", (Seeded,), {"__annotations__": {"b": int}})
