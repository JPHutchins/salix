import importlib
import pkgutil
import sys

import transformers  # type: ignore[import-not-found]

import salix

if len(sys.argv) > 1 and sys.argv[1] == "--stock":
    run_stock = True
else:
    run_stock = False
    import _shim

    _shim.install(include_prefixes=("transformers",))

import transformers.models  # type: ignore[import-not-found]
from transformers.models.bert.configuration_bert import (  # type: ignore[import-not-found]
    BertConfig,
)

count = 0
failed_imports = 0
failed_scans = 0
seen: set[int] = set()
for module in pkgutil.walk_packages(transformers.models.__path__, "transformers.models."):
    try:
        imported = importlib.import_module(module.name)
    except Exception:  # noqa: BLE001 — any import failure counts against the parity
        failed_imports += 1
        continue
    try:
        for _name, value in vars(imported).items():
            if isinstance(value, type) and hasattr(value, "config_class") and id(value) not in seen:
                seen.add(id(value))
                count += 1
    except Exception:  # noqa: BLE001 — any scan failure counts against the parity
        failed_scans += 1
        continue
print(f"model classes with config_class: {count}")
print(f"modules whose import failed: {failed_imports}")
print(f"modules whose class scan failed: {failed_scans}")
assert count == 3266, f"import parity broken: {count} != 3266"

if run_stock:
    assert failed_imports == 311, f"unexpected import failures: {failed_imports} != 311"
    assert failed_scans == 37, f"unexpected class-scan failures: {failed_scans} != 37"
else:
    assert salix.Struct in BertConfig.__mro__, "the shim did not convert a config class"
    assert failed_imports == 314, f"unexpected import failures: {failed_imports} != 314"
    assert failed_scans == 37, f"unexpected class-scan failures: {failed_scans} != 37"
print("PARITY OK")
