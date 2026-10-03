# Known gaps

Failures against omegaconf's suite that the patch cannot close without salix
core changes. Counts are from the 2026-10-03 run (salix e93d04f, omegaconf
a8bcf1f): 8508 passed, 5 failed, 1 module excluded.

## Blockers (salix-level)

1. **Two unrelated Struct bases fail at class creation** (salix #164).
   `class Child(Left, Right)` where Left and Right both add fields raises
   salix's `TypeError: multiple bases have instance lay-out conflict: Left
   and Right each add fields, ...` at statement time — before any decorator
   can intercept. Excluded module: `tests/test_structured_config_unions.py`.

2. **Dataclass-era pickles do not load** (2 tests,
   `test_pickle_backward_compatibility[2.0.6]` and `[2.1.0.rc1]`). Structs
   pickle since salix #99; these pickles were written when omegaconf's
   metadata classes were dataclasses, so their state is an instance dict a
   struct does not have. Out of scope by the #13 ruling.

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

Measured 2026-10-03 (salix e93d04f): 5077 passed, 15 failed, 288 skipped.
Failure families:

- instance attributes (8) — `test_initvar_is_recognized_as_constructor_arg`
  and `test_initvar_of_pep695_generic_does_not_crash` set `self.total` /
  `self.seq` in `__post_init__`, outside the fields; structs have no
  instance `__dict__`. The InitVars themselves now resolve: salix declares
  them natively and the shim lists them in `__dataclass_fields__`.
- empty-struct field equality (2), functools.partial resolution (4) —
  tyro-internal subtleties, documented as patch-tier limits.
- `test_runtime_checkable_edge_case[argparse]` (1) passes when run alone.

Also documented: a namespace `__setattr__` flips salix's hash plan to
unhashable, so the shim injects none (unfrozen structs accept plain
assignment natively; frozen ones raise stock's FrozenInstanceError).

Hydra's internals read and write their own instance `__dict__`
(`InputDefault` and its subclasses), so production use of the patch with
hydra wants `install(exclude_prefixes=("hydra",))`. The patch-tier suite
never shims them (hydra's pytest11 plugin imports them first); the source
tier's `-p shim_install_plugin` does, and 540 of its 544 failures are that
`__dict__` use. The per-library startup measurements are in README.md.

## Hydra tier (patch, zero source changes)

`run_hydra.sh` pins hydra `d1e07c8f` and installs the patch unconditionally;
hydra's internals stay stock (see above). Measured 2026-10-03 (salix e93d04f):
3264 passed, 2 failed — the bash-completion scripts run a subprocess that
cannot import omegaconf in this environment.

## Not yet shimmed (patch-level, no suite hits)



- Decorator-level `init=False` raises `NotImplementedError` (field-level
  `init=False` is shimmed).
- A `ClassVar` annotated without a value is left out of the class and of
  `__dataclass_fields__`: salix requires a class variable to carry its
  value. Stock lists it as a pseudo-field with no default.
- InitVar and ClassVar pseudo-fields come from stock's own
  `dataclasses._get_field`, so they follow its rules. A salix-native struct
  base contributes no pseudo-fields, as stock treats any base without
  `__dataclass_fields__`.
- The shim needs Python 3.12+: `_dataclass_params` builds 3.12's
  `_DataclassParams`.
- `inspect.signature` of the synthesized `__init__` shows the `_INIT_UNSET`
  sentinel as parameter defaults instead of the real values (stock shows
  real defaults; mutable/factory defaults have no real value to show).
- `init=False` fields with `default_factory`: the class attribute is a salix
  member descriptor, not the factory result stock stores on the class (the
  instance value is correct).
- `__match_args__` includes `init=False` fields (stock excludes them).
- Hashable-class defaults with unhashable content (a tuple containing a
  list) become per-instance deepcopy factories where stock shares one
  plain default: salix core refuses such defaults at the builder, and
  `fields()` reports the factory instead of the shared value — salix #167.
- Annotation-only redeclarations keep non-factory base defaults
  (matching stock's class-attribute inheritance), while factory-backed
  base fields become required; the flags/init/kw_only resets are the
  real behavior to rely on.
- `asdict` recurses through list/dict/tuple containers of structs like
  stock but does not honor `dict_factory` options; `replace` is a
  dict-comprehension `salix.replace` equivalent.
- Ordering rule is stricter than stock for inherited defaults followed by
  required fields (salix refuses; pytest's own dataclasses exhibit this when
  the patch is installed before pytest imports) — salix #166.
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
where stock keeps identity hashing. Salix-level (salix #165).
