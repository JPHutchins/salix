import sys
from pathlib import Path

COMMANDS = 30

PARAMETERS = (
    ("name", "str", "'anon'", "Name of the target."),
    ("count", "int", "1", "How many times."),
    ("ratio", "float", "0.5", "Mixing ratio."),
    ("verbose", "bool", "False", "Print more."),
    ("mode", "Mode", "Mode.FAST", "Execution mode."),
    ("output", "Path", "Path('out')", "Where to write."),
    ("tags", "list[str]", "None", "Labels to attach."),
    ("limit", "Optional[int]", "None", "Upper bound."),
)

HEADER = (
    "from enum import Enum\n"
    "from pathlib import Path\n"
    "from typing import Annotated, Optional\n"
    "\n"
)

MODE = "\n\nclass Mode(Enum):\n    FAST = 'fast'\n    SAFE = 'safe'\n"


def annotated(kind: str, help_text: str) -> str:
    return f"Annotated[{kind}, Parameter(help={help_text!r})]"


def default_expression(kind: str, default: str) -> str:
    return default if kind != "list[str]" else "None"


def plain_command(index: int) -> str:
    parameters = ",\n    ".join(
        f"{name}: {annotated(kind, help_text)} = {default_expression(kind, default)}"
        for name, kind, default, help_text in PARAMETERS
    )
    return (
        f"\n\n@app.command\ndef cmd{index:02d}(\n    {parameters},\n) -> tuple[str, int]:\n"
        f'    """Command number {index} that does something useful.\n\n'
        f"    Longer description of command {index}, spanning a couple of sentences\n"
        f'    so that help rendering has realistic text to wrap.\n    """\n'
        f"    return (name, count)\n"
    )


def config_class(index: int, struct: bool) -> str:
    fields = "".join(
        f"    {name}: {annotated(kind, help_text)}{field_default(name, kind, default, struct)}\n"
        for name, kind, default, help_text in PARAMETERS
    )
    header = (
        f"class Config{index:02d}(Struct):"
        if struct
        else f"@dataclass\nclass Config{index:02d}:"
    )
    return f"\n\n{header}\n{fields}"


def field_default(name: str, kind: str, default: str, struct: bool) -> str:
    return "" if name == "name" else f" = {factory_default(kind, default, struct)}"


def factory_default(kind: str, default: str, struct: bool) -> str:
    match (kind, struct):
        case ("list[str]", True):
            return "[]"
        case ("list[str]", False):
            return "field(default_factory=list)"
        case _:
            return default


def schema(struct: bool) -> str:
    imports = (
        "from salix import Struct\n"
        if struct
        else "from dataclasses import dataclass, field\n"
    )
    return (
        HEADER
        + imports
        + "from cyclopts import Parameter\n"
        + MODE
        + "".join(config_class(index, struct) for index in range(COMMANDS))
    )


def grouped_command(index: int) -> str:
    return (
        f"\n\n@app.command\ndef cmd{index:02d}(\n"
        f"    config: Annotated[Config{index:02d}, Parameter(name='*')],\n"
        f"    *,\n    dry_run: Annotated[bool, Parameter(help='Do not act.')] = False,\n"
        f") -> tuple[str, int]:\n"
        f'    """Command number {index} that does something useful.\n\n'
        f"    Longer description of command {index}, spanning a couple of sentences\n"
        f'    so that help rendering has realistic text to wrap.\n    """\n'
        f"    return (config.name, config.count)\n"
    )


def plain_app() -> str:
    return (
        HEADER
        + "from cyclopts import App, Parameter\n"
        + MODE
        + "\napp = App(name='bench', help='A benchmark application.')\n"
        + "".join(plain_command(index) for index in range(COMMANDS))
    )


def grouped_app(schema_module: str) -> str:
    return (
        HEADER
        + "from cyclopts import App, Parameter\n"
        + f"from {schema_module} import {', '.join(f'Config{index:02d}' for index in range(COMMANDS))}\n"
        + "\napp = App(name='bench', help='A benchmark application.')\n"
        + "".join(grouped_command(index) for index in range(COMMANDS))
    )


out = Path(sys.argv[1])
(out / "cli_plain.py").write_text(plain_app())
(out / "cli_schema_dataclass.py").write_text(schema(struct=False))
(out / "cli_schema_salix.py").write_text(schema(struct=True))
(out / "cli_grouped_dataclass.py").write_text(grouped_app("cli_schema_dataclass"))
(out / "cli_grouped_salix.py").write_text(grouped_app("cli_schema_salix"))
