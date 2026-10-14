import dataclasses
import inspect
import sys
from collections.abc import Callable
from types import FrameType
from typing import Any, TypeVar, cast

from salix import Struct

from .methods import (
    _dataclass_params,
    _deepcopy_factory,
    _make_eq,
    _make_hash,
    _make_init,
    _make_ordering,
    _make_post_init,
    _make_repr,
    _mutable_default_or_raise,
    _needs_factory_default,
    _params_equal,
    _rebind_class_cells,
    _stock_mutable_raises,
    _translate_field,
)
from .state import (
    _PLACEHOLDER,
    _factories,
    _field_flags,
    _field_metadata,
    _is_non_field_annotation,
    _kw_only,
    _merged_field_flags,
    _merged_flags_cache,
    _no_init,
    _redeclared,
    _redeclared_names,
)
from .stock import (
    _SALIX_MEMBERS,
    _build_fields,
    _initvar_plan,
    _merged_dataclass_fields,
    _pseudo_field,
    _pseudo_init_param,
    _stash_pseudo_defaults,
    is_struct,
)

_T = TypeVar("_T")


def _called_from_init_subclass(cls: type[Any]) -> bool:
    frame: FrameType | None = sys._getframe(1)
    while frame is not None:
        if frame.f_code.co_name == "__init_subclass__" and frame.f_locals.get("cls") is cls:
            return True
        frame = frame.f_back
    return False


def _rebuild_struct_subclass(
    cls: type[_T],
    init: bool,
    repr: bool,
    eq: bool,
    order: bool,
    unsafe_hash: bool,
    frozen: bool,
    match_args: bool,
    kw_only: bool,
) -> type[_T]:
    struct = cast(type[Struct], cls)
    names = struct.__struct_fields__
    defaults = struct.__struct_defaults__
    inherited_factories: dict[str, Callable[[], Any]] = {}
    no_init: set[str] = set()
    kw_only_names: set[str] = set()
    for base in reversed(struct.__mro__[1:]):
        no_init.difference_update(_redeclared.get(base, frozenset()))
        no_init.update(_no_init.get(base, frozenset()))
        kw_only_names.difference_update(_redeclared.get(base, frozenset()))
        kw_only_names.update(_kw_only.get(base, frozenset()))
    for base in reversed(struct.__mro__):
        inherited_factories.update(_factories.get(base, {}))
    inherited = {
        name
        for base in cls.__bases__
        if is_struct(base)
        for name in cast(type[Struct], base).__struct_fields__
    }
    own_names = [name for name in names if name not in inherited]
    redeclared = _redeclared_names(struct)
    no_init -= redeclared
    kw_only_names -= redeclared
    inherited_factories = {
        name: factory
        for name, factory in inherited_factories.items()
        if name not in redeclared
    }
    defaulted = names[len(names) - len(defaults) :] if defaults else ()
    default_map = dict(zip(defaulted, defaults, strict=True))
    kw_only_overridden = {
        name
        for name in redeclared
        if isinstance(default_map.get(name), dataclasses.Field)
        and default_map[name].kw_only is False
    }
    if _params_equal(
        cls.__dict__.get("__dataclass_params__"),
        _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only),
    ):
        return cls
    namespace: dict[str, Any] = {}
    factories: list[tuple[str, Callable[[], Any]]] = []
    field_metadata: dict[str, Any] = {}
    field_flags_map: dict[str, tuple[bool, bool, bool | None]] = {}
    for name in redeclared:
        if not isinstance(default_map.get(name), dataclasses.Field):
            field_flags_map[name] = (True, True, None)
    for name, value in default_map.items():
        if value is _PLACEHOLDER and name in redeclared:
            continue
        if (
            value is _PLACEHOLDER
            and name in inherited_factories
            and name not in own_names
        ):
            factories.append((name, inherited_factories[name]))
            namespace[name] = _PLACEHOLDER
            continue
        if isinstance(value, dataclasses.Field):
            translated = _translate_field(cls.__name__, name, value)
            if translated.factory is not None:
                factories.append((name, translated.factory))
                if translated.default is not dataclasses.MISSING:
                    namespace[name] = translated.default
            elif translated.default is not dataclasses.MISSING:
                namespace[name] = translated.default
            if translated.metadata:
                field_metadata[name] = translated.metadata
            if not translated.init:
                no_init.add(name)
            if translated.kw_only:
                kw_only_names.add(name)
            field_flags_map[name] = (translated.compare, translated.repr, translated.hash)
            continue
        if _stock_mutable_raises(value):
            _mutable_default_or_raise(name, value)
            continue
        if _needs_factory_default(value):
            factories.append((name, _deepcopy_factory(value)))
            namespace[name] = _PLACEHOLDER
            continue
        namespace[name] = value
    for key, value in cls.__dict__.items():
        if key in names or key in _SALIX_MEMBERS:
            continue
        if key == "__annotations_cache__":
            continue
        if key == "__annotate_func__" and value is None:
            continue
        if key in ("__setattr__", "__delattr__") and value is getattr(object, key):
            continue
        if key == "__hash__" and value is None and eq:
            continue
        if key.startswith("__struct_") or key.startswith("_struct_"):
            continue
        namespace[key] = value
    own_annotations = inspect.get_annotations(cls)
    inherited_fields = {
        name: entry
        for base in reversed(struct.__mro__[1:])
        for name, entry in base.__dict__.get("__dataclass_fields__", {}).items()
    }
    pseudo_fields = {
        name: _pseudo_field(cls, name, own_annotations[name], kw_only)
        if name in own_annotations
        else inherited_fields[name]
        for name in dict.fromkeys([*inherited_fields, *own_annotations])
        if name not in names
        and _is_non_field_annotation(
            own_annotations[name] if name in own_annotations else inherited_fields[name].type
        )
    }
    _stash_pseudo_defaults(namespace, pseudo_fields, own_annotations)
    initvar_names, initvar_options = _initvar_plan(pseudo_fields)
    merged = {
        name: factory
        for name, factory in {**inherited_factories, **dict(factories)}.items()
        if namespace.get(name) is _PLACEHOLDER
    }
    if merged:
        namespace["__post_init__"] = _make_post_init(
            list(merged.items()), getattr(cls, "__post_init__", None), initvar_names
        )
    stored_params = cls.__dict__.get("__dataclass_params__")
    kw_only_changed = stored_params is not None and bool(getattr(stored_params, "kw_only", False)) != kw_only
    required_kw_only = {
        name
        for name in names
        if (name in kw_only_names or (kw_only and name in own_names))
        and namespace.get(name, dataclasses.MISSING) is dataclasses.MISSING
    }
    annotation_map: dict[str, Any] = {}
    metadata_map: dict[str, Any] = {}
    for base in reversed(struct.__mro__):
        annotation_map.update(inspect.get_annotations(base))
        metadata_map.update(_field_metadata.get(base, {}))
    metadata_map.update(field_metadata)
    field_order = tuple(
        name
        for name in dict.fromkeys([*annotation_map, *names])
        if name in names or name in pseudo_fields
    )
    if (
        no_init or kw_only_names or kw_only or kw_only_changed or redeclared or required_kw_only or initvar_options
    ) and (
        "__init__" not in cls.__dict__
        or getattr(cls.__dict__.get("__init__"), "_shim_synthesized", False)
    ):
        namespace["__init__"] = _make_init(
            [
                _pseudo_init_param(pseudo_fields[name])
                if name in pseudo_fields
                else (
                    name,
                    dataclasses.MISSING
                    if name in required_kw_only
                    else namespace.get(name, dataclasses.MISSING),
                    name in kw_only_names
                    or (
                        kw_only
                        and (
                            name in own_names
                            or (name in redeclared and name not in kw_only_overridden)
                        )
                    ),
                    False,
                )
                for name in field_order
                if name not in no_init
                and (name not in pseudo_fields or (name in initvar_names and pseudo_fields[name].init))
            ]
        )
    if kw_only:
        kw_only_names.update(own_names)
        kw_only_names.update(redeclared - kw_only_overridden)
    namespace.update({name: _PLACEHOLDER for name in required_kw_only})
    merged_flags = {
        **_merged_field_flags(struct),
        **field_flags_map,
    }
    hash_restricted = any(
        flags[2] is False or not flags[0] for flags in merged_flags.values()
    )
    compare_restricted = any(not flags[0] for flags in merged_flags.values())
    if eq and compare_restricted:
        namespace["__eq__"] = _make_eq()
    if order and compare_restricted:
        namespace["__lt__"] = _make_ordering("__lt__")
        namespace["__le__"] = _make_ordering("__le__")
        namespace["__gt__"] = _make_ordering("__gt__")
        namespace["__ge__"] = _make_ordering("__ge__")
    if repr and any(not flags[1] for flags in merged_flags.values()):
        namespace["__repr__"] = _make_repr()
    if unsafe_hash or (eq and frozen and hash_restricted):
        namespace["__hash__"] = _make_hash()
    real_fields = _build_fields(
        names,
        struct.__struct_annotations__,
        tuple(
            dataclasses.MISSING
            if name in required_kw_only
            else namespace.get(name, dataclasses.MISSING)
            for name in names
        ),
        struct.__struct_metadata__,
        merged,
        annotation_map,
        metadata_map,
        no_init,
        kw_only_names,
        merged_flags,
    )
    namespace["__dataclass_fields__"] = _merged_dataclass_fields(field_order, real_fields, pseudo_fields)
    namespace["__dataclass_params__"] = _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
    namespace["__qualname__"] = cls.__qualname__
    if not names and _called_from_init_subclass(cls):
        return cls


    field_name_set = frozenset(names)
    built = cast(Any, type(cls))(
        cls.__name__,
        cls.__bases__,
        {
            key: value
            for key, value in namespace.items()
            if key not in field_name_set or key in own_annotations
        },
        frozen=frozen,
        eq=eq,
        order=order,
        repr=repr,
        match_args=match_args,
        weakref=True,
    )
    if merged:
        _factories[built] = merged
    if no_init:
        _no_init[built] = frozenset(no_init)
    _kw_only[built] = frozenset(kw_only_names)
    if field_metadata:
        _field_metadata[built] = field_metadata
    if merged_flags:
        _merged_flags_cache.pop(built, None)
        _field_flags[built] = merged_flags
    _redeclared[built] = redeclared
    _rebind_class_cells(cast(type[Any], built), cls)
    return cast(type[_T], built)
