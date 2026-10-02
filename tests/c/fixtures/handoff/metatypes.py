class Outer(type):
    pass


class Inner(Outer):
    pass


class FromOuter(metaclass=Outer):
    pass


class FromInner(metaclass=Inner):
    pass


result = {
    "outer": Outer,
    "inner": Inner,
    "from_both": (FromOuter, FromInner),
    "from_object": (object,),
}
