class HostileKey(str):
    __hash__ = str.__hash__

    def __eq__(self, other: object) -> bool:
        raise ValueError("nope")


result = {"hostile_key": HostileKey}
