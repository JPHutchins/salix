# Known gaps

Failures against omegaconf's suite that the patch cannot close without salix
core changes. Counts are from the 2026-09-01 run (salix 0.1.0, omegaconf
a8bcf1f): 8473 passed, 40 failed, 1 module excluded.

## Blockers (salix-level)

1. **Two unrelated Struct bases fail at class creation.**
   `class Child(Left, Right)` where Left and Right are both Structs raises
   `TypeError: multiple bases have instance lay-out conflict` from CPython,
   at statement time — before any decorator can intercept. Excluded module:
   `tests/test_structured_config_unions.py`.

2. **Pickle is refused** (37 tests, mostly `tests/test_serialization.py`).
   Salix deliberately refuses pickle (salix #13, lowest priority per JPH).

3. **No instance `__dict__`** (3 tests in `tests/test_to_container.py`).
   Stock non-slots dataclasses accept arbitrary instance attributes
   (`obj.extra = 1`); structs are closed. Omegaconf's `to_container` sets
   non-init fields via `setattr`, which fails for fields not in the struct.
   Salix core's builder injects a plain-assignment `__setattr__` divert into
   the class dict (not set_field-backed); declared fields assign correctly
   through it, undeclared ones cannot exist.

## Tyro tier (patch, zero source changes)

`run_tyro.sh` runs tyro's suite (pinned `d0c9877f`, frozen deps in
`requirements-tyro.txt`) with the patch installed as
`install(exclude_prefixes=("tyro",))`: tyro's own internal dataclasses stay
stock (its source caches on the instance `__dict__`, which structs do not
have), while every user-facing dataclass in the tests is shimmed.

Measured 2026-09-01: 5062 passed, 30 failed, 288 skipped. Failure families:

- pickle (16, incl. `test_generics_and_serialization`) — salix refuses pickle.
- InitVar (8) — salix refuses `InitVar` annotations at class creation, so
  the shim strips them (constructor + `fields()` parity), but tyro's spec
  reader needs the names in `__annotations__` (impossible to restore: salix
  blocks post-build class mutation) and its constructor setattrs InitVars
  onto instances (no instance `__dict__`).
- empty-struct field equality (2), functools.partial resolution (4) —
  tyro-internal subtleties, documented as patch-tier limits.

Also documented: a namespace `__setattr__` flips salix's hash plan to
unhashable, so the shim injects none (unfrozen structs accept plain
assignment natively; frozen ones refuse with AttributeError instead of
stock's FrozenInstanceError).

Hydra's real startup path (composing its example config) hits an
instance-`__dict__` cache in its own internals that the suite never
exercises, so production use of the patch with hydra wants
`install(exclude_prefixes=("hydra",))` — the runner's suite scope uses
the unconditional install because the suite passes that way. The
per-library startup measurements are in README.md.

## Hydra tier (patch, zero source changes)

`run_hydra.sh` pins hydra `d1e07c8f` and installs the patch unconditionally.
Measured 2026-09-01: 3264 passed, 2 failed — the 2 fail identically on a
stock checkout (bash completion scripts), so the patch is at stock parity.
The InitVar story the tyro tier opened (passing InitVars to
`__post_init__` instead of storing them) is what hydra's
`RuntimeValue` tests needed; it is implemented in the shim now.

## Not yet shimmed (patch-level, no suite hits)



- Decorator-level `init=False` raises `NotImplementedError` (field-level
  `init=False` is shimmed).
- InitVar fields declared in a subclass of a shimmed struct fail at the
  class statement: salix refuses the InitVar annotation before the
  decorator runs (the fresh path strips it from plain classes; a struct
  subclass never reaches that code). Salix-level.
- `inspect.signature` of the synthesized `__init__` shows the `_INIT_UNSET`
  sentinel as parameter defaults instead of the real values (stock shows
  real defaults; mutable/factory defaults have no real value to show).
- `init=False` fields with `default_factory`: the class attribute is a salix
  member descriptor, not the factory result stock stores on the class (the
  instance value is correct).
- `__match_args__` includes `init=False` fields (stock excludes them).
- `asdict` recurses through list/dict/tuple containers of structs like
  stock but does not honor `dict_factory` options; `replace` is a
  dict-comprehension `salix.replace` equivalent.
- Ordering rule is stricter than stock for inherited defaults followed by
  required fields (salix refuses; pytest's own dataclasses exhibit this when
  the patch is installed before pytest imports).
- A body `__init__` suppresses `__post_init__` and factory fills — the same
  degenerate state stock dataclasses produce (its `init` parameter is ignored
  when the body defines `__init__`, and the generated init that would call
  `__post_init__` is never created).
- Body `__setattr__` hooks that assign through `object.__setattr__` raise
  `TypeError: can't apply this __setattr__ to <cls> object` on CPython 3.12
  with the repo-built salix wheel (salix's C `tp_setattro` is custom and
  `object.__setattr__` refuses non-generic setattro). Measured on 3.14.6 the
  same pattern works on fresh, frozen, and rebuilt shimmed structs — the
  failure is version-dependent. Assignment hooks should go through
  `super().__setattr__` or salix's assignment path; a salix-core follow-up
  could let `object.__setattr__` apply.

Frozen structs hash by content even with eq=False: salix's tp_hash slot is
content-based and a body `__hash__` only changes the attribute view, not
hash() — so `@dataclass(frozen=True, eq=False)` classes get a content hash
where stock keeps identity hashing. Salix-level, documented.
