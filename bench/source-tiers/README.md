# source-tiers

Drop-in-replacement proofs for [salix #160](https://github.com/JPHutchins/salix/issues/160):
the consumer libraries tested against, pinned as git submodules under
`vendor/` at the tested commits. No speed claims — the proof is the
suites passing with the consumers' dataclasses/attrs replaced by salix
`Struct`s.

| library | pin | state | suite (pin) | suite (stock, same env) |
|---|---|---|---|---|
| tyro | `vendor/tyro-salix` | all 65 internal dataclasses converted to Structs (1 documented exception) | 5100 passed, 288 skipped (2026-10-03, salix e93d04f) | 5080 passed, 300 skipped (2026-09-02) |
| cyclopts | `vendor/cyclopts-salix` | attrs classes converted to Structs (3 documented exceptions) | 2530 passed, 22 failed, 3 errors | identical |
| omegaconf | `vendor/omegaconf-salix` | Metadata/ContainerMetadata converted to Structs | 8512 passed, 1 failed (a pickle omegaconf 2.0.6 wrote), 1 module excluded (2026-10-03, salix e93d04f) | 8555 passed (2026-09-02) |
| transformers | `vendor/transformers-salix` | the repo-local shim patches only transformers (`include_prefixes`); the parity gate (a checked module, `bench/source-tiers/transformers_parity.py`) asserts 3266 distinct config-bearing classes, that the shim actually converted a config class, and the measured failure counts — 314 import failures with the shim (three more than stock's 311: the shim's known init=False refusals in three torch-adjacent modules) and 37 class-scan failures; `--stock` runs the same count without the shim for the baseline column | 3266 classes, 314 + 37 failures | 3266 classes, 311 + 37 failures (`--stock` measured) |
| hydra | `vendor/hydra` | no source changes (stock upstream pin); `-p shim_install_plugin` installs the patch before hydra's pytest11 plugin, so hydra's internals are shimmed too, unlike the patch tier | 2722 passed, 544 failed (2026-10-03, salix e93d04f): 540 `InputDefault` reading its own `__dict__`, 2 pickles an unpatched subprocess wrote, 2 bash completion | 3264 passed, 2 failed (2026-09-02) |

The readers widen the spec readers' dataclass gates to match struct
metadata (tyro's `_struct_compat` / cyclopts' `_struct_field_infos`
synthesize `dataclasses.Field` objects from struct metadata), and the
repos' own classes are Structs with frozen preserved; classes whose
instances are reassigned after construction are `frozen=False`.
Documented conversion exceptions: tyro's `InstantiationError`
(Exception base), cyclopts' `Parameter` and `Group` (attrs converters
plus init-wrapping recorder / field aliases) and its exception
classes, and transformers' `PretrainedConfig` family (the
`__init_subclass__` runtime re-decoration is dataclass machinery).

Each `run_*_salix.sh` reproduces the suite run from this repo alone:
the submodule pins the fork, the runner creates a 3.13 venv, installs
the salix wheel passed as an argument, and runs the suite, removing
the venv unless `--keep-venv` is given. The hydra and omegaconf legs
generate antlr parsers first and need java (or a nix JDK); the
transformers `--suite` leg downloads a tiny Hub config to prove
`AutoConfig` loading.
