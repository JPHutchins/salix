import sys
from pathlib import Path

SECTIONS = 20
LEAVES = 10

LEAF_FIELDS = (
    ("count", "int", "1"),
    ("ratio", "float", "0.5"),
    ("name", "str", "'leaf'"),
    ("enabled", "bool", "True"),
    ("retries", "int", "3"),
    ("scale", "float", "1.0"),
    ("label", "str", "'default'"),
    ("verbose", "bool", "False"),
)


def leaf_name(section: int, leaf: int) -> str:
    return f"Leaf{section:02d}x{leaf:02d}"


def dataclass_schema() -> str:
    leaves = "".join(
        f"\n\n@dataclass\nclass {leaf_name(section, leaf)}:\n"
        + "".join(f"    {name}: {kind} = {default}\n" for name, kind, default in LEAF_FIELDS)
        + "    sizes: List[int] = field(default_factory=lambda: [1, 2, 3])\n"
        for section in range(SECTIONS)
        for leaf in range(LEAVES)
    )
    sections = "".join(
        f"\n\n@dataclass\nclass Section{section:02d}:\n"
        + "".join(
            f"    leaf{leaf:02d}: {leaf_name(section, leaf)} = field(default_factory={leaf_name(section, leaf)})\n"
            for leaf in range(LEAVES)
        )
        for section in range(SECTIONS)
    )
    root = "\n\n@dataclass\nclass Root:\n" + "".join(
        f"    section{section:02d}: Section{section:02d} = field(default_factory=Section{section:02d})\n"
        for section in range(SECTIONS)
    )
    return "from dataclasses import dataclass, field\nfrom typing import List\n" + leaves + sections + root


def salix_schema() -> str:
    leaves = "".join(
        f"\n\nclass {leaf_name(section, leaf)}(Struct, frozen=False):\n"
        + "".join(f"    {name}: {kind} = {default}\n" for name, kind, default in LEAF_FIELDS)
        + "    sizes: List[int] = [1, 2, 3]\n"
        for section in range(SECTIONS)
        for leaf in range(LEAVES)
    )
    sections = "".join(
        f"\n\nclass Section{section:02d}(Struct, frozen=False):\n"
        + "".join(
            f"    leaf{leaf:02d}: {leaf_name(section, leaf)} = {leaf_name(section, leaf)}()\n"
            for leaf in range(LEAVES)
        )
        for section in range(SECTIONS)
    )
    root = "\n\nclass Root(Struct, frozen=False):\n" + "".join(
        f"    section{section:02d}: Section{section:02d} = Section{section:02d}()\n"
        for section in range(SECTIONS)
    )
    return "from typing import List\nfrom salix import Struct\n" + leaves + sections + root


def overrides_yaml() -> str:
    return "".join(
        f"section{section:02d}:\n"
        + "".join(
            f"  leaf{leaf:02d}:\n"
            f"    count: {section * LEAVES + leaf}\n"
            f"    ratio: {leaf / 10}\n"
            f"    name: name{section}x{leaf}\n"
            f"    enabled: false\n"
            f"    retries: {leaf}\n"
            f"    scale: {section / 4}\n"
            f"    label: label{leaf}\n"
            f"    verbose: true\n"
            f"    sizes: [{section}, {leaf}]\n"
            for leaf in range(LEAVES)
        )
        for section in range(SECTIONS)
    )


out = Path(sys.argv[1])
(out / "schema_dataclass.py").write_text(dataclass_schema())
(out / "schema_salix.py").write_text(salix_schema())
(out / "big.yaml").write_text(overrides_yaml())
