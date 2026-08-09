import collections.abc

import pytest

import salix
from salix import Struct

MIXIN = Struct.__mro__[1]
META = type(Struct)

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

    def test_a_delegate_that_writes_its_own_new_is_refused_not_mis_planned(self):
        """The half of #55 that is answerable here: the class the delegate hands
        back is checked against what this call planned, and a mismatch is an
        error rather than a class that quietly is not the one asked for.

        The class statement is not affected, and the test below this one says
        so: there the winner is the requested metatype, so nothing is handed
        off and the keywords and the body's defaults are still in hand.
        """

        class Forwarding(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Forwarding):
            x: int

        namespace = {"__annotations__": {"y": int}, "y": 42}

        with pytest.raises(TypeError, match="did not plan"):
            META("Built", (Base,), namespace, order=True)

    def test_the_class_statement_reaches_none_of_that(self):
        """The hand-off only happens for an explicit metaclass call over a base
        whose metatype wins. Written out, because the refusal above would be a
        serious regression if it reached the ordinary spelling.
        """

        class Forwarding(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Forwarding):
            x: int

        class Statement(Base, order=True):
            y: int = 42

        assert Statement._struct_fields_ == ("x", "y")
        assert Statement(1).y == 42
        assert Statement(1) < Statement(1, 43)

    @pytest.mark.xfail(strict=True, reason="#55")
    def test_both_survive_a_delegate_that_writes_its_own_new(self):
        """What the two above do not cover. They pass because the winner's
        tp_new is StructMeta_new, so create_class builds as the winner and the
        keywords and the untransformed namespace are still in hand. A Python
        __new__ -- even one that only forwards -- makes type_new hand the build
        to it instead, with no keywords and a namespace drop_class_variables has
        already taken the defaults out of.

        Refused rather than mis-planned since the guard above, which is not the
        same as fixed: what #55 wants is for this call to work.
        """

        class Forwarding(META):
            def __new__(metacls, name, bases, namespace, **keywords):
                return super().__new__(metacls, name, bases, namespace, **keywords)

        class Base(Struct, metaclass=Forwarding):
            x: int

        namespace = {"__annotations__": {"y": int}, "y": 42}
        built = META("Built", (Base,), namespace, order=True)

        assert built(1).y == 42
        assert built(1) < built(1, 43)

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
