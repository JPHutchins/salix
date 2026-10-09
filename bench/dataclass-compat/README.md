# dataclass-compat

`_shim.py` builds salix `Struct`s behind the `dataclasses` API, for
[salix #160](https://github.com/JPHutchins/salix/issues/160). The source
tier's shim-based legs (`../source-tiers/`) and the CPython conformance
run below use it. `known_gaps.md` lists where the shim and salix differ
from stock.

## CPython conformance

`camas conformance` builds salix in place and runs CPython's own
`test_dataclasses` through the shim. The tests are fetched by a sparse
checkout of the pinned tag into `.cpython/`. Every test's outcome is
compared with `conformance-v3.14.6.json`, and a change in either
direction fails the run. A fix that turns a test green therefore
re-records the file with `conformance.py --record`.

Measured 2026-10-09 on salix 70bf53c, CPython v3.14.6:

| | pass | skip | fail | error |
|---|---|---|---|---|
| stock `dataclasses` | 275 | 1 | 0 | 0 |
| through the shim | 147 | 1 | 71 | 57 |

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
