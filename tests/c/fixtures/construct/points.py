from salix import Struct


class Point(Struct):
    x: int
    y: int = 7


class Logged(Struct):
    x: int

    def __post_init__(self) -> None:
        calls.append(self.x)


class Refused(Struct):
    x: int

    def __post_init__(self) -> None:
        raise ValueError(self.x)


calls: list[object] = []
first = object()
second = object()
original_x = object()
original_y = object()

result = {
    "point": Point,
    "logged": Logged,
    "refused": Refused,
    "calls": calls,
    "x": "x",
    "z": "z",
    "keyword_y": ("y",),
    "keyword_z": ("z",),
    "first": first,
    "second": second,
    "replaceable": Point(original_x, original_y),
    "original_x": original_x,
    "original_y": original_y,
    "not_a_struct": object(),
}
