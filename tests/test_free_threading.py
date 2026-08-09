import pickle
import sys
import sysconfig
import threading

import pytest

from salix import Struct, set_field

pytestmark = pytest.mark.skipif(
    not sysconfig.get_config_var("Py_GIL_DISABLED"), reason="not a free-threaded build"
)

THREADS = 8
ITERATIONS = 20_000


class Point(Struct):
    x: int
    y: int = 7


class Picklable(Struct, frozen=False):
    """Module level, because pickle looks a class up by qualified name."""

    value: object


class SealedPicklable(Struct):
    value: object


class Dicted:
    pass


class DictCarrying(Struct, Dicted, frozen=False):
    value: object


def test_importing_does_not_re_enable_the_gil():
    assert not sys._is_gil_enabled()


def run_on_every_thread(work):
    failures: list[BaseException] = []

    def guarded():
        try:
            work()
        except BaseException as failure:  # noqa: BLE001 -- any failure in a thread is the result
            failures.append(failure)

    threads = [threading.Thread(target=guarded) for _ in range(THREADS)]

    for thread in threads:
        thread.start()

    for thread in threads:
        thread.join()

    assert not failures, failures[:3]


def test_concurrent_construction_and_reads():
    def work():
        for i in range(ITERATIONS):
            point = Point(i)

            assert point.x == i
            assert point.y == 7
            assert point == Point(i, 7)
            assert hash(point) == hash((i, 7))
            assert repr(point) == f"Point(x={i}, y=7)"

    run_on_every_thread(work)


def test_concurrent_subclass_creation():
    def work():
        for i in range(ITERATIONS // 100):
            subclass = type(Point)("Sub", (Point,), {"__annotations__": {"z": int}, "z": i})

            assert subclass(1).z == i
            assert subclass.__match_args__ == ("x", "y", "z")

    run_on_every_thread(work)


def test_concurrent_writes_to_a_shared_mutable_struct():
    """Writes go through CPython's own member descriptor, so whatever a
    free-threaded build guarantees for __slots__ is inherited rather than
    reimplemented. This is the check on that claim."""

    class Counter(Struct, frozen=False):
        value: object

    shared = Counter(0)

    def work():
        for i in range(ITERATIONS):
            shared.value = i
            assert isinstance(shared.value, int)

    run_on_every_thread(work)


def test_concurrent_set_field_on_a_shared_frozen_struct():
    """The write path the descriptor test above does not cover, and the one that
    used to take the interpreter with it: eight threads writing one slot with a
    plain load-store-decref release the same previous value twice.

    The initial value is one element and every written one is two, so a
    set_field that quietly stopped writing would fail the assertion rather than
    pass on the value it started with.
    """

    class Holder(Struct):
        value: object

    shared = Holder([0])

    def work():
        for i in range(ITERATIONS):
            set_field(shared, "value", [i, i])

            assert len(shared.value) == 2

    run_on_every_thread(work)


def test_a_shared_struct_is_safe_to_read_from_every_thread():
    shared = Point(1, 2)

    def work():
        for _ in range(ITERATIONS):
            assert shared.x == 1
            assert shared._struct_fields_ == ("x", "y")

    run_on_every_thread(work)


def test_a_shared_struct_is_safe_to_read_while_another_thread_writes_it():
    """The read side of the write tests above, and the one that segfaulted.

    Every reader loaded a slot and then increffed what it found, which is two
    steps with a window between them; a concurrent write freed the pointer
    inside it. Measured before the fix, four writers and three readers of one
    kind: `repr` exited 134/139/134 and `==` 139/134/134, while the same loop
    reading through CPython's own member descriptor survived every time.

    Survival is the assertion, because a segfault takes pytest with it. The
    value assertions are there so that a reader which quietly stopped reading
    would fail rather than pass.
    """

    class Shared(Struct, frozen=False):
        value: object

    # Frozen, so that it is hashable -- and still writable through set_field,
    # which is what makes hash the third reader this can race rather than a
    # fourth it cannot.
    class Sealed(Struct):
        value: object

    shared = Shared([0, 0])
    sealed = Sealed((0, 0))

    # Never equal to any list the writer produces, so the comparison is a real
    # walk of both slots every time rather than an early out -- and never equal
    # by accident when the writer happens to reach the value it holds.
    other = Shared(object())

    # Denser than the tests above, because the window is between a load and the
    # incref that follows it and an assertion in the loop is wide enough to hide
    # it: at ITERATIONS with a check every pass, the unfixed build survives.
    rounds = ITERATIONS * 5

    def write():
        for i in range(rounds):
            shared.value = [i, i]
            set_field(sealed, "value", (i, i))

    def read():
        for i in range(rounds):
            rendered = repr(shared)
            equal = shared == other
            hashed = hash(sealed)

            if i % 1000 == 0:
                assert rendered.startswith("Shared(value=")
                assert equal is False
                assert isinstance(hashed, int)

    # Sliced to the thread count rather than doubled from half of it, so an odd
    # THREADS is one role short of a writer instead of one thread short of a
    # role.
    roles = iter(([write, read] * THREADS)[:THREADS])
    claim = threading.Lock()

    def work():
        with claim:
            role = next(roles)

        role()

    run_on_every_thread(work)


def test_concurrent_pickling_while_another_thread_writes_it():
    """__getstate__ reads each slot under the same per-slot critical section
    repr and == use; this is the check that pickle composes that read against
    concurrent writers on the same instance, which is the crash the reader
    tests above pinned. Only the get half is under fire: loads creates a
    fresh instance, so a restore never runs on a shared one.

    Survival is the assertion, because a segfault takes pytest with it. The
    value assertions tolerate any state the struct ever held: the writers only
    ever store two-element lists and two-tuples, so a restored value of any
    other length means a reader saw a torn slot.
    """

    shared = Picklable([0, 0])
    sealed = SealedPicklable((0, 0))
    rounds = ITERATIONS * 5

    def write():
        for i in range(rounds):
            shared.value = [i, i]
            set_field(sealed, "value", (i, i))

    def pickle_round_trip():
        for i in range(rounds):
            restored = pickle.loads(pickle.dumps(shared if i % 2 else sealed))
            value = restored.value

            assert len(value) == 2

    roles = iter(([write, pickle_round_trip] * THREADS)[:THREADS])
    claim = threading.Lock()

    def work():
        with claim:
            role = next(roles)

        role()

    run_on_every_thread(work)


def test_concurrent_getstate_while_another_thread_writes_the_instance_dict():
    """The dict half of __getstate__, which the racers above cannot reach:
    none of them carries a __dict__. The dict slot is read under the
    instance's critical section and copied under the dict's own lock into the
    state's third element, and this is the check that the result composes
    with concurrent attr-assigns into that dict.

    saw_dict_entry pins that the branch actually ran, and restored.value
    stays 0 because the writer never touches the slot.
    """

    shared = DictCarrying(0)
    # Seed an entry before the threads race, so a reader that runs before the
    # writer's first store still observes a non-empty dict and sets the flag.
    shared.extra = 0
    rounds = ITERATIONS * 5
    saw_dict_entry = [False]

    def write():
        for i in range(rounds):
            shared.extra = i

    def read_state_and_round_trip():
        for i in range(rounds):
            state = shared.__getstate__()

            if "extra" in state[2]:
                saw_dict_entry[0] = True

            if i % 1000 == 0:
                restored = pickle.loads(pickle.dumps(shared))
                assert restored.value == 0

    roles = iter(([write, read_state_and_round_trip] * THREADS)[:THREADS])
    claim = threading.Lock()

    def work():
        with claim:
            role = next(roles)

        role()

    run_on_every_thread(work)

    assert saw_dict_entry[0]


def test_a_shared_ordered_struct_is_safe_to_compare_while_another_thread_writes_it():
    """The fourth reader. ordering_result takes its pair of slots the way
    values_equal does, and nothing else here reaches it.

    It is the weaker of the four, and honestly so: reverting just this loop to
    a borrowed read survives 3/3 here, and 3/3 again at five times these rounds.
    The same was true of hash before the fix -- it read exactly as == did and
    exited 0/0/0 while == exited 139/134/134. One racing load per call, behind a
    names_equal tuple comparison and the richcompare dispatch, is apparently not
    often enough. So this pins that ordering keeps working under concurrent
    writes rather than that it would crash without the lock; the crash is pinned
    by the test above, through the helper both paths share.

    `ordered is True` is constant by design and cannot fail in the race
    direction: floor is below every tuple the writer stores, so a reader that
    returns a stale but still-above-floor value answers True exactly as a live
    one does. What it does catch is a read path that raises, or that returns
    anything other than True. Making the writer straddle floor would not buy the
    stale case either -- the reader then cannot know which value is current, so
    it could assert nothing about the result at all.
    """

    class Ranked(Struct, frozen=False, order=True):
        value: object

    shared = Ranked((0, 0))

    # Below every tuple the writer produces, so the comparison has a determinate
    # answer however far the writer has got -- and unequal at the first field,
    # which is the branch that hands both slots to PyObject_RichCompare.
    floor = Ranked((-1, -1))
    rounds = ITERATIONS * 5

    def write():
        for i in range(rounds):
            shared.value = (i, i)

    def read():
        for i in range(rounds):
            ordered = floor < shared

            if i % 1000 == 0:
                assert ordered is True

    roles = iter(([write, read] * THREADS)[:THREADS])
    claim = threading.Lock()

    def work():
        with claim:
            role = next(roles)

        role()

    run_on_every_thread(work)
