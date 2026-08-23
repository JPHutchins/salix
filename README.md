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

A name annotated `ClassVar[...]` at the top level is a class variable:
with an assigned value it is kept in the class dict and excluded from
the field plan, the constructor, and every metadata table; without one
it is refused, because a class variable is a constant. `InitVar[...]`
is refused, and so is a `ClassVar` nested inside another annotation.
The check walks the annotation's forms when it arrives as an object;
when it arrives as source text — a quoted string, or a
future-annotations string — the check is a spelling match, and the two
paths differ at the edges:

| annotation | object path | text path |
| --- | --- | --- |
| an alias of `ClassVar`, `CV[int]` | the form itself: kept with a value, refused without | accepted; the field swallows a positional |
| a type of yours actually named `ClassVar` | accepted | refused |
| `Annotated[int, ClassVar]` | accepted | refused, though the `ClassVar` there is metadata |
| `Optional[Annotated[ClassVar[int], "m"]]` | refused | refused |

3.14 evaluates resolvable annotations by default, so there the alias
reaches the object path and is the form; a name whose root cannot be
resolved arrives as an object too, and is accepted, while any other
evaluation error still fails the class. Every row is pinned in
`tests/test_classvar_paths.py`.

## Caching a computed value

`functools.cached_property` caches into an instance `__dict__`; a struct has
none unless a non-struct base carries one, and without one a slotted
dataclass refuses it the same way. Two answers: `functools.cache` on the
method, or a field that `__post_init__` fills:

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

Under the default eq=True the method cache keys on the value, not the
instance: value-equal structs share one entry, and a `set_field` that
changes the value misses — the cache then holds the old entry and the new
one. With eq=False the struct hashes by identity — unless the body defines
its own `__eq__` without a `__hash__`, which makes it unhashable, or its
own `__hash__`, which replaces the identity hash — and `set_field` after
a hit returns the stale cached value while the hash is unchanged; a body
`__hash__` over field values changes the hash and the lookup misses. The
cache refuses an unhashable struct — anything that makes `hash()` raise:
a body `__eq__` without a body `__hash__`, a body `__hash__` that
refuses, `frozen=False`, an unhashable field value, or equality inherited
from a struct base that defines `__eq__`. An `__init__` that is not
`object.__init__` — a body-written one, inherited or not — replaces the
constructor that runs `__post_init__`, so the field answer only works
without one. The table and the caching behaviors above are pinned in
`tests/test_classvar_paths.py`.

`salix/__init__.pyi` is the API. `src/salix.c` says what this is and is
not. `tasks.py` is what runs. `uv run camas benchmark` says what it costs.

## Working in the repo

```sh
nix develop
uv run camas check
```

## License

MIT
