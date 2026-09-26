import pytest

from salix import Struct


class MessageError(Exception, Struct, frozen=True):
    message: str


class CodeError(Exception, Struct, frozen=True):
    code: int
    detail: str = ""


def test_the_field_constructor_answers_beside_the_exception_init():
    error = MessageError("boom")

    assert error.message == "boom"


def test_keyword_construction_works():
    error = MessageError(message="boom")

    assert error.message == "boom"


def test_raising_carries_the_field():
    with pytest.raises(MessageError) as caught:
        raise MessageError("raised")

    assert caught.value.message == "raised"


def test_multiple_positionals_follow_field_order():
    error = CodeError(7, "d")

    assert error.code == 7
    assert error.detail == "d"
