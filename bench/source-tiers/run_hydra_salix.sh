#!/usr/bin/env bash
set -euo pipefail

PYTHON_VERSION=3.13

usage() {
    echo "usage: $0 --salix-wheel <wheel-or-dir> [--workdir <dir>] [--keep-venv]" >&2
    exit 2
}

SALIX_WHEEL=""
WORKDIR=""
KEEP_VENV=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --salix-wheel) SALIX_WHEEL="$2"; shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        --keep-venv) KEEP_VENV=1; shift ;;
        *) usage ;;
    esac
done
[[ -n "$SALIX_WHEEL" ]] || usage
[[ -e "$SALIX_WHEEL" ]] || { echo "salix wheel not found: $SALIX_WHEEL" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
CHECKOUT="$HERE/vendor/hydra"
[[ -e "$CHECKOUT/.git" ]] || { echo "submodule not initialized: $CHECKOUT" >&2; exit 1; }

WORKDIR="${WORKDIR:-$(mktemp -d)}"
VENV="$WORKDIR/venv"

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
uv pip install --python "$VENV" -r "$HERE/../dataclass-compat/requirements-hydra.txt"
uv pip install --python "$VENV" -e "$CHECKOUT"
uv pip install --python "$VENV" -r "$CHECKOUT/requirements/dev.txt"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall salix==0.1.0

INSTALL_STANZA='from _shim import install
install()'
if ! grep -q "from _shim import install" "$CHECKOUT/conftest.py"; then
    {
        echo ""
        echo "$INSTALL_STANZA"
    } >>"$CHECKOUT/conftest.py"
fi

(
    cd "$CHECKOUT"
    PYTHONPATH="$HERE/../dataclass-compat" "$VENV/bin/python" -m pytest tests/ -q
)

if [[ "$KEEP_VENV" -eq 0 ]]; then
    rm -rf "$VENV"
fi
