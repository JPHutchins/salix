import os
import sys
from pathlib import Path

from setuptools import Extension, setup

# A PEP 517 build runs with the source tree off the path, and build_config is
# not a distribution to be resolved.
sys.path.insert(0, str(Path(__file__).parent))

from build_config import BUILD, STRICT

# Opt-in, because this same file builds the sdist on a stranger's machine.
# camas sets it; nothing else does. Compared against "1" rather than tested for
# truth, so that SALIX_STRICT=0 means what it looks like it means.
STRICT_FLAGS = STRICT if os.environ.get("SALIX_STRICT") == "1" else ()

setup(
    ext_modules=[
        Extension(
            "salix.__init__", list(BUILD.sources), extra_compile_args=[*BUILD.c_flags, *STRICT_FLAGS]
        ),
    ],
)
