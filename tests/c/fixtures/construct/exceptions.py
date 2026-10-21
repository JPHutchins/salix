from salix import Struct


class E(Exception, Struct, frozen=True):
    message: str


class P(Struct):
    alpha: int
    beta: int


result = {
    "exception": E("boom"),
    "beta_named": P(1, beta=9),
}
