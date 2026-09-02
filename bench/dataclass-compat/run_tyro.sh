#!/usr/bin/env bash
set -euo pipefail

TYRO_SHA=d0c9877f6a4a6d1a63894799ddb3daa2720cdb9e
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

WORKDIR="${WORKDIR:-$(mktemp -d)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="$WORKDIR/venv"
CHECKOUT="$WORKDIR/tyro"

if [[ ! -d "$CHECKOUT/.git" ]]; then
    git clone https://github.com/brentyi/tyro.git "$CHECKOUT"
fi
git -C "$CHECKOUT" fetch --depth 1 origin "$TYRO_SHA"
git -C "$CHECKOUT" checkout --detach "$TYRO_SHA"

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
uv pip install --python "$VENV" -r "$HERE/requirements-tyro.txt"
uv pip install --python "$VENV" -e "$CHECKOUT[dev]"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall salix==0.1.0

INSTALL_STANZA='from _shim import install

install(exclude_prefixes=("tyro",))'
if [[ -f "$CHECKOUT/conftest.py" ]]; then
    printf '\n%s\n' "$INSTALL_STANZA" >> "$CHECKOUT/conftest.py"
else
    printf '%s\n' "$INSTALL_STANZA" > "$CHECKOUT/conftest.py"
fi

(
    cd "$CHECKOUT"
    PYTHONPATH="$HERE" "$VENV/bin/python" -m pytest tests/ -q
)

if [[ "$KEEP_VENV" -eq 0 ]]; then
    rm -rf "$VENV"
fi
