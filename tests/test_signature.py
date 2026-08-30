import importlib.metadata
import inspect
import os
from pathlib import Path
from typing import Generic, TypeVar

import pytest

from salix import Struct

try:
    import tomllib
except ImportError:
    import tomli as tomllib

T = TypeVar("T")


class Point(Struct):
    x: float
    y: float = 0.0


class Plain(Struct):
    a: int
    b: str


class WithInit(Struct):
    def __init__(self, a: int) -> None:
        pass


class Inherited(Point):
    z: int = 3


class Generic(Struct, Generic[T]):
    value: T


def parameters(signature: inspect.Signature) -> list[tuple[str, object, object, object]]:
    return [
        (
            name,
            parameter.kind,
            parameter.default,
            parameter.annotation,
        )
        for name, parameter in signature.parameters.items()
    ]


def renamed_signature() -> inspect.Signature:
    return inspect.Signature(
        [inspect.Parameter("renamed", inspect.Parameter.POSITIONAL_OR_KEYWORD)]
    )


def test_inspect_signature_reads_the_fields_and_defaults():
    signature = inspect.signature(Point)

    assert parameters(signature) == [
        ("x", inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.empty, float),
        ("y", inspect.Parameter.POSITIONAL_OR_KEYWORD, 0.0, float),
    ]


def test_a_class_without_defaults_reports_empty_defaults():
    signature = inspect.signature(Plain)

    assert parameters(signature) == [
        ("a", inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.empty, int),
        ("b", inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.empty, str),
    ]


def test_an_instance_reports_the_class_signature():
    """Instances are not callable, so inspect.signature raises before it can
    consult __signature__; the attribute itself is the pin."""

    assert Point(1.0).__signature__ == inspect.signature(Point)


def test_a_body_init_answers_with_its_own_signature():
    signature = inspect.signature(WithInit)

    assert list(signature.parameters) == ["a"]


def test_inherited_fields_appear_in_order():
    signature = inspect.signature(Inherited)

    assert list(signature.parameters) == ["x", "y", "z"]
    assert signature.parameters["z"].default == 3


def test_a_generic_struct_keeps_the_type_variable():
    signature = inspect.signature(Generic)

    assert signature.parameters["value"].annotation is T


def test_a_body_signature_binding_overrides_the_machinery():
    """The getter reads the class's own dict before it computes, so an
    unannotated body binding wins over the computed signature on the class
    and the instance, and a post-creation patch lands in the same dict and
    wins the same way."""

    class Custom(Struct):
        x: int
        __signature__ = renamed_signature()

    assert list(inspect.signature(Custom).parameters) == ["renamed"]
    assert Custom(1).__signature__ == Custom.__signature__

    Custom.__signature__ = inspect.Signature(
        [inspect.Parameter("patched", inspect.Parameter.POSITIONAL_OR_KEYWORD)]
    )

    assert list(inspect.signature(Custom).parameters) == ["patched"]
    assert Custom(1).__signature__ == Custom.__signature__

    del Custom.__signature__

    assert list(inspect.signature(Custom).parameters) == ["x"]
    assert Custom(1).__signature__ == Custom.__signature__

    with pytest.raises(AttributeError, match="__signature__"):
        del Custom.__signature__

    Custom.__signature__ = inspect.Signature(
        [inspect.Parameter("patched", inspect.Parameter.POSITIONAL_OR_KEYWORD)]
    )
    Custom.__signature__ = None

    assert list(inspect.signature(Custom).parameters) == ["x"]
    assert Custom(1).__signature__ == Custom.__signature__


def test_a_none_binding_means_unset():
    """inspect's own protocol reads __signature__ = None as absent, so the
    computed field signature answers instead of a degraded derivation."""

    class Unset(Struct):
        x: int
        __signature__ = None

    assert list(inspect.signature(Unset).parameters) == ["x"]
    assert Unset(1).__signature__ == Unset.__signature__


def test_a_none_binding_does_not_shadow_an_inherited_binding():
    """None means unset on struct classes: the class statement drops a None
    binding, so the walk continues past nothing on every struct level and
    both paths answer the inherited binding. A None on a plain co-base is
    that class's own attribute and answers on the instance path."""

    class Base(Struct):
        x: int
        __signature__ = renamed_signature()

    class Mid(Base):
        y: int = 1
        __signature__ = None

    class Sub(Mid):
        z: int = 1
        __signature__ = None

    assert list(inspect.signature(Sub).parameters) == ["renamed"]
    assert Sub(1, 2, 3).__signature__ == Sub.__signature__


def test_a_real_binding_on_a_plain_co_base_agrees_on_both_paths():
    class PlainBase:
        __signature__ = inspect.Signature(
            [inspect.Parameter("plain", inspect.Parameter.POSITIONAL_OR_KEYWORD)]
        )

    class Sub(PlainBase, Struct):
        x: int

    assert list(inspect.signature(Sub).parameters) == ["plain"]
    assert Sub(1).__signature__ == Sub.__signature__


def test_the_own_init_gate_raises_before_the_walk():
    """The own-init gate raises AttributeError on direct access, and
    inspect falls back to the __init__ signature."""

    class Sub(Struct):
        def __init__(self, q: int) -> None:
            pass

    with pytest.raises(AttributeError, match="own __init__"):
        _ = Sub.__signature__

    assert list(inspect.signature(Sub).parameters) == ["q"]


def test_a_base_signature_binding_is_inherited():
    """The binding lives in the base's class dict, and the getter finds it
    there like any other class attribute."""

    class Custom(Struct):
        x: int
        __signature__ = renamed_signature()

    class Sub(Custom):
        y: int = 1

    assert list(inspect.signature(Sub).parameters) == ["renamed"]


def test_a_subclass_own_init_shadows_an_inherited_binding():
    """The subclass wrote its own constructor, so its __init__ answers and
    the base's binding does not describe it."""

    class Custom(Struct):
        x: int
        __signature__ = renamed_signature()

    class Sub(Custom):
        def __init__(self, q: int) -> None:
            pass

    assert list(inspect.signature(Sub).parameters) == ["q"]


def test_the_computed_signature_is_cached_per_type():
    """Built once on first access and stashed on the type, so repeat
    accesses return the same object and never re-import inspect."""

    assert Point.__signature__ is Point.__signature__


def test_a_subclass_of_an_own_init_struct_inherits_the_init_signature():
    """Measured, not assumed: the subclass's constructor is the inherited
    __init__ — Sub(a=2) constructs and Sub(b=2) fails — so the inherited
    signature is the call form to advertise, and the field keeps its
    default."""

    class WithInit(Struct):
        def __init__(self, a: int) -> None:
            pass

    class Sub(WithInit):
        b: int = 1

    assert list(inspect.signature(Sub).parameters) == ["a"]
    assert Sub(a=2).b == 1


def test_a_struct_meta_subclass_dispatch_resolves_the_class():
    """A StructMeta subclass passes both dispatch branches unchanged."""

    Meta = type(Struct)

    class MyMeta(Meta):
        pass

    class ViaMeta(Struct, metaclass=MyMeta):
        x: int

    assert list(inspect.signature(ViaMeta).parameters) == ["x"]
    assert ViaMeta(1).__signature__ == ViaMeta.__signature__


def test_a_field_named_signature_is_refused():
    with pytest.raises(TypeError, match="signature machinery"):
        type(Struct)("Blocked", (Struct,), {"__annotations__": {"__signature__": int, "x": int}})


def test_the_module_version_matches_its_declaration():
    import salix

    if os.environ.get("SALIX_REQUIRE_INSTALLED") == "1":
        declared = importlib.metadata.version("salix")
    else:
        declared = tomllib.loads(
            (Path(__file__).resolve().parent.parent / "pyproject.toml").read_text(encoding="utf-8")
        )["project"]["version"]

    assert salix.__version__ == declared


def test_struct_has_a_docstring():
    assert Struct.__doc__ and "frozen" in Struct.__doc__


def test_struct_meta_has_a_docstring():
    assert type(Struct).__doc__ and "metaclass" in type(Struct).__doc__
