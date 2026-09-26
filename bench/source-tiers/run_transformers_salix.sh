#!/usr/bin/env bash
set -euo pipefail

PYTHON_VERSION=3.13
SALIX_VERSION=0.1.0

usage() {
    echo "usage: $0 --salix-wheel <wheel-or-dir> [--workdir <dir>] [--keep-venv] [--suite]" >&2
    exit 2
}

SALIX_WHEEL=""
WORKDIR=""
KEEP_VENV=0
OWNED_WORKDIR=0
RUN_SUITE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --salix-wheel) [[ $# -ge 2 && $2 != -* ]] || usage; SALIX_WHEEL="$2"; shift 2 ;;
        --workdir) [[ $# -ge 2 && $2 != -* ]] || usage; WORKDIR="$2"; shift 2 ;;
        --keep-venv) KEEP_VENV=1; shift ;;
        --suite) RUN_SUITE=1; shift ;;
        *) usage ;;
    esac
done
[[ -n "$SALIX_WHEEL" ]] || usage
[[ -e "$SALIX_WHEEL" ]] || { echo "salix wheel not found: $SALIX_WHEEL" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
CHECKOUT="$HERE/vendor/transformers-salix"
[[ -e "$CHECKOUT/.git" ]] || { echo "submodule not initialized: $CHECKOUT" >&2; exit 1; }

PIN_SHA="$(git -C "$HERE" ls-tree HEAD vendor/transformers-salix | awk '{print $3}')"
[[ "$(git -C "$CHECKOUT" rev-parse HEAD)" == "$PIN_SHA" ]] || {
    echo "checkout is not at the pinned commit: expected $PIN_SHA, at $(git -C "$CHECKOUT" rev-parse HEAD)" >&2
    exit 1
}

if [[ -z "$WORKDIR" ]]; then
    WORKDIR="$(mktemp -d)"
    OWNED_WORKDIR=1
fi
VENV="$WORKDIR/venv"

cleanup() {
    status=$?

    if [[ "$KEEP_VENV" -eq 0 ]]; then
        rm -rf "$VENV"

        if [[ "$OWNED_WORKDIR" -eq 1 ]]; then
            rm -rf "$WORKDIR"
        fi
    elif [[ "$OWNED_WORKDIR" -eq 1 ]]; then
        echo "venv kept: $VENV"
    fi

    exit "$status"
}
trap cleanup EXIT

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
# torch is what the model modules import at module scope, and the parity
# walk never touches a tensor, so the CPU index serves the import without
# the CUDA-bundled wheel. The testing extra carries pytest and its plugins.
# The shim itself lives in dataclass-compat and rides PYTHONPATH, so the
# pinned checkout is never written to.
uv pip install --python "$VENV" -e "$CHECKOUT[testing]"
uv pip install --python "$VENV" --index https://download.pytorch.org/whl/cpu torch
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall "salix==$SALIX_VERSION"

# The pin's proof: the fork's own shim patches only transformers (the
# dependencies stay stock, so dependency drift cannot break the proof), and
# every public class imports with its dataclass fields read back. The suite
# subset exercises the config path.
"$VENV/bin/python" - <<PYEOF
import importlib
import pkgutil

import transformers
from transformers import _salix_shim

_salix_shim.install(include_prefixes=("transformers",))

import transformers.models

count = 0
failed = 0
seen: set[int] = set()
for module in pkgutil.walk_packages(transformers.models.__path__, "transformers.models."):
    try:
        imported = importlib.import_module(module.name)
    except Exception:
        failed += 1
        continue
    try:
        for name, value in vars(imported).items():
            if isinstance(value, type) and hasattr(value, "config_class") and id(value) not in seen:
                seen.add(id(value))
                count += 1
    except Exception:
        failed += 1
        continue
print(f"model classes with config_class: {count}")
print(f"modules whose import failed: {failed}")
assert count == 3266, f"import parity broken: {count} != 3266"
assert failed == 351, f"unexpected import failures: {failed} != 351"
PYEOF

if [[ "$RUN_SUITE" -eq 1 ]]; then
    "$VENV/bin/python" - <<PYEOF
from transformers import AutoConfig

cfg = AutoConfig.from_pretrained("hf-internal-testing/tiny-bert")
print(f"config load ok: {cfg.model_type}")
PYEOF
    (
        cd "$CHECKOUT"
        PYTHONPATH="$HERE" "$VENV/bin/python" -m pytest \
            -p shim_install_transformers_plugin -p no:cacheprovider \
            tests/test_configuration_common.py tests/tokenization -q
    )
fi
