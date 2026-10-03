from dataclasses import InitVar

from salix import Struct

first = object()
second = object()
third = object()
default_scale = object()


class Seeded(Struct):
    a: object
    flag: InitVar[object]
    b: object = None
    scale: InitVar[object] = default_scale

    def __post_init__(self, flag: object, scale: object) -> None:
        calls.append((flag, scale))


calls: list[tuple[object, object]] = []

result = {
    "seeded": Seeded,
    "calls": calls,
    "first": first,
    "second": second,
    "third": third,
    "default_scale": default_scale,
    "flag": "flag",
    "scale": "scale",
    "seed": "seed",
}
