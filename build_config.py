import sys
from pathlib import Path
from typing import Final, NamedTuple

if sys.version_info >= (3, 11):
    import tomllib
else:
    import tomli as tomllib

VERSION: Final = tomllib.loads(
    Path(__file__).resolve().with_name("pyproject.toml").read_text(encoding="utf-8")
)["project"]["version"]


class BuildConfig(NamedTuple):
    sources: tuple[str, ...]
    test_sources: tuple[str, ...]
    c_flags: tuple[str, ...]


BUILD: Final = BuildConfig(
    sources=(
        "src/salix.c",
        "src/annotations.c",
        "src/compare.c",
        "src/construct/construct.c",
        "src/construct/binding.c",
        "src/construct/replace.c",
        "src/construct/defaults.c",
        "src/construct/exceptions.c",
        "src/fields/fields.c",
        "src/fields/forms.c",
        "src/hash.c",
        "src/meta/meta.c",
        "src/meta/bases.c",
        "src/meta/namespace/namespace.c",
        "src/meta/namespace/slots.c",
        "src/meta/create/create.c",
        "src/meta/create/handoff.c",
        "src/meta/install/install.c",
        "src/meta/install/init_owner.c",
        "src/meta/settle/settle.c",
        "src/meta/settle/mro.c",
        "src/meta/settle/comparison.c",
        "src/meta/settle/restore.c",
        "src/mixin.c",
        "src/mixin/copy.c",
        "src/mixin/deepcopy.c",
        "src/options.c",
        "src/repr.c",
    ),
    test_sources=(
        "src/testing.c",
        "tests/c/main.c",
    ),
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

STRICT: Final = ("-Werror",)

SHIPPED: Final = ("-g0",)


if __name__ == "__main__":
    import sys

    match sys.argv[1:]:
        case ["sources"]:
            print("\n".join(BUILD.sources))
        case ["test-sources"]:
            print("\n".join(BUILD.test_sources))
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
                "usage: build_config.py {sources|test-sources|c-flags [--strict [--shipped]]}"
            )
