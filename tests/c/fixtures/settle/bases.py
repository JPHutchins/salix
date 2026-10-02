from salix import Struct


class Left(Struct):
    x: int


class Right(Struct):
    y: int


class Plain:
    pass


result = {
    "two_structs": (Left, Right),
    "one_struct": (Plain, Left),
    "no_struct": (Plain, object),
}
