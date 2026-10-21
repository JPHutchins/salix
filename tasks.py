import json
import re
from enum import Enum, auto
from pathlib import Path
from typing import NamedTuple, TypedDict

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


def release_order(python: str) -> tuple[int, ...]:
    return tuple(map(int, python.split(".")))


NEWEST = max(PYTHONS, key=release_order)

_PROJECT_VERSION = re.search(
    r'^version = "([^"]+)"',
    Path("pyproject.toml").read_text(encoding="utf-8"),
    re.MULTILINE,
)
assert _PROJECT_VERSION is not None
VERSION = _PROJECT_VERSION.group(1)


class PythonPin(TypedDict):
    version: str
    abiTag: str
    pbsVariant: str
    hashes: dict[str, str]


_TARGETS = json.loads(Path("nix/python-targets.json").read_text(encoding="utf-8"))
_PINS: dict[str, PythonPin] = _TARGETS["pythons"]
PLATFORM_TAGS: dict[str, str] = {
    name: platform["platformTag"] for name, platform in _TARGETS["platforms"].items()
}
WHEEL_PLATFORMS = frozenset(platform for pin in _PINS.values() for platform in pin["hashes"])
assert all(
    name.endswith("t") == pin["abiTag"].endswith("t") == (pin["pbsVariant"] == "-freethreaded")
    for name, pin in _PINS.items()
)
PINNED = {name: pin["version"] for name, pin in _PINS.items()}
assert any(name.endswith("t") for name in PINNED)

_UNTESTED_PYTHONS = json.loads(Path("nix/untested-pythons.json").read_text(encoding="utf-8"))
assert isinstance(_UNTESTED_PYTHONS, list)
assert all(isinstance(python, str) for python in _UNTESTED_PYTHONS)
UNTESTED: tuple[str, ...] = tuple(_UNTESTED_PYTHONS)
assert set(UNTESTED) <= {python for python in PINNED if python.endswith("t")}
FREE_THREADED = tuple(
    python for python in PINNED if python.endswith("t") and python not in UNTESTED
)

PYTEST = (
    "uv run --package salix-tests --managed-python --python {PYTHON} python -m pytest"
)
README_FENCES = " --markdown-docs README.md"
README_FENCES_FROM_TESTS = " --markdown-docs ../README.md"
ENVIRONMENT_PER_INTERPRETER = {"UV_PROJECT_ENVIRONMENT": ".venvs/{PY}"}

ISOLATED_RUN = "uv run --no-project --isolated"
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
readme_fences = Task(
    PYTEST + README_FENCES,
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
MYPY = "mypy==2.4.0"
PYRIGHT = "pyright==1.1.414"
TY = "ty==0.0.63"
mypy = Task(
    TYPE_CHECK + " --with " + MYPY + " mypy --strict --warn-unused-ignores"
    " --python-version {CHECKED_PYTHON} tests/typing"
)
pyright = Task(
    TYPE_CHECK + " --with " + PYRIGHT + " pyright --pythonversion {CHECKED_PYTHON} tests/typing"
)

ty = Task(
    TYPE_CHECK + " --with " + TY + " ty check --python-version {CHECKED_PYTHON}"
    " tests/typing/accepted.py"
)
typed_api = Parallel(
    mypy,
    pyright,
    ty,
    variants=tuple(
        {"CHECKED_PYTHON": python} for python in sorted(PYTHONS, key=release_order)
    ),
)

tooling = Task(
    TYPE_CHECK + " --with " + MYPY + " --with camas --with setuptools --with types-setuptools"
    " --with msgspec --with record-type"
    " mypy --strict --warn-unused-ignores --explicit-package-bases"
    " --exclude bench/source-tiers/vendor"
    " setup.py tasks.py build_config.py tools/ bench/",
    env={"MYPYPATH": ".:tools/check_wheel"},
)
type_check = Parallel(typed_api, tooling)

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

source_build_glibc = Task("nix build .#source-build-glibc --no-link", when=NIX_INPUTS)

wheels = Task("nix build .#default --out-link result-wheels", when=NIX_INPUTS, mutates=True)
flake_evaluates = Task(
    "sh -c 'nix flake show --all-systems --json > /dev/null'", when=NIX_INPUTS
)
flake_check = Sequential(flake_evaluates, Task("nix flake check", when=NIX_INPUTS))

test = Parallel(
    Sequential(build, interpreter_guard, Parallel(pytest, readme_fences)),
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
        Parallel(
            *(
                Task(
                    on_free_threaded_axes(command),
                    env={name: on_free_threaded_axes(value) for name, value in ENVIRONMENT_PER_INTERPRETER.items()},
                    agent_format=JUNIT_FORMAT,
                )
                for command in (PYTEST, PYTEST + README_FENCES)
            )
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
HEADER_GATE_SUITE = (
    " --with pytest --with hypothesis --with packaging --with pyrsistent"
    " python -m pytest -p no:cacheprovider --ignore=test_check_wheel.py ."
)
HEADER_GATE_README = (
    " --with pytest --with pytest-markdown-docs python -m pytest -p no:cacheprovider"
    + README_FENCES_FROM_TESTS
)
header_gates = Parallel(
    Sequential(
        Task(
            ISOLATED_RUN + " --managed-python --python {BEFORE} --with setuptools"
            " python setup.py build_ext --build-lib build/{BEFORE} --build-temp build/{BEFORE}-temp",
            mutates=True,
            env=STRICT_BUILD,
        ),
        Task(
            ISOLATED_RUN + " --managed-python --python {BEFORE} python -c 'import salix'",
            cwd=Path("tests"),
            env=HEADER_GATE_BUILD,
        ),
        Parallel(
            *(
                Task(
                    ISOLATED_RUN + " --managed-python --python " + interpreter + run,
                    cwd=Path("tests"),
                    env=HEADER_GATE_BUILD,
                )
                for interpreter in ("{BEFORE}", "{AFTER}")
                for run in (HEADER_GATE_SUITE, HEADER_GATE_README)
            )
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
        " --no-emit-package immutables --output-file dist/pyodide-test-requirements.txt",
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
    Parallel(
        *(
            Task(
                "../dist/pyodide-python/bin/pyodide-python -m pytest -p no:cacheprovider" + run,
                cwd=Path("tests"),
                when=".",
                env={"SALIX_REQUIRE_INSTALLED": "1", "PYTHONPATH": "../dist/pyodide-site"},
            )
            for run in (" .", README_FENCES_FROM_TESTS)
        )
    ),
)

in_place_build = Task("uv run python setup.py build_ext --inplace", mutates=True, env=STRICT_BUILD)
benchmark = Sequential(in_place_build, bench)
CONFORMANCE_PYTHON = "3.14.6"
CONFORMANCE_RUN = ISOLATED_RUN + f" --managed-python --python {CONFORMANCE_PYTHON}"
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

WHEEL_RUN = ISOLATED_RUN + " --no-cache --managed-python --python {PY}"
wheel_guard = Task(
    WHEEL_RUN + " --no-index --find-links ../result-wheels"
    f' --with "salix=={VERSION}"'
    " python -c \"import salix, sysconfig; from importlib.metadata import distribution;"
    " assert bool(sysconfig.get_config_var('Py_GIL_DISABLED')) == '{ABI_TAG}'.endswith('t');"
    " tags = [line for line in (distribution('salix').read_text('WHEEL') or '').splitlines()"
    " if line.startswith('Tag: ')];"
    " assert any(tag.endswith('-{ABI_TAG}-{PLATFORM_TAG}') for tag in tags), tags\"",
    cwd=Path("tests"),
)
WHEEL_PYTEST = WHEEL_RUN + f' --find-links ../result-wheels --with "salix=={VERSION}" --with pytest'
wheel_test = Task(
    WHEEL_PYTEST + " --with hypothesis --with pyrsistent python -m pytest .",
    cwd=Path("tests"),
    env={"SALIX_REQUIRE_INSTALLED": "1"},
)
wheel_readme_fences = Task(
    WHEEL_PYTEST + " --with pytest-markdown-docs python -m pytest" + README_FENCES_FROM_TESTS,
    cwd=Path("tests"),
    env={"SALIX_REQUIRE_INSTALLED": "1"},
)


class Sample(Enum):
    EVERY_PIN = auto()
    OLDEST_NEWEST_AND_FREE_THREADED = auto()


class CoverageRunner(NamedTuple):
    platform: str
    sample: Sample
    interpreter_request: str = "{}"


COVERAGE_RUNNERS = {
    "ubuntu-latest": CoverageRunner("manylinux-x86_64", Sample.EVERY_PIN),
    "ubuntu-24.04-arm": CoverageRunner("manylinux-aarch64", Sample.OLDEST_NEWEST_AND_FREE_THREADED),
    "macos-latest": CoverageRunner("macos-aarch64", Sample.OLDEST_NEWEST_AND_FREE_THREADED),
    "windows-latest": CoverageRunner("windows-x86_64", Sample.OLDEST_NEWEST_AND_FREE_THREADED),
    "macos-15-intel": CoverageRunner("macos-x86_64", Sample.OLDEST_NEWEST_AND_FREE_THREADED),
    "windows-11-arm": CoverageRunner(
        "windows-aarch64", Sample.OLDEST_NEWEST_AND_FREE_THREADED, "cpython-{}-windows-aarch64"
    ),
}


def has_wheel(pin: str, platform: str) -> bool:
    return pin in _PINS and platform in _PINS[pin]["hashes"]


def sampled_pins(runner: CoverageRunner) -> tuple[str, ...]:
    gil_pins = tuple(pin for pin in PYTHONS if has_wheel(pin, runner.platform))
    free_threaded_pins = tuple(pin for pin in FREE_THREADED if has_wheel(pin, runner.platform))

    match runner.sample:
        case Sample.EVERY_PIN:
            return (*gil_pins, *free_threaded_pins)
        case Sample.OLDEST_NEWEST_AND_FREE_THREADED:
            bounds = (
                (min(gil_pins, key=release_order), max(gil_pins, key=release_order))
                if gil_pins
                else ()
            )
            return (*dict.fromkeys(bounds), *free_threaded_pins)


COVERAGE_SAMPLES = {name: sampled_pins(runner) for name, runner in COVERAGE_RUNNERS.items()}
_COVERED_PLATFORMS = frozenset(runner.platform for runner in COVERAGE_RUNNERS.values())
assert PLATFORM_TAGS.keys() >= WHEEL_PLATFORMS, sorted(WHEEL_PLATFORMS - PLATFORM_TAGS.keys())
assert _COVERED_PLATFORMS == WHEEL_PLATFORMS, {
    "runners without wheels": sorted(_COVERED_PLATFORMS - WHEEL_PLATFORMS),
    "wheels without runners": sorted(WHEEL_PLATFORMS - _COVERED_PLATFORMS),
}
assert all(COVERAGE_SAMPLES.values()), [name for name, pins in COVERAGE_SAMPLES.items() if not pins]

coverage = Parallel(
    Sequential(wheel_guard, Parallel(wheel_test, wheel_readme_fences)),
    variants=tuple(
        {
            "OS": name,
            "PY": runner.interpreter_request.format(pin),
            "ABI_TAG": _PINS[pin]["abiTag"],
            "PLATFORM_TAG": PLATFORM_TAGS[runner.platform],
        }
        for name, runner in COVERAGE_RUNNERS.items()
        for pin in COVERAGE_SAMPLES[name]
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
