class Outer(type):
    pass


class Inner(Outer):
    pass


class FromOuter(metaclass=Outer):
    pass


class FromInner(metaclass=Inner):
    pass


result = (Outer, Inner, (FromOuter, FromInner), (object,))
