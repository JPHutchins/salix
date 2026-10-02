from salix import Struct, set_field


class Plain(Struct):
    x: int


class Authored(Struct):
    x: int

    def __init__(self, x: int) -> None:
        set_field(self, "x", x)


class Raised(Exception, Struct, frozen=True):
    message: str


class SystemFailure(OSError, Struct, frozen=True):
    message: str


def init(self: object) -> None:
    pass


result = {
    "plain": Plain,
    "authored": Authored,
    "raised": Raised,
    "system_failure": SystemFailure,
    "authored_namespace": {"__init__": init},
    "empty_namespace": {},
}
