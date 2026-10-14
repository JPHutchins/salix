import dataclasses
import inspect
import sys
from collections.abc import Callable
from types import FrameType
from typing import Any, TypeVar, cast

import salix
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
    _rebind_class_cells,
    _stock_mutable_raises,
    _translate_field,
)
from .rebuild import _rebuild_struct_subclass
from .state import (
    _PLACEHOLDER,
    _PY_TPFLAGS_HEAPTYPE,
    _excluded_prefixes,
    _factories,
    _field_flags,
    _field_metadata,
    _included_prefixes,
    _is_classvar,
    _is_initvar,
    _is_non_field_annotation,
    _kw_only,
    _merged_flags_cache,
    _no_init,
    _redeclared,
)
from .stock import (
    _SALIX_MEMBERS,
    _build_fields,
    _builder_for,
    _initvar_plan,
    _merged_dataclass_fields,
    _pseudo_field,
    _pseudo_init_param,
    _stash_pseudo_defaults,
    _to_stock,
    asdict,
    fields,
    is_dataclass,
    is_struct,
    replace,
)

_T = TypeVar("_T")


def _caller_excluded() -> bool:
    frame: FrameType | None = sys._getframe(1)
    while frame is not None and frame.f_globals.get("__name__", "") == __name__:
        frame = frame.f_back
    caller_module = frame.f_globals.get("__name__", "") if frame is not None else ""

    if _included_prefixes:
        return not any(
            caller_module == prefix or caller_module.startswith(prefix + ".")
            for prefix in _included_prefixes
        )

    return any(
        caller_module == prefix or caller_module.startswith(prefix + ".")
        for prefix in _excluded_prefixes
    )


def _needs_stock_fallback(bases: tuple[type[Any], ...]) -> bool:
    return any(
        (
            base is not object
            and not base.__flags__ & _PY_TPFLAGS_HEAPTYPE
        )
        or (
            not is_struct(base)
            and hasattr(base, "__dataclass_fields__")
        )
        for base in bases
    )


def _has_descriptor_field_collision(cls: type[Any], field_names: tuple[str, ...]) -> bool:
    return any(
        hasattr(cls.__dict__.get(name), "__get__") for name in field_names
    )


def dataclass(
    _cls: type[_T] | None = None,
    *,
    init: bool = True,
    repr: bool = True,
    eq: bool = True,
    order: bool = False,
    unsafe_hash: bool = False,
    frozen: bool = False,
    match_args: bool = True,
    kw_only: bool = False,
    slots: bool = False,
    weakref_slot: bool = False,
) -> Callable[[type[_T]], type[_T]] | type[_T]:
    def wrap(cls: type[_T]) -> type[_T]:
        if is_struct(cls):
            return _rebuild_struct_subclass(cls, init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
        if _caller_excluded():
            return _to_stock(cls, init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only, slots, weakref_slot)
        body_annotations = inspect.get_annotations(cls)
        if (
            _needs_stock_fallback(cls.__bases__)
            or _has_descriptor_field_collision(cls, tuple(body_annotations))
        ):
            return _to_stock(cls, init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only, slots, weakref_slot)
        if not init:
            raise NotImplementedError(f"init=False is not shimmed yet: {cls.__name__}")
        namespace: dict[str, Any] = {}
        factories: list[tuple[str, Callable[[], Any]]] = []
        no_init: set[str] = set()
        kw_only_names: set[str] = set()
        field_metadata: dict[str, Any] = {}
        field_flags_map: dict[str, tuple[bool, bool, bool | None]] = {}
        declared_pseudo_fields = {
            name: _pseudo_field(cls, name, annotation, kw_only)
            for name, annotation in body_annotations.items()
            if _is_non_field_annotation(annotation)
        }
        pseudo_fields = {
            name: pseudo
            for name, pseudo in declared_pseudo_fields.items()
            if _is_initvar(pseudo.type) or pseudo.default is not dataclasses.MISSING
        }
        initvar_names, initvar_options = _initvar_plan(pseudo_fields)
        for name, value in cls.__dict__.items():
            if name in _SALIX_MEMBERS:
                continue
            if name in pseudo_fields:
                continue
            if isinstance(value, dataclasses.Field):
                if name not in body_annotations:
                    raise TypeError(f"'{name}' is a field but has no type annotation")
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
            elif name in body_annotations and _stock_mutable_raises(value):
                _mutable_default_or_raise(name, value)
            elif name in body_annotations and _needs_factory_default(value):
                factories.append((name, _deepcopy_factory(value)))
                namespace[name] = _PLACEHOLDER
                field_flags_map[name] = (True, True, None)
            else:
                namespace[name] = value
                if name in body_annotations:
                    field_flags_map[name] = (True, True, None)
        _stash_pseudo_defaults(namespace, pseudo_fields, pseudo_fields)
        namespace["__annotations__"] = {
            name: annotation
            for name, annotation in body_annotations.items()
            if name not in declared_pseudo_fields or name in pseudo_fields
        }
        field_names = tuple(name for name in body_annotations if not _is_non_field_annotation(body_annotations[name]))
        required_kw_only = {
            name
            for name in field_names
            if (name in kw_only_names or kw_only)
            and name not in namespace
        }
        if factories:
            namespace["__post_init__"] = _make_post_init(
                factories,
                getattr(cls, "__post_init__", None),
                tuple(name for name in body_annotations if name in initvar_names),
            )
        if (
            no_init
            or kw_only_names
            or kw_only
            or initvar_options
            or cls.__doc__ is not None
        ) and "__init__" not in cls.__dict__:
            namespace["__init__"] = _make_init(
                [
                    _pseudo_init_param(pseudo_fields[name])
                    if name in initvar_names
                    else (
                        name,
                        dataclasses.MISSING
                        if name in required_kw_only
                        else namespace.get(name, dataclasses.MISSING),
                        name in kw_only_names or kw_only,
                        False,
                    )
                    for name in body_annotations
                    if name not in no_init
                    and not _is_classvar(body_annotations[name])
                    and (name not in initvar_names or pseudo_fields[name].init)
                ]
            )
        if kw_only:
            kw_only_names.update(field_names)
        namespace.update({name: _PLACEHOLDER for name in required_kw_only})
        hash_restricted = any(
            flags[2] is False or not flags[0] for flags in field_flags_map.values()
        )
        compare_restricted = any(not flags[0] for flags in field_flags_map.values())
        if eq and compare_restricted:
            namespace["__eq__"] = _make_eq()
        if order and compare_restricted:
            namespace["__lt__"] = _make_ordering("__lt__")
            namespace["__le__"] = _make_ordering("__le__")
            namespace["__gt__"] = _make_ordering("__gt__")
            namespace["__ge__"] = _make_ordering("__ge__")
        if repr and any(not flags[1] for flags in field_flags_map.values()):
            namespace["__repr__"] = _make_repr()
        if unsafe_hash or (eq and frozen and hash_restricted):
            namespace["__hash__"] = _make_hash()
        real_fields = _build_fields(
            field_names,
            tuple(body_annotations[name] for name in field_names),
            tuple(
                dataclasses.MISSING
                if name in required_kw_only or name not in namespace
                else namespace[name]
                for name in field_names
            ),
            tuple(() for _ in field_names),
            dict(factories),
            {name: body_annotations[name] for name in field_names},
            field_metadata,
            no_init,
            kw_only_names,
            field_flags_map,
        )
        namespace["__dataclass_fields__"] = _merged_dataclass_fields(body_annotations, real_fields, pseudo_fields)
        namespace["__dataclass_params__"] = _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
        namespace["__qualname__"] = cls.__qualname__
        built = _builder_for(type(cls))(
            cls.__name__,
            (Struct, *cls.__bases__),
            namespace,
            frozen=frozen,
            eq=eq,
            order=order,
            repr=repr,
            match_args=match_args,
            weakref=True,
        )
        if factories:
            _factories[built] = dict(factories)
        _no_init[built] = frozenset(no_init)
        _kw_only[built] = frozenset(kw_only_names)
        if field_metadata:
            _field_metadata[built] = field_metadata
        if field_flags_map:
            _merged_flags_cache.pop(built, None)
            _field_flags[built] = field_flags_map
        _redeclared[built] = frozenset()
        _rebind_class_cells(cast(type[Any], built), cls)
        return cast(type[_T], built)

    if _cls is None:
        return wrap
    return wrap(_cls)


def install(exclude_prefixes: tuple[str, ...] = (), include_prefixes: tuple[str, ...] = ()) -> None:
    _excluded_prefixes[:] = exclude_prefixes
    _included_prefixes[:] = include_prefixes
    dataclasses.dataclass = cast(Any, dataclass)
    dataclasses.fields = cast(Any, fields)
    dataclasses.asdict = cast(Any, asdict)
    dataclasses.replace = cast(Any, replace)
    dataclasses.is_dataclass = cast(Any, is_dataclass)
    vars(dataclasses)["FrozenInstanceError"] = salix.FrozenInstanceError
