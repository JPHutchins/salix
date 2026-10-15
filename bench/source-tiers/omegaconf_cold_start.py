import importlib
import sys
import time

started = time.perf_counter()
OmegaConf = importlib.import_module("omegaconf").OmegaConf
imported = time.perf_counter()
schema = importlib.import_module(sys.argv[1])
defined = time.perf_counter()
structured = OmegaConf.structured(schema.Root)
built = time.perf_counter()
merged = OmegaConf.merge(structured, OmegaConf.load(sys.argv[2]))
loaded = time.perf_counter()
instance = OmegaConf.to_object(merged)
finished = time.perf_counter()

assert type(instance).__name__ == "Root"
assert instance.section19.leaf09.count == 199
assert instance.section03.leaf04.sizes == [3, 4]
print(
    f"import omegaconf {1e3 * (imported - started):.3f} ms\n"
    f"define schema {1e3 * (defined - imported):.3f} ms\n"
    f"structured(Root) {1e3 * (built - defined):.3f} ms\n"
    f"load + merge yaml {1e3 * (loaded - built):.3f} ms\n"
    f"to_object {1e3 * (finished - loaded):.3f} ms\n"
    f"in-process total {1e3 * (finished - started):.3f} ms"
)
