import copy

from salix import Struct


class Pair(Struct):
    x: int
    y: int


result = {
    "pair": Pair(1, 2),
    "rebuilt_pair": Pair(3, 4),
    "tuple_reduction": (Pair, (3, 4)),
    "string_reduction": "pair",
    "copy_module": copy,
    "memo": {},
}
