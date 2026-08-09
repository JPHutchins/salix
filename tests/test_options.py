import weakref

import pytest
from values import EVERY, HASHABLE, identify

from salix import Struct, set_field


class Plain(Struct):
    x: object
    y: object = "default"


class NoEq(Struct, eq=False):
    x: object


class Ordered(Struct, order=True):
    x: object
    y: object = 0


class NoRepr(Struct, repr=False):
    x: object


class NoMatchArgs(Struct, match_args=False):
    x: object


class Weak(Struct, weakref=True):
    x: object


def test_the_defaults_are_dataclass_defaults():
    assert Plain(1, 2) == Plain(1, 2)
    assert repr(Plain(1, 2)) == "Plain(x=1, y=2)"
    assert Plain.__match_args__ == ("x", "y")
    assert hash(Plain(1, 2)) == hash((1, 2))

    with pytest.raises(TypeError):
        _ = Plain(1, 2) < Plain(1, 3)

    with pytest.raises(TypeError, match="cannot create weak reference"):
        weakref.ref(Plain(1, 2))


def test_an_unknown_keyword_names_the_ones_that_exist():
    with pytest.raises(TypeError, match="'frozn' is not a struct class keyword"):

        class Typo(Struct, frozn=False):
            x: int


def test_a_deferred_dataclass_option_is_rejected_rather_than_ignored():
    """kw_only is not implemented; silently accepting it would be the bug."""

    with pytest.raises(TypeError, match="is not a struct class keyword"):

        class Unsupported(Struct, kw_only=True):
            x: int


class TestEq:
    def test_identity_replaces_structural_equality(self):
        instance = NoEq(1)

        assert instance == instance  # noqa: PLR0124 -- reflexivity is the assertion
        assert NoEq(1) != NoEq(1)

    def test_the_hash_becomes_the_identity_hash(self):
        instance = NoEq(1)

        assert NoEq.__hash__ is object.__hash__
        assert hash(instance) == object.__hash__(instance)

    @pytest.mark.parametrize("value", EVERY, ids=identify)
    def test_an_unhashable_field_no_longer_makes_the_struct_unhashable(self, value):
        """Identity hashing does not touch the values, so every value works."""

        instance = NoEq(value)

        assert hash(instance) == object.__hash__(instance)

    def test_it_is_inherited(self):
        class Child(NoEq):
            y: object = 0

        assert Child(1) != Child(1)

    def test_a_child_may_turn_it_back_on(self):
        class Child(NoEq, eq=True):
            y: object = 0

        assert Child(1) == Child(1)
        assert hash(Child(1)) == hash((1, 0))

    def test_a_body_eq_clears_the_hash_the_way_python_does(self):
        class Custom(Struct):  # noqa: PLW1641 -- the absent __hash__ is the assertion
            x: object

            def __eq__(self, other: object) -> bool:
                return True

        assert Custom.__hash__ is None

        with pytest.raises(TypeError, match="unhashable"):
            hash(Custom(1))

    def test_a_subclass_of_a_body_eq_class_is_unhashable_too(self):
        """It resolves that __eq__ through the MRO, so it owes the same debt --
        and a structural hash beside an __eq__ that answers True would put two
        equal instances in two slots of a set.
        """

        class Custom(Struct):  # noqa: PLW1641 -- the absent __hash__ is the assertion
            x: object

            def __eq__(self, other: object) -> bool:
                return True

        class Child(Custom):
            y: object = 0

        class Grandchild(Child):
            z: object = 0

        assert Child.__hash__ is None
        assert Grandchild.__hash__ is None

    def test_freezing_under_a_body_eq_base_does_not_buy_back_the_hash(self):
        """A fieldless base may be mutable and its child frozen, so `__hash__ is
        None` on the base does not say which rule put it there. Only the one
        about __eq__ survives into the child.
        """

        class Loose(Struct, frozen=False):  # noqa: PLW1641 -- the absent __hash__ is the assertion
            def __eq__(self, other: object) -> bool:
                return True

        class Frozen(Loose, frozen=True):
            x: int

        assert Frozen.__hash__ is None

    def test_a_subclass_inherits_the_hash_beside_the_eq_it_inherits(self):
        """When the body defined both, the pair travels together. salix binding
        its own structural hash here would break a contract the base kept.
        """

        class Both(Struct):
            x: object

            def __eq__(self, other: object) -> bool:
                return True

            def __hash__(self) -> int:
                return 42

        class Child(Both):
            y: object = 0

        assert hash(Child(1, 0)) == 42
        assert len({Child(1, 0), Child(2, 0)}) == 1

    def test_a_subclass_may_take_equality_back_and_become_hashable(self):
        class Custom(Struct):  # noqa: PLW1641 -- eq=False below is the replacement
            x: object

            def __eq__(self, other: object) -> bool:
                return True

        class Child(Custom, eq=False):
            y: object = 0

        assert Child.__hash__ is object.__hash__
        assert Child(1) != Child(1)

    def test_a_body_definition_wins_over_the_option(self):
        class Custom(Struct, eq=False):
            x: object

            def __eq__(self, other: object) -> bool:
                return True

            __hash__ = None  # type: ignore[assignment]

        assert Custom(1) == "anything"


class TestOrder:
    def test_the_comparisons_follow_the_field_values(self):
        assert Ordered(1, 2) < Ordered(1, 3)
        assert Ordered(1, 3) > Ordered(1, 2)
        assert Ordered(1, 2) <= Ordered(1, 2)
        assert Ordered(1, 2) >= Ordered(1, 2)
        assert not Ordered(1, 2) < Ordered(1, 2)

    def test_the_first_differing_field_decides(self):
        assert Ordered(1, 99) < Ordered(2, 0)

    def test_it_sorts(self):
        instances = [Ordered(2, 0), Ordered(1, 9), Ordered(1, 2)]

        assert sorted(instances) == [Ordered(1, 2), Ordered(1, 9), Ordered(2, 0)]

    def test_it_orders_the_way_the_tuple_of_its_values_does(self):
        pairs = [(1, 2), (1, 3), (2, 0), (2, 0)]

        for left in pairs:
            for right in pairs:
                assert (Ordered(*left) < Ordered(*right)) == (left < right)
                assert (Ordered(*left) <= Ordered(*right)) == (left <= right)

    def test_it_is_structural_like_equality(self):
        """Matching field names, any class -- the rule `==` already follows."""

        class Same(Struct, order=True):
            x: object
            y: object = 0

        assert Same(1, 2) == Ordered(1, 2)
        assert Same(1, 2) < Ordered(1, 3)

    def test_different_field_names_are_not_ordered(self):
        class Other(Struct, order=True):
            a: object

        with pytest.raises(TypeError):
            _ = Ordered(1, 2) < Other(1)

    def test_an_unordered_struct_is_not_ordered_against_an_ordered_one(self):
        with pytest.raises(TypeError):
            _ = Ordered(1, 2) < Plain(1, 2)

        with pytest.raises(TypeError):
            _ = Plain(1, 2) < Ordered(1, 2)

    def test_a_field_that_does_not_order_reports_its_own_failure(self):
        class Holder(Struct, order=True):
            value: object

        with pytest.raises(TypeError):
            _ = Holder(object()) < Holder(object())

    def test_a_field_that_rewrites_its_own_slot_mid_comparison(self):
        class Fickle:
            def __eq__(self, other):
                set_field(holder, "x", 0)
                return False

            def __lt__(self, other):
                return True

            __hash__ = None

        holder = Ordered(Fickle(), 1)

        assert holder < Ordered(Fickle(), 2)

    def test_it_is_inherited(self):
        class Child(Ordered):
            z: object = 0

        assert Child(1, 2, 3) < Child(1, 2, 4)

    def test_a_child_may_turn_it_off(self):
        class Child(Ordered, order=False):
            z: object = 0

        with pytest.raises(TypeError):
            _ = Child(1, 2, 3) < Child(1, 2, 4)

    def test_ordering_without_equality_is_refused(self):
        with pytest.raises(TypeError, match="order=True needs eq=True"):

            class Contradiction(Struct, order=True, eq=False):
                x: int

    def test_a_child_may_not_drop_equality_and_keep_ordering(self):
        with pytest.raises(TypeError, match="order=True needs eq=True"):

            class Child(Ordered, eq=False):
                z: object = 0


class TestRepr:
    def test_the_default_repr_replaces_the_structural_one(self):
        rendered = repr(NoRepr(1))

        assert rendered.startswith("<")
        assert "NoRepr object at" in rendered

    def test_it_is_inherited(self):
        class Child(NoRepr):
            y: object = 0

        assert "object at" in repr(Child(1))

    def test_a_child_may_turn_it_back_on(self):
        class Child(NoRepr, repr=True):
            y: object = 0

        assert repr(Child(1)) == "Child(x=1, y=0)"

    def test_the_other_dunders_are_untouched(self):
        assert NoRepr(1) == NoRepr(1)
        assert hash(NoRepr(1)) == hash((1,))


class TestMatchArgs:
    def test_a_positional_pattern_works_by_default(self):
        match Plain(1, 2):
            case Plain(first, second):
                assert (first, second) == (1, 2)
            case _:  # pragma: no cover
                pytest.fail("the positional pattern did not match")

    def test_opting_out_leaves_no_positional_pattern(self):
        assert NoMatchArgs.__match_args__ == ()

        with pytest.raises(TypeError, match="accepts 0 positional sub-patterns"):
            match NoMatchArgs(1):
                case NoMatchArgs(_):  # pragma: no cover
                    pytest.fail("the positional pattern should not have matched")

    def test_a_keyword_pattern_still_works(self):
        match NoMatchArgs(1):
            case NoMatchArgs(x=value):
                assert value == 1
            case _:  # pragma: no cover
                pytest.fail("the keyword pattern did not match")

    def test_opting_out_keeps_whatever_the_base_declared(self):
        """As a dataclass does: unset, not emptied."""

        class Child(Plain, match_args=False):
            z: object = 0

        assert Child.__match_args__ == ("x", "y")


class TestWeakref:
    def test_a_struct_is_not_weak_referenceable_by_default(self):
        with pytest.raises(TypeError, match="cannot create weak reference"):
            weakref.ref(Plain(1, 2))

    def test_opting_in_makes_it_one(self):
        instance = Weak(1)
        reference = weakref.ref(instance)

        assert reference() is instance

    def test_the_reference_dies_with_the_instance(self):
        reference = weakref.ref(Weak(1))

        assert reference() is None

    def test_it_is_inherited_without_a_second_slot(self):
        class Child(Weak):
            y: object = 0

        instance = Child(1)

        assert weakref.ref(instance)() is instance

    def test_a_child_repeating_the_request_is_not_an_error(self):
        class Child(Weak, weakref=True):
            y: object = 0

        instance = Child(1)

        assert weakref.ref(instance)() is instance

    @pytest.mark.parametrize("value", HASHABLE, ids=identify)
    def test_the_slot_does_not_disturb_the_fields(self, value):
        instance = Weak(value)

        assert instance.x is value
        assert instance == Weak(value)
        assert hash(instance) == hash((value,))


class TestCombinations:
    def test_mutable_and_identity_stays_hashable_across_a_write(self):
        class Instance(Struct, frozen=False, eq=False):
            x: object

        instance = Instance(1)
        before = hash(instance)
        instance.x = 2

        assert hash(instance) == before

    def test_mutable_and_structural_is_unhashable(self):
        class Instance(Struct, frozen=False):
            x: object

        assert Instance.__hash__ is None

    def test_turning_equality_back_on_under_a_mutable_base_removes_the_hash(self):
        class Base(Struct, frozen=False, eq=False):
            x: object

        class Child(Base, eq=True):
            y: object = 0

        assert Child.__hash__ is None

    def test_every_option_at_once(self):
        class Everything(Struct, frozen=False, eq=False, repr=False, match_args=False, weakref=True):
            x: object

        instance = Everything(1)

        assert instance != Everything(1)
        assert "object at" in repr(instance)
        assert weakref.ref(instance)() is instance
        assert hash(instance) == object.__hash__(instance)

        instance.x = 2

        assert instance.x == 2
