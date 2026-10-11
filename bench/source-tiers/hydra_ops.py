import importlib
import sys
import time
from collections.abc import Callable

hydra = importlib.import_module("hydra")
ConfigStore = importlib.import_module("hydra.core.config_store").ConfigStore
OmegaConf = importlib.import_module("omegaconf").OmegaConf
schema = importlib.import_module(sys.argv[1])
ConfigStore.instance().store(name="root", node=schema.Root)
OVERRIDES = ["section07.leaf03.count=11", "section02.leaf01.name=override"]


def per_iteration(label: str, iterations: int, operation: Callable[[], object]) -> None:
    operation()
    started = time.perf_counter()
    for _ in range(iterations):
        operation()
    print(f"{label} {1e6 * (time.perf_counter() - started) / iterations:.3f} us/iter")


with hydra.initialize_config_dir(config_dir=sys.argv[2]):
    config = hydra.compose("config", overrides=OVERRIDES)
    assert OmegaConf.to_object(config).section07.leaf03.count == 11
    per_iteration("compose", 10, lambda: hydra.compose("config", overrides=OVERRIDES))
    per_iteration("to_object", 20, lambda: OmegaConf.to_object(config))
