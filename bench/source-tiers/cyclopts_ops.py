import importlib
import io
import sys
import time
from collections.abc import Callable

module = importlib.import_module(sys.argv[1])
iterations = int(sys.argv[2])
buffer = io.StringIO()
console = importlib.import_module("rich.console").Console(file=buffer, width=100)
argv = [
    "cmd17",
    "--name",
    "alpha",
    "--count",
    "7",
    "--ratio",
    "0.25",
    "--mode",
    "safe",
    "--tags",
    "a",
    "--tags",
    "b",
]


def microseconds_per_iteration(operation: Callable[[], object], iterations: int) -> float:
    operation()
    started = time.perf_counter()
    for _ in range(iterations):
        operation()
    return 1e6 * (time.perf_counter() - started) / iterations


def dispatch() -> object:
    return module.app(
        argv, exit_on_error=False, print_error=False, result_action="return_value"
    )


def render_help() -> None:
    buffer.seek(0)
    buffer.truncate()
    module.app.help_print(["cmd23"], console=console)


assert dispatch() == ("alpha", 7)
dispatch_cost = microseconds_per_iteration(dispatch, iterations)
help_cost = microseconds_per_iteration(render_help, max(iterations // 10, 5))
print(f"parse + dispatch {dispatch_cost:.1f} us/iter")
print(f"render --help {help_cost:.1f} us/iter")
