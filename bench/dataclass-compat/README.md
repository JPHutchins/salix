# dataclass-compat

`_shim.py` builds salix `Struct`s behind the `dataclasses` API, for
[salix #160](https://github.com/JPHutchins/salix/issues/160). The source
tier's shim-based legs (`../source-tiers/`) and the CPython conformance
run below use it. `known_gaps.md` lists where the shim and salix differ
from stock.

`install()` has to run before the code under test imports
`dataclasses`. A class with a static-type base, such as `dict`, stays a
stock dataclass. `exclude_prefixes` keeps a package's own dataclasses
stock: hydra needs it, because its internals read their own instance
`__dict__` (540 of the source tier's 544 hydra failures).

## CPython conformance

`camas conformance` builds salix in place and runs CPython's own
`test_dataclasses` through the shim, on the exact interpreter the tests
were tagged for. The tests are fetched by a sparse checkout of the tag
into `.cpython/`. Every test's outcome, and for a failure its exception
type, is compared with `conformance-v3.14.6.json`. A change in either
direction fails the run. A fix that turns a test green therefore
re-records the file with `--record`, which prints the per-test delta
before writing. Skips are left out of the file, because a skip measures
the interpreter rather than salix.

Measured 2026-10-09, CPython v3.14.6:

| | pass | skip | fail | error |
|---|---|---|---|---|
| stock `dataclasses` | 275 | 1 | 0 | 0 |
| through the shim | 164 | 1 | 56 | 55 |

The skipped test needs `_testcapi`.

## Measured startup deltas, per library (real workloads, 2026-09-01)

Fresh interpreter per run, median of 5, on each library's own workload:
- omegaconf runs its suite's structured-config corpus.
- transformers is a plain import.

`startup_bench.py` and `typebench.py` hard-code machine-specific
interpreter paths and `/tmp` fixtures. The numbers below are this
repo's measured run.

| library | pre | post | delta |
|---|---|---|---|
| omegaconf (fork migration) | 134.5 ms | 129.7 ms | −4.8 ms (−3.6%) |
| transformers (fork migration) | 588.9 ms | 577.4 ms | −11.5 ms (−2.0%) |
