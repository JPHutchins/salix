import importlib.metadata
import inspect
import os
import sys
from pathlib import Path
from typing import Generic, TypeVar

import pytest

from salix import Struct

if sys.version_info >= (3, 11):
    import tomllib
else:
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
    """The getter consults the class dict first, so an unannotated body
    binding wins over the computed signature on the class and the instance;
    assigning after creation is refused, because the getset has no setter."""

    class Custom(Struct):
        x: int
        __signature__ = inspect.Signature(
            [inspect.Parameter("renamed", inspect.Parameter.POSITIONAL_OR_KEYWORD)]
        )

    assert list(inspect.signature(Custom).parameters) == ["renamed"]
    assert Custom(1).__signature__ == Custom.__signature__

    with pytest.raises(AttributeError, match="not writable"):
        Custom.__signature__ = inspect.Signature()


def test_a_field_named_signature_is_refused():
    with pytest.raises(TypeError, match="signature machinery"):
        type(Struct)("Blocked", (Struct,), {"__annotations__": {"__signature__": int, "x": int}})


def test_the_module_version_matches_its_declaration():
    import salix

    if os.environ.get("SALIX_REQUIRE_INSTALLED") == "1":
        declared = importlib.metadata.version("salix")
    else:
        declared = tomllib.loads(Path("pyproject.toml").read_text())["project"]["version"]

    assert salix.__version__ == declared


def test_struct_has_a_docstring():
    assert Struct.__doc__ and "frozen" in Struct.__doc__


def test_struct_meta_has_a_docstring():
    assert type(Struct).__doc__ and "metaclass" in type(Struct).__doc__
