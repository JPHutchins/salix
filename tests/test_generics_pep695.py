from salix import Struct


def test_a_pep_695_generic_struct_constructs_and_matches():
    class Ok[T](Struct):
        value: T

    ok = Ok(3)

    assert ok.value == 3
    assert Ok.__struct_fields__ == ("value",)
    assert Ok.__match_args__ == ("value",)
    assert Ok(3) == Ok(3)
    assert hash(Ok(3)) == hash((3,))

    match Ok("x"):
        case Ok(v):
            assert v == "x"
        case _:
            raise AssertionError


def test_a_pep_695_generic_struct_can_be_subscripted_and_inherited():
    class Ok[T](Struct):
        value: T

    assert Ok[int](3).value == 3

    class Tagged[T](Ok[T]):
        tag: str = ""

    assert Tagged.__struct_fields__ == ("value", "tag")
    assert Tagged[int](3, "t").value == 3
    assert Tagged[int](3, "t").tag == "t"
