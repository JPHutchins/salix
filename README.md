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

## Install

```sh
pip install salix
```

Wheels are published for Windows, macOS, and glibc Linux on x86_64 and
arm64, for the supported CPythons whose ABI is frozen — a release or an rc.
A pre-release interpreter's wheels are built and tested but held back until
its rc. salix supports CPython 3.10 through 3.15, including the free-threaded
builds. The full matrix is `nix/python-targets.nix`; a configuration missing
from it has no wheel. On a machine PyPI has no wheel for, pip compiles the
sdist — which needs setuptools and a C compiler — except on Windows, where
MSVC cannot compile this source. PyPy and GraalPy satisfy the version floor
and have no wheels, and whether the sdist builds there is untested.

## What salix is and is not

A struct's fields are the annotated names of its class body, and the
constructor takes them in that order. Instances are frozen by default: reads,
value equality, hashing, and a repr come with the class; writes do not.
A body-written `__init__` — inherited or not — replaces the field
constructor. Inheritance extends the field list — a subclass adds its fields
after its base's — and the metadata reads back: `_struct_fields_`,
`_struct_defaults_`, `_struct_annotations_`, and `_struct_metadata_`, with
`__`-prefixed spellings of the same four names answering the same values.

The keywords opt out of the defaults. `frozen=False` opens the record for
writes and drops the hashing — a writable struct is unhashable. `eq=False`
drops the value equality and hashing, leaving identity. `order=True` adds
the field-order comparisons and requires `eq=True`. `repr`, `match_args`,
and `weakref` default to `True`, `True`, and `False` and stand in for the
`__repr__`, the pattern-matching signature, and the `__weakref__` slot.

salix is not the libraries that do more. `dataclasses` builds the same record
in pure Python. `attrs` layers converters, validators, and a plugin ecosystem
on top. `NamedTuple` is the tuple-shaped predecessor. `msgspec` is a
serialization library whose `Struct` validates. salix does the record and
nothing else — no serialization, no field-value validation, no coercion — in
C, which is where its import and type-creation times come from. Programs that
need those features need those libraries; salix is for the ones that only
need the record. `salix/__init__.pyi` is the whole of the API.

## Defaults

An empty default of `list`, `dict`, `set`, or `bytearray` is copied per
instance, not shared:

```python
>>> class Box(Struct):
...     items: list = []

>>> left, right = Box(), Box()
>>> left.items.append("left")
>>> right.items
[]

```

The copy preserves the exact type or it is not a copy — a `defaultdict` copy
would be a plain `dict` — so the rule stops at the four, and a subclass of one
of them is stored as the class body's object itself, shared. A non-empty
default of the four is refused at class creation, because a copy can only be
shallow and the contents would still be shared: default it empty and fill it
in `__post_init__` with `set_field` — unless the body writes its own
`__init__`, which replaces the constructor that runs `__post_init__`.
A default whose type hashes but whose value cannot is refused the same way —
`x: tuple = (1, [])` dies at class creation — because every instance would
share it while its hash raises; a type that declares `__hash__ = None` is
left alone and shared. A field without a default cannot follow one that has
it: append defaulted fields, or default the new field too. For the four,
`__struct_defaults__` holds the class's own copy, severed from whatever the
class body named: appending to the module-level object afterwards does not
reach the class. The getters hand the stored objects out, not copies: do not
mutate what `__struct_defaults__` returns.

## ClassVar and InitVar

A name annotated `ClassVar[...]` at the top level is a class variable:
with an assigned value it is kept in the class dict and excluded from
the field plan, the constructor, and every metadata table; without one
it is refused, because a class variable is a constant. An inherited
field name stays a field: the inheritance rule outranks the ClassVar
annotation, so re-annotating one re-declares the field. `InitVar[...]`
is refused, and so is a `ClassVar` nested inside another annotation.
The check walks the annotation's forms when it arrives as an object;
when it arrives as source text — a quoted string, or a
future-annotations string — the check is a spelling match, and the two
paths differ at the edges:

| annotation | object path | text path |
| --- | --- | --- |
| an alias of `ClassVar`, `CV[int]` | the form itself: kept with a value, refused without | accepted; the field swallows a positional |
| a type of yours actually named `ClassVar` | accepted | kept with a value, refused without |
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

## Working in the repo

```sh
nix develop
uv run camas check
uv run camas benchmark
```

## License

MIT
