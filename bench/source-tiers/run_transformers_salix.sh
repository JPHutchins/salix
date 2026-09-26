#!/usr/bin/env bash
set -euo pipefail

PYTHON_VERSION=3.13
SALIX_VERSION=0.1.0

usage() {
    echo "usage: $0 --salix-wheel <wheel-or-dir> [--workdir <dir>] [--keep-venv] [--suite] [--stock]" >&2
    exit 2
}

SALIX_WHEEL=""
WORKDIR=""
KEEP_VENV=0
OWNED_WORKDIR=0
RUN_SUITE=0
RUN_STOCK=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --salix-wheel) [[ $# -ge 2 && $2 != -* ]] || usage; SALIX_WHEEL="$2"; shift 2 ;;
        --workdir) [[ $# -ge 2 && $2 != -* ]] || usage; WORKDIR="$(realpath "$2")"; shift 2 ;;
        --keep-venv) KEEP_VENV=1; shift ;;
        --suite) RUN_SUITE=1; shift ;;
        --stock) RUN_STOCK=1; shift ;;
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
    elif [[ "$KEEP_VENV" -eq 1 ]]; then
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
# the CUDA-bundled wheel. It is installed first: the testing extra pulls
# accelerate, which requires torch, and a later line must not satisfy that
# requirement from the default index first. The testing extra carries
# pytest and its plugins. The shim itself lives in dataclass-compat and
# rides PYTHONPATH, so the pinned checkout is never written to.
uv pip install --python "$VENV" --index https://download.pytorch.org/whl/cpu torch
uv pip install --python "$VENV" -e "$CHECKOUT[testing]"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall "salix==$SALIX_VERSION"

# The pin's proof, in a module the repo's checkers see: the repo-local shim
# patches only transformers (the dependencies stay stock, so dependency
# drift cannot break the proof), and every public class imports with its
# dataclass fields read back. --stock runs the same count without the shim
# for the baseline column. The suite subset exercises the config path.
PYTHONPATH="$HERE/../dataclass-compat:$HERE" "$VENV/bin/python" -m transformers_parity "$([[ "$RUN_STOCK" -eq 1 ]] && echo --stock)"

if [[ "$RUN_SUITE" -eq 1 ]]; then
    PYTHONPATH="$HERE/../dataclass-compat" "$VENV/bin/python" - <<PYEOF
import _shim

_shim.install(include_prefixes=("transformers",))

from transformers import AutoConfig

cfg = AutoConfig.from_pretrained("hf-internal-testing/tiny-bert")
print(f"config load ok: {cfg.model_type}")
PYEOF
    (
        cd "$CHECKOUT"
        PYTHONPATH="$HERE/../dataclass-compat:$HERE" "$VENV/bin/python" -m pytest \
            -p shim_install_transformers_plugin -p no:cacheprovider \
            tests/test_configuration_common.py tests/tokenization -q
    )
fi
