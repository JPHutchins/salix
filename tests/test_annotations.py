import functools
import sys

import pytest

from salix import Struct


def boom():
    """An annotation that fails for its own reasons, wearing a NameError.

    Shared by the two tests that read the exemption's boundary from opposite
    sides, so that moving the boundary cannot leave one of them agreeing with
    the old answer.
    """

    raise NameError("boom")


def test_a_hand_written_annotate_is_called_directly():
    """The only path an interpreter without PEP 649 has: no __annotations__ in
    the namespace, and an __annotate__ the class body wrote itself. Outside the
    class below because that pre-3.14 branch has nothing else covering it.
    """

    def annotate(format):
        return {"x": int, "y": int}

    Manual = type(Struct)("Manual", (Struct,), {"__annotate__": annotate})

    assert Manual._struct_fields_ == ("x", "y")
    assert Manual(1, 2).x == 1


class TestANonFunctionAnnotate:
    """annotationlib's FORWARDREF path rebuilds the callable from its
    __globals__ and __code__, which only a plain function has. Anything else is
    left unescalated so it raises what a plain function raises, rather than an
    AttributeError about the missing attribute.
    """

    @staticmethod
    def protocol_correct(format):
        """PEP 649 says: implement VALUE, raise for the rest."""

        if format != 1:
            raise NotImplementedError

        return {"x": Undefined}  # noqa: F821 -- unresolvable on purpose

    @staticmethod
    def wrapped_in_an_object(annotate):
        class Wrapper:
            def __call__(self, format):
                return annotate(format)

        return Wrapper()

    @pytest.mark.parametrize("wrap", ["plain", "partial", "callable"])
    def test_an_unresolvable_name_reports_itself_whatever_the_callable_is(self, wrap):
        """All three surface the NameError rather than an AttributeError about
        __globals__, which is what the PyFunction_Check guard is for.

        The message alone cannot tell any of that: a protocol-correct annotate
        refuses VALUE_WITH_FAKE_GLOBALS too, so annotationlib falls back to a
        real-globals VALUE re-run and raises the same NameError a direct call
        would have. `__context__` is what differs, and asserting it makes both
        the guard and the escalation detectable here -- deleting either one
        moves a `__context__` that this now checks.
        """

        annotate = TestANonFunctionAnnotate.protocol_correct
        wrapped = {
            "plain": annotate,
            "partial": functools.partial(annotate),
            "callable": TestANonFunctionAnnotate.wrapped_in_an_object(annotate),
        }[wrap]

        with pytest.raises(NameError, match="Undefined") as raised:
            type(Struct)("Wrapped", (Struct,), {"__annotate__": wrapped})

        # The discriminator the message cannot carry: only the plain function
        # reaches annotationlib, and only it comes back with a __context__ from
        # having done so. Below 3.14 nothing escalates, so nobody has one.
        escalates = wrap == "plain" and sys.version_info >= (3, 14)

        assert (raised.value.__context__ is not None) is escalates

    def test_a_callable_object_is_accepted_when_its_names_resolve(self):
        class Annotate:
            def __call__(self, format):
                return {"x": int, "y": int}

        Built = type(Struct)("Built", (Struct,), {"__annotate__": Annotate()})

        assert Built._struct_fields_ == ("x", "y")
        assert Built(1, 2).y == 2

    @pytest.mark.skipif(
        sys.version_info < (3, 14),
        reason="the escalation this drives is compiled out before 3.14",
    )
    def test_a_plain_function_can_reach_the_escalation_success_branch(self):
        """A hand-written annotate returning through `escalated != NULL`.
        `test_the_escalation_answers_with_what_forwardref_returned` is the
        other; this one is about reaching the branch, that one about what comes
        back from it.

        annotationlib rebuilds the function against fake globals and calls the
        copy with VALUE_WITH_FAKE_GLOBALS, where names resolve to stringifiers.
        A hand-written annotate reaches this only by evaluating for that format
        -- one that answers NotImplementedError to everything but VALUE drives
        the failure branch instead, which is the parametrized test above.
        """

        def fake_globals_aware(format):
            if format in (1, 2):
                return {"x": Undefined, "y": int}  # noqa: F821 -- unresolvable on purpose

            raise NotImplementedError

        Built = type(Struct)("Built", (Struct,), {"__annotate__": fake_globals_aware})

        assert Built._struct_fields_ == ("x", "y")
        assert Built(1, 2).y == 2

    @pytest.mark.skipif(
        sys.version_info < (3, 14),
        reason="the escalation this describes is compiled out before 3.14",
    )
    def test_the_escalation_answers_with_what_forwardref_returned(self):
        """A hand-written annotate that answers different keys for the two
        formats gets the FORWARDREF ones, because that is the call that
        succeeded -- there is no VALUE dict to prefer, only the NameError it
        raised.

        Diffing the two would mean asking twice on every rescue to catch an
        annotate that contradicts itself. Recorded rather than guarded.
        """

        def inconsistent(format):
            if format == 1:
                raise NameError("nope", name="nope")

            # Keyed by format, so this fails rather than passes if the
            # escalation ever asks for STRING (4) instead of FORWARDREF (3).
            return {f"answered_{format}": int}

        Built = type(Struct)("Built", (Struct,), {"__annotate__": inconsistent})

        assert Built._struct_fields_ == ("answered_3",)

    @pytest.mark.skipif(
        sys.version_info < (3, 14),
        reason="the discriminator this pins is compiled out before 3.14",
    )
    def test_an_empty_name_is_not_a_forward_reference(self):
        """`.name` set but empty names no symbol, so it is not the interpreter
        saying a lookup failed. The boundary of the discriminator, pinned.
        """

        def forged():
            raise NameError(name="")

        with pytest.raises(NameError):

            class Refused(Struct):
                v: forged()

    def test_an_annotate_that_refuses_VALUE_is_refused_back(self):
        """PEP 649 makes VALUE the one format an __annotate__ must implement,
        and salix asks for it first. An older flow tried another format first
        and fell back, so an annotate implementing only that one built its
        class; it does not any more, and this is the shape that changed.
        """

        def inverted(format):
            if format == 1:
                raise NotImplementedError

            return {"x": int}

        with pytest.raises(NotImplementedError):
            type(Struct)("Inverted", (Struct,), {"__annotate__": inverted})

    def test_a_VALUE_failure_of_any_other_kind_is_not_rescued_either(self):
        """NotImplementedError is one of the shapes the removed flow retried,
        not the shape. The gate is three conditions -- a plain function, a
        NameError, a non-empty `.name` -- so anything failing VALUE for any
        other reason now propagates even where the other arm would have
        answered.
        """

        def refuses(format):
            if format == 1:
                raise ValueError("boom")

            return {"x": int}

        with pytest.raises(ValueError, match="boom"):
            type(Struct)("Refuses", (Struct,), {"__annotate__": refuses})

    @pytest.mark.skipif(
        sys.version_info < (3, 14),
        reason="the discriminator this pins is compiled out before 3.14",
    )
    def test_a_hand_written_empty_name_is_not_a_forward_reference_either(self):
        """The generated-annotate half is
        `test_an_empty_name_is_not_a_forward_reference`. This is the shape where
        the escalation would have answered, so the narrowing is the only thing
        stopping it and deleting the length check makes this build.
        """

        def forged(format):
            if format == 1:
                raise NameError(name="")

            return {"x": int}

        with pytest.raises(NameError):
            type(Struct)("Forged", (Struct,), {"__annotate__": forged})

    @pytest.mark.skipif(
        sys.version_info < (3, 14),
        reason="the escalation this describes is compiled out before 3.14",
    )
    def test_an_escalation_that_answers_with_a_non_dict_loses_the_name(self):
        """FORWARDREF's answer is handed back unchecked, so a non-dict is
        refused downstream by the `__annotations__` check and the name that
        started the rescue is gone from the message. The cost of not validating
        here, pinned rather than left to be found.
        """

        def answers_a_list(format):
            if format == 1:
                raise NameError("nope", name="Missing")

            return ["x"]

        with pytest.raises(TypeError, match="__annotations__ must be a dict") as raised:
            type(Struct)("Listed", (Struct,), {"__annotate__": answers_a_list})

        assert "Missing" not in str(raised.value)

    def test_a_partial_is_accepted_when_its_names_resolve(self):
        def annotate(format):
            return {"z": int}

        Built = type(Struct)("Built", (Struct,), {"__annotate__": functools.partial(annotate)})

        assert Built._struct_fields_ == ("z",)
        assert Built(1).z == 1


@pytest.mark.skipif(
    sys.version_info < (3, 14),
    reason="before PEP 649 an annotation is evaluated where it is written",
)
class TestForwardReferences:
    def test_a_bare_self_reference_is_a_field(self):
        class Node(Struct):
            value: int
            nxt: Node = None  # noqa: F821

        assert Node._struct_fields_ == ("value", "nxt")
        assert Node(1, Node(2)).nxt.value == 2

    def test_an_annotation_that_never_resolves_is_a_field_anyway(self):
        class Bare(Struct):
            x: NeverDefined  # noqa: F821

        assert Bare._struct_fields_ == ("x",)
        assert Bare(1).x == 1

    def test_two_classes_may_refer_to_each_other(self):
        class Left(Struct):
            right: Right = None  # noqa: F821

        class Right(Struct):
            left: Left = None

        assert Left(Right()).right.left is None

    def test_the_order_survives_a_mix_of_resolvable_and_not(self):
        class Mixed(Struct):
            first: int
            second: Unresolvable  # noqa: F821
            third: str

        assert Mixed._struct_fields_ == ("first", "second", "third")

    def test_a_subclass_may_forward_reference_too(self):
        class Base(Struct):
            x: int

        class Child(Base):
            y: Child = None  # noqa: F821

        assert Child._struct_fields_ == ("x", "y")

    def test_the_annotations_still_resolve_once_the_class_exists(self):
        """Reading them early does not leave a ForwardRef behind for everyone else."""

        class Node(Struct):
            nxt: Node = None  # noqa: F821

        assert Node.__annotations__ == {"nxt": Node}

    def test_an_annotation_that_fails_for_its_own_reasons_says_so(self):
        """Not resolving a name is the exemption; arbitrary failure is not."""

        with pytest.raises(ZeroDivisionError):

            class Broken(Struct):
                x: 1 / 0

    def test_a_second_bad_annotation_does_not_bury_the_first(self):
        """The escalation re-evaluates every annotation, so a later one that
        fails for its own reasons raises during the rescue of the earlier one.
        The name that did not resolve is what the reader needs.
        """

        with pytest.raises(NameError, match="Missing") as raised:

            class Broken(Struct):
                x: Missing  # noqa: F821
                y: 1 / 0

        assert isinstance(raised.value.__context__, ZeroDivisionError)

    def test_a_NameError_from_inside_a_called_function_still_defers(self):
        """`.name` is filled in wherever the lookup failed, including inside a
        callee, so a buggy helper reads as a forward reference and the class
        builds. Plain 3.14 defers this too -- the divergence is the explicit
        `raise NameError`, which salix propagates and CPython would defer.

        How many times annotationlib re-runs the annotation on the way there is
        its business and not asserted: a count is not the claim, and it moves
        when annotationlib changes without the claim moving with it.
        """

        def helper():
            return undefined_inside  # noqa: F821

        class Deferred(Struct):
            x: helper()

        assert Deferred._struct_fields_ == ("x",)

    def test_a_forged_name_attribute_reads_as_a_forward_reference(self):
        """The discriminator asks whether `.name` is set, and the keyword form
        sets it -- so this is the exemption's boundary, not a wall. Pinned
        because the positional form beside it propagates, and the difference is
        one word in the raising code.
        """

        def forged():
            raise NameError(name="x")

        class Built(Struct):
            v: forged()

        assert Built._struct_fields_ == ("v",)

    def test_the_exemption_is_order_dependent(self):
        """The rescue is all-or-nothing: it re-evaluates every annotation and
        stringifies what it cannot resolve, so an arbitrary failure that comes
        *after* a forward reference is swallowed with it, and the same pair in
        the other order propagates.

        Not fixable while the rescue goes through `__annotate__`, which hands
        back the whole dict or nothing.
        """

        class Rescued(Struct):
            x: Missing  # noqa: F821
            y: boom()

        assert Rescued._struct_fields_ == ("x", "y")

        with pytest.raises(NameError, match="boom"):

            class Propagates(Struct):
                y: boom()
                x: Missing  # noqa: F821

    def test_the_annotations_beside_a_forward_reference_evaluate_again(self):
        """annotationlib rebuilds the callable and re-runs the whole dict, so
        the siblings of an unresolved name are evaluated a second time where
        plain 3.14 evaluates once and defers. That divergence is the claim.

        Not the number: how many times annotationlib re-runs the annotation is
        its business, which is what
        `test_a_NameError_from_inside_a_called_function_still_defers` says a few
        tests up. Asserting 2 here would have made this file hold two policies.
        """

        evaluations = []

        def counted():
            evaluations.append(1)

            return int

        class Mixed(Struct):
            a: counted()
            b: Missing  # noqa: F821

        assert Mixed._struct_fields_ == ("a", "b")
        assert len(evaluations) > 1

    def test_a_raised_NameError_is_arbitrary_failure_too(self):
        """The exemption is the interpreter's failure to find a name, not the
        exception type it uses to say so.
        """

        with pytest.raises(NameError, match="boom"):

            class Broken(Struct):
                x: boom()

    def test_the_name_that_did_not_resolve_survives_a_failed_escalation(self, monkeypatch):
        """The escalation needs annotationlib, and the NameError it displaced is
        the one worth reading if that import is what fails.
        """

        monkeypatch.setitem(sys.modules, "annotationlib", None)

        with pytest.raises(NameError, match="Missing") as raised:

            class Shadowed(Struct):
                x: Missing  # noqa: F821

        assert isinstance(raised.value.__context__, ImportError)

    def test_an_unresolved_name_inside_a_larger_expression_still_defers(self):
        """`.name` is filled in wherever the lookup was, so the exemption does
        not stop at a bare annotation.
        """

        class Deferred(Struct):
            x: Unresolvable + 1  # noqa: F821

        assert Deferred._struct_fields_ == ("x",)
        assert Deferred(1).x == 1


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
def test_a_pre_existing_context_is_left_alone(monkeypatch):
    """The rescue's own failure is attached only where the NameError arrived
    without a chain.

    A stand-in for annotationlib records the call, because the ValueError this
    checks for is put there by the enclosing `except` block and would be there
    in a world where no rescue ran at all.
    """

    formats = []

    class Recording:
        @staticmethod
        def call_annotate_function(annotate, format):
            formats.append(format)

            raise ModuleNotFoundError("no annotationlib")

    monkeypatch.setitem(sys.modules, "annotationlib", Recording)

    try:
        raise ValueError("earlier")
    except ValueError:
        with pytest.raises(NameError, match="Missing") as raised:

            class Chained(Struct):
                x: Missing  # noqa: F821

    assert formats == [3]
    assert isinstance(raised.value.__context__, ValueError)
    assert raised.value.__context__.__context__ is None


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
def test_a_cause_only_chain_counts_as_a_chain(monkeypatch):
    """`raise ... from` leaves `__context__` empty, so reading only that field
    would append the rescue's failure beside a cause the raising code chose.
    Both fields are read, and neither is written over.
    """

    monkeypatch.setitem(sys.modules, "annotationlib", None)

    def annotate(format):
        if format == 1:
            raise NameError("nope", name="Missing") from ValueError("the cause")

        return {"x": int}

    with pytest.raises(NameError, match="nope") as raised:
        type(Struct)("Caused", (Struct,), {"__annotate__": annotate})

    assert isinstance(raised.value.__cause__, ValueError)
    assert raised.value.__context__ is None


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
def test_a_suppressed_chain_counts_as_a_chain_too(monkeypatch):
    """The mirror of the test above, and the shape neither exception field can
    see: `from None` stores None as the cause, which reads back as no cause at
    all. It is the loudest way to ask for nothing to be attached, so the
    suppression flag is read as well.
    """

    monkeypatch.setitem(sys.modules, "annotationlib", None)

    def annotate(format):
        if format == 1:
            raise NameError("nope", name="Missing") from None

        return {"x": int}

    with pytest.raises(NameError, match="nope") as raised:
        type(Struct)("Suppressed", (Struct,), {"__annotate__": annotate})

    assert raised.value.__suppress_context__ is True
    assert raised.value.__cause__ is None
    assert raised.value.__context__ is None


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
def test_an_interrupt_during_the_rescue_is_not_demoted_to_context():
    """A rescue failure becomes the NameError's `__context__`; an exit is not a
    failure to diagnose. Swallowing KeyboardInterrupt to report a name would be
    the worst of both.
    """

    def annotate(format):
        if format == 1:
            raise NameError(name="Missing")

        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt) as raised:
        type(Struct)("Interrupted", (Struct,), {"__annotate__": annotate})

    assert isinstance(raised.value.__context__, NameError)


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
def test_an_interrupt_that_refuses_a_chain_keeps_refusing_it():
    """Where the exit rule and the suppression field meet: the interrupt wins,
    and then the field it was raised with decides whether the displaced name
    goes behind it. `from None` says no, and nothing is attached.

    Neither of the two tests around this reaches it -- one raises a bare
    interrupt (the name is attached) and one raises it inside an `except` (the
    name is dropped for a chain that already exists). This is the third answer,
    and the only one that reads the field.
    """

    def annotate(format):
        if format == 1:
            raise NameError(name="Missing")

        raise KeyboardInterrupt from None

    with pytest.raises(KeyboardInterrupt) as raised:
        type(Struct)("Interrupted", (Struct,), {"__annotate__": annotate})

    assert raised.value.__context__ is None
    assert raised.value.__suppress_context__ is True


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
def test_an_interrupt_that_arrives_chained_keeps_the_chain_it_came_with():
    """The other half of the same rule, and the half where the name is lost: an
    exit raised from inside an `except` block already carries a chain, and the
    displaced NameError is dropped rather than put somewhere it would displace
    what the exit came with.
    """

    def annotate(format):
        if format == 1:
            raise NameError(name="Missing")

        try:
            raise ValueError("earlier")
        except ValueError:
            raise KeyboardInterrupt  # noqa: B904 -- the shape under test

    with pytest.raises(KeyboardInterrupt) as raised:
        type(Struct)("Interrupted", (Struct,), {"__annotate__": annotate})

    assert isinstance(raised.value.__context__, ValueError)
    assert raised.value.__context__.__context__ is None


@pytest.mark.skipif(
    sys.version_info < (3, 14),
    reason="the discriminator this drives is compiled out before 3.14",
)
class TestAHostileNameAttribute:
    """Reading `.name` runs the attribute protocol on an exception whose class
    the annotation author wrote, so looking can raise. What it raises gets the
    same rule as a rescue failure rather than one of its own.
    """

    @staticmethod
    def raising(error):
        def annotate(format):
            raise error

        return annotate

    def test_an_exit_from_the_lookup_wins(self):
        class Hostile(NameError):
            def __getattribute__(self, attribute):
                if attribute == "name":
                    raise KeyboardInterrupt

                return super().__getattribute__(attribute)

        with pytest.raises(KeyboardInterrupt) as raised:
            type(Struct)("H", (Struct,), {"__annotate__": self.raising(Hostile("x"))})

        assert isinstance(raised.value.__context__, Hostile)

    def test_an_ordinary_failure_from_the_lookup_loses(self):
        class Hostile(NameError):
            def __getattribute__(self, attribute):
                if attribute == "name":
                    raise RuntimeError("looking hurt")

                return super().__getattribute__(attribute)

        with pytest.raises(Hostile) as raised:
            type(Struct)("H", (Struct,), {"__annotate__": self.raising(Hostile("x"))})

        assert isinstance(raised.value.__context__, RuntimeError)


@pytest.mark.skipif(sys.version_info < (3, 14), reason="no escalation before 3.14")
class TestAShadowedSuppressionFlag:
    """The suppression flag is read as the field `raise ... from None` writes,
    so the attribute of the same name is never asked for. What the annotation
    author's class says through that attribute -- truthy, falsy, or an
    exception -- reaches nothing here, and only the raiser is heard.

    The `.name` read is the other way round, because there is no field to read:
    see TestAHostileNameAttribute, where a hostile lookup does decide the
    outcome.
    """

    @staticmethod
    def raising(error, monkeypatch):
        monkeypatch.setitem(sys.modules, "annotationlib", None)

        def annotate(format):
            if format == 1:
                raise error

            return {"x": int}

        return annotate

    def test_the_attribute_is_not_read_at_all(self, monkeypatch):
        """Counted rather than raised, and that is the finding this replaces: a
        `__getattribute__` that raises on the flag takes the *reporter* down
        with it, because `traceback.py` reads `__suppress_context__` while it
        formats. So the version of this test that raised could not report its
        own failure -- it ended the pytest session instead.

        Counting says the same thing and says it directly. `reads` is the
        design: the field is what is consulted, so the attribute is never asked
        for, by anyone, on the way to the answer below.
        """

        reads = []

        class Watched(NameError):
            def __getattribute__(self, attribute):
                if attribute == "__suppress_context__":
                    reads.append(attribute)

                return super().__getattribute__(attribute)

        error = Watched("nope", name="Missing")

        with pytest.raises(Watched) as raised:
            type(Struct)("H", (Struct,), {"__annotate__": self.raising(error, monkeypatch)})

        assert reads == []
        assert isinstance(raised.value.__context__, ImportError)

    def test_a_truthy_shadow_does_not_refuse_the_context(self, monkeypatch):
        class Shadowed(NameError):
            __suppress_context__ = 1

        error = Shadowed("nope", name="Missing")

        with pytest.raises(Shadowed) as raised:
            type(Struct)("H", (Struct,), {"__annotate__": self.raising(error, monkeypatch)})

        assert raised.value.__suppress_context__ == 1
        assert isinstance(raised.value.__context__, ImportError)

    def test_a_falsy_shadow_does_not_undo_a_from_None(self, monkeypatch):
        """The shape the attribute read got wrong: `from None` sets the field,
        the shadow answers for the attribute, and the two disagree.
        """

        class Shadowed(NameError):
            __suppress_context__ = 0

        monkeypatch.setitem(sys.modules, "annotationlib", None)

        def annotate(format):
            if format == 1:
                raise Shadowed("nope", name="Missing") from None

            return {"x": int}

        with pytest.raises(Shadowed) as raised:
            type(Struct)("H", (Struct,), {"__annotate__": annotate})

        assert raised.value.__suppress_context__ == 0
        assert raised.value.__context__ is None
