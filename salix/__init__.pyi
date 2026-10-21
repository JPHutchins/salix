from collections.abc import Mapping
from typing import Any, Final, Protocol, TypeVar, final, overload

from typing_extensions import ParamSpec, Self, dataclass_transform

_T = TypeVar("_T")

@overload
def field(*, default: _T, kw_only: bool = ...) -> _T: ...
@overload
def field(*, kw_only: bool = ...) -> Any: ...

@dataclass_transform(frozen_default=True, field_specifiers=(field,))
class Struct:
    _struct_fields_: Final[tuple[str, ...]]
    _struct_defaults_: Final[tuple[Any, ...]]
    __struct_fields__: Final[tuple[str, ...]]
    __struct_defaults__: Final[tuple[Any, ...]]
    _struct_annotations_: Final[tuple[Any, ...]]
    __struct_annotations__: Final[tuple[Any, ...]]
    _struct_metadata_: Final[tuple[tuple[Any, ...], ...]]
    __struct_metadata__: Final[tuple[tuple[Any, ...], ...]]
    def __copy__(self) -> Self: ...
    def __deepcopy__(self, memo: dict[int, Any]) -> Self: ...
    def __replace__(self, /, **changes: object) -> Self: ...
    def __init_subclass__(
        cls,
        *,
        frozen: bool = True,
        eq: bool = True,
        order: bool = False,
        repr: bool = True,
        match_args: bool = True,
        weakref: bool = False,
        kw_only: bool = False,
    ) -> None: ...

_StructT = TypeVar("_StructT", bound=Struct)
_StructT_co = TypeVar("_StructT_co", bound=Struct, covariant=True)
_Changes = ParamSpec("_Changes")

class _ReplaceableStruct(Protocol[_Changes, _StructT_co]):
    @property
    def __struct_fields__(self) -> tuple[str, ...]: ...
    def __replace__(self, /, *args: _Changes.args, **changes: _Changes.kwargs) -> _StructT_co: ...


def set_field(instance: Struct, name: str, value: object, /) -> None: ...
def replace(
    instance: _ReplaceableStruct[_Changes, _StructT_co],
    /,
    *args: _Changes.args,
    **changes: _Changes.kwargs,
) -> _StructT_co: ...
def from_mapping(cls: type[_StructT], values: Mapping[str, object], /) -> _StructT: ...
def to_dict(instance: Struct, /) -> dict[str, object]: ...

class FrozenInstanceError(AttributeError): ...

@final
class _MissingType: ...

MISSING: Final[_MissingType]

@final
class Field:
    @property
    def name(self) -> str: ...
    @property
    def type(self) -> Any: ...
    @property
    def default(self) -> Any: ...
    @property
    def default_factory(self) -> Any: ...
    @property
    def init(self) -> bool: ...
    @property
    def repr(self) -> bool: ...
    @property
    def hash(self) -> bool | None: ...
    @property
    def compare(self) -> bool: ...
    @property
    def metadata(self) -> Mapping[Any, Any]: ...
    @property
    def kw_only(self) -> bool: ...
    @property
    def annotated_metadata(self) -> tuple[Any, ...]: ...
    def __replace__(self, /, **changes: object) -> Self: ...

def fields(class_or_instance: type[Struct] | Struct, /) -> tuple[Field, ...]: ...

__version__: str
