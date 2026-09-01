import statistics
import subprocess
import time


def bench(py: str, label: str, code: str, cwd: str | None = None, runs: int = 5) -> None:
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = subprocess.run([py, "-c", code], capture_output=True, text=True, cwd=cwd, check=False)
        times.append(time.perf_counter() - t0)
        if r.returncode != 0:
            print(f"{label}: FAILED\n{r.stderr[-400:]}")
            return
    print(f"{label}: {statistics.median(times)*1000:7.1f} ms per start (median of {runs})")

OMEGA_VENV = "/home/jp/repos/omegaconf-salix-venv/bin/python"
HYDRA_VENV = "/home/jp/repos/hydra-salix-venv/bin/python"
TYRO_VENV = "/home/jp/repos/tyro-salix-venv/bin/python"
HF_VENV = "/home/jp/repos/transformers-salix-venv/bin/python"
SHIM = "/home/jp/repos/jp-struct/bench/dataclass-compat"

omega_core = "import app_cfg; from omegaconf import OmegaConf; OmegaConf.structured(app_cfg.StructuredWithMissing); OmegaConf.structured(app_cfg.ConcretePlugin); OmegaConf.structured(app_cfg.NestedContainers)"
bench(OMEGA_VENV, "omegaconf stock   (suite dataclasses)", "import sys; sys.path.insert(0, '/tmp'); import pytest; sys.path.insert(0, '/home/jp/repos/omegaconf'); " + omega_core)
bench(OMEGA_VENV, "omegaconf migrated (fork + shim)", "import sys; sys.path.insert(0, '/tmp'); import pytest; sys.path.insert(0, '/home/jp/repos/omegaconf-salix'); sys.path.insert(0, '" + SHIM + "'); from _shim import install; install(); " + omega_core)

hydra_core = "from hydra import compose, initialize; initialize(version_base=None, config_path='../examples/configure_hydra/logging/conf'); compose(config_name='config')"
bench(HYDRA_VENV, "hydra stock     (example config)", "import sys; sys.path.insert(0, '/home/jp/repos/hydra'); " + hydra_core, cwd="/home/jp/repos/hydra/tests")
bench(HYDRA_VENV, "hydra patched   (example config)", "import sys; sys.path.insert(0, '/home/jp/repos/hydra'); sys.path.insert(0, '" + SHIM + "'); from _shim import install; install(exclude_prefixes=('hydra',)); " + hydra_core, cwd="/home/jp/repos/hydra/tests")

bench(TYRO_VENV, "tyro stock   (cli parse)", "import runpy; runpy.run_path('/tmp/tyro_app.py')")
bench(TYRO_VENV, "tyro patched (cli parse)", "import sys; sys.path.insert(0, '" + SHIM + "'); from _shim import install; install(exclude_prefixes=('tyro',)); import runpy; runpy.run_path('/tmp/tyro_app.py')")

bench(HF_VENV, "transformers stock    import", "import transformers")
bench(HF_VENV, "transformers migrated import", "import sys; sys.path.insert(0, '/home/jp/repos/transformers-salix/src'); from transformers._salix_shim import install; install(); import transformers")
