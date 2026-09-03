#!/usr/bin/env bash
set -euo pipefail

FORK_SHA=4b0ab3cc6d3d1de9b75400d72d28bab360d6ced3
STOCK_SHA=d0c9877f6a4a6d1a63894799ddb3daa2720cdb9e
PYTHON_VERSION=3.13

usage() {
    echo "usage: $0 --salix-wheel <wheel-or-dir> --mode <fork|stock> [--workdir <dir>] [--keep-venv]" >&2
    exit 2
}

SALIX_WHEEL=""
MODE=""
WORKDIR=""
KEEP_VENV=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --salix-wheel) SALIX_WHEEL="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        --keep-venv) KEEP_VENV=1; shift ;;
        *) usage ;;
    esac
done
[[ -n "$SALIX_WHEEL" && -n "$MODE" ]] || usage
[[ "$MODE" == "fork" || "$MODE" == "stock" ]] || usage
[[ -e "$SALIX_WHEEL" ]] || { echo "salix wheel not found: $SALIX_WHEEL" >&2; exit 1; }

WORKDIR="${WORKDIR:-$(mktemp -d)}"
VENV="$WORKDIR/venv"
CHECKOUT="$WORKDIR/tyro"

if [[ -d "$CHECKOUT" && ! -d "$CHECKOUT/.git" ]]; then
    rm -rf "$CHECKOUT"
fi
if [[ ! -d "$CHECKOUT/.git" ]]; then
    git clone https://github.com/JPHutchins/tyro-salix.git "$CHECKOUT"
fi
PIN="$FORK_SHA"
if [[ "$MODE" == "stock" ]]; then
    PIN="$STOCK_SHA"
fi
git -C "$CHECKOUT" fetch origin "$PIN"
git -C "$CHECKOUT" checkout --detach "$PIN"
[[ "$(git -C "$CHECKOUT" rev-parse HEAD)" == "$PIN" ]]

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
uv pip install --python "$VENV" -e "$CHECKOUT[dev]"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall salix==0.1.0

(
    cd "$CHECKOUT"
    "$VENV/bin/python" -m pytest tests/ -q
)

if [[ "$KEEP_VENV" -eq 0 ]]; then
    rm -rf "$VENV"
fi
