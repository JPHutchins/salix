from salix import Struct


class Point(Struct):
    x: int
    y: int = 7


result = {
    "point": Point(1, 2),
    "keyword_y": ("y",),
    "keyword_z": ("z",),
    "value": 5,
    "not_a_struct": object(),
}
