from builtins import ExceptionGroup

from salix import Struct


class Fieldless(ExceptionGroup, Struct, frozen=False):
    x: int = 0


result = {"fieldless": Fieldless(1)}
