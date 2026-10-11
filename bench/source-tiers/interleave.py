import re
import statistics
import subprocess
import sys
from collections import defaultdict
from typing import NamedTuple

MEASUREMENT = re.compile(r"^(?P<label>.+?)\s+(?P<value>[0-9.]+) (?P<unit>us/iter|ms)$")


class Variant(NamedTuple):
    name: str
    python_path: str
    arguments: tuple[str, ...]


def parse_variant(specification: str) -> Variant:
    name, python_path, *arguments = specification.split("|")
    return Variant(name, python_path, tuple(arguments))


def measurements(python: str, variant: Variant) -> dict[str, tuple[float, str]]:
    completed = subprocess.run(
        [python, *variant.arguments],
        env={"PYTHONPATH": variant.python_path, "PATH": "/usr/bin:/bin"},
        capture_output=True,
        text=True,
        check=True,
    )
    return {
        match["label"].strip(): (float(match["value"]), match["unit"])
        for match in map(MEASUREMENT.match, completed.stdout.splitlines())
        if match
    }


def main(python: str, rounds: int, variants: list[Variant]) -> None:
    samples: dict[str, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    units: dict[str, str] = {}
    for _ in range(rounds):
        for variant in variants:
            for label, (value, unit) in measurements(python, variant).items():
                samples[label][variant.name].append(value)
                units[label] = unit
    print(
        f"{'measurement':28s} "
        + " ".join(f"{variant.name:>22s}" for variant in variants)
        + f"   ratio to {variants[0].name}"
    )
    for label, by_variant in samples.items():
        medians = [statistics.median(by_variant[variant.name]) for variant in variants]
        print(
            f"{label:28s} "
            + " ".join(f"{median:16.1f} {units[label]:>5s}" for median in medians)
            + "   "
            + " ".join(f"{median / medians[0]:5.2f}x" for median in medians[1:])
        )


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), [parse_variant(argument) for argument in sys.argv[3:]])
