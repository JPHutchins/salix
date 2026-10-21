from salix import Struct


class P(Struct):
    alpha: int
    beta: int


class E(Struct):
    pass


result = {"two_field": P(1, 2), "frozen_empty": E()}
