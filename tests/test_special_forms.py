import sys
from dataclasses import InitVar
from typing import Annotated, ClassVar, Final

import pytest

from salix import Struct


def test_a_class_var_is_refused():
    """The shape a dataclass port arrives with, which used to fail with a
    message about default ordering that never mentioned ClassVar.
    """

    with pytest.raises(TypeError, match="annotated ClassVar"):

        class Registry(Struct):
            instances: ClassVar[list] = []
            name: str


def test_a_bare_class_var_is_refused():
    with pytest.raises(TypeError, match="annotated ClassVar"):

        class Bare(Struct):
            marker: ClassVar


def test_an_init_var_is_refused():
    with pytest.raises(TypeError, match="annotated InitVar"):

        class Seeded(Struct):
            seed: InitVar[int]


def test_each_says_what_to_do_instead():
    with pytest.raises(TypeError, match="without an annotation"):
        type(Struct)("A", (Struct,), {"__annotations__": {"v": ClassVar[int]}})

    with pytest.raises(TypeError, match="set_field"):
        type(Struct)("B", (Struct,), {"__annotations__": {"v": InitVar[int]}})


def test_an_ordinary_annotation_of_the_same_shape_is_untouched():
    """`Final[int]` is a generic alias too, and is nobody's special case."""

    class Ordinary(Struct):
        a: Final[int] = 1
        b: str | None = None

    assert Ordinary._struct_fields_ == ("a", "b")


def test_a_field_named_after_the_form_is_still_a_field():
    """The annotation is what is inspected, never the name."""

    class Named(Struct):
        ClassVar: int = 1
        InitVar: int = 2

    assert Named._struct_fields_ == ("ClassVar", "InitVar")
    assert Named().ClassVar == 1


def test_a_bare_init_var_is_refused_like_a_bare_class_var():
    """`InitVar` unsubscripted is the class itself, not an instance of it."""

    with pytest.raises(TypeError, match="annotated InitVar"):

        class Seeded(Struct):
            seed: InitVar


def test_an_annotated_class_var_does_not_hide_the_form():
    """Annotated reaches the form two hops down __origin__, and wrapping it was
    otherwise the same position-swallowing bug wearing a wrapper.
    """

    with pytest.raises(TypeError, match="annotated ClassVar"):
        type(Struct)(
            "Wrapped", (Struct,), {"__annotations__": {"v": Annotated[ClassVar[int], "meta"]}}
        )


@pytest.mark.skipif(
    sys.version_info < (3, 11), reason="typing refuses Annotated[InitVar[...]] before 3.11"
)
def test_an_annotated_init_var_does_not_hide_the_form_either():
    annotation = Annotated[InitVar[int], "meta"]

    with pytest.raises(TypeError, match="annotated InitVar"):
        type(Struct)("Wrapped", (Struct,), {"__annotations__": {"v": annotation}})


@pytest.mark.parametrize(
    ("text", "form"),
    [
        ("ClassVar[int]", "ClassVar"),
        ("ClassVar", "ClassVar"),
        ("typing.ClassVar[int]", "ClassVar"),
        ("t.ClassVar[int]", "ClassVar"),
        ("InitVar[int]", "InitVar"),
        ("Annotated[ClassVar[int], 'meta']", "ClassVar"),
        ("Annotated[InitVar[int], 'meta']", "InitVar"),
        ("ClassVar ", "ClassVar"),
        ("ClassVar [int]", "ClassVar"),
        ("Annotated[ ClassVar[int], 'meta' ]", "ClassVar"),
        ("Annotated[ ClassVar ]", "ClassVar"),
        ("Annotated[InitVar]", "InitVar"),
        ("Optional[ClassVar[int]]", "ClassVar"),
        ("x\x00ClassVar[int]", "ClassVar"),
        ("'ClassVar[int]'", "ClassVar"),
        ("ClassVar\u20ac", "ClassVar"),
        ("\u20acClassVar", "ClassVar"),
        pytest.param(chr(0xD800) + "ClassVar", "ClassVar", id="lone-surrogate"),
    ],
)
def test_the_source_text_form_is_refused_too(text, form):
    """`from __future__ import annotations` leaves the annotation as its own
    source, where the spelling is all there is to go on.

    Each case is a shape the spelling has to survive, and each is a pin on the
    current rule rather than a claim about how it got here. The spacings are
    legal Python and stored verbatim. A str may hold a NUL, so the scan takes
    its length from the str and not from a terminator. The quoted one is a
    nested forward reference and has to be refused for the same reason the bare
    spelling is. The euro sign is not an identifier character, and a lone
    surrogate cannot be encoded to UTF-8 at all -- so the boundary works on
    code points, where neither is anything special. The surrogate is built with
    chr() rather than written as a literal, because a source file holding one
    cannot be compiled.

    (Every one of those was a bug at some point, which is why the list looks
    like this. None of these tests can tell you that: they would pass on an
    implementation that never had them.)

    The expected form is asserted, not just the refusal: matching only "salix
    does not support" would pass with the two `names_form` calls swapped.
    """

    with pytest.raises(TypeError, match=f"annotated {form}"):
        type(Struct)("Textual", (Struct,), {"__annotations__": {"v": text}})


@pytest.mark.parametrize(
    "text",
    [
        "int",
        "MyClassVarThing",
        "ClassVarish",
        "ClassVar_",
        "_ClassVar",
        "list[int]",
        "dict[str, int]",
        'Annotated[int, "x"]',
        "théClassVar",
        "ClassVaré",
        "ÄClassVar",
        "ClassVar\u0301",
        "ClassVar\u00b7",
        "x1ClassVar",
    ],
)
def test_a_name_that_merely_contains_the_form_is_a_field(text):
    """The boundary is an identifier character on either side, so widening what
    counts as a separator must not widen what counts as the form.

    Eleven of the fourteen are legal identifiers; the last six are the ones
    that say something a plain ASCII rule would get wrong. The combining acute
    and the middle dot are why the boundary asks Python rather than a table:
    both continue an identifier without being letters, which is the kind of
    character a hand-written rule misses in the silent direction.

    `x1ClassVar` is why the opening side asks the same question as the closing
    one. A digit continues a name without being able to start one, so a
    leading-side rule built on "can this character begin an identifier" refuses
    this, and Python is perfectly happy with it. Whether a prefix is a valid
    identifier is not decidable one character at a time.
    """

    Ordinary = type(Struct)("Ordinary", (Struct,), {"__annotations__": {"v": text}})

    assert Ordinary._struct_fields_ == ("v",)


def test_re_annotating_an_inherited_field_stays_a_no_op():
    """The guard runs after the inheritance check, so this is what it always
    was -- no new slot, no swallowed argument, and now no new refusal either.
    """

    class Base(Struct):
        x: int

    class Sub(Base):
        x: ClassVar[int]

    assert Sub._struct_fields_ == ("x",)
    assert Sub(1).x == 1


def test_a_renamed_import_is_not_resolved_in_the_source_text_form():
    """`from typing import ClassVar as CV` gives `CV[int]`, which names nothing
    the text can match. Recorded as the known hole in the heuristic rather than
    guessed at, which is what dataclasses does against sys.modules.
    """

    Escaped = type(Struct)("Escaped", (Struct,), {"__annotations__": {"v": "CV[int]"}})

    assert Escaped._struct_fields_ == ("v",)


def test_a_plain_annotated_is_still_a_field():
    """The positive control for the object path's Annotated handling: every
    other Annotated test here is a refusal, so a walk that started reporting a
    form for everything would pass all of them.
    """

    class Tagged(Struct):
        v: Annotated[int, "meta"]
        w: Annotated[list, "meta", "more"]

    assert Tagged._struct_fields_ == ("v", "w")
    assert Tagged(1, []).v == 1


@pytest.mark.parametrize("FORM", [ClassVar, InitVar], ids=["ClassVar", "InitVar"])
def test_a_real_future_annotations_module_takes_the_text_path(tmp_path, FORM):
    """Every other text-path case here hands salix a string it built itself.
    This one makes the compiler produce the annotations, which is the only way
    to know the path is reachable the way a user reaches it -- and both forms
    go through it, because what the compiler stores is the source text and the
    two forms are different text.
    """

    module = tmp_path / "future_struct.py"
    module.write_text(
        "from __future__ import annotations\n"
        f"from {FORM.__module__} import {FORM.__name__}\n"
        "from salix import Struct\n"
        "\n"
        "class Registry(Struct):\n"
        f"    instances: {FORM.__name__}[list] = []\n"
        "    name: str\n"
    )

    with pytest.raises(TypeError, match=f"annotated {FORM.__name__}"):
        exec(compile(module.read_text(), str(module), "exec"), {"__name__": "future_struct"})


def test_a_real_future_annotations_module_still_builds_ordinary_fields():
    """The other half: the text path must not refuse a class that names no form.
    Without this, refusing everything would pass the test above.
    """

    source = (
        "from __future__ import annotations\n"
        "from typing import Annotated\n"
        "from salix import Struct\n"
        "\n"
        "class Frame(Struct):\n"
        "    length: int\n"
        "    tag: Annotated[str, 'meta'] = 'x'\n"
    )
    namespace: dict[str, object] = {"__name__": "future_ordinary"}

    exec(compile(source, "<future_ordinary>", "exec"), namespace)
    Frame = namespace["Frame"]

    assert Frame._struct_fields_ == ("length", "tag")  # type: ignore[attr-defined]
    assert Frame(1).tag == "x"  # type: ignore[operator]


@pytest.mark.skipif(
    sys.version_info < (3, 11),
    reason="typing refuses a bare special form as an Annotated argument before 3.11",
)
@pytest.mark.parametrize("form", [ClassVar, InitVar], ids=["ClassVar", "InitVar"])
def test_a_bare_form_inside_annotated_is_refused_on_the_object_path(form):
    """`Annotated[ClassVar, 'meta']` -- the form unsubscripted, one hop down.
    The subscripted shape is covered above; this is the one where `__origin__`
    reaches the form object itself rather than a `_GenericAlias` of it.
    """

    label = "InitVar" if form is InitVar else "ClassVar"

    with pytest.raises(TypeError, match=f"annotated {label}"):
        type(Struct)("Wrapped", (Struct,), {"__annotations__": {"v": Annotated[form, "meta"]}})


@pytest.mark.parametrize(
    ("annotation", "form"),
    [
        (Annotated[ClassVar[int], "m"] | None, "ClassVar"),
        (list[ClassVar[int]], "ClassVar"),
        (dict[str, ClassVar[int]], "ClassVar"),
        (tuple[ClassVar[int]], "ClassVar"),
        (list[InitVar[int]], "InitVar"),
    ],
    ids=["optional-annotated", "list", "dict-value", "tuple", "list-initvar"],
)
def test_a_form_kept_in_the_arguments_is_refused(annotation, form):
    """#57's ruling: the walk reads `__args__` as well as `__origin__`, because
    a subscript keeps the form where a chain walk never looked. Every one of
    these was a field on the object path while the text path refused the same
    source, which is #14 on the path almost every class takes.
    """

    with pytest.raises(TypeError, match=f"annotated {form}"):
        type(Struct)("Nested", (Struct,), {"__annotations__": {"v": annotation}})


@pytest.mark.parametrize(
    "annotation",
    [int, list[int], dict[str, int], int | None, Annotated[int, "meta"], tuple[int, str]],
    ids=["plain", "list", "dict", "optional", "annotated", "tuple"],
)
def test_walking_the_arguments_does_not_widen_what_counts_as_a_form(annotation):
    """The other direction of the same change: reading `__args__` visits more
    objects, and none of them may start answering yes.
    """

    Ordinary = type(Struct)("Ordinary", (Struct,), {"__annotations__": {"v": annotation}})

    assert Ordinary._struct_fields_ == ("v",)


@pytest.mark.skipif(
    sys.version_info < (3, 11),
    reason="typing refuses a bare special form as an Annotated argument before 3.11",
)
def test_the_form_as_annotated_metadata_is_still_a_field_on_the_object_path():
    """`Annotated` keeps its metadata in `__metadata__`, not in `__args__`:

        Annotated[int, ClassVar].__args__      (<class 'int'>,)
        Annotated[int, ClassVar].__metadata__  (typing.ClassVar,)

    So walking the arguments does not reach it, and the cost #57 predicted for
    option 1 -- that the object path would adopt the text path's refusal of
    this shape -- is not a cost it has. The two paths still disagree here, and
    that disagreement is #57's, not this walk's.
    """

    Metadata = type(Struct)(
        "Metadata", (Struct,), {"__annotations__": {"v": Annotated[int, ClassVar]}}
    )

    assert Metadata._struct_fields_ == ("v",)


@pytest.mark.parametrize(
    "trailing",
    ["́", "·", "‌", "‍", "々", "s", "_", "1", " ", "[", "€"],
    ids=["acute", "middot", "zwnj", "zwj", "iteration-mark", "letter", "underscore",
         "digit", "space", "bracket", "euro"],
)
def test_the_boundary_agrees_with_python_about_identifiers(trailing):
    """Not a table of expected answers: the assertion is that salix reaches the
    same verdict `str.isidentifier` does, on whatever interpreter is running.

    That matters because the answer moves. CPython made ZWNJ and ZWJ continue an
    identifier in 3.13, so `ClassVar‌` is one name there and two tokens on
    3.12 -- and salix refuses it on 3.12 and accepts it on 3.13 for exactly the
    right reason. A hardcoded expectation here would pin one of those and call
    the other a bug.
    """

    for text, part_of_a_longer_name in (
        ("ClassVar" + trailing, ("a" + trailing).isidentifier()),
        (trailing + "ClassVar", ("a" + trailing).isidentifier()),
    ):
        _assert_boundary_matches_python(text, part_of_a_longer_name)


def _assert_boundary_matches_python(text: str, part_of_a_longer_name: bool) -> None:

    try:
        type(Struct)("Boundary", (Struct,), {"__annotations__": {"v": text}})
        refused = False
    except TypeError:
        refused = True

    assert refused is not part_of_a_longer_name, text


def test_a_failing_origin_probe_fails_the_class():
    """The object path asks every non-type annotation for `__origin__`, and a
    user object answers with whatever its `__getattr__` does. Not having one
    ends the walk; anything else is a failure to look, and a failure to look
    must not read as "this is an ordinary field".
    """

    class Hostile:
        def __getattr__(self, name: str) -> object:
            if name == "__origin__":
                raise RuntimeError("boom")

            raise AttributeError(name)

    with pytest.raises(RuntimeError, match="boom"):

        class Refused(Struct):
            v: Hostile()

    class Quiet:
        def __getattr__(self, name: str) -> object:
            raise AttributeError(name)

    Ordinary = type(Struct)("Ordinary", (Struct,), {"__annotations__": {"v": Quiet()}})

    assert Ordinary._struct_fields_ == ("v",)


def test_an_annotation_that_rewrites_the_annotations_does_not_take_the_walk_with_it():
    """The object path asks the annotation for `__origin__`, which runs the
    class author's code, which can reach the dict being walked. `PyDict_Next`
    is only defined while the dict is unmodified, so the names are taken first
    and each annotation is looked up again as it is reached.

    A field added during the walk is not declared -- it was not there when the
    class body ended -- and one deleted during it is skipped rather than read
    from a pointer the dict no longer owns.
    """

    annotations = {}

    class Rewrites:
        def __getattr__(self, name: str) -> object:
            if name == "__origin__":
                annotations.pop("doomed", None)

                for i in range(64):
                    annotations[f"grown{i}"] = int

            raise AttributeError(name)

    annotations.update({"first": Rewrites(), "doomed": int, "last": int})

    Built = type(Struct)("Rewritten", (Struct,), {"__annotations__": annotations})

    assert Built._struct_fields_ == ("first", "last")


@pytest.mark.skipif(sys.version_info < (3, 12), reason="PEP 695 `type` is a syntax error before 3.12")
@pytest.mark.parametrize(
    ("alias", "form"),
    [
        ("type Aliased = ClassVar[int]", "ClassVar"),
        ("type Aliased = InitVar[int]", "InitVar"),
        ("type Inner = ClassVar[int]\ntype Aliased = list[Inner]", "ClassVar"),
    ],
    ids=["ClassVar", "InitVar", "through-a-subscript"],
)
def test_a_pep_695_alias_is_the_form_it_aliases(alias, form):
    """A `TypeAliasType` has neither `__origin__` nor `__args__`, so the walk
    reads `__value__` as well -- asked only where the other two were absent,
    which is what a TypeAliasType looks like and what an ordinary subscripted
    annotation never does.

    JPH approved this on #57 after the `__args__` walk landed; it is the same
    #14 symptom reached by a different attribute.
    """

    namespace = {}
    exec(f"from typing import ClassVar\nfrom dataclasses import InitVar\n{alias}", namespace)

    with pytest.raises(TypeError, match=f"annotated {form}"):
        type(Struct)("Aliased", (Struct,), {"__annotations__": {"v": namespace["Aliased"]}})


@pytest.mark.skipif(sys.version_info < (3, 12), reason="PEP 695 `type` is a syntax error before 3.12")
def test_a_pep_695_alias_to_something_else_is_still_a_field():
    """The other direction: following `__value__` must not make every alias
    suspect. One that aliases a plain type answers no, the way it did before.
    """

    namespace = {}
    exec("type Aliased = int", namespace)

    Ordinary = type(Struct)(
        "Ordinary", (Struct,), {"__annotations__": {"v": namespace["Aliased"]}}
    )

    assert Ordinary._struct_fields_ == ("v",)


def test_a_str_injected_during_the_walk_is_matched_rather_than_crashing():
    """The text matcher's needles are built at the first str annotation, and
    not from a question asked of the dict before the loop.

    The walk runs the class author's `__getattr__`, which can put a str into
    that dict after such a question has answered "there is no text here". That
    combination segfaulted: the answer was cached, the needles stayed NULL, and
    the injected str reached `PyUnicode_GET_LENGTH(NULL)`.

    Asserted as the refusal it should be, because a segfault takes pytest with
    it and there would be nothing left to report.
    """

    annotations = {}

    class Injects:
        def __getattr__(self, name: str) -> object:
            if name == "__origin__":
                annotations["late"] = "ClassVar[int]"

            raise AttributeError(name)

    annotations.update({"first": Injects(), "late": int})

    with pytest.raises(TypeError, match="annotated ClassVar"):
        type(Struct)("Injected", (Struct,), {"__annotations__": annotations})


def test_the_object_path_is_skipped_when_neither_module_is_loaded(monkeypatch):
    """Both probes absent means no annotation object can be either form, so the
    walk is skipped entirely and an ordinary class still builds -- asserted by
    an annotation that records every attribute anyone asks it for.

    Removed from a populated sys.modules rather than run on a bare interpreter,
    which is the shape available here. It is the same state a module salix has
    not imported would produce, but this test does not establish that salix
    imports neither: what it pins is what the code does when they are absent.
    """

    monkeypatch.delitem(sys.modules, "typing", raising=False)
    monkeypatch.delitem(sys.modules, "dataclasses", raising=False)

    probed = []

    class Watched:
        def __getattr__(self, name: str) -> object:
            probed.append(name)
            raise AttributeError(name)

    Ordinary = type(Struct)("Ordinary", (Struct,), {"__annotations__": {"v": Watched()}})

    assert Ordinary._struct_fields_ == ("v",)
    assert probed == []
