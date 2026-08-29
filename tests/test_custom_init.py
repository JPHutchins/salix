import pickle
import sys

import pytest

from salix import Struct, set_field


class Generated(Struct):
    x: int
    y: int = 0


class Handwritten(Struct, frozen=False):
    x: int
    y: int = 0

    def __init__(self, both: int) -> None:
        self.x = both
        self.y = both


def test_the_generated_constructor_is_still_the_default():
    assert Generated(1, 2) == Generated(x=1, y=2)
    assert Generated(1).y == 0


def test_a_body_init_runs():
    assert Handwritten(7) == Handwritten(7)
    assert (Handwritten(7).x, Handwritten(7).y) == (7, 7)


def test_the_signature_is_the_body_s():
    with pytest.raises(TypeError, match="takes 2 positional arguments"):
        Handwritten(1, 2)


def test_the_struct_machinery_is_otherwise_untouched():
    assert Handwritten._struct_fields_ == ("x", "y")
    assert Handwritten.__match_args__ == ("x", "y")
    assert repr(Handwritten(7)) == "Handwritten(x=7, y=7)"
    assert Handwritten(7) == Generated(7, 7)


def test_an_inherited_init_is_honoured_too():
    class Child(Handwritten):
        pass

    assert Child(3) == Handwritten(3)


def test_a_body_init_under_a_generated_base_declines_the_vectorcall_too():
    """The other direction: the base keeps the generated constructor and the
    subclass writes its own __init__, so the subclass is where tp_new arrives.

    The base must be unaffected -- it still has the vectorcall, and its own
    defaults still come from fill_defaults rather than from tp_new.
    """

    class Generated2(Struct, frozen=False):
        x: int
        y: int = 5

    class Handwritten2(Generated2):
        z: int = 9

        def __init__(self) -> None:
            self.x = 1

    built = Handwritten2()

    assert (built.x, built.y, built.z) == (1, 5, 9)
    assert (Generated2(2).x, Generated2(2).y) == (2, 5)

    with pytest.raises(TypeError, match="takes at most 2 positional"):
        Generated2(1, 2, 3)


def test_a_child_may_go_back_to_the_generated_one():
    """__init__ is a name like any other; object's is what un-defines it."""

    class Child(Handwritten):
        __init__ = object.__init__  # type: ignore[assignment]

    assert (Child(1, 2).x, Child(1, 2).y) == (1, 2)
    assert Child(1).y == 0


def test_defaults_are_written_before_a_body_init_runs():
    """What the class declared, the instance carries.

    The declaration used to be inert: the vectorcall filled defaults and this
    class declined it, so `_struct_defaults_` advertised a value no instance
    would ever have. tp_new writes them now, which is where a dataclass leaves
    them readable too.
    """

    class Partial(Struct, frozen=False):
        x: int
        y: int = 99

        def __init__(self) -> None:
            self.x = 1

    assert Partial().y == 99
    assert repr(Partial()) == "Partial(x=1, y=99)"


def test_a_body_init_can_read_the_default_and_overwrite_it():
    """Written before it runs, so it is a starting point rather than a fact."""

    class Doubling(Struct, frozen=False):
        y: int = 99

        def __init__(self) -> None:
            self.y = self.y * 2

    assert Doubling().y == 198


def test_a_field_with_no_default_is_left_unset():
    """Supplying those is what the body's __init__ is for."""

    class Required(Struct, frozen=False):
        x: int
        y: int = 99

        def __init__(self) -> None:
            pass

    with pytest.raises(AttributeError):
        _ = Required().x

    assert Required().y == 99


def test_a_subclass_that_writes_no_init_gets_its_defaults_too():
    """The case that made this a bug rather than a consequence. `defines_own_init`
    reads tp_init, which an inherited __init__ satisfies -- so a subclass that
    says nothing about construction lost its own declared default as well as the
    base's, and had no way to ask for it back.
    """

    class Base(Struct, frozen=False):
        x: int = 7

        def __init__(self) -> None:
            pass

    class Child(Base):
        y: int = 5

    assert (Child().x, Child().y) == (7, 5)


def test_a_frozen_class_with_a_body_init_still_gets_them():
    """It cannot write a field, so before this the instance had none at all."""

    class Frozen(Struct):
        x: int = 7

        def __init__(self) -> None:
            pass

    assert Frozen().x == 7


def test_each_instance_gets_its_own_mutable_default():
    """tp_new takes the same copy the vectorcall does, so the rule that a
    mutable default belongs to the instance holds on both paths.
    """

    class Mutable(Struct, frozen=False):
        xs: list = []  # noqa: RUF012 -- the copy is the feature under test

        def __init__(self) -> None:
            pass

    first = Mutable()
    first.xs.append(1)

    assert Mutable().xs == []
    assert first.xs is not Mutable().xs


def test_post_init_does_not_run_for_a_body_init():
    ran = []

    class Both(Struct, frozen=False):
        x: int

        def __init__(self) -> None:
            self.x = 1

        def __post_init__(self) -> None:  # pragma: no cover
            ran.append(True)

    Both()

    assert ran == []


def test_a_frozen_struct_with_a_body_init_cannot_write_its_fields():
    """Frozen is frozen; a struct whose __init__ assigns asks for frozen=False."""

    class Frozen(Struct):
        x: int

        def __init__(self) -> None:
            self.x = 1

    with pytest.raises(AttributeError, match="does not support attribute assignment"):
        Frozen()


def test_a_frozen_body_init_writes_its_fields_with_set_field():
    """The portable way. object.__setattr__ below is the one that is not."""

    class Frozen(Struct):
        x: int

        def __init__(self) -> None:
            set_field(self, "x", 1)

    assert Frozen().x == 1


@pytest.mark.skipif(
    sys.version_info < (3, 13), reason="object.__setattr__ reaches a struct only on 3.13+"
)
def test_a_frozen_body_init_can_use_object_setattr_where_the_interpreter_allows_it():
    class Frozen(Struct):
        x: int

        def __init__(self) -> None:
            object.__setattr__(self, "x", 1)

    assert Frozen().x == 1


class PicklableFrozen(Struct):
    """Module level, because pickle looks a class up by qualified name."""

    x: int = 7

    def __init__(self) -> None:
        pass


class PicklableMutable(Struct, frozen=False):
    x: int = 7

    def __init__(self) -> None:
        pass


def test_a_mutable_body_init_struct_now_pickles_what_it_holds():
    """Writing the defaults is what makes the state worth restoring.

    Before this, the state was empty and the round trip returned an instance
    with every field unset -- not a failure, which is what made it easy to miss.

    The written value is a non-default one on purpose: tp_new writes `7` during
    `loads` whatever the pickle said, so asserting the default back would pass
    just as well on a round trip that restored nothing at all.
    """

    instance = PicklableMutable()
    instance.x = 99

    assert pickle.loads(pickle.dumps(instance)).x == 99
    assert PicklableMutable().x == 7


@pytest.mark.xfail(strict=True, reason="#13: a frozen struct has no __setstate__ that can write")
def test_a_frozen_body_init_struct_should_pickle_too():
    """The frozen half of the same, and the one this cost something.

    The default reducer restores state through setattr, which a frozen struct
    refuses, so `pickle.loads` now raises where it used to return an instance
    with every field unset. That is a real change and not a lost round trip:
    nothing was ever restored. #13 is the issue -- a __getstate__/__setstate__
    pair that writes slots the way set_field does -- and this goes green when it
    lands.

    Written through set_field to a non-default value first, for the same reason
    the mutable test does it: tp_new writes `7` during loads whatever the pickle
    said, so asserting the default back would let #13 land with a broken restore
    and still flip this green.
    """

    instance = PicklableFrozen()
    set_field(instance, "x", 99)

    assert pickle.loads(pickle.dumps(instance)).x == 99


def test_the_mixin_stays_uninstantiable():
    """Only a class that declined the vectorcall gets tp_new, and it is a struct.

    Otherwise the mixin's dunders would be reachable over an object with no
    field table behind it.
    """

    with pytest.raises(TypeError, match="cannot create"):
        Struct.__mro__[1]()
