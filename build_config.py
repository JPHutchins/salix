import re
from pathlib import Path
from typing import Final, NamedTuple

_version_match = re.search(
    r'^version\s*=\s*["\']([^"\']+)["\']',
    Path(__file__).resolve().with_name("pyproject.toml").read_text(),
    re.MULTILINE,
)

if _version_match is None:
    raise SystemExit("build_config.py: no version found in pyproject.toml")

VERSION: Final = _version_match.group(1)


class BuildConfig(NamedTuple):
    sources: tuple[str, ...]
    c_flags: tuple[str, ...]


BUILD: Final = BuildConfig(
    sources=(
        "src/salix.c",
        "src/annotations.c",
        "src/compare.c",
        "src/construct/construct.c",
        "src/construct/binding.c",
        "src/construct/defaults.c",
        "src/fields.c",
        "src/hash.c",
        "src/meta/meta.c",
        "src/meta/bases.c",
        "src/meta/namespace.c",
        "src/meta/class/create.c",
        "src/meta/class/install.c",
        "src/meta/class/settle.c",
        "src/mixin.c",
        "src/options.c",
        "src/repr.c",
    ),
    # -Wno-unused-parameter: CPython slot signatures are fixed by the API and
    # routinely ignore an argument.
    # -std=c2x, not -std=c23: they select the same language mode -- both give
    # __STDC_VERSION__ 202311 -- but c2x is accepted by GCC 9+ and Clang 9+
    # while c23 needs GCC 14+ or Clang 18+. An sdist has to compile on whatever
    # the machine has, and Ubuntu 24.04 LTS still ships GCC 13.
    c_flags=(
        f"-DSALIX_VERSION={VERSION}",
        "-std=c2x",
        "-O2",
        "-Wdouble-promotion",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
    ),
)

# setup.py passes these after CFLAGS, so an sdist build cannot override them.
# One new warning in a future compiler would then turn `pip install salix` into
# a failure, on the machines that have no wheel and must compile.
STRICT: Final = ("-Werror",)

# 84% of a published payload was DWARF that nothing on the far end reads. Not
# in c_flags, so a local build stays debuggable.
SHIPPED: Final = ("-g0",)


if __name__ == "__main__":
    import sys

    match sys.argv[1:]:
        case ["sources"]:
            print("\n".join(BUILD.sources))
        case ["c-flags"]:
            print("\n".join(BUILD.c_flags))
        case ["c-flags", "--strict"]:
            print("\n".join(BUILD.c_flags + STRICT))
        case ["c-flags", "--strict", "--shipped"]:
            print("\n".join(BUILD.c_flags + STRICT + SHIPPED))
        case ["c-flags", "--shipped", *_]:
            raise SystemExit("build_config.py: --shipped only follows --strict")
        case _:
            raise SystemExit(
                "usage: build_config.py {sources|c-flags [--strict [--shipped]]}"
            )
