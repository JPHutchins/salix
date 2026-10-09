import re
from pathlib import Path

from camas import (
    AgentFormat,
    Claude,
    Config,
    Parallel,
    Project,
    Sequential,
    Task,
    by_suffix,
)

C_SOURCES = by_suffix((".c", ".h"), default=tuple(sorted(str(p) for p in Path("src").rglob("*.[ch]"))))

C_TRANSLATION_UNITS = by_suffix(
    (".c",), default=tuple(sorted(str(p) for p in Path("src").rglob("*.c")))
)
NIX_SOURCES = by_suffix(
    (".nix",),
    default=tuple(sorted(str(p) for p in (*Path(".").glob("*.nix"), *Path("nix").glob("*.nix")))),
)
PYTHONS = tuple(Path(".python-version").read_text().split())
OLDEST = min(PYTHONS, key=lambda python: tuple(map(int, python.split("."))))
NEWEST = max(PYTHONS, key=lambda python: tuple(map(int, python.split("."))))

_PROJECT_VERSION = re.search(
    r'^version = "([^"]+)"',
    Path("pyproject.toml").read_text(encoding="utf-8"),
    re.MULTILINE,
)
assert _PROJECT_VERSION is not None
VERSION = _PROJECT_VERSION.group(1)

PYTEST = (
    "uv run --package salix-tests --managed-python --python {PY} python -m pytest"
)
ENVIRONMENT_PER_INTERPRETER = {"UV_PROJECT_ENVIRONMENT": ".venvs/{PY}"}

MAKE_ENV = "uv venv --clear --python {PY} --managed-python .venvs/{PY}"
make_env = Task(MAKE_ENV, mutates=True)

BUILD = (
    "uv run --no-project --python .venvs/{PY}/bin/python --with setuptools"
    ' --with "tomli>=2; python_version < \'3.11\'"'
    " python setup.py build_ext --inplace"
)

STRICT_BUILD = {"SALIX_STRICT": "1"}

NIX_INPUTS = (
    "src/",
    "salix/",
    "nix/",
    "tools/",
    "tests/",
    "flake.nix",
    "flake.lock",
    "pyproject.toml",
    "build_config.py",
    "setup.py",
    ".python-version",
    "README.md",
    "LICENSE",
    "MANIFEST.in",
    "CODE_OF_CONDUCT.md",
)

build = Sequential(make_env, Task(BUILD, mutates=True, env=STRICT_BUILD))
FULL_SUITE_JUNIT = 1_000_000
JUNIT_FORMAT = AgentFormat("--junitxml {report}", "junit", limit=FULL_SUITE_JUNIT)
pytest = Task(
    PYTEST,
    env=ENVIRONMENT_PER_INTERPRETER,
    agent_format=JUNIT_FORMAT,
)
compile_flags = Task("uv run --no-sync python tools/compile_flags.py", mutates=True)

clean = Task(
    "git clean -xdf -e .venv -e .venvs -e .free-threaded-python -e .camas -e .claude -e .cpython",
    mutates=True,
)
update_python_targets = Task("uv run python tools/update_python_targets.py", mutates=True)

c_format = Task("jphfmt -i {paths}", paths=C_SOURCES, mutates=True)
c_format_check = Task("jphfmt --check {paths}", paths=C_SOURCES)
nix_format = Task("nixfmt {paths}", paths=NIX_SOURCES, mutates=True)
nix_format_check = Task("nixfmt --check {paths}", paths=NIX_SOURCES)
format = Parallel(c_format, nix_format)
format_check = Parallel(c_format_check, nix_format_check)
lock_check = Task("uv lock --check")

c_tidy = Task("clang-tidy --quiet {paths}", paths=C_TRANSLATION_UNITS)
c_analyzer = Task(
    "gcc -fanalyzer -fsyntax-only @compile_flags.txt {paths}", paths=C_TRANSLATION_UNITS
)
analyze = Sequential(compile_flags, Parallel(c_tidy, c_analyzer))

TYPE_CHECK = "uv run --no-project --with typing_extensions"
mypy = Task(
    TYPE_CHECK + " --with mypy mypy --strict --warn-unused-ignores"
    " --python-version " + OLDEST + " tests/typing"
)
pyright = Task(TYPE_CHECK + " --with pyright pyright --pythonversion " + OLDEST + " tests/typing")

ty = Task(
    TYPE_CHECK + " --with ty ty check --python-version " + OLDEST + " tests/typing/accepted.py"
)

tooling = Task(
    TYPE_CHECK + " --with mypy --with camas --with setuptools --with types-setuptools"
    " --with msgspec --with record-type"
    " mypy --strict --warn-unused-ignores --explicit-package-bases"
    " --exclude bench/source-tiers/vendor"
    " setup.py tasks.py build_config.py tools/ bench/",
    env={"MYPYPATH": "."},
)
type_check = Parallel(mypy, pyright, ty, tooling)

RUFF_CHECK = "uv run --no-project --with ruff ruff check"
HUNDREDS_OF_DIAGNOSTICS = 64_000
LINT_FORMAT = AgentFormat("--output-format rdjson", "rdjson", limit=HUNDREDS_OF_DIAGNOSTICS)
lint = Parallel(
    Task(
        RUFF_CHECK + " . --extend-exclude tests/test_generics_pep695.py",
        agent_format=LINT_FORMAT,
    ),
    Task(
        RUFF_CHECK + " --target-version py312 tests/test_generics_pep695.py",
        when=("tests/test_generics_pep695.py", "pyproject.toml"),
        agent_format=LINT_FORMAT,
    ),
)

bench = Project("bench")

c_test = Task("nix build .#c-tests --no-link", when=NIX_INPUTS)

wheels = Task("nix build .#default --out-link result-wheels", when=NIX_INPUTS, mutates=True)
flake_evaluates = Task(
    "sh -c 'nix flake show --all-systems --json > /dev/null'", when=NIX_INPUTS
)
flake_check = Sequential(flake_evaluates, Task("nix flake check", when=NIX_INPUTS))

test = Parallel(Sequential(build, pytest), matrix={"PY": PYTHONS})

FREE_THREADED = "3.14t"

FREE_THREADED_ROOT = {"UV_PYTHON_INSTALL_DIR": ".free-threaded-python"}
free_threaded_build = Sequential(
    Task(
        MAKE_ENV.format(PY=FREE_THREADED),
        mutates=True,
        env=FREE_THREADED_ROOT,
        when=lambda changed: not (Path(".venvs") / FREE_THREADED / "bin/python").exists(),
    ),
    Task(BUILD.format(PY=FREE_THREADED), mutates=True, env=FREE_THREADED_ROOT | STRICT_BUILD),
)
free_threaded_pytest = Task(
    PYTEST.format(PY=FREE_THREADED),
    env=FREE_THREADED_ROOT | {"UV_PROJECT_ENVIRONMENT": ".venvs/" + FREE_THREADED},
    agent_format=JUNIT_FORMAT,
)
free_threaded = Sequential(free_threaded_build, free_threaded_pytest)
in_place_build = Task("uv run python setup.py build_ext --inplace", mutates=True, env=STRICT_BUILD)
benchmark = Sequential(in_place_build, bench)
CONFORMANCE_PYTHON = "3.14.6"
CONFORMANCE_RUN = f"uv run --no-project --managed-python --python {CONFORMANCE_PYTHON}"
conformance = Sequential(
    Task(
        CONFORMANCE_RUN + " --with setuptools python setup.py build_ext --inplace",
        mutates=True,
        env=STRICT_BUILD,
    ),
    Task(
        CONFORMANCE_RUN + f" python dataclass-compat/conformance.py {CONFORMANCE_PYTHON}",
        cwd=Path("bench"),
        when=".",
    ),
)
check = Parallel(test, free_threaded, format_check, lock_check, lint, analyze, c_test, type_check)

WHEEL_RUN = "uv run --no-cache --no-project --managed-python --python {PY}"
wheel_guard = Task(
    WHEEL_RUN + " --no-index --find-links ../result-wheels"
    f' --with "salix=={VERSION}"'
    ' python -c "import salix"',
    cwd=Path("tests"),
)
wheel_test = Task(
    WHEEL_RUN + " --find-links ../result-wheels"
    f' --with "salix=={VERSION}" --with pytest --with hypothesis'
    " python -m pytest .",
    cwd=Path("tests"),
    env={"SALIX_REQUIRE_INSTALLED": "1"},
)

WINDOWS_ARM_OLDEST = "3.11"

WINDOWS_ARM_PYTHON = "cpython-{}-windows-aarch64"

coverage = Parallel(
    Sequential(wheel_guard, wheel_test),
    variants=(
        *({"OS": "ubuntu-latest", "PY": python} for python in PYTHONS),
        {"OS": "macos-latest", "PY": OLDEST},
        {"OS": "macos-latest", "PY": NEWEST},
        {"OS": "windows-latest", "PY": OLDEST},
        {"OS": "windows-latest", "PY": NEWEST},
        {"OS": "macos-15-intel", "PY": OLDEST},
        {"OS": "macos-15-intel", "PY": NEWEST},
        {"OS": "windows-11-arm", "PY": WINDOWS_ARM_PYTHON.format(WINDOWS_ARM_OLDEST)},
        {"OS": "windows-11-arm", "PY": WINDOWS_ARM_PYTHON.format(NEWEST)},
    ),
)

ci = Parallel(
    flake_check, free_threaded, format_check, lock_check, lint, analyze, type_check, conformance
)

_ = Config(default_task=check, github_task=ci, agent=Claude(fix=format, check=check))
