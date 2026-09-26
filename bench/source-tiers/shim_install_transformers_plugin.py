from transformers import _salix_shim  # type: ignore[import-not-found]

_salix_shim.install(include_prefixes=("transformers",))
