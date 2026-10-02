from builtins import ExceptionGroup

from salix import Struct


class Grouped(ExceptionGroup, Struct, frozen=False):
    message: str
    exceptions: list[Exception]


result = Grouped
