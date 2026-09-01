import copy
import dataclasses
import inspect
import re
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
_field_doc_required = "doc" in inspect.signature(dataclasses.Field.__init__).parameters


def _caller_excluded() -> bool:
    frame: FrameType | None = sys._getframe(1)
    while frame is not None and frame.f_globals.get("__name__", "") == "_shim":
        frame = frame.f_back
    caller_module = frame.f_globals.get("__name__", "") if frame is not None else ""
    return any(
        caller_module == prefix or caller_module.startswith(prefix + ".")
        for prefix in _excluded_prefixes
    )


def _called_from_init_subclass(cls: type[Any]) -> bool:
    frame: FrameType | None = sys._getframe(1)
    while frame is not None:
        if frame.f_code.co_name == "__init_subclass__" and frame.f_locals.get("cls") is cls:
            return True
        frame = frame.f_back
    return False


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


def _is_initvar(annotation: Any) -> bool:
    if isinstance(annotation, str):
        head = re.split(r"[\s\[\],]+", annotation, maxsplit=1)[0]
        return head == "InitVar" or head.endswith(".InitVar")
    return isinstance(annotation, dataclasses.InitVar) or (
        getattr(annotation, "__origin__", None) is dataclasses.InitVar
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


def _merged_field_flags(struct_cls: type[Struct]) -> dict[str, tuple[bool, bool, bool | None]]:
    merged: dict[str, tuple[bool, bool, bool | None]] = {}
    for base in reversed(struct_cls.__mro__):
        merged.update(_field_flags.get(base, {}))
    return merged


def _make_eq() -> Callable[[Struct, object], bool]:
    def __eq__(self: Struct, other: object) -> bool:
        if other.__class__ is self.__class__:
            flags = _merged_field_flags(type(self))
            compared = [
                name
                for name in type(self).__struct_fields__
                if flags.get(name, (True, True, None))[0]
            ]
            return all(getattr(self, name) == getattr(other, name) for name in compared)
        return NotImplemented

    return __eq__


def _make_repr() -> Callable[[Struct], str]:
    def __repr__(self: Struct) -> str:
        flags = _merged_field_flags(type(self))
        shown = [
            name
            for name in type(self).__struct_fields__
            if flags.get(name, (True, True, None))[1]
        ]
        inner = ", ".join(f"{name}={getattr(self, name)!r}" for name in shown)
        return f"{type(self).__qualname__}({inner})"

    return __repr__


def _make_ordering(op: str) -> Callable[[Struct, object], Any]:
    def compare(self: Struct, other: object) -> Any:
        if other.__class__ is self.__class__:
            flags = _merged_field_flags(type(self))
            compared = [
                name
                for name in type(self).__struct_fields__
                if flags.get(name, (True, True, None))[0]
            ]
            return cast(
                bool,
                getattr(
                    tuple(getattr(self, name) for name in compared), op
                )(tuple(getattr(other, name) for name in compared)),
            )
        return NotImplemented

    return compare


def _make_hash() -> Callable[[Struct], int]:
    def __hash__(self: Struct) -> int:
        flags = _merged_field_flags(type(self))
        hashed = [
            name
            for name in type(self).__struct_fields__
            if (flags.get(name, (True, True, None))[2]
                if flags.get(name, (True, True, None))[2] is not None
                else flags.get(name, (True, True, None))[0])
        ]
        return hash(tuple(getattr(self, name) for name in hashed))

    return __hash__


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
    compare: bool
    repr: bool
    hash: bool | None


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
    flags_map: dict[str, tuple[bool, bool, bool | None]],
) -> dict[str, dataclasses.Field[Any]]:
    missing = len(names) - len(defaults)
    result: dict[str, dataclasses.Field[Any]] = {}
    for position, name in enumerate(names):
        factory = factory_map.get(name)
        compare, repr_flag, hash_flag = flags_map.get(name, (True, True, None))
        field_kwargs: dict[str, Any] = {
            "default": dataclasses.MISSING if (factory is not None or position < missing) else defaults[position - missing],
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
            _merged_field_flags(struct),
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


def _needs_factory_default(value: Any) -> bool:
    if isinstance(value, (list, dict, set, bytearray)):
        return True
    if type(value).__hash__ is None:
        return True
    try:
        hash(value)
    except TypeError:
        return True
    return False


def _deepcopy_factory(default: Any) -> Callable[[], Any]:
    return lambda: copy.deepcopy(default)


def _translate_field(
    cls_name: str,
    name: str,
    value: dataclasses.Field[Any],
) -> _Translated:
    kw_only = value.kw_only is True
    flags = (value.compare, value.repr, value.hash)
    metadata = dict(value.metadata)
    if value.default is not dataclasses.MISSING:
        if _needs_factory_default(value.default):
            try:
                copy.deepcopy(value.default)
            except TypeError:
                return _Translated(value.default, value.init, None, metadata, kw_only, *flags)
            return _Translated(_PLACEHOLDER, value.init, _deepcopy_factory(value.default), metadata, kw_only, *flags)
        return _Translated(value.default, value.init, None, metadata, kw_only, *flags)
    if value.default_factory is not dataclasses.MISSING:
        return _Translated(_PLACEHOLDER, value.init, value.default_factory, metadata, kw_only, *flags)
    return _Translated(dataclasses.MISSING, value.init, None, metadata, kw_only, *flags)


def _params_equal(a: Any, b: Any) -> bool:
    if a is None or b is None:
        return False
    return all(
        getattr(a, attr) == getattr(b, attr)
        for attr in (
            "init",
            "repr",
            "eq",
            "order",
            "unsafe_hash",
            "frozen",
            "match_args",
            "kw_only",
        )
    )


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


def _rebind_class_cells(built: type[Any], old_cls: type[Any]) -> None:
    # Body functions close over the statement-time class via their
    # __class__ cell; the build replaces the class, so zero-arg super()
    # and __class__ references would target the wrong object.
    for value in vars(built).values():
        func = getattr(value, "__func__", value)
        closure = getattr(func, "__closure__", None)
        if closure:
            for cell in closure:
                try:
                    contents = cell.cell_contents
                except ValueError:
                    continue
                if contents is old_cls:
                    cell.cell_contents = built


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
            if default is not dataclasses.MISSING and default is not _PLACEHOLDER:
                body.append(f"    if {name} is _INIT_UNSET: {name} = _fresh(_defaults[{name!r}])")
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
    if _params_equal(
        cls.__dict__.get("__dataclass_params__"),
        _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only),
    ):
        return cls
    namespace: dict[str, Any] = {}
    factories: list[tuple[str, Callable[[], Any]]] = []
    field_metadata: dict[str, Any] = {}
    field_flags_map: dict[str, tuple[bool, bool, bool | None]] = {}
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
            field_flags_map[name] = (translated.compare, translated.repr, translated.hash)
            continue
        if _needs_factory_default(value):
            factories.append((name, _deepcopy_factory(value)))
            namespace[name] = _PLACEHOLDER
            continue
        namespace[name] = value
    for key, value in cls.__dict__.items():
        if key in names or key in _SALIX_MEMBERS:
            continue
        if key == "__hash__" and value is None and eq:
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
                (
                    name,
                    namespace.get(name, dataclasses.MISSING),
                    name in kw_only_names or (kw_only and name in own_names),
                    False,
                )
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
    if kw_only:
        kw_only_names.update(own_names)
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
    namespace["__dataclass_fields__"] = _build_fields(
        names,
        struct.__struct_annotations__,
        tuple(namespace.get(name, dataclasses.MISSING) for name in names),
        struct.__struct_metadata__,
        merged,
        annotation_map,
        metadata_map,
        no_init,
        kw_only_names,
        merged_flags,
    )
    namespace["__dataclass_params__"] = _dataclass_params(init, repr, eq, order, unsafe_hash, frozen, match_args, kw_only)
    if not names and _called_from_init_subclass(cls):
        # Mid-__init_subclass__: salix has not finalized the struct metadata,
        # and rebuilding here would re-trigger __init_subclass__ recursively.
        # Defer to the decorator that runs after the class statement.
        return cls
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
    if merged_flags:
        _field_flags[built] = merged_flags
    _rebind_class_cells(cast(type[Any], built), cls)
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
        field_flags_map: dict[str, tuple[bool, bool, bool | None]] = {}
        body_annotations = inspect.get_annotations(cls)
        initvars: dict[str, Any] = {}
        for name, value in cls.__dict__.items():
            if name in body_annotations and _is_initvar(body_annotations[name]):
                if (
                    isinstance(value, dataclasses.Field)
                    and value.default_factory is not dataclasses.MISSING
                ):
                    raise TypeError(f"field {name} cannot have a default factory")
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
            elif name in body_annotations and _needs_factory_default(value):
                factories.append((name, _deepcopy_factory(value)))
                namespace[name] = _PLACEHOLDER
                field_flags_map[name] = (True, True, None)
            else:
                namespace[name] = value
                if name in body_annotations:
                    field_flags_map[name] = (True, True, None)
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
        if kw_only:
            kw_only_names.update(field_names)
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
            field_flags_map,
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
        if field_flags_map:
            _field_flags[built] = field_flags_map
        _rebind_class_cells(cast(type[Any], built), cls)
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
