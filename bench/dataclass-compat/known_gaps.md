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
   The patch injects a `set_field`-backed `__setattr__`, so declared fields
   assign correctly; undeclared ones cannot exist.

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

## Hydra tier (patch, zero source changes)

`run_hydra.sh` pins hydra `d1e07c8f` and installs the patch unconditionally.
Measured 2026-09-01: 3264 passed, 2 failed — the 2 fail identically on a
stock checkout (bash completion scripts), so the patch is at stock parity.
The InitVar story the tyro tier opened (passing InitVars to
`__post_init__` instead of storing them) is what hydra's
`RuntimeValue` tests needed; it is implemented in the shim now.

## Not yet shimmed (patch-level, no suite hits)



- `kw_only=True` and decorator-level `init=False` raise `NotImplementedError`.
- `init=False` fields with `default_factory` keep `_PLACEHOLDER` as a class
  attribute instead of stock's factory-on-class quirk.
- `__match_args__` includes `init=False` fields (stock excludes them).
- `inspect.signature` of generated init is `(self, /, *args, **kwargs)`
  (salix's C signature), not the per-field stock signature.
- `asdict`/`replace` for structs are dict-comprehension/`salix.replace`
  equivalents; `asdict` deep-copies but does not recurse through nested
  dataclasses with `dict_factory` options.
- Ordering rule is stricter than stock for inherited defaults followed by
  required fields (salix refuses; pytest's own dataclasses exhibit this when
  the patch is installed before pytest imports).
- A body `__init__` suppresses `__post_init__` and factory fills — the same
  degenerate state stock dataclasses produce (its `init` parameter is ignored
  when the body defines `__init__`, and the generated init that would call
  `__post_init__` is never created).
