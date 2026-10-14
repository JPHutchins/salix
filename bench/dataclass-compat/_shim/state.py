import dataclasses
import inspect
import re
import sys
import typing
import weakref
from collections.abc import Callable
from typing import Any

from salix import Struct

_PLACEHOLDER = object()
_INIT_UNSET = object()
_factories: weakref.WeakKeyDictionary[type[Any], dict[str, Callable[[], Any]]] = (
    weakref.WeakKeyDictionary()
)
_no_init: weakref.WeakKeyDictionary[type[Any], frozenset[str]] = weakref.WeakKeyDictionary()
_kw_only: weakref.WeakKeyDictionary[type[Any], frozenset[str]] = weakref.WeakKeyDictionary()
_field_metadata: weakref.WeakKeyDictionary[type[Any], dict[str, Any]] = weakref.WeakKeyDictionary()
_field_flags: weakref.WeakKeyDictionary[
    type[Any], dict[str, tuple[bool, bool, bool | None]]
] = weakref.WeakKeyDictionary()
_stock_is_dataclass = dataclasses.is_dataclass
_stock_fields = dataclasses.fields
_stock_asdict = dataclasses.asdict
_stock_replace = dataclasses.replace
_stock_dataclass = dataclasses.dataclass
_combined_metaclasses: dict[type[Any], type[Any]] = {}
_PY_TPFLAGS_HEAPTYPE = 1 << 9
_excluded_prefixes: list[str] = []
_included_prefixes: list[str] = []
_field_doc_required = "doc" in inspect.signature(dataclasses.Field.__init__).parameters
_replace_refusal = TypeError if sys.version_info >= (3, 13) else ValueError


def _is_initvar(annotation: Any) -> bool:
    if isinstance(annotation, str):
        head = re.split(r"[\s\[\],]+", annotation, maxsplit=1)[0]
        return head == "InitVar" or head.endswith(".InitVar")
    return (
        annotation is dataclasses.InitVar
        or isinstance(annotation, dataclasses.InitVar)
        or getattr(annotation, "__origin__", None) is dataclasses.InitVar
    )


def _is_classvar(annotation: Any) -> bool:
    if isinstance(annotation, str):
        head = re.split(r"[\s\[\],]+", annotation, maxsplit=1)[0]
        return head == "ClassVar" or head.endswith(".ClassVar")
    return annotation is typing.ClassVar or (
        getattr(annotation, "__origin__", None) is typing.ClassVar
    )


def _is_non_field_annotation(annotation: Any) -> bool:
    return _is_initvar(annotation) or _is_classvar(annotation)


_redeclared: weakref.WeakKeyDictionary[type[Any], frozenset[str]] = weakref.WeakKeyDictionary()


def _redeclared_names(struct: type[Struct]) -> frozenset[str]:
    base_annotations: dict[str, Any] = {}
    for base in struct.__mro__[1:]:
        base_annotations.update(inspect.get_annotations(base))
    return frozenset(
        name
        for name in struct.__struct_fields__
        if name in base_annotations and name in getattr(struct, "__annotations__", {})
    )


_merged_flags_cache: weakref.WeakKeyDictionary[
    type[Any], dict[str, tuple[bool, bool, bool | None]]
] = weakref.WeakKeyDictionary()


def _merged_field_flags(struct_cls: type[Struct]) -> dict[str, tuple[bool, bool, bool | None]]:
    merged = _merged_flags_cache.get(struct_cls)
    if merged is None:
        merged = {}
        for base in reversed(struct_cls.__mro__):
            merged.update(_field_flags.get(base, {}))
        _merged_flags_cache[struct_cls] = merged
    return merged
