# Known gaps

Where the shim and salix differ from stock dataclasses. CPython's own
`test_dataclasses` measures the gap as a whole; see README.md.

## salix-level

1. **Two unrelated fielded struct bases are refused at class creation**,
   as `dataclass(slots=True)` refuses them. `class Child(Left, Right)`, where
   Left and Right both add fields, raises `TypeError: multiple bases have
   instance lay-out conflict: Left and Right each add fields, and a struct
   keeps its fields in slots, so like dataclass(slots=True) a class can
   extend only one base that adds slots` (measured 2026-10-09, f7cb728).

2. **Dataclass-era pickles do not load**, for example omegaconf's
   `test_pickle_backward_compatibility[2.0.6]`. Structs
   pickle since salix #99; these pickles were written when omegaconf's
   metadata classes were dataclasses, so their state is an instance dict a
   struct does not have. Out of scope by the #13 ruling.

3. **No instance `__dict__`**.
   Stock non-slots dataclasses accept arbitrary instance attributes
   (`obj.extra = 1`); structs are closed. Omegaconf's `to_container` sets
   non-init fields via `setattr`, which fails for fields not in the struct.
   Salix core's builder injects a plain-assignment `__setattr__` divert into
   the class dict (not set_field-backed); declared fields assign correctly
   through it, undeclared ones cannot exist.

4. **A ClassVar or a field name cannot be rebound or deleted on the class**
   (the #134 and #255 rulings). In `test_class_var` and
   `test_class_var_frozen`, `C.z += 1` raises `TypeError: cannot set 'z'
   attribute of struct class 'C': it is a ClassVar, and a class variable is
   a constant` (measured 2026-10-10).

## shim-level

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
- A hashable-class default with unhashable content (a tuple holding a
  list) is copied per instance, where stock shares one value.
- Annotation-only redeclarations keep non-factory base defaults
  (matching stock's class-attribute inheritance), while factory-backed
  base fields become required; the flags/init/kw_only resets are the
  real behavior to rely on.
- `asdict` recurses through list/dict/tuple containers of structs like
  stock but does not honor `dict_factory` options; `replace` is a
  dict-comprehension `salix.replace` equivalent.
- The ordering rule counts a default on an `init=False` field, which stock
  ignores: CPython's `test_class_marker` errors with `non-default field 'z'
  follows a field with a default`.
- The shim does not write stock's generated `__doc__`, the class's
  signature for a body without a docstring: 12 of CPython's 14
  `TestDocString` tests error on its absence.
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
