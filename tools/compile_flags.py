import shlex
import sys
import sysconfig
from pathlib import Path
from typing import Final

ROOT: Final = Path(__file__).resolve().parent.parent
DATABASE: Final = ROOT / "compile_flags.txt"

sys.path.insert(0, str(ROOT))

from build_config import BUILD, STRICT  # noqa: E402

FLAGS: Final = (
    *shlex.split(sysconfig.get_config_var("CFLAGS")),
    *shlex.split(sysconfig.get_config_var("CCSHARED")),
    f"-I{sysconfig.get_config_var('prefix')}/include",
    f"-I{sysconfig.get_paths()['include']}",
    *BUILD.c_flags,
    # Unconditional here, unlike setup.py's opt-in: this database feeds clangd,
    # clang-tidy and the analyzer, which are this repository's own tools and
    # never a stranger's build.
    *STRICT,
)


if __name__ == "__main__":
    DATABASE.write_text("\n".join(FLAGS) + "\n")
    print(f"wrote {len(FLAGS)} flags to {DATABASE.relative_to(ROOT)}")
