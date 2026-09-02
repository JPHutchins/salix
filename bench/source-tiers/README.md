# source-tiers

Drop-in-replacement proofs for [salix #160](https://github.com/JPHutchins/salix/issues/160):
consumer libraries with the dataclasses replaced by salix `Struct`s,
run against their own test suites. No speed claims — the proof is that
the suites pass.

| library | fork | suite (fork) | suite (stock, same env) |
|---|---|---|---|
| tyro | [JPHutchins/tyro-salix](https://github.com/JPHutchins/tyro-salix) PR #1 | 5088 passed, 300 skipped | identical (upstream sha) |
| cyclopts | [JPHutchins/cyclopts-salix](https://github.com/JPHutchins/cyclopts-salix) PR #1 | 2530 passed, 22 failed, 3 errors | identical (upstream sha) |

The forks change two things: the consumer's dataclasses become salix
`Struct`s, and the spec readers' dataclass gates also match struct
metadata (tyro's `_struct_compat` / cyclopts' `_struct_field_infos`
synthesize `dataclasses.Field` objects from `__struct_fields__`/
`__struct_annotations__`/`__struct_defaults__`, so the existing
machinery consumes Structs unchanged).

`run_tyro_salix.sh` and `run_cyclopts_salix.sh` reproduce the suite
runs from this repo alone: they pin the fork and stock shas, create a
3.13 venv, and install the salix wheel passed as an argument
(`--mode fork|stock`).
