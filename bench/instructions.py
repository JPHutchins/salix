from __future__ import annotations

import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
COLLECTED = re.compile(r"Collected : (\d+)")
HEADER = ("| operation | instructions per operation |", "| --- | --: |")


def main() -> None:
    report = "\n".join((*HEADER, *(row(operation) for operation in OPERATIONS))) + "\n"
    print(report, end="")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with Path(summary).open("a", encoding="utf-8") as file:
            file.write(f"## salix instruction counts, CPython {sys.version.split()[0]}\n\n{report}\n")


class Operation(NamedTuple):
    name: str
    setup: str
    statement: str
    iterations: int


POINT = "from salix import Struct\nclass Point(Struct):\n    x: int\n    y: int\n    z: int\npoint = Point(1, 2, 3)"
WIDE = (
    "from salix import Struct\n"
    'fields = {f"f{index}": int for index in range(300)}\n'
    'Wide = type("Wide", (Struct,), {"__annotations__": fields})'
)

OPERATIONS = (
    Operation(
        "create a 3-field class",
        "from salix import Struct",
        "class Point(Struct):\n    x: int\n    y: int\n    z: int",
        50,
    ),
    Operation("create a 300-field class", WIDE, 'type("Wide", (Struct,), {"__annotations__": fields})', 4),
    Operation("create a 1-field subclass of a 300-field class", WIDE, "class Sub(Wide):\n    extra: int = 0", 4),
    Operation("write a free name on a 300-field class", WIDE, "Wide.free = 1", 2000),
    Operation("construct a 3-field struct", POINT, "Point(1, 2, 3)", 5000),
    Operation("salix.replace a field", f"import salix\n{POINT}", "salix.replace(point, x=5)", 2000),
    Operation("copy.copy", f"import copy\n{POINT}", "copy.copy(point)", 2000),
    Operation("copy.deepcopy", f"import copy\n{POINT}", "copy.deepcopy(point)", 2000),
    Operation("pickle round trip", f"import pickle\n{POINT}", "pickle.loads(pickle.dumps(point))", 2000),
)


def row(operation: Operation) -> str:
    return f"| {operation.name} | {per_operation(operation):,} |"


def per_operation(operation: Operation) -> int:
    once = instructions(operation, operation.iterations)
    twice = instructions(operation, 2 * operation.iterations)
    return round((twice - once) / operation.iterations)


def instructions(operation: Operation, iterations: int) -> int:
    script = f"{operation.setup}\nfor _ in range({iterations}):\n{textwrap.indent(operation.statement, '    ')}\n"
    completed = subprocess.run(
        ["valgrind", "--tool=callgrind", "--callgrind-out-file=/dev/null", sys.executable, "-c", script],
        cwd=ROOT,
        env={**os.environ, "PYTHONHASHSEED": "0"},
        capture_output=True,
        text=True,
        check=False,
    )
    counted = COLLECTED.search(completed.stderr)
    if completed.returncode != 0 or counted is None:
        raise SystemExit(f"{operation.name} exited {completed.returncode} under callgrind:\n{completed.stderr}")
    return int(counted.group(1))


if __name__ == "__main__":
    main()
