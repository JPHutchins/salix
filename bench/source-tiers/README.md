# source-tiers

The [salix #160](https://github.com/JPHutchins/salix/issues/160) proof:
libraries whose own dataclasses, attrs classes and NamedTuples are salix
`Struct`s, each measured against the upstream commit its fork is based on.
Each fork's `main` is that upstream commit, and the rework is an open pull
request against it:

| library | rework |
|---|---|
| omegaconf | [JPHutchins/omegaconf-salix#1](https://github.com/JPHutchins/omegaconf-salix/pull/1) |
| tyro | [JPHutchins/tyro-salix#1](https://github.com/JPHutchins/tyro-salix/pull/1) |
| cyclopts | [JPHutchins/cyclopts-salix#1](https://github.com/JPHutchins/cyclopts-salix/pull/1) |
| hydra | [JPHutchins/hydra-salix#5](https://github.com/JPHutchins/hydra-salix/pull/5) |
| transformers | [JPHutchins/transformers-salix#1](https://github.com/JPHutchins/transformers-salix/pull/1) |

`run_benchmarks.py` gates each pinned fork with `fork_gate.py`, then
measures it against its base with `interleave.py`, writing every table and
a provenance record to `--out`:

```sh
git submodule update --init
uv run --no-project python bench/source-tiers/run_benchmarks.py --salix-wheel <cp313 wheel> --out <dir>
```

Each `run_<library>_salix.sh` runs that fork's own test suite against a
salix wheel.
