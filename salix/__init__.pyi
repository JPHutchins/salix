from typing import Any, Final

from typing_extensions import dataclass_transform

@dataclass_transform(frozen_default=True)
class Struct:
    _struct_fields_: Final[tuple[str, ...]]
    _struct_defaults_: Final[tuple[Any, ...]]
    __struct_fields__: Final[tuple[str, ...]]
    __struct_defaults__: Final[tuple[Any, ...]]
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
    def __getstate__(self) -> tuple[dict[str, Any], tuple[str, ...]]: ...
    def __setstate__(self, state: tuple[dict[str, Any], tuple[str, ...]], /) -> None: ...

def set_field(instance: Struct, name: str, value: object, /) -> None: ...
