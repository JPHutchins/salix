from typing import Protocol

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


class Builder(Protocol):
    def build(self) -> object: ...


class BuilderStructMeta(type(Struct), type(Builder)):
    pass


class Built(Struct, Builder, metaclass=BuilderStructMeta):
    bar: str = "foo"


class BuiltSubclass(Built):
    extra: int = 0


def init(self: object) -> None:
    pass


result = {
    "plain": Plain,
    "authored": Authored,
    "raised": Raised,
    "system_failure": SystemFailure,
    "protocol_placeholder": Built,
    "protocol_subclass": BuiltSubclass,
    "authored_namespace": {"__init__": init},
    "empty_namespace": {},
}
