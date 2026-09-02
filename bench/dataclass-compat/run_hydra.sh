#!/usr/bin/env bash
set -euo pipefail

HYDRA_SHA=d1e07c8f68144ad481aa2a773e6a1f4daa7e5808
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
CHECKOUT="$WORKDIR/hydra"

if [[ ! -d "$CHECKOUT/.git" ]]; then
    git clone https://github.com/facebookresearch/hydra.git "$CHECKOUT"
fi
git -C "$CHECKOUT" fetch --depth 1 origin "$HYDRA_SHA"
git -C "$CHECKOUT" checkout --detach "$HYDRA_SHA"

if [[ ! -d "$VENV" ]]; then
    uv venv --python "$PYTHON_VERSION" "$VENV"
fi
uv pip install --python "$VENV" -r "$HERE/requirements-hydra.txt"
uv pip install --python "$VENV" -e "$CHECKOUT"
uv pip install --python "$VENV" -r "$CHECKOUT/requirements/dev.txt"
if [[ -d "$SALIX_WHEEL" ]]; then
    WHEEL_LINKS="$SALIX_WHEEL"
else
    WHEEL_LINKS="$(dirname "$SALIX_WHEEL")"
fi
uv pip install --python "$VENV" --no-index --find-links "$WHEEL_LINKS" --reinstall salix==0.1.0

if command -v java >/dev/null 2>&1; then
    ( cd "$CHECKOUT" && "$VENV/bin/python" setup.py antlr )
else
    ( cd "$CHECKOUT" && nix shell nixpkgs#jdk17 --command "$VENV/bin/python" setup.py antlr )
fi

INSTALL_STANZA='from _shim import install

install()'
git -C "$CHECKOUT" checkout -- conftest.py 2>/dev/null || true
if [[ -f "$CHECKOUT/conftest.py" ]]; then
    if ! grep -q "from _shim import install" "$CHECKOUT/conftest.py"; then
        printf '\n%s\n' "$INSTALL_STANZA" >> "$CHECKOUT/conftest.py"
    fi
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
