import argparse
import hashlib
import platform
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterator, Sequence
from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
VENDOR = HERE / "vendor"
EXCLUDE_NEWER = "2026-10-15T00:00:00Z"
PYTORCH_CPU_INDEX = "https://download.pytorch.org/whl/cpu"


class Fork(NamedTuple):
    submodule: str
    base: str
    source_root: str
    generates_parsers: bool
    preinstall: tuple[str, ...]
    install_extra: str
    reads_forks: tuple[str, ...]


class Sides(NamedTuple):
    stock: Path
    fork: Path


class Variant(NamedTuple):
    label: str
    python_path: tuple[Path, ...]
    script: str
    arguments: tuple[str, ...]


class Row(NamedTuple):
    title: str
    output: str
    rounds: int
    variants: tuple[Variant, ...]


class Generation(NamedTuple):
    script: str
    python_path: tuple[Path, ...]


class Plan(NamedTuple):
    generations: tuple[Generation, ...]
    gate: Variant
    rows: tuple[Row, ...]


def omegaconf_plan(sources: dict[str, Sides], inputs: Path) -> Plan:
    omegaconf = sources["omegaconf"]
    big = str(inputs / "big.yaml")
    cold_start = Variant("fork+salix", (omegaconf.fork, inputs), "omegaconf_cold_start.py", ("schema_salix", big))
    return Plan(
        (Generation("omegaconf_schema.py", ()),),
        cold_start,
        (
            Row("omegaconf cold start", "omegaconf-cold", 15, (
                Variant("stock+dataclass", (omegaconf.stock, inputs), "omegaconf_cold_start.py", ("schema_dataclass", big)),
                cold_start,
            )),
            Row("omegaconf ops", "omegaconf-ops", 9, (
                Variant("stock+dataclass", (omegaconf.stock,), "omegaconf_ops.py", ("dataclass",)),
                Variant("fork+salix", (omegaconf.fork,), "omegaconf_ops.py", ("salix",)),
            )),
        ),
    )


def tyro_plan(sources: dict[str, Sides], inputs: Path) -> Plan:
    tyro = sources["tyro"]
    cold_start = Variant("fork+salix", (tyro.fork, inputs), "tyro_cold_start.py", ("schema_salix",))
    return Plan(
        (Generation("tyro_schema.py", ()),),
        cold_start,
        (
            Row("tyro cold start", "tyro-cold", 15, (
                Variant("stock+dataclass", (tyro.stock, inputs), "tyro_cold_start.py", ("schema_dataclass",)),
                cold_start,
            )),
            Row("tyro ops", "tyro-ops", 9, (
                Variant("stock+dataclass", (tyro.stock, inputs), "tyro_ops.py", ("schema_dataclass",)),
                Variant("fork+salix", (tyro.fork, inputs), "tyro_ops.py", ("schema_salix",)),
            )),
        ),
    )


def cyclopts_plan(sources: dict[str, Sides], inputs: Path) -> Plan:
    cyclopts = sources["cyclopts"]
    stock = (cyclopts.stock, inputs)
    fork = (cyclopts.fork, inputs)
    cold_start = Variant("fork+salix", fork, "cyclopts_cold_start.py", ("cli_grouped_salix",))
    return Plan(
        (Generation("cyclopts_app.py", (cyclopts.fork,)),),
        cold_start,
        (
            Row("cyclopts cold start, grouped", "cyclopts-cold-grouped", 15, (
                Variant("stock+dataclass", stock, "cyclopts_cold_start.py", ("cli_grouped_dataclass",)),
                cold_start,
            )),
            Row("cyclopts cold start, plain functions", "cyclopts-cold-plain", 15, (
                Variant("stock", stock, "cyclopts_cold_start.py", ("cli_plain",)),
                Variant("fork", fork, "cyclopts_cold_start.py", ("cli_plain",)),
            )),
            Row("cyclopts ops", "cyclopts-ops", 7, (
                Variant("stock+dataclass", stock, "cyclopts_ops.py", ("cli_grouped_dataclass", "300")),
                Variant("fork+salix", fork, "cyclopts_ops.py", ("cli_grouped_salix", "300")),
                Variant("stock plain", stock, "cyclopts_ops.py", ("cli_plain", "300")),
                Variant("fork plain", fork, "cyclopts_ops.py", ("cli_plain", "300")),
            )),
        ),
    )


def hydra_plan(sources: dict[str, Sides], inputs: Path) -> Plan:
    stock = (sources["omegaconf"].stock, sources["hydra"].stock, inputs)
    fork = (sources["omegaconf"].fork, sources["hydra"].fork, inputs)
    conf = str(inputs / "conf")
    runs = inputs / "runs"
    app = ("--config-dir", conf, "section07.leaf03.count=11")
    cold_start = Variant("fork+salix", fork, "hydra_cold_start.py", ("schema_salix", conf))
    return Plan(
        (Generation("omegaconf_schema.py", ()), Generation("hydra_config.py", ())),
        cold_start,
        (
            Row("hydra app (@hydra.main)", "hydra-app", 21, (
                Variant("stock+dataclass", stock, "hydra_app.py", ("schema_dataclass", *app, f"hydra.run.dir={runs / 'stock'}")),
                Variant("fork+salix", fork, "hydra_app.py", ("schema_salix", *app, f"hydra.run.dir={runs / 'fork'}")),
            )),
            Row("hydra cold start (compose API)", "hydra-cold", 21, (
                Variant("stock+dataclass", stock, "hydra_cold_start.py", ("schema_dataclass", conf)),
                cold_start,
            )),
            Row("hydra ops", "hydra-ops", 7, (
                Variant("stock+dataclass", stock, "hydra_ops.py", ("schema_dataclass", conf)),
                Variant("fork+salix", fork, "hydra_ops.py", ("schema_salix", conf)),
            )),
        ),
    )


def transformers_plan(sources: dict[str, Sides], inputs: Path) -> Plan:
    transformers = sources["transformers"]
    cold_start = Variant("fork+salix", (transformers.fork,), "transformers_cold_start.py", ())
    return Plan(
        (),
        cold_start,
        (
            Row("transformers cold start", "transformers-cold", 11, (
                Variant("stock", (transformers.stock,), "transformers_cold_start.py", ()),
                cold_start,
            )),
            Row("transformers ops", "transformers-ops", 5, (
                Variant("stock", (transformers.stock,), "transformers_ops.py", ()),
                Variant("fork+salix", (transformers.fork,), "transformers_ops.py", ()),
            )),
        ),
    )


FORKS = {
    "omegaconf": Fork("omegaconf-salix", "a8bcf1f001d24a40d1d4821a424c25d4e4525c42", ".", True, (), "", ()),
    "tyro": Fork("tyro-salix", "d0c9877f6a4a6d1a63894799ddb3daa2720cdb9e", "src", False, (), "", ()),
    "cyclopts": Fork("cyclopts-salix", "4edba04e1db3478bea551c7c73d426abd59427bd", ".", False, (), "", ()),
    "hydra": Fork("hydra-salix", "d1e07c8f68144ad481aa2a773e6a1f4daa7e5808", ".", True, (), "", ("omegaconf",)),
    "transformers": Fork(
        "transformers-salix",
        "42ca97014c85d71a88ad60d55f08cb9fb4d26e2c",
        "src",
        False,
        ("--index", PYTORCH_CPU_INDEX, "torch"),
        "[torch]",
        (),
    ),
}

PLANS: dict[str, Callable[[dict[str, Sides], Path], Plan]] = {
    "omegaconf": omegaconf_plan,
    "tyro": tyro_plan,
    "cyclopts": cyclopts_plan,
    "hydra": hydra_plan,
    "transformers": transformers_plan,
}


def run(command: Sequence[str | Path], cwd: Path | None = None) -> str:
    return subprocess.run(
        [str(part) for part in command], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout


def pinned_tree(fork: Fork) -> Path:
    tree = VENDOR / fork.submodule
    pin = run(["git", "-C", HERE, "ls-tree", "HEAD", f"vendor/{fork.submodule}"]).split()[2]
    head = run(["git", "-C", tree, "rev-parse", "HEAD"]).strip()
    if head != pin:
        raise SystemExit(f"{tree} is at {head}, not the pinned {pin}: run git submodule update --init")
    run(["git", "-C", tree, "merge-base", "--is-ancestor", fork.base, pin])
    return tree


@contextmanager
def base_worktree(fork: Fork, scratch: Path) -> Iterator[Path]:
    tree = scratch / "stock" / fork.submodule
    run(["git", "-C", VENDOR / fork.submodule, "worktree", "add", "--detach", tree, fork.base])
    try:
        yield tree
    finally:
        run(["git", "-C", VENDOR / fork.submodule, "worktree", "remove", "--force", tree])


def java_command() -> tuple[str, ...]:
    return () if shutil.which("java") else ("nix", "shell", "nixpkgs#jdk17", "--command")


def generate_parsers(tree: Path, python: str) -> None:
    run(
        [*java_command(), "uv", "run", "--no-project", "--python", python, "--with", "setuptools", "python", "setup.py", "antlr"],
        cwd=tree,
    )


def build_venv(name: str, stock_tree: Path, wheel: Path, python: str, scratch: Path) -> Path:
    fork = FORKS[name]
    venv = scratch / "venv" / name
    install = ("uv", "pip", "install", "--python", venv, "--exclude-newer", EXCLUDE_NEWER)
    run(["uv", "venv", "--python", python, venv])
    if fork.preinstall:
        run([*install, *fork.preinstall])
    run([*install, f"{stock_tree}{fork.install_extra}"])
    run([*install, "--reinstall", "--no-deps", wheel])
    return venv / "bin" / "python"


def environment(python_path: tuple[Path, ...]) -> dict[str, str]:
    return {"PYTHONPATH": ":".join(map(str, python_path)), "PATH": "/usr/bin:/bin"}


def variant_specification(variant: Variant) -> str:
    return "|".join(
        (variant.label, ":".join(map(str, variant.python_path)), str(HERE / variant.script), *variant.arguments)
    )


def measure(row: Row, python: Path, workdir: Path, out: Path) -> None:
    table = run(
        [sys.executable, HERE / "interleave.py", python, str(row.rounds), *map(variant_specification, row.variants)],
        cwd=workdir,
    )
    (out / f"{row.output}.txt").write_text(f"### {row.title}\n{table}")
    print(f"### {row.title}\n{table}", flush=True)


def gate(name: str, python: Path, workload: Variant, workdir: Path, out: Path) -> None:
    fork = FORKS[name]
    tree = VENDOR / fork.submodule
    completed = subprocess.run(
        [
            sys.executable,
            HERE / "fork_gate.py",
            python,
            ":".join(map(str, workload.python_path)),
            tree / fork.source_root / name,
            tree,
            name,
            HERE / workload.script,
            *workload.arguments,
        ],
        cwd=workdir,
        capture_output=True,
        text=True,
    )
    (out / f"{name}-gate.txt").write_text(completed.stdout + completed.stderr)
    print(f"{name}: {(completed.stdout or completed.stderr).strip().splitlines()[-1]}", flush=True)
    if completed.returncode != 0:
        raise SystemExit(f"{name}: the fork gate failed, see {out / f'{name}-gate.txt'}")


def provenance(names: Sequence[str], wheel: Path, pythons: dict[str, Path]) -> str:
    return "\n".join(
        (
            f"salix {run(['git', '-C', HERE, 'rev-parse', 'HEAD']).strip()}",
            f"wheel {wheel.name} sha256 {hashlib.sha256(wheel.read_bytes()).hexdigest()}",
            f"host {platform.platform()}",
            f"dependencies resolved with --exclude-newer {EXCLUDE_NEWER}",
            *(
                f"{name} fork {run(['git', '-C', VENDOR / FORKS[name].submodule, 'rev-parse', 'HEAD']).strip()}"
                f" base {FORKS[name].base}"
                for name in names
            ),
            *(
                f"\n[{name}] {run([python, '--version']).strip()}\n{run(['uv', 'pip', 'freeze', '--python', python])}"
                for name, python in pythons.items()
            ),
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--salix-wheel", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--python", default="3.13")
    parser.add_argument("--only", nargs="+", choices=sorted(FORKS), default=sorted(FORKS))
    arguments = parser.parse_args()
    selected: list[str] = arguments.only
    names = sorted({*selected, *(read for name in selected for read in FORKS[name].reads_forks)})
    wheel = arguments.salix_wheel.resolve()
    out = arguments.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary, ExitStack() as stack:
        scratch = Path(temporary)
        trees = {
            name: Sides(stack.enter_context(base_worktree(FORKS[name], scratch)), pinned_tree(FORKS[name]))
            for name in names
        }
        for name in names:
            if FORKS[name].generates_parsers:
                generate_parsers(trees[name].stock, arguments.python)
                generate_parsers(trees[name].fork, arguments.python)
        sources = {
            name: Sides(sides.stock / FORKS[name].source_root, sides.fork / FORKS[name].source_root)
            for name, sides in trees.items()
        }
        pythons = {name: build_venv(name, trees[name].stock, wheel, arguments.python, scratch) for name in selected}
        (out / "provenance.txt").write_text(provenance(names, wheel, pythons))
        plans = {name: PLANS[name](sources, scratch / "inputs" / name) for name in selected}
        for name, plan in plans.items():
            (scratch / "inputs" / name).mkdir(parents=True)
            for generation in plan.generations:
                subprocess.run(
                    [pythons[name], HERE / generation.script, scratch / "inputs" / name],
                    env=environment(generation.python_path),
                    check=True,
                )
            gate(name, pythons[name], plan.gate, scratch, out)
        for name, plan in plans.items():
            for row in plan.rows:
                measure(row, pythons[name], scratch, out)


if __name__ == "__main__":
    main()
