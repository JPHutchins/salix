from collections.abc import Mapping
from typing import Any, Final, TypeVar

from typing_extensions import Self, dataclass_transform

@dataclass_transform(frozen_default=True)
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
    def __init_subclass__(
        cls,
        *,
        frozen: bool = True,
        eq: bool = True,
        order: bool = False,
        repr: bool = True,
        match_args: bool = True,
        weakref: bool = False,
    ) -> None: ...

_StructT = TypeVar("_StructT", bound=Struct)


def set_field(instance: Struct, name: str, value: object, /) -> None: ...
def from_mapping(cls: type[_StructT], values: Mapping[str, object], /) -> _StructT: ...
