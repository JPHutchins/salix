#!/usr/bin/env bash
set -euo pipefail

PYTHON_VERSION=3.13
SALIX_VERSION=0.1.0

usage() {
    echo "usage: $0 --salix-wheel <wheel-or-dir> [--workdir <dir>] [--keep-venv]" >&2
    exit 2
}

SALIX_WHEEL=""
WORKDIR=""
KEEP_VENV=0
OWNED_WORKDIR=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --salix-wheel) [[ $# -ge 2 && $2 != -* ]] || usage; SALIX_WHEEL="$2"; shift 2 ;;
        --workdir) [[ $# -ge 2 && $2 != -* ]] || usage; WORKDIR="$2"; shift 2 ;;
        --keep-venv) KEEP_VENV=1; shift ;;
        *) usage ;;
    esac
done
[[ -n "$SALIX_WHEEL" ]] || usage
[[ -e "$SALIX_WHEEL" ]] || { echo "salix wheel not found: $SALIX_WHEEL" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
CHECKOUT="$HERE/vendor/tyro-salix"
[[ -e "$CHECKOUT/.git" ]] || { echo "submodule not initialized: $CHECKOUT" >&2; exit 1; }

PIN_SHA="$(git -C "$HERE" ls-tree HEAD vendor/tyro-salix | awk '{print $3}')"
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
uv pip install --python "$VENV" -r "$HERE/../dataclass-compat/requirements-tyro.txt"
uv pip install --python "$VENV" -e "$CHECKOUT[dev]"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall "salix==$SALIX_VERSION"

(
    cd "$CHECKOUT"
    "$VENV/bin/python" -m pytest -p no:cacheprovider tests/ -q
)
