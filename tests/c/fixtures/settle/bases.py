from salix import Struct


class Left(Struct):
    pass


class Right(Struct):
    pass


class Plain:
    pass


class Later(Struct):
    x: int = 0

    def __eq__(self, other: object) -> bool:
        return True

    __hash__ = None


class Both(Left, Later):
    pass


class Only(Later):
    pass


result = {
    "two_structs": (Left, Right),
    "one_struct": (Plain, Left),
    "no_struct": (Plain, object),
    "both": Both,
    "only": Only,
    "both_pair": (Both(1), Both(2)),
    "only_pair": (Only(1), Only(2)),
}
