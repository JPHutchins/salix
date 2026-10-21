from salix import Struct


class C(Struct):
    x: int = 1


class D(Struct):
    y: int = 2


result = {"compared": C, "other": D}
