import os
import platform
import re
import subprocess
import sys
import sysconfig
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

sys.path.insert(0, str(Path(__file__).parent))

from build_config import BUILD, PLATFORM_FLAGS, STRICT

STRICT_FLAGS = STRICT if os.environ.get("SALIX_STRICT") == "1" else ()
BASELINE_EXTENSIONS = frozenset({"MODE64", "CMOV", "FPU", "MMX", "SSE1", "SSE2"})
EXTENSION_USED = re.compile(r"([A-Z0-9_]+) \(.+\)")


class BaselineCpuBuild(build_ext):
    def run(self) -> None:
        destinations = self.artifacts(self.inplace)
        self.force = self.force or any(
            beyond_baseline(artifact) for artifact in {*destinations, *self.artifacts(False)} if artifact.is_file()
        )
        super().run()

        for destination in destinations:
            refuse_beyond_baseline(destination)

    def artifacts(self, inplace: bool) -> list[Path]:
        caller_inplace, self.inplace = self.inplace, inplace
        paths = [Path(self.get_ext_fullpath(ext.name)) for ext in self.extensions]
        self.inplace = caller_inplace

        return paths


def refuse_beyond_baseline(artifact: Path) -> None:
    beyond = beyond_baseline(artifact)

    if beyond:
        artifact.unlink()
        raise SystemExit(
            f"{artifact} uses instruction set extensions beyond the baseline CPU, so it was removed: "
            + ", ".join(sorted(beyond))
        )


def beyond_baseline(artifact: Path) -> frozenset[str]:
    return extensions_used(artifact) - BASELINE_EXTENSIONS


def extensions_used(artifact: Path) -> frozenset[str]:
    try:
        report = subprocess.run(
            ["elfx86exts", str(artifact)], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(
            f"{artifact} cannot be checked against the baseline CPU (it needs elfx86exts; build inside nix develop)"
        ) from error

    used = frozenset(match.group(1) for match in map(EXTENSION_USED.fullmatch, report.splitlines()) if match)

    if "MODE64" not in used:
        raise SystemExit(f"{artifact} cannot be checked against the baseline CPU: elfx86exts read no 64-bit code")

    return used


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
    cmdclass={"build_ext": BaselineCpuBuild}
    if os.environ.get("SALIX_BASELINE_CPU") == "1" and platform.machine() == "x86_64"
    else {},
)
