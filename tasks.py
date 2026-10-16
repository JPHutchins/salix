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

_PYTHON_TARGETS = Path("nix/python-targets.nix").read_text(encoding="utf-8")
_PINS = re.findall(
    r'^    "([0-9.]+t?)" = \{\n'
    r'      version = "([^"]+)";\n'
    r'      tag = "[^"]+";\n'
    r'      abiTag = "([^"]+)";\n'
    r'      pbsVariant = "([^"]*)";\r?$',
    _PYTHON_TARGETS,
    re.MULTILINE,
)
assert len(_PINS) == _PYTHON_TARGETS.count("pbsVariant = ")
assert all(
    name.endswith("t") == abi_tag.endswith("t") == (variant == "-freethreaded")
    for name, _, abi_tag, variant in _PINS
)
PINNED = {name: version for name, version, _, _ in _PINS}
assert any(name.endswith("t") for name in PINNED)

_UNTESTED_PYTHONS = re.search(
    r"^ +untestedPythons = \[([^\]]*)\];\r?$",
    Path("flake.nix").read_text(encoding="utf-8"),
    re.MULTILINE,
)
assert _UNTESTED_PYTHONS is not None
UNTESTED = tuple(re.findall(r'"([^"]+)"', re.sub(r"/\*.*?\*/", "", _UNTESTED_PYTHONS.group(1))))
assert set(UNTESTED) <= set(PINNED)
FREE_THREADED = tuple(
    python for python in PINNED if python.endswith("t") and python not in UNTESTED
)

PYTEST = (
    "uv run --package salix-tests --managed-python --python {PYTHON} python -m pytest"
)
ENVIRONMENT_PER_INTERPRETER = {"UV_PROJECT_ENVIRONMENT": ".venvs/{PY}"}

MAKE_ENV = "uv venv --clear --python {PYTHON} --managed-python .venvs/{PY}"
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
INTERPRETER_GUARD = (
    ".venvs/{PY}/bin/python -c \"import platform, sysconfig;"
    " assert platform.python_version()"
    " + ('t' if sysconfig.get_config_var('Py_GIL_DISABLED') else '') == '{PYTHON}'\""
)
interpreter_guard = Task(INTERPRETER_GUARD)
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

memcheck = Task("nix build .#memcheck --no-link", when=NIX_INPUTS)

wheels = Task("nix build .#default --out-link result-wheels", when=NIX_INPUTS, mutates=True)
flake_evaluates = Task(
    "sh -c 'nix flake show --all-systems --json > /dev/null'", when=NIX_INPUTS
)
flake_check = Sequential(flake_evaluates, Task("nix flake check", when=NIX_INPUTS))

test = Parallel(
    Sequential(build, interpreter_guard, pytest),
    variants=tuple({"PY": python, "PYTHON": PINNED.get(python, python)} for python in PYTHONS),
)

instructions = Parallel(
    Sequential(
        build,
        interpreter_guard,
        Task("nix shell --inputs-from . nixpkgs#valgrind --command .venvs/{PY}/bin/python bench/instructions.py"),
    ),
    variants=({"PY": NEWEST, "PYTHON": PINNED.get(NEWEST, NEWEST)},),
)

FREE_THREADED_ROOT = {"UV_PYTHON_INSTALL_DIR": ".free-threaded-python"}


def on_free_threaded_axes(command: str) -> str:
    return command.replace("{PY}", "{FT}").replace("{PYTHON}", "{FT_PYTHON}")


free_threaded = Parallel(
    Sequential(
        Task(on_free_threaded_axes(MAKE_ENV), mutates=True),
        Task(on_free_threaded_axes(BUILD), mutates=True, env=STRICT_BUILD),
        Task(on_free_threaded_axes(INTERPRETER_GUARD)),
        Task(
            on_free_threaded_axes(PYTEST),
            env={name: on_free_threaded_axes(value) for name, value in ENVIRONMENT_PER_INTERPRETER.items()},
            agent_format=JUNIT_FORMAT,
        ),
    ),
    variants=tuple({"FT": python, "FT_PYTHON": f"{PINNED[python]}t"} for python in FREE_THREADED),
    env=FREE_THREADED_ROOT,
)
HEADER_GATES = (
    {"BEFORE": "3.13.11", "AFTER": "3.13.12"},
    {"BEFORE": "3.14.3", "AFTER": "3.14.4"},
)
HEADER_GATE_BUILD = {"PYTHONPATH": "../build/{BEFORE}"}
HEADER_GATE_TEST = (
    " --with pytest --with hypothesis --with packaging"
    " python -m pytest -p no:cacheprovider --ignore=test_check_wheel.py . ../README.md"
)
header_gates = Parallel(
    Sequential(
        Task(
            "uv run --no-project --managed-python --python {BEFORE} --with setuptools"
            " python setup.py build_ext --build-lib build/{BEFORE} --build-temp build/{BEFORE}-temp",
            mutates=True,
            env=STRICT_BUILD,
        ),
        Task(
            "uv run --no-project --managed-python --python {BEFORE} python -c 'import salix'",
            cwd=Path("tests"),
            env=HEADER_GATE_BUILD,
        ),
        Parallel(
            Task(
                "uv run --no-project --managed-python --python {BEFORE}" + HEADER_GATE_TEST,
                cwd=Path("tests"),
                env=HEADER_GATE_BUILD,
            ),
            Task(
                "uv run --no-project --managed-python --python {AFTER}" + HEADER_GATE_TEST,
                cwd=Path("tests"),
                env=HEADER_GATE_BUILD,
            ),
        ),
    ),
    variants=HEADER_GATES,
)

PYODIDE_PIN = '"$(nix eval --raw --file nix/pyodide.nix {})"'
pyodide = Sequential(
    Task("nix build .#wheel-pyodide --out-link dist/wheel-pyodide", mutates=True),
    Task("nix build .#pyodide-python --out-link dist/pyodide-python", mutates=True),
    Task(
        "uv export --frozen --package salix-tests --no-hashes --no-emit-workspace"
        " --output-file dist/pyodide-test-requirements.txt",
        mutates=True,
    ),
    Task(
        "sh -c 'rm -rf dist/pyodide-site && uvx --python 3.14 --from pip==26.2.1 pip install"
        " --target dist/pyodide-site --no-deps --only-binary=:all:"
        f" --platform {PYODIDE_PIN.format('platformTag')}"
        f" --python-version {PYODIDE_PIN.format('python')}"
        " -r dist/pyodide-test-requirements.txt dist/wheel-pyodide/salix-*.whl'",
        mutates=True,
    ),
    Task(
        "../dist/pyodide-python/bin/pyodide-python -m pytest -p no:cacheprovider . ../README.md",
        cwd=Path("tests"),
        when=".",
        env={"SALIX_REQUIRE_INSTALLED": "1", "PYTHONPATH": "../dist/pyodide-site"},
    ),
)

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
    " python -c \"import salix, sysconfig;"
    " assert bool(sysconfig.get_config_var('Py_GIL_DISABLED')) == '{PY}'.endswith('t')\"",
    cwd=Path("tests"),
)
wheel_test = Task(
    WHEEL_RUN + " --find-links ../result-wheels"
    f' --with "salix=={VERSION}" --with pytest --with hypothesis'
    " python -m pytest . ../README.md",
    cwd=Path("tests"),
    env={"SALIX_REQUIRE_INSTALLED": "1"},
)

WINDOWS_ARM_OLDEST = "3.11"

WINDOWS_ARM_PYTHON = "cpython-{}-windows-aarch64"

COVERAGE_RUNNERS = {
    "ubuntu-latest": (*PYTHONS, *FREE_THREADED),
    "macos-latest": (OLDEST, NEWEST, *FREE_THREADED),
    "windows-latest": (OLDEST, NEWEST, *FREE_THREADED),
    "macos-15-intel": (OLDEST, NEWEST, *FREE_THREADED),
    "windows-11-arm": (
        WINDOWS_ARM_PYTHON.format(WINDOWS_ARM_OLDEST),
        WINDOWS_ARM_PYTHON.format(NEWEST),
    ),
}

coverage = Parallel(
    Sequential(wheel_guard, wheel_test),
    variants=tuple(
        {"OS": runner, "PY": python}
        for runner, pythons in COVERAGE_RUNNERS.items()
        for python in pythons
    ),
)

ci = Parallel(
    flake_check,
    free_threaded,
    header_gates,
    format_check,
    lock_check,
    lint,
    analyze,
    type_check,
    conformance,
)

_ = Config(default_task=check, github_task=ci, agent=Claude(fix=format, check=check))
