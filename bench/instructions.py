from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import sys
import sysconfig
import textwrap
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
COLLECTED = re.compile(r"Collected : (\d+)")
TIMEOUT_SECONDS = 600
HEADER = ("| operation | instructions per operation |", "| --- | --: |")


def main() -> None:
    if shutil.which("valgrind") is None:
        raise SystemExit("valgrind is not on PATH")
    with ThreadPoolExecutor() as pool:
        loop, *outcomes = pool.map(measure, (EMPTY_LOOP, *OPERATIONS))
    step = per_iteration(loop) if isinstance(loop, Measured) else 0
    rows = [row(operation, outcome, step) for operation, outcome in zip(OPERATIONS, outcomes, strict=True)]
    report = "\n".join((*HEADER, *rows, f"| {EMPTY_LOOP.name}, subtracted from every row | {cell(loop, 0)} |")) + "\n"
    print(report, end="")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with Path(summary).open("a", encoding="utf-8") as file:
            file.write(f"## salix instruction counts\n\n{environment()}\n\n{report}\n")
    if not all(isinstance(outcome, Measured) and outcome.twice > outcome.once for outcome in (loop, *outcomes)):
        raise SystemExit("some operations could not be measured")


class Operation(NamedTuple):
    name: str
    setup: str
    statement: str
    iterations: int


class Measured(NamedTuple):
    once: int
    twice: int
    iterations: int


class Failed(NamedTuple):
    reason: str


POINT_CLASS = "class Point(Struct):\n    x: int\n    y: int\n    z: int"
POINT = f"from salix import Struct\n{POINT_CLASS}\npoint = Point(1, 2, 3)"
WIDE = (
    "from salix import Struct\n"
    'fields = {f"f{index}": int for index in range(300)}\n'
    'Wide = type("Wide", (Struct,), {"__annotations__": fields})'
)

EMPTY_LOOP = Operation("an empty loop step", "", "pass", 20000)

OPERATIONS = (
    Operation("create a 3-field class", "from salix import Struct", POINT_CLASS, 50),
    Operation("create a 300-field class", WIDE, 'type("Wide", (Struct,), {"__annotations__": fields})', 20),
    Operation("create a 1-field subclass of a 300-field class", WIDE, "class Sub(Wide):\n    extra: int = 0", 20),
    Operation("overwrite a free name on a 300-field class", WIDE, "Wide.free = 1", 2000),
    Operation("construct a 3-field struct", POINT, "Point(1, 2, 3)", 5000),
    Operation("salix.replace a field", f"import salix\n{POINT}", "salix.replace(point, x=5)", 2000),
    Operation("copy.copy", f"import copy\n{POINT}", "copy.copy(point)", 2000),
    Operation("copy.deepcopy", f"import copy\n{POINT}", "copy.deepcopy(point)", 2000),
    Operation("pickle round trip", f"import pickle\n{POINT}", "pickle.loads(pickle.dumps(point))", 2000),
)


def row(operation: Operation, outcome: Measured | Failed, step: float) -> str:
    return f"| {operation.name} | {cell(outcome, step)} |"


def cell(outcome: Measured | Failed, step: float) -> str:
    match outcome:
        case Failed(reason):
            return f"error: {reason}"
        case Measured(once, twice, _) if twice <= once:
            return f"n/a ({once:,} at N, {twice:,} at 2N)"
        case Measured():
            return f"{round(per_iteration(outcome) - step):,}"


def per_iteration(outcome: Measured) -> float:
    return (outcome.twice - outcome.once) / outcome.iterations


def measure(operation: Operation) -> Measured | Failed:
    once = instructions(operation, operation.iterations)
    twice = instructions(operation, 2 * operation.iterations)
    if isinstance(once, Failed):
        return once
    if isinstance(twice, Failed):
        return twice
    return Measured(once, twice, operation.iterations)


def instructions(operation: Operation, iterations: int) -> int | Failed:
    script = f"{operation.setup}\nfor _ in range({iterations}):\n{textwrap.indent(operation.statement, '    ')}\n"
    try:
        completed = subprocess.run(
            ["valgrind", "--tool=callgrind", "--callgrind-out-file=/dev/null", sys.executable, "-c", script],
            cwd=ROOT,
            env={**os.environ, "PYTHONHASHSEED": "0", "PYTHONDONTWRITEBYTECODE": "1"},
            capture_output=True,
            text=True,
            check=False,
            timeout=TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return Failed(f"timed out after {TIMEOUT_SECONDS} s")
    counted = COLLECTED.search(completed.stderr)
    if completed.returncode != 0:
        return Failed(f"exited {completed.returncode}: {(completed.stderr.strip().splitlines() or [''])[-1]}")
    if counted is None:
        return Failed("callgrind printed no 'Collected' line")
    return int(counted.group(1))


def environment() -> str:
    valgrind = subprocess.run(["valgrind", "--version"], capture_output=True, text=True, check=False).stdout.strip()
    commit = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True, check=False
    ).stdout.strip()
    compiler = os.environ.get("CC") or sysconfig.get_config_var("CC")
    return (
        f"CPython {sys.version.split()[0]}, {valgrind}, compiler `{compiler}`, "
        f"{platform.platform()}, commit `{commit}`"
    )


if __name__ == "__main__":
    main()
