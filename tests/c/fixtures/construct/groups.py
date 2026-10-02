from builtins import ExceptionGroup

from salix import Struct


class Grouped(ExceptionGroup, Struct, frozen=False):
    exceptions: list[Exception]
    message: str


result = {
    "grouped": Grouped,
    "message": object(),
    "exceptions": object(),
}
