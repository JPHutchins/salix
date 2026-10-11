import sys
from pathlib import Path

SECTIONS = 20

FIELDS = (
    ("count", "int", "1"),
    ("ratio", "float", "0.5"),
    ("name", "str", "'section'"),
    ("enabled", "bool", "True"),
    ("retries", "int", "3"),
    ("scale", "float", "1.0"),
    ("label", "str", "'default'"),
    ("mode", "Literal['fast', 'slow', 'auto']", "'auto'"),
    ("verbose", "bool", "False"),
    ("sizes", "List[int]", "[1, 2, 3]"),
)


def dataclass_field_line(name: str, kind: str, default: str) -> str:
    return (
        f"    {name}: {kind} = field(default_factory=lambda: {default})\n"
        if default.startswith("[")
        else f"    {name}: {kind} = {default}\n"
    )


def dataclass_schema() -> str:
    sections = "".join(
        f"\n\n@dataclass\nclass Section{section:02d}:\n"
        + "".join(dataclass_field_line(*entry) for entry in FIELDS)
        for section in range(SECTIONS)
    )
    root = "\n\n@dataclass\nclass Root:\n" + "".join(
        f"    section{section:02d}: Section{section:02d} = field(default_factory=Section{section:02d})\n"
        for section in range(SECTIONS)
    )
    return (
        "from dataclasses import dataclass, field\nfrom typing import List, Literal\n"
        + sections
        + root
    )


def salix_schema() -> str:
    sections = "".join(
        f"\n\nclass Section{section:02d}(Struct):\n"
        + "".join(f"    {name}: {kind} = {default}\n" for name, kind, default in FIELDS)
        for section in range(SECTIONS)
    )
    root = "\n\nclass Root(Struct):\n" + "".join(
        f"    section{section:02d}: Section{section:02d} = Section{section:02d}()\n"
        for section in range(SECTIONS)
    )
    return "from typing import List, Literal\nfrom salix import Struct\n" + sections + root


out = Path(sys.argv[1])
(out / "schema_dataclass.py").write_text(dataclass_schema())
(out / "schema_salix.py").write_text(salix_schema())
