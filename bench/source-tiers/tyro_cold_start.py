import importlib
import sys
import time

CHECKED_SECTIONS = range(0, 20)

ARGV = [
    argument
    for section in CHECKED_SECTIONS
    for argument in (
        f"--section{section:02d}.count", str(100 + section),
        f"--section{section:02d}.mode", "fast",
        f"--section{section:02d}.sizes", str(section), "7",
        f"--section{section:02d}.no-enabled",
        f"--section{section:02d}.name", f"name{section}",
    )
]

started = time.perf_counter()
tyro = importlib.import_module("tyro")
imported = time.perf_counter()
schema = importlib.import_module(sys.argv[1])
defined = time.perf_counter()
instance = tyro.cli(schema.Root, args=ARGV)
parsed = time.perf_counter()
second = tyro.cli(schema.Root, args=ARGV)
reparsed = time.perf_counter()

assert type(instance).__name__ == "Root"
assert instance.section19.count == 119
assert instance.section03.mode == "fast"
assert instance.section07.sizes == [7, 7]
assert instance.section11.enabled is False
assert instance.section05.name == "name5"
assert instance.section05.ratio == 0.5
assert second.section19.count == 119
print(
    f"import tyro {1e3 * (imported - started):.3f} ms\n"
    f"define schema {1e3 * (defined - imported):.3f} ms\n"
    f"tyro.cli first {1e3 * (parsed - defined):.3f} ms\n"
    f"tyro.cli second {1e3 * (reparsed - parsed):.3f} ms\n"
    f"in-process total {1e3 * (parsed - started):.3f} ms"
)
