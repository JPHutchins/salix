import os
import sys
import sysconfig
from pathlib import Path

from setuptools import Extension, setup

sys.path.insert(0, str(Path(__file__).parent))

from build_config import BUILD, PLATFORM_FLAGS, STRICT

STRICT_FLAGS = STRICT if os.environ.get("SALIX_STRICT") == "1" else ()

setup(
    ext_modules=[
        Extension(
            "salix.__init__",
            list(BUILD.sources),
            extra_compile_args=[
                *BUILD.c_flags,
                *PLATFORM_FLAGS.get(sysconfig.get_platform(), ()),
                *STRICT_FLAGS,
            ],
        ),
    ],
)
