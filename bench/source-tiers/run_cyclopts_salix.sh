#!/usr/bin/env bash
set -euo pipefail

FORK_SHA=05c6e71
STOCK_SHA=4edba04
PYTHON_VERSION=3.13

usage() {
    echo "usage: $0 --salix-wheel <wheel-or-dir> --mode <fork|stock> [--workdir <dir>]" >&2
    exit 2
}

SALIX_WHEEL=""
MODE=""
WORKDIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --salix-wheel) SALIX_WHEEL="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$SALIX_WHEEL" && -n "$MODE" ]] || usage
[[ "$MODE" == "fork" || "$MODE" == "stock" ]] || usage

WORKDIR="${WORKDIR:-$(mktemp -d)}"
VENV="$WORKDIR/venv"
CHECKOUT="$WORKDIR/cyclopts"

if [[ ! -d "$CHECKOUT/.git" ]]; then
    git clone https://github.com/JPHutchins/cyclopts-salix.git "$CHECKOUT"
fi
if [[ "$MODE" == "fork" ]]; then
    git -C "$CHECKOUT" fetch --depth 1 origin "$FORK_SHA" || true
    git -C "$CHECKOUT" checkout --detach "$FORK_SHA" 2>/dev/null || true
else
    git -C "$CHECKOUT" fetch --depth 1 origin "$STOCK_SHA" || true
    git -C "$CHECKOUT" checkout --detach "$STOCK_SHA" 2>/dev/null || true
fi

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
uv pip install --python "$VENV" -e "$CHECKOUT[dev]"
uv pip install --python "$VENV" docutils
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
