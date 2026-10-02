from salix import Struct


class Handing(type(Struct)):
    def __new__(
        mcs,
        name: str,
        bases: tuple[type, ...],
        namespace: dict[str, object],
        **keywords: object,
    ) -> type:
        return super().__new__(mcs, name, bases, namespace, **keywords)


class Plain(type(Struct)):
    pass


result = {
    "handing": Handing,
    "plain": Plain,
    "handing_new": Handing.__new__,
}
