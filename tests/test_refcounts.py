import copy
import gc
import sys
import sysconfig

import pytest

from salix import Struct, set_field

pytestmark = pytest.mark.skipif(
    bool(sysconfig.get_config_var("Py_GIL_DISABLED")),
    reason="getrefcount is not a reliable probe without the GIL",
)


class Sentinel:
    """Never immortal, never cached, never interned."""


class Pair(Struct):
    first: object
    second: object


class Defaulted(Struct):
    required: object
    optional: object = Sentinel()


def test_the_probe_detects_a_retained_reference():
    """A negative control: without this, every test below could pass vacuously."""

    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)
    keeper = [sentinel]

    assert sys.getrefcount(sentinel) == before + 1

    del keeper

    assert sys.getrefcount(sentinel) == before


def test_construction_takes_one_reference_per_field():
    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)
    pair = Pair(sentinel, sentinel)

    assert sys.getrefcount(sentinel) == before + 2

    del pair

    assert sys.getrefcount(sentinel) == before


def test_keyword_binding_takes_the_same_count_as_positional():
    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)
    pair = Pair(first=sentinel, second=sentinel)

    assert sys.getrefcount(sentinel) == before + 2

    del pair


def test_a_copied_default_hands_out_no_reference_to_itself():
    """The other half of the default story, and the one this file did not have:
    a mutable default is copied per instance, so the stored default's count does
    not move however many instances exist, and each copy goes with its own.
    """

    class Holder(Struct):
        xs: list = []  # noqa: RUF012 -- the copy is what is being counted

    (stored,) = Holder._struct_defaults_
    before = sys.getrefcount(stored)
    instances = [Holder() for _ in range(10)]

    assert sys.getrefcount(stored) == before
    assert all(instance.xs is not stored for instance in instances)
    assert len({id(instance.xs) for instance in instances}) == 10

    del instances
    gc.collect()

    assert sys.getrefcount(stored) == before


def test_an_uncopied_default_is_shared_by_every_instance():
    """`Sentinel()` is not one of the four, so this is the sharing path -- the
    name says which of the two it pins, now that both exist.
    """

    sentinel = Defaulted(None).optional
    before = sys.getrefcount(sentinel)
    instances = [Defaulted(None) for _ in range(10)]

    assert sys.getrefcount(sentinel) == before + 10

    del instances

    assert sys.getrefcount(sentinel) == before


def test_a_body_init_shares_an_uncopied_default_the_same_way():
    """tp_new writes the defaults for a class that declined the vectorcall, so
    the two paths are counted separately -- one taking a reference per instance
    is not evidence about the other.
    """

    class Declining(Struct, frozen=False):
        optional: object = Sentinel()

        def __init__(self) -> None:
            pass

    (sentinel,) = Declining._struct_defaults_
    before = sys.getrefcount(sentinel)
    instances = [Declining() for _ in range(10)]

    assert sys.getrefcount(sentinel) == before + 10

    del instances

    assert sys.getrefcount(sentinel) == before


def test_a_body_init_copies_a_mutable_default_without_retaining_it():
    class Declining(Struct, frozen=False):
        xs: list = []  # noqa: RUF012 -- the copy is what is being counted

        def __init__(self) -> None:
            pass

    (stored,) = Declining._struct_defaults_
    before = sys.getrefcount(stored)
    instances = [Declining() for _ in range(10)]

    assert sys.getrefcount(stored) == before
    assert len({id(instance.xs) for instance in instances}) == 10

    del instances
    gc.collect()

    assert sys.getrefcount(stored) == before


def test_a_body_init_that_raises_releases_the_defaults_tp_new_wrote():
    class Failing(Struct, frozen=False):
        optional: object = Sentinel()

        def __init__(self) -> None:
            raise ValueError

    (sentinel,) = Failing._struct_defaults_
    before = sys.getrefcount(sentinel)

    for _ in range(10):
        with pytest.raises(ValueError):
            Failing()

    assert sys.getrefcount(sentinel) == before

    # Without this the test is vacuously green on a build whose tp_new writes
    # nothing: no reference taken is no reference to leak.
    class Writing(Struct, frozen=False):
        optional: object = Sentinel()

        def __init__(self) -> None:
            pass

    assert Writing().optional is Writing._struct_defaults_[0]


def test_reading_a_field_does_not_accumulate():
    sentinel = Sentinel()
    pair = Pair(sentinel, None)
    before = sys.getrefcount(sentinel)

    for _ in range(100):
        assert pair.first is sentinel

    assert sys.getrefcount(sentinel) == before

    del pair


@pytest.mark.parametrize(
    "operation",
    # PLR0124: comparing an instance with itself is what exercises the dunder.
    [repr, hash, lambda pair: pair == pair, lambda pair: pair != pair],  # noqa: PLR0124
    ids=["repr", "hash", "eq", "ne"],
)
def test_the_dunders_do_not_leak(operation):
    sentinel = Sentinel()
    pair = Pair(sentinel, sentinel)
    before = sys.getrefcount(sentinel)

    for _ in range(100):
        operation(pair)

    assert sys.getrefcount(sentinel) == before

    del pair


def test_a_construction_that_fails_late_releases_what_it_already_bound():
    """The unwind path: positional slots are written before keywords are checked."""

    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)

    with pytest.raises(TypeError, match="unexpected keyword argument"):
        Pair(sentinel, sentinel, nope=1)

    assert sys.getrefcount(sentinel) == before


def test_a_construction_short_an_argument_releases_what_it_already_bound():
    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)

    with pytest.raises(TypeError, match="missing required argument"):
        Pair(sentinel)

    assert sys.getrefcount(sentinel) == before


def test_a_rejected_class_body_releases_its_defaults():
    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)

    with pytest.raises(TypeError, match="non-default field"):

        class Bad(Struct):
            a: object = sentinel
            b: object

    gc.collect()

    assert sys.getrefcount(sentinel) == before


def test_discarding_a_class_releases_its_defaults():
    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)

    class Temporary(Struct):
        a: object = sentinel

    assert sys.getrefcount(sentinel) > before

    del Temporary
    gc.collect()

    assert sys.getrefcount(sentinel) == before


def test_discarding_a_class_releases_its_post_init():
    """The class holds the hook it resolved, so it has one more to give back."""

    def hook(self: object) -> None:
        return None

    before = sys.getrefcount(hook)

    class Temporary(Struct):
        a: object
        __post_init__ = hook

    assert sys.getrefcount(hook) > before

    del Temporary
    gc.collect()

    assert sys.getrefcount(hook) == before


def test_a_post_init_that_raises_releases_the_fields_it_already_saw():
    class Rejects(Struct):
        a: object

        def __post_init__(self) -> None:
            raise ValueError("no")

    sentinel = Sentinel()
    before = sys.getrefcount(sentinel)

    with pytest.raises(ValueError, match="no"):
        Rejects(sentinel)

    gc.collect()

    assert sys.getrefcount(sentinel) == before


def test_set_field_releases_the_value_it_replaces():
    """The write is a replacement, so the old value has to go.

    Sealing a frozen struct's members with Py_READONLY was tried here and
    reverted: CPython's clear_slots skips a read-only member, so every frozen
    struct leaked one reference per field. These tests are what caught it.
    """

    replaced = Sentinel()
    pair = Pair(replaced, "second")
    before = sys.getrefcount(replaced)

    set_field(pair, "first", "written")

    assert sys.getrefcount(replaced) == before - 1
    assert pair.first == "written"


def test_set_field_takes_one_reference_to_what_it_writes():
    written = Sentinel()
    pair = Pair("first", "second")
    before = sys.getrefcount(written)

    set_field(pair, "first", written)

    assert sys.getrefcount(written) == before + 1

    del pair
    gc.collect()

    assert sys.getrefcount(written) == before


def test_a_refused_set_field_takes_nothing():
    value = Sentinel()
    pair = Pair("first", "second")
    before = sys.getrefcount(value)

    with pytest.raises(AttributeError):
        set_field(pair, "absent", value)

    assert sys.getrefcount(value) == before


def test_a_delegating_metatype_installs_the_field_table_once():
    """`type.__new__` hands off to the most derived metatype, which re-enters
    the metaclass's `__new__` and builds the class in full. The outer call used
    to install a second field table over the first, releasing nothing -- one
    leaked __post_init__, defaults tuple and slot-offset array per class.
    """

    class Derived(type(Struct)):
        pass

    def post_init(self):
        pass

    class Base(Struct, metaclass=Derived):
        x: int
        __post_init__ = post_init

    namespace = {"__annotations__": {"y": int}, "__post_init__": post_init}
    before = sys.getrefcount(post_init)
    built = type(Struct)("Built", (Base,), namespace)

    assert built._struct_fields_ == ("x", "y")
    assert built(1, 2).y == 2

    # `before` already counts the namespace dict's reference. The two are the
    # built class's own __post_init__ binding and the struct_post_init that
    # install_post_init resolved. A second install would take a third and never
    # give it back.
    assert sys.getrefcount(post_init) - before == 2


def test_copy_takes_one_reference_per_field_and_releases_them_with_the_copy():
    """The copy is a fresh instance whose slots hold fresh references: sharing
    the value is not sharing the count. A copy that borrowed the source's
    references would leave the sentinel's count unmoved, and a copy that forgot
    to release them would leave it stuck one pair high after `del`."""

    sentinel = Sentinel()
    pair = Pair(sentinel, sentinel)
    before = sys.getrefcount(sentinel)
    copied = copy.copy(pair)

    assert sys.getrefcount(sentinel) == before + 2

    del copied

    assert sys.getrefcount(sentinel) == before


def test_a_deferred_co_base_copy_does_not_leak_the_method():
    """The deferral calls the co-base's __copy__ with an owned reference,
    and the scope exit releases it: a leak would keep the method alive one
    reference per copy."""

    class HasCopy:
        def __copy__(self):
            return self

    class Real(Struct, HasCopy, frozen=False):
        x: int

    method = HasCopy.__dict__["__copy__"]
    before = sys.getrefcount(method)

    for _ in range(1000):
        copy.copy(Real(1))

    assert sys.getrefcount(method) == before
