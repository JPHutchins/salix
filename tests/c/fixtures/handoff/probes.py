def positional_only(a: int, /, b: int, *, c: int) -> None:
    pass


def takes_every_keyword(**keywords: object) -> None:
    pass


def names_weakref(mcs: type, weakref: bool = False) -> None:
    pass


result = {
    "positional_only": [positional_only],
    "takes_every_keyword": [takes_every_keyword],
    "names_weakref": [names_weakref],
    "written_in_c": [len],
    "named": {"b": True, "c": True},
    "positional": {"a": True},
    "anything": {"anything": True},
}
