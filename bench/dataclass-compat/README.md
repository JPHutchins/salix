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

## Source tier

Planned: a fork of omegaconf rewritten to salix-native idioms (no `@dataclass`
decorators), per the discussion in #160. Not started yet.
