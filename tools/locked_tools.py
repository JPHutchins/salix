import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from itertools import takewhile
from pathlib import Path
from typing import Literal, NotRequired, TypedDict

import tomllib
from packaging.markers import Marker

REPORT = """
import importlib.metadata as metadata, json, platform, sys

implementation = sys.implementation.version
print(json.dumps({
    "prefix": sys.prefix,
    "environment": {
        "implementation_name": sys.implementation.name,
        "implementation_version": f"{implementation.major}.{implementation.minor}.{implementation.micro}"
        + ("" if implementation.releaselevel == "final" else implementation.releaselevel[0] + str(implementation.serial)),
        "platform_python_implementation": platform.python_implementation(),
        "python_full_version": platform.python_version(),
        "python_version": ".".join(platform.python_version_tuple()[:2]),
    },
    "versions": {
        name: next((distribution.version for distribution in metadata.distributions(name=name)), None)
        for name in sys.argv[1:]
    },
}))
"""
POISONED_VERSION = "0.0.0"
ASSIGNMENT = re.compile(r"[A-Z_][A-Z0-9_]*=.*")
SourceFlag = Literal["--only-group", "--package"]
SOURCE_FLAGS: tuple[SourceFlag, ...] = ("--only-group", "--package")


class Dependency(TypedDict):
    name: str
    version: NotRequired[str]
    marker: NotRequired[str]


Package = TypedDict(
    "Package",
    {
        "name": str,
        "version": str,
        "source": dict[str, str],
        "dependencies": NotRequired[list[Dependency]],
        "dev-dependencies": NotRequired[dict[str, list[Dependency]]],
    },
)


def main(arguments: list[str]) -> int:
    assignments = list(takewhile(ASSIGNMENT.fullmatch, arguments))
    runner = arguments[len(assignments) :]
    flag, source = runner_source(runner)
    packages: list[Package] = tomllib.loads(Path("uv.lock").read_text(encoding="utf-8"))["package"]
    dependencies = source_dependencies(packages, flag, source)
    names = sorted({dependency["name"] for dependency in dependencies})

    with tempfile.TemporaryDirectory() as root:
        completed = subprocess.run(
            [*runner, "python", "-c", REPORT, *names],
            env={
                **poisoned_caller(Path(root), names),
                **{name: value for name, _, value in (assignment.partition("=") for assignment in assignments)},
            },
            capture_output=True,
            text=True,
            check=False,
        )

        if completed.returncode != 0:
            print(completed.stdout, completed.stderr, sep="\n", file=sys.stderr)

            return completed.returncode

        report = json.loads(completed.stdout.strip().splitlines()[-1])
        caller_environment = Path(report["prefix"]).resolve().is_relative_to(Path(root).resolve())

    expected = {
        dependency["name"]: locked_version(packages, dependency)
        for dependency in dependencies
        if "marker" not in dependency or Marker(dependency["marker"]).evaluate(report["environment"])
    }
    versions = report["versions"]
    drifted = {name: (versions.get(name), version) for name, version in expected.items() if versions.get(name) != version}

    if caller_environment:
        print(f"{source}: ran in the caller's environment {report['prefix']}, not an isolated one", file=sys.stderr)

    for name, (got, locked) in sorted(drifted.items()):
        print(f"{source}: {name} ran as {got}, uv.lock pins {locked}", file=sys.stderr)

    return 1 if drifted or caller_environment else 0


def runner_source(runner: list[str]) -> tuple[SourceFlag, str]:
    match [(flag, runner[runner.index(flag) + 1]) for flag in SOURCE_FLAGS if flag in runner]:
        case [source]:
            return source
        case sources:
            raise SystemExit(
                f"{shlex.join(runner)} names {len(sources)} sources; it needs exactly one of {', '.join(SOURCE_FLAGS)}"
            )


def source_dependencies(packages: list[Package], flag: SourceFlag, source: str) -> list[Dependency]:
    match flag:
        case "--only-group":
            return next(package for package in packages if package["source"] == {"virtual": "."})[
                "dev-dependencies"
            ][source]
        case "--package":
            return next(package for package in packages if package["name"] == source)["dependencies"]


def locked_version(packages: list[Package], dependency: Dependency) -> str:
    match dependency:
        case {"version": version}:
            return version
        case _:
            match sorted({package["version"] for package in packages if package["name"] == dependency["name"]}):
                case [version]:
                    return version
                case versions:
                    raise SystemExit(
                        f"uv.lock holds {', '.join(versions)} for {dependency['name']}, and the dependency names none"
                    )


def poisoned_caller(root: Path, names: list[str]) -> dict[str, str]:
    site = root / "site"
    executables = root / "bin"
    executables.mkdir(parents=True)

    for name in names:
        dist_info = site / f"{name.replace('-', '_')}-{POISONED_VERSION}.dist-info"
        dist_info.mkdir(parents=True)
        (dist_info / "METADATA").write_text(
            f"Metadata-Version: 2.1\nName: {name}\nVersion: {POISONED_VERSION}\n", encoding="utf-8"
        )

    fake_python = executables / "python"
    fake_python.write_text(
        "#!/bin/sh\necho '"
        + json.dumps({"prefix": str(root), "environment": {}, "versions": dict.fromkeys(names, POISONED_VERSION)})
        + "'\n",
        encoding="utf-8",
    )
    fake_python.chmod(0o755)

    return {
        **os.environ,
        "PYTHONPATH": str(site),
        "PATH": f"{executables}{os.pathsep}{os.environ['PATH']}",
        "VIRTUAL_ENV": str(root),
        "UV_PROJECT_ENVIRONMENT": str(root / "project-environment"),
    }


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
