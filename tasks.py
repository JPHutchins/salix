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

# Analysis takes translation units; a header is reached through the unit that
# includes it, and compiling one alone is an error under -Werror.
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

# The tests member declares pytest and hypothesis, and supplies them: a
# per-interpreter project environment is what keeps six of them from fighting
# over the one .venv a workspace otherwise shares.
PYTEST = (
    "uv run --package salix-tests --managed-python --python {PY} python -m pytest"
)
ENVIRONMENT_PER_INTERPRETER = {"UV_PROJECT_ENVIRONMENT": ".venvs/{PY}"}

# setuptools is a build tool, not something the tests member should carry.
# The two leaves must name the same interpreter, and neither may name a
# version string the ambient VIRTUAL_ENV can flip to a free-threaded one:
# uv venv creation is immune to that preference and resolves the plain
# managed variant, so the venv the pytest leaf reuses is the one the
# build leaf compiles against.
MAKE_ENV = "uv venv --clear --python {PY} --managed-python .venvs/{PY}"
make_env = Task(MAKE_ENV, mutates=True)

BUILD = (
    "uv run --no-project --python .venvs/{PY}/bin/python --with setuptools"
    ' --with "tomli>=2; python_version < \'3.11\'"'
    " python setup.py build_ext --inplace"
)

# -Werror is ours to opt into. setup.py leaves it off so that an sdist build on
# someone else's compiler cannot fail on a warning nobody here has seen.
STRICT_BUILD = {"SALIX_STRICT": "1"}

NIX_INPUTS = ("src/", "nix/", "tools/", "tests/", "flake.nix", "flake.lock", "pyproject.toml")

build = Sequential(make_env, Task(BUILD, mutates=True, env=STRICT_BUILD))
FULL_SUITE_JUNIT = 1_000_000
JUNIT_FORMAT = AgentFormat("--junitxml {report}", "junit", limit=FULL_SUITE_JUNIT)
pytest = Task(
    PYTEST,
    env=ENVIRONMENT_PER_INTERPRETER,
    agent_format=JUNIT_FORMAT,
)
# --no-sync: analyze reaches this from ci, and CI never installs the project.
compile_flags = Task("uv run --no-sync python tools/compile_flags.py", mutates=True)

clean = Task("git clean -xdf -e .venv -e .venvs -e .free-threaded-python -e .camas -e .claude", mutates=True)
update_python_targets = Task("uv run python tools/update_python_targets.py", mutates=True)

c_format = Task("jphfmt -i {paths}", paths=C_SOURCES, mutates=True)
c_format_check = Task("jphfmt --check {paths}", paths=C_SOURCES)
nix_format = Task("nixfmt {paths}", paths=NIX_SOURCES, mutates=True)
nix_format_check = Task("nixfmt --check {paths}", paths=NIX_SOURCES)
format = Parallel(c_format, nix_format)
format_check = Parallel(c_format_check, nix_format_check)
lock_check = Task("uv lock --check")

# Two engines rather than one: they are independent implementations, and the
# flags carry -Werror, so gcc also holds the build to a second compiler.
c_tidy = Task("clang-tidy --quiet {paths}", paths=C_TRANSLATION_UNITS)
c_analyzer = Task(
    "gcc -fanalyzer -fsyntax-only @compile_flags.txt {paths}", paths=C_TRANSLATION_UNITS
)
analyze = Sequential(compile_flags, Parallel(c_tidy, c_analyzer))

# A checker reads salix/__init__.pyi and never imports the extension, so
# there is nothing to build first. Targeting the floor is the point: that is
# where the stub has to hold, whatever interpreter the checker itself runs on.
TYPE_CHECK = "uv run --no-project --with typing_extensions"
mypy = Task(
    TYPE_CHECK + " --with mypy mypy --strict --warn-unused-ignores"
    " --python-version " + OLDEST + " tests/typing"
)
pyright = Task(TYPE_CHECK + " --with pyright pyright --pythonversion " + OLDEST + " tests/typing")

# ty does not honour the suppression comments the other two do, so it reads the
# acceptances; the rejections are asserted by the checkers that report a
# suppression they did not need.
ty = Task(
    TYPE_CHECK + " --with ty ty check --python-version " + OLDEST + " tests/typing/accepted.py"
)

# The repo's own Python, which is a different question from the stub's: these
# are programs, not assertions about the API, so they are checked on the newest
# interpreter rather than the floor, and record-type does not reach the floor
# anyway. tests/ is excluded on purpose -- passing a wrong type is what most of
# those tests do, and tests/typing is where typing is asserted.
#
# --explicit-package-bases, because bench/tasks.py and tasks.py are both
# `tasks` otherwise.
tooling = Task(
    TYPE_CHECK + " --with mypy --with camas --with setuptools --with types-setuptools"
    " --with msgspec --with record-type"
    " mypy --strict --warn-unused-ignores --explicit-package-bases"
    " setup.py tasks.py build_config.py tools/ bench/",
    env={"MYPYPATH": "."},
)
type_check = Parallel(mypy, pyright, ty, tooling)

# Lint, not format: the style here is the style already in the files, so ruff
# runs as a checker and never as a formatter. The rule set is in pyproject.
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

# Built by nix because they embed CPython: libpython and unity have to be on
# the link line, and nix is where those paths come from.
c_test = Task("nix build .#c-tests --no-link", when=NIX_INPUTS)

wheels = Task("nix build .#default --out-link result-wheels", when=NIX_INPUTS, mutates=True)
# Two steps, because building and evaluating are different questions and only
# one of them is portable. `nix flake check` builds the checks for the machine
# it is on and silently omits the rest. `--all-systems` does not fix that: it
# makes nix *build* the others, which no single runner can do -- an x86_64 CI
# box cannot build the aarch64-darwin check, and that is what turned CI red.
#
# `nix flake show` forces every output on every system to evaluate without
# building any of it, in about a second, which is the part that catches a typo
# in a darwin or aarch64 path.
# sh -c, because camas does not run a shell and the point is the exit status,
# not the 50KB of JSON that proves it got there.
flake_evaluates = Task(
    "sh -c 'nix flake show --all-systems --json > /dev/null'", when=NIX_INPUTS
)
flake_check = Sequential(flake_evaluates, Task("nix flake check", when=NIX_INPUTS))

test = Parallel(Sequential(build, pytest), matrix={"PY": PYTHONS})

# The wheel matrix does build free-threaded targets, derived from these names
# rather than listed alongside them: a `3.14t` in .python-version would enter
# the test matrix too, where uv cannot keep it apart from `3.14` (below). One
# leg is enough -- a module that does not declare Py_mod_gil silently
# re-enables the GIL, and this is the build that would notice.
FREE_THREADED = "3.14t"

# uv resolves a plain `--python 3.14` to a free-threaded interpreter as soon as
# one is installed -- any patch, even with the default variant also installed --
# so this one is kept in a root of its own where it cannot rebind the matrix.
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
benchmark = Sequential(
    Task("uv run python setup.py build_ext --inplace", mutates=True, env=STRICT_BUILD), bench
)
check = Parallel(test, free_threaded, format_check, lock_check, lint, analyze, c_test, type_check)

# Installed, not compiled: MSVC has no __attribute__((cleanup)), so the Windows
# leg cannot build this source at all.
#
# --no-project rather than the tests member, because this leg must not see the
# workspace at all: salix is a member of it, and uv resolves the name to the
# source tree and compiles it instead of taking the wheel. Naming the two
# dependencies is the cost -- uv run takes neither a pyproject.toml for
# --with-requirements nor a member whose sources it is told to ignore.
#
# --no-cache: uv keys its cache on name and version, so a rebuilt wheel of the
# same released version is indistinguishable from one built before and it
# serves the old archive. Neither --refresh-package nor --reinstall-package
# dislodges it, and `uv cache clean` wants a lock no leaf in a parallel tree
# can take.
#
# salix must come from the local tree, and once it is published the index
# offers the same name and version. wheel_guard resolves salix with --no-index,
# so a leg whose local wheel is missing fails there instead of passing against
# the published artifact; the leg then resolves with the index, where
# --find-links wins for salix (measured: uv selects the flat-index wheel for
# an identical name and version) and the index serves the test dependencies.
wheel_guard = Task(
    "uv run --no-cache --no-project --managed-python --python {PY}"
    " --no-index --find-links ../result-wheels --with salix"
    ' python -c "import salix"',
    cwd=Path("tests"),
)
wheel_test = Task(
    "uv run --no-cache --no-project --managed-python --python {PY}"
    " --find-links ../result-wheels"
    " --with salix --with pytest --with hypothesis python -m pytest .",
    cwd=Path("tests"),
    env={"SALIX_REQUIRE_INSTALLED": "1"},
)

# python-build-standalone has no Windows ARM64 build below 3.11, so the wheel
# set has no cp310 win_arm64 to import and the leg below cannot ask for one.
WINDOWS_ARM_OLDEST = "3.11"

# Spelled out, because a bare version is not enough on this one runner: uv
# resolves `--python 3.15` there to the *x86_64* build, Windows ARM64 runs it
# under emulation, and an emulated interpreter installs win_amd64 -- so the leg
# passed twice while importing the wheel windows-latest already imports. The
# architecture is the whole point of the leg, so it is named.
WINDOWS_ARM_PYTHON = "cpython-{}-windows-aarch64"

# Sampled rather than crossed, to stay inside the OSS concurrency limit.
#
# The last four legs are the machines that can load what the macos-latest and
# windows-latest legs cannot: macos-latest is Apple silicon and windows-latest
# is x86_64, which left macosx_10_13_x86_64 and win_arm64 shipping inspected by
# check_wheel.py and imported by nothing (#37). Both are cross-compiled by zig,
# which is the half of the build with nobody standing behind it.
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

ci = Parallel(flake_check, free_threaded, format_check, lock_check, lint, analyze, type_check)

_ = Config(default_task=check, github_task=ci, agent=Claude(fix=format, check=check))
