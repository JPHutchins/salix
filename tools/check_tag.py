from __future__ import annotations

import re
import sys
from pathlib import Path

import tomllib

CANONICAL_VERSION = re.compile(
    r"([1-9][0-9]*!)?(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*))*"
    r"((a|b|rc)(0|[1-9][0-9]*))?(\.post(0|[1-9][0-9]*))?(\.dev(0|[1-9][0-9]*))?"
)


def declared_version(pyproject: Path) -> str:
    version = tomllib.loads(pyproject.read_text())["project"]["version"]

    if not isinstance(version, str):
        raise SystemExit(
            f"pyproject.toml declares a {type(version).__name__} version; it must be a string"
        )

    return version


def main() -> None:
    match sys.argv[1:]:
        case [tag]:
            pass
        case _:
            raise SystemExit("usage: check_tag.py TAG")

    declared = declared_version(Path("pyproject.toml"))

    if CANONICAL_VERSION.fullmatch(declared) is None:
        raise SystemExit(
            f"pyproject.toml declares {declared}, which is not a canonical PEP 440 "
            f"version, so PyPI would publish it under a different string than the "
            f"tag names. Write the normalised form: 1.0.0rc1, not 1.0.0-rc1."
        )

    if tag != f"v{declared}":
        raise SystemExit(
            f"tag {tag} names {tag.removeprefix('v')}, but pyproject.toml declares {declared}"
        )

    print(f"{tag} matches the declared version {declared}")


if __name__ == "__main__":
    main()
