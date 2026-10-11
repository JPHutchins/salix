import json
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

import tomllib

REPLACED_MODULES = ("attr", "attrs", "dataclasses")
SOURCE_MENTION = re.compile(
    r"^\s*(?:import|from)\s+(?:attrs?|dataclasses)\b"
    r"|\b(?:attrs?|dataclasses)\.\w"
    r"|\bNamedTuple\b"
)
REQUIREMENT_NAME = re.compile(r"^\s*([A-Za-z0-9_.-]+)")
PROBE = """
import importlib.abc
import json
import runpy
import sys

replaced = {replaced!r}
importers = {{}}


class Recorder(importlib.abc.MetaPathFinder):
    def find_spec(self, name, path, target=None):
        if name in replaced and name not in importers:
            frame = sys._getframe(1)
            while frame is not None and frame.f_globals.get("__name__", "").startswith(("importlib", "_frozen_importlib")):
                frame = frame.f_back
            importers[name] = frame.f_globals.get("__name__", "?") if frame is not None else "?"
        return None


sys.meta_path.insert(0, Recorder())
__import__({library!r})
if {workload!r}:
    sys.argv = [{workload!r}, *{arguments!r}]
    runpy.run_path({workload!r}, run_name="__main__")
print("FORK-GATE " + json.dumps(importers))
"""


class SourceHit(NamedTuple):
    path: str
    line: int
    text: str


def source_hits(package: Path) -> list[SourceHit]:
    return [
        SourceHit(str(path), number, line.strip())
        for path in sorted(package.rglob("*.py"))
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1)
        if SOURCE_MENTION.search(line)
    ]


def declared_replacements(pyproject: Path) -> list[str]:
    project = tomllib.loads(pyproject.read_text(encoding="utf-8")).get("project", {})
    return [
        requirement
        for requirement in project.get("dependencies", [])
        if (match := REQUIREMENT_NAME.match(requirement)) is not None
        and match.group(1).lower() in REPLACED_MODULES
    ]


def runtime_importers(
    python: str, python_path: str, library: str, workload: str, arguments: list[str]
) -> dict[str, str]:
    probe = PROBE.format(
        replaced=REPLACED_MODULES, library=library, workload=workload, arguments=arguments
    )
    completed = subprocess.run(
        [python, "-c", probe],
        env={"PYTHONPATH": python_path, "PATH": "/usr/bin:/bin"},
        capture_output=True,
        text=True,
        check=True,
    )
    line = next(line for line in completed.stdout.splitlines() if line.startswith("FORK-GATE "))
    loaded: dict[str, str] = json.loads(line.removeprefix("FORK-GATE "))
    return loaded


def main() -> int:
    python, python_path, package, pyproject, library, *rest = sys.argv[1:]
    workload, arguments = (rest[0], rest[1:]) if rest else ("", [])
    hits = source_hits(Path(package))
    declared = declared_replacements(Path(pyproject))
    importers = runtime_importers(python, python_path, library, workload, arguments)
    own = {
        module: importer
        for module, importer in importers.items()
        if importer == library or importer.startswith(f"{library}.")
    }
    third_party = {module: importer for module, importer in importers.items() if module not in own}
    for hit in hits:
        print(f"SOURCE {hit.path}:{hit.line}: {hit.text}")
    for requirement in declared:
        print(f"DEPENDENCY {requirement}")
    for module, importer in own.items():
        print(f"IMPORTED BY THE FORK {module} <- {importer}")
    for module, importer in third_party.items():
        print(f"IMPORTED BY A THIRD PARTY {module} <- {importer}")
    valid = not hits and not declared and not own
    print(f"GATE {'PASS' if valid else 'FAIL'}: {len(hits)} source hits, {len(declared)} dependencies, {len(own)} fork imports")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
