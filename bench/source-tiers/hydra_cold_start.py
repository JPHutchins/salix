import importlib
import sys
import time

started = time.perf_counter()
hydra = importlib.import_module("hydra")
ConfigStore = importlib.import_module("hydra.core.config_store").ConfigStore
OmegaConf = importlib.import_module("omegaconf").OmegaConf
imported = time.perf_counter()
schema = importlib.import_module(sys.argv[1])
defined = time.perf_counter()
ConfigStore.instance().store(name="root", node=schema.Root)
stored = time.perf_counter()
with hydra.initialize_config_dir(config_dir=sys.argv[2]):
    config = hydra.compose("config", overrides=["section07.leaf03.count=11"])
composed = time.perf_counter()
instance = OmegaConf.to_object(config)
finished = time.perf_counter()

assert type(instance).__name__ == "Root"
assert instance.section19.leaf09.count == 199
assert instance.section07.leaf03.count == 11
assert instance.section03.leaf04.sizes == [3, 4]
print(
    f"import hydra {1e3 * (imported - started):.3f} ms\n"
    f"define schema {1e3 * (defined - imported):.3f} ms\n"
    f"store schema {1e3 * (stored - defined):.3f} ms\n"
    f"compose {1e3 * (composed - stored):.3f} ms\n"
    f"to_object {1e3 * (finished - composed):.3f} ms\n"
    f"in-process total {1e3 * (finished - started):.3f} ms"
)
