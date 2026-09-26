import inspect
import pickle

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


class FileError(OSError, Struct, frozen=True):
    code: int


def test_an_exception_family_base_gets_the_field_constructor_too():
    """The carve-out used to match only BaseException's own init; OSError and
    its family install their own, so the field constructor was displaced."""

    error = FileError(7)

    assert error.code == 7


def test_an_exception_family_base_takes_keywords():
    error = FileError(code=9)

    assert error.code == 9

def test_the_exception_contract_reads_the_c_level_args():
    """str()/repr()/pickle all read the C-level args member without a NULL
    check; the field constructor must initialize it from the positionals."""

    error = MessageError("boom")

    assert error.args == ("boom",)
    assert str(error) == "boom"
    assert repr(error) == "MessageError('boom')"
    assert pickle.loads(pickle.dumps(error)) == error


class Authored(Exception, Struct, frozen=False):
    x: int

    def __init__(self, value: int) -> None:
        self.x = value * 2


def test_an_author_init_on_the_class_is_not_displaced():
    assert Authored(3).x == 6


class AuthoredBase(OSError):
    def __init__(self, n: int) -> None:
        self.n = n


class Inheriting(AuthoredBase, Struct, frozen=False):
    y: int = 9


def test_an_author_init_on_an_exception_base_is_not_displaced():
    instance = Inheriting(5)

    assert instance.n == 5
    assert instance.y == 9

def test_a_keyword_constructed_exception_pickles_round_trip():
    """The args carry the field values, so __reduce__'s (cls, args)
    reconstructs keyword-constructed instances positionally."""

    error = MessageError(message="kw")

    assert error.args == ("kw",)
    assert str(error) == "kw"
    assert pickle.loads(pickle.dumps(error)) == error


def test_the_own_init_path_initializes_args_from_the_call():
    assert str(Authored(3)) == "3"
    assert Authored(3).args == (3,)


def test_copy_and_deepcopy_carry_the_args():
    import copy

    error = MessageError("boom")

    assert str(copy.copy(error)) == "boom"
    assert str(copy.deepcopy(error)) == "boom"
    assert pickle.loads(pickle.dumps(copy.copy(error))) == error


def test_replace_and_from_mapping_carry_the_args():
    import salix

    error = MessageError("boom")

    assert str(salix.replace(error, message="replaced")) == "replaced"
    assert str(salix.from_mapping(MessageError, {"message": "mapped"})) == "mapped"


class StructFirst(Struct, Exception):
    x: int


def test_a_struct_first_exception_base_gets_the_field_constructor_too():
    instance = StructFirst(1)

    assert instance.x == 1
    assert str(instance) == "1"
    assert str(inspect.signature(StructFirst)) == "(x: int)"
