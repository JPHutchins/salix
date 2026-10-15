import copy
import importlib
import pickle
import sys
import timeit
from typing import Any

OmegaConf = importlib.import_module("omegaconf").OmegaConf


if sys.argv[1] == "salix":
    from salix import Struct

    class User(Struct, frozen=False):
        name: str = "bond"
        age: int = 7

    class Server(Struct, frozen=False):
        host: str = "localhost"
        port: int = 8080
        admin: User = User()
        tags: dict[str, str] = {}  # noqa: RUF012

else:
    import dataclasses

    @dataclasses.dataclass
    class User:  # type: ignore[no-redef]
        name: str = "bond"
        age: int = 7

    @dataclasses.dataclass
    class Server:  # type: ignore[no-redef]
        host: str = "localhost"
        port: int = 8080
        admin: User = dataclasses.field(default_factory=User)
        tags: dict[str, str] = dataclasses.field(default_factory=dict)


SERVER = Server
NESTED = {"a": 1, "b": {"c": [1, 2, 3], "d": {"e": "f"}}}
config = OmegaConf.create(NESTED)
structured = OmegaConf.structured(SERVER)
large = OmegaConf.create({f"k{index}": {"v": [index] * 10} for index in range(200)})


def report(label: str, statement: str, number: int) -> None:
    namespace: dict[str, Any] = {
        "OmegaConf": OmegaConf,
        "NESTED": NESTED,
        "SERVER": SERVER,
        "config": config,
        "structured": structured,
        "large": large,
        "copy": copy,
        "pickle": pickle,
    }
    seconds = min(timeit.repeat(statement, globals=namespace, number=number, repeat=3)) / number
    print(f"{label} {seconds * 1e6:.3f} us/iter")


report("create(nested dict)", "OmegaConf.create(NESTED)", 2000)
report("structured(schema)", "OmegaConf.structured(SERVER)", 2000)
report("to_object(structured)", "OmegaConf.to_object(structured)", 2000)
report("to_container(200-key)", "OmegaConf.to_container(large)", 300)
report("deepcopy(config)", "copy.deepcopy(config)", 2000)
report("deepcopy(structured)", "copy.deepcopy(structured)", 2000)
report("pickle round trip", "pickle.loads(pickle.dumps(structured))", 2000)
