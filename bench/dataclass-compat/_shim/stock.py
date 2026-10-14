import copy
import dataclasses
import inspect
from collections.abc import Callable, Iterable
from typing import Any, TypeVar, cast

from salix import Struct

from .state import (
    _PLACEHOLDER,
    _combined_metaclasses,
    _factories,
    _field_doc_required,
    _field_metadata,
    _is_initvar,
    _kw_only,
    _merged_field_flags,
    _no_init,
    _redeclared,
    _redeclared_names,
    _replace_refusal,
    _stock_asdict,
    _stock_dataclass,
    _stock_fields,
    _stock_is_dataclass,
    _stock_replace,
)

_T = TypeVar("_T")


def _to_stock(
    cls: type[_T],
    init: bool,
    repr: bool,
    eq: bool,
    order: bool,
    unsafe_hash: bool,
    frozen: bool,
    match_args: bool,
    kw_only: bool,
    slots: bool,
    weakref_slot: bool,
) -> type[_T]:
    return _stock_dataclass(
        cls,
        init=init,
        repr=repr,
        eq=eq,
        order=order,
        unsafe_hash=unsafe_hash,
        frozen=frozen,
        match_args=match_args,
        kw_only=kw_only,
        slots=slots,
        weakref_slot=weakref_slot,
    )


def _builder_for(metaclass: type[Any]) -> type[Any]:
    struct_meta = type(Struct)
    if metaclass is type:
        return struct_meta
    if issubclass(metaclass, struct_meta):
        return metaclass
    cached = _combined_metaclasses.get(metaclass)
    if cached is None:
        cached = _combined_metaclasses[metaclass] = type(
            f"_ShimMeta_{metaclass.__name__}",
            (struct_meta, metaclass),
            {},
        )
    return cached

_SALIX_MEMBERS = frozenset(
    {
        "__dict__",
        "__weakref__",
        "__slots__",
        "__static_attributes__",
        "__classcell__",
    }
)


def is_struct(cls_or_instance: object) -> bool:
    cls = cls_or_instance if isinstance(cls_or_instance, type) else type(cls_or_instance)
    return hasattr(cls, "__struct_fields__")


def is_dataclass(obj: object) -> bool:
    return _stock_is_dataclass(obj) or is_struct(obj)


def _build_fields(
    names: tuple[str, ...],
    annotations: tuple[Any, ...],
    defaults: tuple[Any, ...],
    extras: tuple[tuple[Any, ...], ...],
    factory_map: dict[str, Callable[[], Any]],
    annotation_map: dict[str, Any],
    metadata_map: dict[str, Any],
    no_init_names: set[str],
    kw_only_names: set[str],
    flags_map: dict[str, tuple[bool, bool, bool | None]],
) -> dict[str, dataclasses.Field[Any]]:
    missing = len(names) - len(defaults)
    result: dict[str, dataclasses.Field[Any]] = {}
    for position, name in enumerate(names):
        factory = factory_map.get(name)
        compare, repr_flag, hash_flag = flags_map.get(name, (True, True, None))
        field_kwargs: dict[str, Any] = {
            "default": (
                dataclasses.MISSING
                if (
                    factory is not None
                    or position < missing
                    or defaults[position - missing] is _PLACEHOLDER
                )
                else defaults[position - missing]
            ),
            "default_factory": factory if factory is not None else dataclasses.MISSING,
            "init": name not in no_init_names,
            "repr": repr_flag,
            "hash": hash_flag,
            "compare": compare,
            "metadata": metadata_map.get(name) or ({} if not extras[position] else {"extras": extras[position]}),
            "kw_only": name in kw_only_names,
            **({"doc": None} if _field_doc_required else {}),
        }
        field = dataclasses.Field(**field_kwargs)
        field.name = name
        field.type = annotation_map.get(name, annotations[position])
        field._field_type = dataclasses._FIELD  # type: ignore[attr-defined]
        result[name] = field
    return result


def _pseudo_field(cls: type[Any], name: str, annotation: Any, kw_only: bool) -> dataclasses.Field[Any]:
    return cast(
        dataclasses.Field[Any],
        dataclasses._get_field(cls, name, annotation, kw_only),  # type: ignore[attr-defined]
    )


def _initvar_plan(pseudo_fields: dict[str, dataclasses.Field[Any]]) -> tuple[tuple[str, ...], bool]:
    names = tuple(name for name, pseudo in pseudo_fields.items() if _is_initvar(pseudo.type))
    return names, any(pseudo_fields[name].kw_only is True or not pseudo_fields[name].init for name in names)


def _pseudo_init_param(pseudo: dataclasses.Field[Any]) -> tuple[str, Any, bool, bool]:
    return (pseudo.name, pseudo.default, pseudo.kw_only is True, True)


def _stash_pseudo_defaults(
    namespace: dict[str, Any], pseudo_fields: dict[str, dataclasses.Field[Any]], names: Iterable[str]
) -> None:
    for name in names:
        if name in pseudo_fields:
            namespace.pop(name, None)
            if pseudo_fields[name].default is not dataclasses.MISSING:
                namespace[name] = pseudo_fields[name].default


def _merged_dataclass_fields(
    order: Iterable[str],
    real_fields: dict[str, dataclasses.Field[Any]],
    pseudo_fields: dict[str, dataclasses.Field[Any]],
) -> dict[str, dataclasses.Field[Any]]:
    return {
        name: real_fields[name] if name in real_fields else pseudo_fields[name]
        for name in order
        if name in real_fields or name in pseudo_fields
    }


def fields(cls: Any) -> tuple[dataclasses.Field[Any], ...]:
    if not is_struct(cls):
        return _stock_fields(cast(Any, cls))
    struct = cast(type[Struct], cls if isinstance(cls, type) else type(cls))
    factory_map: dict[str, Callable[[], Any]] = {}
    annotation_map: dict[str, Any] = {}
    metadata_map: dict[str, Any] = {}
    no_init_names: set[str] = set()
    kw_only_names: set[str] = set()
    for base in reversed(struct.__mro__):
        factory_map.update(_factories.get(base, {}))
        annotation_map.update(inspect.get_annotations(base))
        metadata_map.update(_field_metadata.get(base, {}))
    default_map = dict(
        zip(
            struct.__struct_fields__[
                len(struct.__struct_fields__) - len(struct.__struct_defaults__) :
            ],
            struct.__struct_defaults__,
            strict=True,
        )
    )
    redeclared = _redeclared.get(struct)
    if redeclared is None:
        redeclared = _redeclared_names(struct)
        _redeclared[struct] = redeclared
    no_init_names = set()
    kw_only_names = set()
    for base in reversed(struct.__mro__):
        no_init_names.difference_update(_redeclared.get(base, frozenset()))
        no_init_names.update(_no_init.get(base, frozenset()))
        kw_only_names.difference_update(_redeclared.get(base, frozenset()))
        kw_only_names.update(_kw_only.get(base, frozenset()))
    own_factories = _factories.get(struct, {})
    factory_map = {
        name: factory
        for name, factory in factory_map.items()
        if name in own_factories
        or (name not in redeclared and default_map.get(name) is _PLACEHOLDER)
    }
    return tuple(
        _build_fields(
            struct.__struct_fields__,
            struct.__struct_annotations__,
            struct.__struct_defaults__,
            struct.__struct_metadata__,
            factory_map,
            annotation_map,
            metadata_map,
            no_init_names,
            kw_only_names,
            _merged_field_flags(struct),
        ).values()
    )


def _asdict_value(value: Any) -> Any:
    if is_struct(value):
        return asdict(value)
    if isinstance(value, list):
        return [_asdict_value(item) for item in value]
    if isinstance(value, tuple):
        return tuple(_asdict_value(item) for item in value)
    if isinstance(value, dict):
        return {_asdict_value(key): _asdict_value(item) for key, item in value.items()}
    return copy.deepcopy(value)


def asdict(obj: object) -> dict[str, Any]:
    if not is_struct(obj):
        return _stock_asdict(cast(Any, obj))
    return {
        name: _asdict_value(getattr(obj, name))
        for name in cast(type[Struct], type(obj)).__struct_fields__
    }


def replace(obj: _T, /, **changes: Any) -> _T:
    if is_struct(obj):
        struct = cast(type[Struct], type(obj))
        declared = getattr(struct, "__dataclass_fields__", None) or {
            field.name: field for field in fields(struct)
        }
        for entry in declared.values():
            kind = getattr(entry, "_field_type", None)
            if kind is dataclasses._FIELD_CLASSVAR:  # type: ignore[attr-defined]
                continue
            if not entry.init and entry.name in changes:
                raise _replace_refusal(
                    f"field {entry.name} is declared with init=False, it cannot be specified with replace()"
                )
            if (
                kind is dataclasses._FIELD_INITVAR  # type: ignore[attr-defined]
                and entry.init
                and entry.default is dataclasses.MISSING
                and entry.name not in changes
            ):
                raise _replace_refusal(f"InitVar {entry.name!r} must be specified with replace()")
        return cast(
            _T,
            struct(
                **{
                    **{
                        field.name: getattr(obj, field.name)
                        for field in fields(struct)
                        if field.init
                    },
                    **changes,
                }
            ),
        )
    return cast(_T, _stock_replace(cast(Any, obj), **changes))
