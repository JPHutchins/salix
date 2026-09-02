import statistics
import subprocess

PY = "/home/jp/repos/transformers-salix-venv/bin/python"
SHIM_DIR = "/home/jp/repos/jp-struct/bench/dataclass-compat"

def self_time(prologue: str, module: str) -> int:
    out = subprocess.run(
        [PY, "-X", "importtime", "-c", f"{prologue} import {module}"],
        capture_output=True, text=True, check=True,
    ).stderr
    for line in out.splitlines():
        parts = line.split("|")
        if len(parts) == 3 and parts[2].strip() == module:
            return int(parts[0].split()[-1])
    raise SystemExit(f"module {module} not found in importtime output")

variants = [
    ("stock dataclass    ", "import sys; sys.path.insert(0, '/tmp/typebench');", "bench_dc"),
    ("shim dataclass     ", f"import sys; sys.path.insert(0, '{SHIM_DIR}'); sys.path.insert(0, '/tmp/typebench'); from _shim import install; install();", "bench_dc"),
    ("native salix Struct", "import sys; sys.path.insert(0, '/tmp/typebench');", "bench_struct"),
]
for label, prologue, module in variants:
    runs = [self_time(prologue, module) for _ in range(5)]
    med = statistics.median(runs)
    print(f"{label} {med:7.0f} us self / 200 types = {med/200:6.1f} us per type  (runs: {runs})")

for label, code in [
    ("dataclasses", "import dataclasses"),
    ("_shim", f"import sys; sys.path.insert(0, '{SHIM_DIR}'); from _shim import install; install()"),
    ("salix", "import salix"),
]:
    out = subprocess.run(
        [PY, "-X", "importtime", "-c", code], capture_output=True, text=True, check=True
    ).stderr
    for line in out.splitlines():
        parts = line.split("|")
        if (len(parts) == 3 and (parts[2].strip() == "_shim" or parts[2].strip().endswith("salix"))) or (label == "dataclasses" and len(parts) == 3 and parts[2].strip() == "dataclasses"):
            print(f"{label} one-time import: {parts[0].split()[-1]} us self")
            break
