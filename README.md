# salix

A C-backed, inheritable `Struct` base class for Python, for programs that pay
for their imports.

```python
>>> from salix import Struct

>>> class Point(Struct):
...     x: float
...     y: float = 0.0

>>> Point(1.0, 2.0)
Point(x=1.0, y=2.0)

```

## ClassVar and InitVar

A field annotated `ClassVar[...]` or `InitVar[...]` is refused at class
creation, exactly when the annotation is an object. When the annotation
reaches salix as source text (under `from __future__ import annotations`)
the check is a spelling match, and the spellings it cannot see are:

| annotation | what the spelling match does |
| --- | --- |
| an alias of `ClassVar`, `CV[int]` | accepted; the field swallows a positional |
| a factory-local alias, 3.14 | accepted; the same symptom |
| `Optional[Annotated[ClassVar[int], "m"]]` | the object path accepts, the text path refuses |
| a type of yours actually named `ClassVar` | refused |
| `Annotated[int, ClassVar]` | refused, though the form is metadata |

## Caching a computed value

`functools.cached_property` caches into an instance `__dict__`, and a struct
has none; a slotted frozen dataclass refuses it the same way. Cache on the
method instead -- the cache then holds every instance it has seen -- or fill
a field from `__post_init__`:

```python
>>> import functools
>>> from salix import set_field

>>> class Computed(Struct):
...     x: int
...
...     @functools.cache
...     def double(self) -> int:
...         return self.x * 2

>>> Computed(3).double()
6

>>> class Filled(Struct):
...     x: int
...     double: int = 0
...
...     def __post_init__(self) -> None:
...         set_field(self, "double", self.x * 2)

>>> Filled(4).double
8

```

`salix/__init__.pyi` is the API. `src/salix.c` says what this is and is
not. `tasks.py` is what runs. `uv run camas benchmark` says what it costs.

## Working in the repo

```sh
nix develop
uv run camas check
```

## License

MIT
