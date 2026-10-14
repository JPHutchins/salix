import copy
import dataclasses
import inspect
from collections.abc import Callable
from typing import Any, NamedTuple, NoReturn, cast

from salix import Struct

from .state import _INIT_UNSET, _PLACEHOLDER, _merged_field_flags


def _make_eq() -> Callable[[Struct, object], bool]:
    def __eq__(self: Struct, other: object) -> bool:
        if self is other:
            return True
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


class _Translated(NamedTuple):
    default: Any
    init: bool
    factory: Callable[[], Any] | None
    metadata: dict[str, Any]
    kw_only: bool
    compare: bool
    repr: bool
    hash: bool | None


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


def _stock_mutable_raises(value: Any) -> bool:
    return isinstance(value, (list, dict, set, bytearray)) or type(value).__hash__ is None


def _deepcopy_factory(default: Any) -> Callable[[], Any]:
    return lambda: copy.deepcopy(default)


def _mutable_default_or_raise(name: str, value: Any) -> NoReturn:
    raise ValueError(
        f"mutable default {type(value)} for field {name} is not allowed: use default_factory"
    )


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
            if _stock_mutable_raises(value.default):
                _mutable_default_or_raise(name, value.default)
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


def _exec_name(base: str, taken: set[str]) -> str:
    candidate = base
    counter = 0
    while candidate in taken:
        counter += 1
        candidate = f"{base}_{counter}"
    taken.add(candidate)
    return candidate


def _make_init(params: list[tuple[str, Any, bool, bool]]) -> Callable[[Struct], None]:
    from salix import set_field

    taken = {name for name, _, _, _ in params}
    receiver = "self" if "self" not in taken else _exec_name("_shim_receiver", taken)
    role_bases = {
        "set_field": "_shim_set_field",
        "unset": "_shim_unset",
        "defaults": "_shim_defaults",
        "fresh": "_shim_fresh",
        "post": "_shim_post",
        "type": "_shim_type",
        "getattr": "_shim_getattr",
    }
    roles = {role: _exec_name(base, taken) for role, base in role_bases.items()}
    ordered = sorted(params, key=lambda item: item[2])
    arg_list = [receiver]
    for name, default, kw_only, _ in ordered:
        if kw_only and "*" not in arg_list:
            arg_list.append("*")
        arg_list.append(name if default is dataclasses.MISSING else f"{name}={roles['unset']}")
    body = [f"def __init__({', '.join(arg_list)}):"]
    for name, default, _, is_initvar in params:
        if is_initvar:
            if default is not dataclasses.MISSING and default is not _PLACEHOLDER:
                body.append(
                    f"    if {name} is {roles['unset']}: {name} = {roles['fresh']}({roles['defaults']}[{name!r}])"
                )
            continue
        if default is dataclasses.MISSING:
            body.append(f"    {roles['set_field']}({receiver}, {name!r}, {name})")
            continue
        body.append(f"    if {name} is not {roles['unset']}:")
        body.append(f"        {roles['set_field']}({receiver}, {name!r}, {name})")
        if default is _PLACEHOLDER:
            continue
        body.append("    else:")
        body.append(f"        {roles['set_field']}({receiver}, {name!r}, {roles['fresh']}({roles['defaults']}[{name!r}]))")
    initvar_names = [name for name, _, _, is_initvar in params if is_initvar]
    initvar_args = "" if not initvar_names else ", " + ", ".join(initvar_names)
    body.append(f'    {roles["post"]} = {roles["getattr"]}({roles["type"]}({receiver}), "__post_init__", None)')
    body.append(f"    if {roles['post']} is not None:")
    body.append(f"        {roles['post']}({receiver}{initvar_args})")
    namespace: dict[str, Any] = {
        roles["set_field"]: set_field,
        roles["unset"]: _INIT_UNSET,
        roles["defaults"]: {name: default for name, default, _, _ in params if default is not dataclasses.MISSING},
        roles["fresh"]: _fresh_default,
        roles["type"]: type,
        roles["getattr"]: getattr,
    }
    exec(compile("\n".join(body), "<shim __init__>", "exec"), namespace)
    synthesized = cast(Callable[[Struct], None], namespace["__init__"])
    synthesized._shim_synthesized = True  # type: ignore[attr-defined]
    return synthesized
