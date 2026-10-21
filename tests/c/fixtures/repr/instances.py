from salix import Struct


class P(Struct):
    x: int
    y: int


class Q(Struct):
    x: int


result = {
    "pair": P(1, 2),
    "text": Q("value"),
}
