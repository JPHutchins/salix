import hashlib
import os
import re
import shlex
import subprocess
import sys
import sysconfig
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

sys.path.insert(0, str(Path(__file__).parent))

from build_config import BUILD, PLATFORM_FLAGS, STRICT

STRICT_FLAGS = STRICT if os.environ.get("SALIX_STRICT") == "1" else ()
MACHINE_FLAG = re.compile(r"-m[a-z]")


class BaselineCpuBuild(build_ext):
    def run(self) -> None:
        artifacts = {
            Path(self.build_temp) / f"{ext.name}.artifact": Path(self.get_ext_fullpath(ext.name))
            for ext in self.extensions
        }
        self.force = self.force or any(
            recorded(identity) != digest(artifact) for identity, artifact in artifacts.items()
        )
        super().run()

        for identity, artifact in artifacts.items():
            record(identity, digest(artifact))

    def build_extension(self, ext: Extension) -> None:
        commands = (
            [*self.compiler.compiler_so, *ext.extra_compile_args],
            [*self.compiler.linker_so, *ext.extra_link_args],
        )

        for command in commands:
            refuse_beyond_baseline(command)

        fingerprint = Path(self.build_temp) / f"{ext.name}.commands"
        record(fingerprint, "".join(f"{shlex.join(command)}\n" for command in commands))
        ext.depends = [*ext.depends, str(fingerprint)]
        super().build_extension(ext)


def predefined_macros(command: list[str]) -> set[str]:
    try:
        return set(
            subprocess.run(
                [*command, "-dM", "-E", "-x", "c", "-"],
                input="",
                capture_output=True,
                text=True,
                check=True,
            ).stdout.splitlines()
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(
            f"{shlex.join(command)} cannot be checked against the baseline CPU"
            " (it needs zig cc's -mcpu=baseline; build inside nix develop)"
        ) from error


def refuse_beyond_baseline(command: list[str]) -> None:
    beyond = predefined_macros(command) - predefined_macros(
        [*(argument for argument in command if not MACHINE_FLAG.match(argument)), "-mcpu=baseline"]
    )

    if beyond:
        raise SystemExit(
            f"{shlex.join(command)} targets more than the baseline CPU: "
            + ", ".join(sorted(macro.split()[1] for macro in beyond))
        )


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else ""


def recorded(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def record(path: Path, text: str) -> None:
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


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
    cmdclass={"build_ext": BaselineCpuBuild} if os.environ.get("SALIX_BASELINE_CPU") == "1" else {},
)
