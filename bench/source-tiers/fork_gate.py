import io
import json
import re
import subprocess
import sys
import tokenize
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
            chain = []
            frame = sys._getframe(1)
            while frame is not None:
                module = frame.f_globals.get("__name__", "?")
                if not module.startswith(("importlib", "_frozen_importlib")) and (not chain or chain[-1] != module):
                    chain.append(module)
                frame = frame.f_back
            importers[name] = chain
        return None


sys.meta_path.insert(0, Recorder())
__import__({library!r})
if {workload!r}:
    sys.argv = [{workload!r}, *{arguments!r}]
    runpy.run_path({workload!r}, run_name="__main__")
print("FORK-GATE " + json.dumps(importers))
"""


STRING_TOKENS = frozenset(
    {
        tokenize.STRING,
        getattr(tokenize, "FSTRING_MIDDLE", tokenize.STRING),
        getattr(tokenize, "TSTRING_MIDDLE", tokenize.STRING),
    }
)


class SourceHit(NamedTuple):
    path: str
    line: int
    text: str
    in_string: bool


class Span(NamedTuple):
    start: tuple[int, int]
    end: tuple[int, int]


def string_spans(source: str) -> list[Span]:
    try:
        return [
            Span(token.start, token.end)
            for token in tokenize.generate_tokens(io.StringIO(source).readline)
            if token.type in STRING_TOKENS
        ]
    except (tokenize.TokenError, SyntaxError):
        return []


def inside_a_string(spans: list[Span], position: tuple[int, int]) -> bool:
    return any(span.start <= position < span.end for span in spans)


def file_hits(path: Path) -> list[SourceHit]:
    source = path.read_text(encoding="utf-8")
    spans = string_spans(source)
    return [
        SourceHit(str(path), number, line.strip(), inside_a_string(spans, (number, match.start())))
        for number, line in enumerate(source.splitlines(), start=1)
        for match in SOURCE_MENTION.finditer(line)
    ]


def source_hits(package: Path) -> list[SourceHit]:
    return [hit for path in sorted(package.rglob("*.py")) for hit in file_hits(path)]


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
) -> dict[str, list[str]]:
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
    loaded: dict[str, list[str]] = json.loads(line.removeprefix("FORK-GATE "))
    return loaded


def responsible_module(chain: list[str]) -> str:
    return next(
        (module for module in chain if module.split(".")[0] not in sys.stdlib_module_names),
        "?",
    )


def belongs_to(module: str, library: str) -> bool:
    return module == library or module.startswith(f"{library}.")


def main() -> int:
    python, python_path, package, pyproject, library, *rest = sys.argv[1:]
    workload, arguments = (rest[0], rest[1:]) if rest else ("", [])
    found = source_hits(Path(package))
    hits = [hit for hit in found if not hit.in_string]
    strings = [hit for hit in found if hit.in_string]
    declared = declared_replacements(Path(pyproject))
    importers = runtime_importers(python, python_path, library, workload, arguments)
    responsible = {module: responsible_module(chain) for module, chain in importers.items()}
    own = {module: chain for module, chain in importers.items() if belongs_to(responsible[module], library)}
    third_party = {module: chain for module, chain in importers.items() if module not in own}
    for hit in hits:
        print(f"SOURCE {hit.path}:{hit.line}: {hit.text}")
    for hit in strings:
        print(f"STRING {hit.path}:{hit.line}: {hit.text}")
    for requirement in declared:
        print(f"DEPENDENCY {requirement}")
    for module, chain in own.items():
        print(f"IMPORTED BY THE FORK {module} <- {' <- '.join(chain)}")
    for module, chain in third_party.items():
        print(f"IMPORTED BY THIRD PARTY {responsible[module]}: {module} <- {' <- '.join(chain)}")
    valid = not hits and not declared and not own
    print(
        f"GATE {'PASS' if valid else 'FAIL'}: {len(hits)} source hits, "
        f"{len(strings)} string literals (listed, not failing), "
        f"{len(declared)} dependencies, {len(own)} fork imports"
    )
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
