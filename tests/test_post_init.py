import sys

import pytest
from values import EVERY, identify

from salix import Struct, set_field


class Validated(Struct):
    x: int

    def __post_init__(self) -> None:
        if self.x < 0:
            raise ValueError("x must be non-negative")


class Derived(Struct, frozen=False):
    reading: int
    doubled: int = 0

    def __post_init__(self) -> None:
        self.doubled = self.reading * 2


def test_a_struct_without_one_is_unaffected():
    class Plain(Struct):
        x: int

    assert Plain._struct_fields_ == ("x",)
    assert Plain(1).x == 1


def test_it_runs_during_construction():
    assert Validated(1).x == 1


def test_a_raise_reaches_the_caller():
    with pytest.raises(ValueError, match="x must be non-negative"):
        Validated(-1)


def test_every_field_is_written_before_it_runs():
    seen: list[tuple[object, ...]] = []

    class Observer(Struct):
        a: object
        b: object = "default"

        def __post_init__(self) -> None:
            seen.append((self.a, self.b))

    Observer(1)
    Observer(1, 2)

    assert seen == [(1, "default"), (1, 2)]


def test_a_mutable_struct_can_derive_a_field_from_the_others():
    assert Derived(21).doubled == 42


def test_object_setattr_on_a_frozen_struct_follows_the_interpreter():
    """The frozen-dataclass escape hatch reaches a struct only on 3.13 and up.

    CPython's setattr hackcheck walks the MRO to whichever type defines the
    current tp_setattro and refuses unless it is object's. For a dataclass that
    is object; for a struct it is the mixin, whose slot is what freezing *is*.
    3.13 dropped the check.

    This is why set_field exists: it is the door that does not depend on the
    interpreter. object.__setattr__ is not a supported way into a struct, and
    this test pins what it does rather than endorsing it.
    """

    class Frozen(Struct):
        reading: int
        doubled: int = 0

        def __post_init__(self) -> None:
            object.__setattr__(self, "doubled", self.reading * 2)

    if sys.version_info >= (3, 13):
        assert Frozen(21).doubled == 42
    else:
        with pytest.raises(TypeError, match="can't apply this __setattr__"):
            Frozen(21)


def test_set_field_derives_a_frozen_field_on_every_interpreter():
    """The supported way, and the reason this issue is closed.

    set_field resolves the name against the field table and writes that slot --
    the path the constructor takes -- so it reaches exactly the fields the class
    declared and cannot add an attribute.
    """

    class Frozen(Struct):
        reading: int
        doubled: int = 0

        def __post_init__(self) -> None:
            set_field(self, "doubled", self.reading * 2)

    assert Frozen(21).doubled == 42


def test_set_field_refuses_a_name_the_class_did_not_declare():
    with pytest.raises(AttributeError, match="has no field 'absent'"):
        set_field(Derived(1), "absent", 9)


def test_set_field_refuses_something_that_is_not_a_struct():
    with pytest.raises(TypeError, match="expects a struct, not int"):
        set_field(1, "reading", 9)  # type: ignore[arg-type]


def test_set_field_refuses_a_name_that_is_not_a_string():
    with pytest.raises(TypeError, match="field name must be str, not int"):
        set_field(Derived(1), 5, 9)  # type: ignore[arg-type]


def test_it_is_inherited():
    class Child(Derived):
        label: str = "child"

    assert Child(21) == Child(21, 42, "child")


def test_a_child_may_replace_it():
    class Child(Derived):
        def __post_init__(self) -> None:
            self.doubled = -1

    assert Child(21).doubled == -1


def test_the_hook_is_resolved_at_class_creation():
    """Stated rather than lamented: this is what buys the null check."""

    class Late(Struct):
        x: int

    Late.__post_init__ = lambda self: pytest.fail("should not run")  # type: ignore[attr-defined]

    assert Late(1).x == 1


@pytest.mark.parametrize("value", EVERY, ids=identify)
def test_it_sees_any_field_value(value):
    seen: list[object] = []

    class Observer(Struct):
        held: object

        def __post_init__(self) -> None:
            seen.append(self.held)

    Observer(value)

    assert seen[0] is value
