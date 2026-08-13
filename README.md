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
creation. The check is exact when the annotation reaches salix as an object;
when it arrives as source text -- a quoted string, or an annotation that
nothing evaluated -- the check is a spelling match, and the two paths differ
at the edges:

| annotation | object path | text path |
| --- | --- | --- |
| an alias of `ClassVar`, `CV[int]` | refused | accepted; the field swallows a positional |
| a type of yours actually named `ClassVar` | accepted | refused |
| `Annotated[int, ClassVar]` | accepted | refused, though the form is metadata |
| `Optional[Annotated[ClassVar[int], "m"]]` | refused | refused |

3.14 evaluates resolvable annotations by default, so there the alias reaches
the object path and is refused; the text path sees only what nothing can
resolve.

## Caching a computed value

`functools.cached_property` caches into an instance `__dict__`, and a struct
has none; a slotted frozen dataclass refuses it the same way. Two answers:
`functools.cache` on the method, or a field that `__post_init__` fills:

```python
>>> import functools
>>> from salix import Struct, set_field

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

The method cache holds every instance it has seen -- one entry per mutation
state, since structs hash by value -- and refuses when the instance is
unhashable. A body-defined `__init__` replaces the constructor that runs
`__post_init__`, so the field answer only works without one.

`salix/__init__.pyi` is the API. `src/salix.c` says what this is and is
not. `tasks.py` is what runs. `uv run camas benchmark` says what it costs.

## Working in the repo

```sh
nix develop
uv run camas check
```

## License

MIT
