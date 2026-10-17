from __future__ import annotations

import argparse
import json
import re
import subprocess
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, NamedTuple

RELEASES = "https://api.github.com/repos/astral-sh/python-build-standalone/releases"
ASSET = re.compile(
    r"^cpython-(?P<version>\d+\.\d+(?:\.\w+)?)\+\d+-(?P<triple>.+?)"
    r"(?P<variant>-freethreaded)?-install_only_stripped\.tar\.gz$"
)


class Target(NamedTuple):
    """An interpreter to build against: a declared minor, or its free-threaded twin.

    `name` is what nix keys on and what nodot turns into the ABI tag, so the
    trailing "t" of a free-threaded target carries itself all the way to
    cp314t and to cpython-314t in the payload filename.
    """

    name: str
    minor: str
    variant: str


class Platform(NamedTuple):
    triple: str
    zig_target: str
    platform_tag: str
    module_name: str
    extra_flags: tuple[str, ...]
    link_python_library: bool


UNBUILDABLE: frozenset[tuple[str, str]] = frozenset({("-freethreaded", "windows-aarch64")})

PLATFORMS: dict[str, Platform] = {
    "manylinux-x86_64": Platform(
        "x86_64-unknown-linux-gnu",
        "x86_64-linux-gnu.2.17",
        "manylinux_2_17_x86_64",
        "__init__.cpython-{nodot}-x86_64-linux-gnu.so",
        (),
        False,
    ),
    "manylinux-aarch64": Platform(
        "aarch64-unknown-linux-gnu",
        "aarch64-linux-gnu.2.17",
        "manylinux_2_17_aarch64",
        "__init__.cpython-{nodot}-aarch64-linux-gnu.so",
        (),
        False,
    ),
    "macos-x86_64": Platform(
        "x86_64-apple-darwin",
        "x86_64-macos.10.13",
        "macosx_10_13_x86_64",
        "__init__.cpython-{nodot}-darwin.so",
        ("-undefined", "dynamic_lookup"),
        False,
    ),
    "macos-aarch64": Platform(
        "aarch64-apple-darwin",
        "aarch64-macos.11.0",
        "macosx_11_0_arm64",
        "__init__.cpython-{nodot}-darwin.so",
        ("-undefined", "dynamic_lookup"),
        False,
    ),
    "windows-x86_64": Platform(
        "x86_64-pc-windows-msvc",
        "x86_64-windows-gnu",
        "win_amd64",
        "__init__.pyd",
        (),
        True,
    ),
    "windows-aarch64": Platform(
        "aarch64-pc-windows-msvc",
        "aarch64-windows-gnu",
        "win_arm64",
        "__init__.pyd",
        (),
        True,
    ),
}


def fetch(url: str) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"User-Agent": "salix-update-targets"})

    with urllib.request.urlopen(request) as response:
        loaded: dict[str, Any] = json.load(response)

    return loaded


def to_sri(digest: str) -> str:
    return subprocess.run(
        ["nix", "hash", "convert", "--to", "sri", "--hash-algo", "sha256", digest],
        capture_output=True, text=True, check=True,
    ).stdout.strip()


def minor_of(version: str) -> str:
    return ".".join(version.split(".")[:2])


def nodot_of(minor: str) -> str:
    return minor.replace(".", "")


def platform_entry(platform: Platform) -> dict[str, object]:
    return {
        "pbsTriple": platform.triple,
        "zigTarget": platform.zig_target,
        "platformTag": platform.platform_tag,
        "moduleName": platform.module_name,
        "extraFlags": list(platform.extra_flags),
        "linkPythonLibrary": platform.link_python_library,
    }


def python_entry(target: Target, version: str, hashes: dict[str, str]) -> dict[str, object]:
    return {
        "version": version,
        "tag": f"cp{nodot_of(target.minor)}",
        "abiTag": f"cp{nodot_of(target.name)}",
        "pbsVariant": target.variant,
        "hashes": hashes,
    }


def recorded_release(output: Path) -> str | None:
    if not output.exists():
        return None

    recorded = json.loads(output.read_text()).get("release")

    if not isinstance(recorded, str):
        raise SystemExit(f"{output} records no release; pass --release")

    return recorded


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Regenerate nix/python-targets.json from a python-build-standalone release. "
            "Interpreters come from .python-version; the GitHub API serves each asset's "
            "digest, so nothing is downloaded to pin it."
        )
    )
    parser.add_argument(
        "--release",
        default=None,
        help="a python-build-standalone tag, or latest; defaults to the release the output records",
    )
    parser.add_argument("--python-version", type=Path, default=Path(".python-version"))
    parser.add_argument("--output", type=Path, default=Path("nix/python-targets.json"))
    arguments = parser.parse_args()

    wanted = arguments.python_version.read_text().split()

    if not wanted:
        raise SystemExit(f"{arguments.python_version} lists no interpreters")

    tag = arguments.release or recorded_release(arguments.output) or "latest"

    try:
        release = fetch(f"{RELEASES}/latest" if tag == "latest" else f"{RELEASES}/tags/{tag}")
    except urllib.error.HTTPError as error:
        raise SystemExit(f"python-build-standalone release {tag}: {error}") from error

    assets = {}
    for asset in release["assets"]:
        matched = ASSET.match(asset["name"])

        if matched is not None:
            assets[(minor_of(matched["version"]), matched["triple"], matched["variant"] or "")] = (
                matched["version"],
                asset["digest"].removeprefix("sha256:"),
            )

    targets = [
        target
        for minor in wanted
        for target in (Target(minor, minor, ""), Target(f"{minor}t", minor, "-freethreaded"))
        if any((minor, platform.triple, target.variant) in assets for platform in PLATFORMS.values())
    ]

    available = {
        target: {
            name: platform
            for name, platform in PLATFORMS.items()
            if (target.minor, platform.triple, target.variant) in assets
            and (target.variant, name) not in UNBUILDABLE
        }
        for target in targets
    }

    for minor in wanted:
        if not any(target.minor == minor for target in targets):
            raise SystemExit(f"release {release['tag_name']} has no assets at all for {minor}")

    for target, platforms in available.items():
        for name in PLATFORMS.keys() - platforms.keys():
            reason = (
                "cannot be cross-compiled"
                if (target.variant, name) in UNBUILDABLE
                else f"not published in {release['tag_name']}"
            )
            print(f"skipping {target.name}/{name}: {reason}")

    targets_document = {
        "release": release["tag_name"],
        "platforms": {name: platform_entry(platform) for name, platform in PLATFORMS.items()},
        "pythons": {
            target.name: python_entry(
                target,
                assets[(target.minor, next(iter(platforms.values())).triple, target.variant)][0],
                {
                    name: to_sri(assets[(target.minor, platform.triple, target.variant)][1])
                    for name, platform in platforms.items()
                },
            )
            for target, platforms in available.items()
        },
    }
    arguments.output.write_text(json.dumps(targets_document, indent=2) + "\n")

    wheels = sum(len(platforms) for platforms in available.values())
    print(f"{arguments.output}: {len(available)} interpreters, {wheels} wheels")


if __name__ == "__main__":
    main()
