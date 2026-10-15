import importlib
import sys
import time
from collections.abc import Callable

schema = importlib.import_module(sys.argv[1])
tyro = importlib.import_module("tyro")

ARGV = [
    "--section00.count", "5", "--section01.mode", "fast", "--section02.sizes", "4", "5",
]


def per_iteration(label: str, iterations: int, function: Callable[[], object]) -> None:
    function()
    started = time.perf_counter()
    for _ in range(iterations):
        function()
    print(f"{label} {1e6 * (time.perf_counter() - started) / iterations:.3f} us/iter")


per_iteration("cli big warm", 20, lambda: tyro.cli(schema.Root, args=ARGV))
per_iteration("cli small", 200, lambda: tyro.cli(schema.Section00, args=["--count", "5"]))
per_iteration("cli small defaults", 200, lambda: tyro.cli(schema.Section00, args=[]))
per_iteration("construct Section00", 20000, schema.Section00)
per_iteration("construct Root", 2000, schema.Root)
