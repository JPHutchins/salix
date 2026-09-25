# source-tiers

Drop-in-replacement proofs for [salix #160](https://github.com/JPHutchins/salix/issues/160):
consumer libraries with their dataclasses replaced by salix `Struct`s,
run against their own test suites. The proof is the suites passing; no
speed claims.

| library | fork | state | suite (fork mode) | suite (stock mode) |
|---|---|---|---|---|
| tyro | [JPHutchins/tyro-salix](https://github.com/JPHutchins/tyro-salix) PR #1 | **all 65 internal dataclasses converted to Structs** (2 documented exceptions) | 5088 passed, 300 skipped | 5080 passed, 300 skipped |
| cyclopts | [JPHutchins/cyclopts-salix](https://github.com/JPHutchins/cyclopts-salix) PR #1 | **attrs classes converted to Structs** (3 documented exceptions) | 2530 passed, 22 failed, 3 errors | identical (same env) |

The forks widen the spec readers' dataclass gates to also match
struct metadata (tyro's `_struct_compat` / cyclopts'
`_struct_field_infos` synthesize `dataclasses.Field` objects from
`__struct_fields__`/`__struct_annotations__`/`__struct_defaults__`, so
the existing machinery consumes Structs unchanged), and replace the
repos' own classes with Structs: tyro's internal dataclasses are all
converted (exceptions: `LoweredArgumentDefinition`, which the lowering
rules mutate ~40 times, and the `InstantiationError` Exception base).
Cyclopts is attrs-based; its attrs classes are converted to Structs
with the same discipline (frozen unless the instances are reassigned;
converters and validators move into __post_init__). Three classes stay
attrs: Parameter and Group (attrs converters plus init-wrapping
recorder / field aliases, which the generated struct constructor
cannot host) and the exception classes (Exception bases). The 22
failures in both columns are the sphinx-dependent tests, pre-existing
on this environment.

`run_tyro_salix.sh` and `run_cyclopts_salix.sh` reproduce the suite
runs from this repo alone: they pin the fork and stock shas (full
40-hex, asserted after checkout), create a 3.13 venv, install the
salix wheel passed as an argument, and remove the venv unless
`--keep-venv` is given (`--mode fork|stock`).
