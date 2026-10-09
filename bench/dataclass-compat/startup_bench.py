import statistics
import subprocess
import time


def bench(py: str, label: str, code: str, cwd: str | None = None, runs: int = 5) -> bool:
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = subprocess.run([py, "-c", code], capture_output=True, text=True, cwd=cwd, check=False)
        times.append(time.perf_counter() - t0)
        if r.returncode != 0:
            print(f"{label}: FAILED\n{r.stderr[-400:]}")
            return False
    print(f"{label}: {statistics.median(times)*1000:7.1f} ms per start (median of {runs})")
    return True

results = []

OMEGA_VENV = "/home/jp/repos/omegaconf-salix-venv/bin/python"
HF_VENV = "/home/jp/repos/transformers-salix-venv/bin/python"
SHIM = "/home/jp/repos/jp-struct/bench/dataclass-compat"

omega_core = "import app_cfg; from omegaconf import OmegaConf; OmegaConf.structured(app_cfg.StructuredWithMissing); OmegaConf.structured(app_cfg.ConcretePlugin); OmegaConf.structured(app_cfg.NestedContainers)"
results.append(bench(OMEGA_VENV, "omegaconf stock   (suite dataclasses)", "import sys; sys.path.insert(0, '/tmp'); import pytest; sys.path.insert(0, '/home/jp/repos/omegaconf'); " + omega_core))
results.append(bench(OMEGA_VENV, "omegaconf migrated (fork + shim)", "import sys; sys.path.insert(0, '/tmp'); import pytest; sys.path.insert(0, '/home/jp/repos/omegaconf-salix'); sys.path.insert(0, '" + SHIM + "'); from _shim import install; install(); " + omega_core))

results.append(bench(HF_VENV, "transformers stock    import", "import transformers"))
results.append(bench(HF_VENV, "transformers migrated import", "import sys; sys.path.insert(0, '/home/jp/repos/transformers-salix/src'); from transformers._salix_shim import install; install(); import transformers"))

raise SystemExit(0 if all(results) else 1)
