# dataclass-compat

Consumer-tier experiments for [salix #160](https://github.com/JPHutchins/salix/issues/160):
can salix serve as a drop-in replacement for `dataclasses` in real libraries?

## Patch tier: omegaconf, zero source changes

`_shim.py` replaces `dataclasses.dataclass`, `fields`, `asdict`, `replace`, and
`is_dataclass` with implementations that build salix `Struct`s behind the
dataclasses API. Omegaconf's source is untouched: its `@dataclass` decorators
route through the patch, and its own internal dataclasses become salix Structs.

`run_omegaconf.sh` runs omegaconf's own test suite against the patch. It pins
the omegaconf revision, the dependency set (`requirements.txt`), and the salix
wheel, so a run is reproducible from this repo's build alone.

```sh
./run_omegaconf.sh --salix-wheel <path-to-built-salix-wheel>
```

### Measured result (salix 0.1.0, omegaconf a8bcf1f, 2026-09-01)

- 8473 passed
- 40 failed, all in three documented gaps (see `known_gaps.md`):
  - 37 pickle (salix refuses pickle)
  - 3 arbitrary instance attributes (structs have no `__dict__`)
- 1 module excluded at collection: two unrelated Struct bases (layout conflict)

### Mechanics

The patch is installed by a generated root `conftest.py`
(`from _shim import install; install()`) so it is active before any test module
imports. Stock dataclasses functions are captured at import; non-struct
objects fall through to them. Classes with static-type bases (e.g. `dict`)
fall back to stock dataclasses entirely — salix's C layout cannot coexist
with them.

## Patch tier: tyro, zero source changes

`run_tyro.sh` does the same against tyro's suite (pinned `d0c9877f`), with
`install(exclude_prefixes=("tyro",))` — tyro's own internal dataclasses stay
stock, every user-facing dataclass in the tests is shimmed.

### Measured result (tyro d0c9877f, 2026-09-01)

- 5062 passed
- 30 failed, in four documented families: pickle (16), InitVar (8),
  empty-struct field equality (2), functools.partial resolution (4)
  — see `known_gaps.md`

## Patch tier: hydra, zero source changes

`run_hydra.sh` runs hydra's suite (pinned `d1e07c8f`) with the patch installed
unconditionally — hydra's own internals are shimmed too, no exclusions.

### Measured result (hydra d1e07c8f, 2026-09-01)

- 3264 passed
- 2 failed — both fail identically on a stock checkout (bash completion
  scripts in this environment), so the patch is at full stock parity
- 219 skipped, 1 xfailed

## Measured startup deltas, per library (real workloads, 2026-09-01)

Fresh interpreter per run, median of 5; the libraries' own workloads —
omegaconf runs its suite's structured-config corpus, hydra composes its
own example config, tyro parses a real CLI module, transformers is a
plain import. Reproduced by the `startup_bench.py` script in this
directory (paths are machine-specific; the numbers below are this
repo's measured run).

| library | pre | post | delta |
|---|---|---|---|
| omegaconf (fork migration) | 134.5 ms | 129.7 ms | −4.8 ms (−3.6%) |
| transformers (fork migration) | 588.9 ms | 577.4 ms | −11.5 ms (−2.0%) |
| hydra (patch tier, internals excluded) | 156.5 ms | 157.6 ms | +0.7% |
| tyro (patch tier, internals excluded) | 55.2 ms | 57.9 ms | +4.9% |

Real migrations start faster; patch-only tiers don't, because their own
internals stay stock.

## Source tier

Fork [JPHutchins/omegaconf-salix](https://github.com/JPHutchins/omegaconf-salix)
(branch `salix-native`, draft PR), rewriting omegaconf to salix-native idioms:
`Metadata`/`ContainerMetadata` are salix `Struct`s with `__reduce__`
reconstruction, struct input is recognized in `_utils`, and pickle works
through `__getstate__`/`__setstate__` resolving the class by module+qualname.

Measured (2026-09-01): 8553 passed / 2 failed — the 2 are legacy-pickle
artifact tests. `benchmarks/salix_vs_stock.py` A/B: deepcopy −22%,
pickle +47%, the rest parity.
