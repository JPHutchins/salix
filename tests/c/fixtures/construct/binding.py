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


calls: list[int] = []

result = {
    "point": Point,
    "logged": Logged,
    "refused": Refused,
    "calls": calls,
    "x": "x",
    "y": "y",
    "z": "z",
    "keyword_y": ("y",),
    "first": 1,
    "second": 2,
}
