from builtins import ExceptionGroup

from salix import Struct


class G(ExceptionGroup, Struct, frozen=False):
    message: str
    exceptions: list


result = {
    "group": G("boom", [ValueError("x")]),
}
