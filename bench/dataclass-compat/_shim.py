import copy
import dataclasses
import inspect
import sys
import typing
import weakref
from collections.abc import Callable
from types import FrameType
from typing import Any, NamedTuple, TypeVar, cast

from salix import Struct

_T = TypeVar("_T")

_PLACEHOLDER = object()
_INIT_UNSET = object()
_factories: weakref.WeakKeyDictionary[type[Any], dict[str, Callable[[], Any]]] = (
    weakref.WeakKeyDictionary()
)
_no_init: weakref.WeakKeyDictionary[type[Any], frozenset[str]] = weakref.WeakKeyDictionary()
_kw_only: weakref.WeakKeyDictionary[type[Any], frozenset[str]] = weakref.WeakKeyDictionary()
_field_metadata: weakref.WeakKeyDictionary[type[Any], dict[str, Any]] = weakref.WeakKeyDictionary()
_stock_is_dataclass = dataclasses.is_dataclass
_stock_fields = dataclasses.fields
_stock_asdict = dataclasses.asdict
_stock_replace = dataclasses.replace
_stock_dataclass = dataclasses.dataclass
_combined_metaclasses: dict[type[Any], type[Any]] = {}
_PY_TPFLAGS_HEAPTYPE = 1 << 9
_excluded_prefixes: list[str] = []


def _caller_excluded() -> bool:
    frame: FrameType | None = sys._getframe(1)
    while frame is not None and frame.f_globals.get("__name__", "") == "_shim":
        frame = frame.f_back
    caller_module = frame.f_globals.get("__name__", "") if frame is not None else ""
    return any(
        caller_module == prefix or caller_module.startswith(prefix + ".")
        for prefix in _excluded_prefixes
    )


def _needs_stock_fallback(bases: tuple[type[Any], ...]) -> bool:
    return any(
        base is not object and not base.__flags__ & _PY_TPFLAGS_HEAPTYPE for base in bases
    )


def _has_descriptor_field_collision(cls: type[Any], field_names: tuple[str, ...]) -> bool:
    return any(
        hasattr(cls.__dict__.get(name), "__get__") for name in field_names
    )


def _is_initvar(annotation: Any) -> bool:
    if isinstance(annotation, str):
        return annotation == "InitVar" or annotation.startswith("InitVar[")
    return isinstance(annotation, dataclasses.InitVar) or (
        getattr(annotation, "__origin__", None) is dataclasses.InitVar
    )


def _is_classvar(annotation: Any) -> bool:
    if isinstance(annotation, str):
        return annotation == "ClassVar" or annotation.startswith("ClassVar[")
    return annotation is typing.ClassVar or (
        getattr(annotation, "__origin__", None) is typing.ClassVar
    )


def _is_non_field_annotation(annotation: Any) -> bool:
    return _is_initvar(annotation) or _is_classvar(annotation)


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


class _Translated(NamedTuple):
    default: Any
    init: bool
    factory: Callable[[], Any] | None
    metadata: dict[str, Any]
    kw_only: bool


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

# Members the metatype owns and that must not ride a rebuild namespace.
# __hash__, __match_args__, and __firstlineno__ ride: salix honors a
# body-defined hash and match-args, and inspect.findsource reads
# __firstlineno__ from the class's own dict.
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
) -> dict[str, dataclasses.Field[Any]]:
    missing = len(names) - len(defaults)
    result: dict[str, dataclasses.Field[Any]] = {}
    for position, name in enumerate(names):
        factory = factory_map.get(name)
        field_kwargs: dict[str, Any] = {
            "default": dataclasses.MISSING if (factory is not None or position < missing) else defaults[position - missing],
            "default_factory": factory if factory is not None else dataclasses.MISSING,
            "init": name not in no_init_names,
            "repr": True,
            "hash": None,
            "compare": True,
            "metadata": metadata_map.get(name) or ({} if not extras[position] else {"extras": extras[position]}),
            "kw_only": name in kw_only_names,
        }
        field = dataclasses.Field(**field_kwargs)
        field.name = name
        field.type = annotation_map.get(name, annotations[position])
        result[name] = field
    return result


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
        no_init_names.update(_no_init.get(base, frozenset()))
        kw_only_names.update(_kw_only.get(base, frozenset()))
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
        ).values()
    )


def asdict(obj: object) -> dict[str, Any]:
    if not is_struct(obj):
        return _stock_asdict(cast(Any, obj))
    result = {}
    for name in cast(type[Struct], type(obj)).__struct_fields__:
        value = getattr(obj, name)
        result[name] = asdict(value) if is_struct(value) else copy.deepcopy(value)
    return result


def replace(obj: _T, /, **changes: Any) -> _T:
    if is_struct(obj):
        struct = cast(type[Struct], type(obj))
        return cast(
            _T,
            struct(
                **{
                    field.name: getattr(obj, field.name)
                    for field in fields(struct)
                    if field.init
                },
                **changes,
            ),
        )
    return cast(_T, _stock_replace(cast(Any, obj), **changes))


def _needs_factory_default(value: Any) -> bool:
    if isinstance(value, (list, dict, set, bytearray)):
        return True
    try:
        hash(value)
    except TypeError:
        return type(value).__hash__ is not None
    return False


def _deepcopy_factory(default: Any) -> Callable[[], Any]:
    return lambda: copy.deepcopy(default)


def _translate_field(
    cls_name: str,
    name: str,
    value: dataclasses.Field[Any],
) -> _Translated:
    kw_only = value.kw_only is True
    if value.repr is False or value.compare is False or value.hash is False:
        raise NotImplementedError(f"per-field repr/compare/hash flags are not shimmed yet: {cls_name}.{name}")
    metadata = dict(value.metadata)
    if value.default is not dataclasses.MISSING:
        if _needs_factory_default(value.default):
            return _Translated(_PLACEHOLDER, value.init, _deepcopy_factory(value.default), metadata, kw_only)
        return _Translated(value.default, value.init, None, metadata, kw_only)
    if value.default_factory is not dataclasses.MISSING:
        return _Translated(_PLACEHOLDER, value.init, value.default_factory, metadata, kw_only)
    return _Translated(dataclasses.MISSING, value.init, None, metadata, kw_only)


def _dataclass_params(
    init: bool,
    repr: bool,
    eq: bool,
    order: bool,
    unsafe_hash: bool,
    frozen: bool,
    match_args: bool,
    kw_only: bool,
) -> Any:
    params_cls = cast(Any, dataclasses)._DataclassParams
    return params_cls(
        init=init,
        repr=repr,
        eq=eq,
        order=order,
        unsafe_hash=unsafe_hash,
        frozen=frozen,
        match_args=match_args,
        kw_only=kw_only,
        slots=True,
        weakref_slot=True,
    )


def _make_post_init(
    factories: list[tuple[str, Callable[[], Any]]],
    user: Any,
    initvar_names: tuple[str, ...] = (),
) -> Callable[[Struct], None]:
    from salix import set_field

    pass_initvars = False
    if user is not None and initvar_names:
        try:
            pass_initvars = len(inspect.signature(user).parameters) > 0
        except (TypeError, ValueError):
            pass_initvars = False

    def __post_init__(self: Struct, *initvar_values: Any) -> None:
        for name, factory in factories:
            if getattr(self, name) is _PLACEHOLDER:
                set_field(self, name, factory())
        if user is not None:
            if pass_initvars:
                user(self, *initvar_values)
            else:
                user(self)

    return __post_init__


def _fresh_default(value: Any) -> Any:
    return copy.deepcopy(value) if isinstance(value, (list, dict, set)) else value


# A namespace __setattr__ flips salix's hash plan to unhashable, so the shim
# does not inject one: unfrozen structs already accept plain assignment via
# salix's C setattro, and frozen structs refuse it (AttributeError instead of
# stock's FrozenInstanceError — a message-parity gap only).
def _make_init(params: list[tuple[str, Any, bool, bool]]) -> Callable[[Struct], None]:
    from salix import set_field as _set_field

    receiver = "__dataclass_self__" if any(name == "self" for name, _, _, _ in params) else "self"
    arg_list = [receiver]
    for name, default, kw_only, _ in params:
        if kw_only and "*" not in arg_list:
            arg_list.append("*")
        arg_list.append(name if default is dataclasses.MISSING else f"{name}=_INIT_UNSET")
    body = [f"def __init__({', '.join(arg_list)}):"]
    for name, default, _, is_initvar in params:
        if is_initvar:
            continue
        if default is dataclasses.MISSING:
            body.append(f"    _set_field({receiver}, {name!r}, {name})")
            continue
        body.append(f"    if {name} is not _INIT_UNSET:")
        body.append(f"        _set_field({receiver}, {name!r}, {name})")
        if default is _PLACEHOLDER:
            continue
        body.append("    else:")
        body.append(f"        _set_field({receiver}, {name!r}, _fresh(_defaults[{name!r}]))")
    initvar_names = [name for name, _, _, is_initvar in params if is_initvar]
    initvar_args = "" if not initvar_names else ", " + ", ".join(initvar_names)
    body.append(f'    _post = getattr(type({receiver}), "__post_init__", None)')
    body.append("    if _post is not None:")
    body.append(f"        _post({receiver}{initvar_args})")
    namespace: dict[str, Any] = {
        "_set_field": _set_field,
        "_INIT_UNSET": _INIT_UNSET,
        "_defaults": {name: default for name, default, _, _ in params if default is not dataclasses.MISSING},
        "_fresh": _fresh_default,
    }
    exec(compile("\n".join(body), "<shim __init__>", "exec"), namespace)
    return cast(Callable[[Struct], None], namespace["__init__"])


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
    for base in struct.__mro__:
        inherited_factories.update(_factories.get(base, {}))
        no_init.update(_no_init.get(base, frozenset()))
        kw_only_names.update(_kw_only.get(base, frozenset()))
    inherited = {
        name
        for base in cls.__bases__
        if is_struct(base)
        for name in cast(type[Struct], base).__struct_fields__
    }
    own_names = [name for name in names if name not in inherited]
    defaulted = names[len(names) - len(defaults) :] if defaults else ()
    default_map = dict(zip(defaulted, defaults, strict=True))
    if (
        not own_names
        and not any(
            isinstance(value, dataclasses.Field) or _needs_factory_default(value)
            for value in default_map.values()
        )
        and not inherited_factories
        and not no_init
        and not kw_only_names
        and cls.__dict__.get("__dataclass_params__")
        == _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
    ):
        return cls
    namespace: dict[str, Any] = {}
    factories: list[tuple[str, Callable[[], Any]]] = []
    field_metadata: dict[str, Any] = {}
    for name, value in default_map.items():
        if value is _PLACEHOLDER and name in inherited_factories:
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
            continue
        if _needs_factory_default(value):
            factories.append((name, _deepcopy_factory(value)))
            namespace[name] = _PLACEHOLDER
            continue
        namespace[name] = value
    for key, value in cls.__dict__.items():
        if key in names or key in _SALIX_MEMBERS:
            continue
        if key.startswith("__struct_") or key.startswith("_struct_"):
            continue
        namespace[key] = value
    merged = {**inherited_factories, **dict(factories)}
    if merged:
        namespace["__post_init__"] = _make_post_init(list(merged.items()), getattr(cls, "__post_init__", None))
    if (no_init or kw_only_names or kw_only) and "__init__" not in cls.__dict__:
        namespace["__init__"] = _make_init(
            [
                (name, namespace.get(name, dataclasses.MISSING), name in kw_only_names or kw_only, False)
                for name in names
                if name not in no_init
            ]
        )
    annotation_map: dict[str, Any] = {}
    metadata_map: dict[str, Any] = {}
    for base in reversed(struct.__mro__):
        annotation_map.update(inspect.get_annotations(base))
        metadata_map.update(_field_metadata.get(base, {}))
    metadata_map.update(field_metadata)
    namespace["__dataclass_fields__"] = _build_fields(
        names,
        struct.__struct_annotations__,
        defaults,
        struct.__struct_metadata__,
        merged,
        annotation_map,
        metadata_map,
        no_init,
        kw_only_names,
    )
    namespace["__dataclass_params__"] = _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
    built = cast(Any, type(cls))(
        cls.__name__,
        cls.__bases__,
        namespace,
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
    if kw_only_names:
        _kw_only[built] = frozenset(kw_only_names)
    if field_metadata:
        _field_metadata[built] = field_metadata
    return cast(type[_T], built)


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
        if not init:
            raise NotImplementedError(f"init=False is not shimmed yet: {cls.__name__}")
        if is_struct(cls):
            # The class statement already built this class as a Struct: a
            # Struct base binds the metatype, which runs before the decorator
            # sees the class. The statement-time namespace is recoverable
            # (salix aligns inherited annotations first and defaults trailing)
            # and the rebuild translates field() and honors the options.
            return _rebuild_struct_subclass(cls, init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
        if (
            _caller_excluded()
            or _needs_stock_fallback(cls.__bases__)
            or _has_descriptor_field_collision(
                cls, tuple(inspect.get_annotations(cls))
            )
        ):
            return _to_stock(cls, init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only, slots, weakref_slot)
        namespace: dict[str, Any] = {}
        factories: list[tuple[str, Callable[[], Any]]] = []
        no_init: set[str] = set()
        kw_only_names: set[str] = set()
        field_metadata: dict[str, Any] = {}
        body_annotations = inspect.get_annotations(cls)
        initvars: dict[str, Any] = {}
        for name, value in cls.__dict__.items():
            if name in body_annotations and _is_initvar(body_annotations[name]):
                initvars[name] = (
                    _translate_field(cls.__name__, name, value).default
                    if isinstance(value, dataclasses.Field)
                    else value
                )
                continue
            if name in body_annotations and _is_classvar(body_annotations[name]):
                namespace[name] = value
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
            elif name in body_annotations and _needs_factory_default(value):
                factories.append((name, _deepcopy_factory(value)))
                namespace[name] = _PLACEHOLDER
            else:
                namespace[name] = value
        for name, annotation in body_annotations.items():
            if _is_initvar(annotation) and name not in initvars:
                initvars[name] = dataclasses.MISSING
        namespace["__annotations__"] = {
            name: annotation
            for name, annotation in body_annotations.items()
            if not _is_non_field_annotation(annotation)
        }
        if factories:
            namespace["__post_init__"] = _make_post_init(
                factories,
                getattr(cls, "__post_init__", None),
                tuple(name for name in body_annotations if name in initvars),
            )
        if (
            no_init
            or kw_only_names
            or kw_only
            or initvars
            or cls.__doc__ is not None
        ) and "__init__" not in cls.__dict__:
            namespace["__init__"] = _make_init(
                [
                    (
                        name,
                        initvars[name]
                        if name in initvars
                        else namespace.get(name, dataclasses.MISSING),
                        False if name in initvars else name in kw_only_names or kw_only,
                        name in initvars,
                    )
                    for name in body_annotations
                    if name not in no_init and not _is_classvar(body_annotations[name])
                ]
            )
        field_names = tuple(name for name in body_annotations if not _is_non_field_annotation(body_annotations[name]))
        namespace["__dataclass_fields__"] = _build_fields(
            field_names,
            tuple(body_annotations[name] for name in field_names),
            tuple(namespace[name] for name in field_names if name in namespace),
            tuple(() for _ in field_names),
            dict(factories),
            {name: body_annotations[name] for name in field_names},
            field_metadata,
            no_init,
            kw_only_names,
        )
        namespace["__dataclass_params__"] = _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
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
        if no_init:
            _no_init[built] = frozenset(no_init)
        if kw_only_names:
            _kw_only[built] = frozenset(kw_only_names)
        if field_metadata:
            _field_metadata[built] = field_metadata
        return cast(type[_T], built)

    if _cls is None:
        return wrap
    return wrap(_cls)


def install(exclude_prefixes: tuple[str, ...] = ()) -> None:
    _excluded_prefixes[:] = exclude_prefixes
    dataclasses.dataclass = cast(Any, dataclass)
    dataclasses.fields = cast(Any, fields)
    dataclasses.asdict = cast(Any, asdict)
    dataclasses.replace = cast(Any, replace)
    dataclasses.is_dataclass = cast(Any, is_dataclass)
