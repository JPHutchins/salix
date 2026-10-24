import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import tomllib

REPORT = (
    "import importlib.metadata as metadata, json, sys;"
    " print(json.dumps({'prefix': sys.prefix, 'versions': {name: metadata.version(name) for name in sys.argv[1:]}}))"
)
POISONED_VERSION = "0.0.0"


def requirement_name(requirement: str) -> str:
    return re.split(r"[\s;\[<>=!~]", requirement, maxsplit=1)[0].lower()


def locked_versions(group: str) -> dict[str, str]:
    requirements = tomllib.loads(Path("pyproject.toml").read_text(encoding="utf-8"))["dependency-groups"][group]
    packages = tomllib.loads(Path("uv.lock").read_text(encoding="utf-8"))["package"]
    versions = {package["name"]: package["version"] for package in packages if "version" in package}

    return {
        requirement_name(requirement): versions[requirement_name(requirement)]
        for requirement in requirements
        if isinstance(requirement, str)
    }


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
        + json.dumps({"prefix": str(root), "versions": dict.fromkeys(names, POISONED_VERSION)})
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


def main(group: str, runner: list[str]) -> int:
    expected = locked_versions(group)
    names = sorted(expected)

    with tempfile.TemporaryDirectory() as root:
        completed = subprocess.run(
            [*runner, "python", "-c", REPORT, *names],
            env=poisoned_caller(Path(root), names),
            capture_output=True,
            text=True,
            check=False,
        )

        if completed.returncode != 0:
            print(completed.stdout, completed.stderr, sep="\n", file=sys.stderr)

            return completed.returncode

        report = json.loads(completed.stdout.strip().splitlines()[-1])
        caller_environment = Path(report["prefix"]).resolve().is_relative_to(Path(root).resolve())

    versions = report["versions"]
    drifted = {name: (versions.get(name), version) for name, version in expected.items() if versions.get(name) != version}

    if caller_environment:
        print(f"{group}: ran in the caller's environment {report['prefix']}, not an isolated one", file=sys.stderr)

    for name, (got, locked) in sorted(drifted.items()):
        print(f"{group}: {name} ran as {got}, uv.lock pins {locked}", file=sys.stderr)

    return 1 if drifted or caller_environment else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1], sys.argv[2:]))
