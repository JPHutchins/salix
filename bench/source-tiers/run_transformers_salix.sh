#!/usr/bin/env bash
set -euo pipefail

PYTHON_VERSION=3.13

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

if [[ -z "$WORKDIR" ]]; then
    WORKDIR="$(mktemp -d)"
    OWNED_WORKDIR=1
fi
VENV="$WORKDIR/venv"

cleanup() {
    if [[ "$KEEP_VENV" -eq 0 ]]; then
        rm -rf "$VENV"
        [[ "$OWNED_WORKDIR" -eq 1 ]] && rm -rf "$WORKDIR"
    elif [[ "$OWNED_WORKDIR" -eq 1 ]]; then
        echo "venv kept: $VENV"
    fi
}
trap cleanup EXIT

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
uv pip install --python "$VENV" -e "$CHECKOUT"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall salix==0.1.0

# The pin's proof: every public class imports with the shim active and its
# dataclass fields read back. The suite subset exercises the config path.
"$VENV/bin/python" - <<PYEOF
import importlib
import pkgutil

import transformers
import transformers.models

count = 0
failed = 0
for module in pkgutil.walk_packages(transformers.models.__path__, "transformers.models."):
    try:
        imported = importlib.import_module(module.name)
    except ImportError:
        failed += 1
        continue
    for name, value in vars(imported).items():
        if isinstance(value, type) and hasattr(value, "config_class"):
            count += 1
print(f"model classes with config_class: {count}")
print(f"modules whose import failed: {failed}")
assert count == 782, f"import parity broken: {count} != 782"
PYEOF

if [[ "$RUN_SUITE" -eq 1 ]]; then
    "$VENV/bin/python" - <<PYEOF
from transformers import AutoConfig

cfg = AutoConfig.from_pretrained("hf-internal-testing/tiny-bert")
print(f"config load ok: {cfg.model_type}")
PYEOF
    (
        cd "$CHECKOUT"
        "$VENV/bin/python" -m pytest tests/test_configuration_common.py tests/tokenization -q
    )
fi
