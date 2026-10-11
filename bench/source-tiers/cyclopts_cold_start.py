import importlib
import io
import sys
import time

started = time.perf_counter()
cyclopts = importlib.import_module("cyclopts")
imported = time.perf_counter()
module = importlib.import_module(sys.argv[1])
defined = time.perf_counter()
result = module.app(
    [
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
    ],
    exit_on_error=False,
    print_error=False,
    result_action="return_value",
)
dispatched = time.perf_counter()
buffer = io.StringIO()
module.app.help_print(
    ["cmd23"],
    console=importlib.import_module("rich.console").Console(
        file=buffer, width=100, force_terminal=False
    ),
)
rendered = time.perf_counter()

assert result == ("alpha", 7), result
assert "Command number 23" in buffer.getvalue()
print(
    f"import cyclopts {1e3 * (imported - started):.3f} ms\n"
    f"define app {1e3 * (defined - imported):.3f} ms\n"
    f"parse + dispatch {1e3 * (dispatched - defined):.3f} ms\n"
    f"render --help {1e3 * (rendered - dispatched):.3f} ms\n"
    f"in-process total {1e3 * (rendered - started):.3f} ms"
)
