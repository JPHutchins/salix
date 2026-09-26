import inspect
import pickle
import sys

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
    code: int = 0


def test_an_exception_family_init_owns_construction():
    """A family init that populates C members (OSError's errno/strerror) is
    the construction's owner; the field constructor does not displace it."""

    error = FileError(2, "msg")

    assert error.errno == 2
    assert error.strerror == "msg"
    assert error.code == 0
    assert str(error) == "[Errno 2] msg"

    single = FileError("plain")

    assert single.errno is None
    assert single.strerror is None
    assert str(single) == "plain"


class SE(SyntaxError, Struct, frozen=True):
    detail: str = ""


def test_the_syntax_error_family_init_owns_construction():
    error = SE("hello")

    assert error.msg == "hello"
    assert str(error) == "hello"
    assert error.args == ("hello",)
    assert error.detail == ""


class UDE(UnicodeDecodeError, Struct, frozen=True):
    x: int = 0


def test_the_unicode_decode_error_family_init_owns_construction():
    error = UDE("ascii", b"x", 0, 1, "why")

    assert error.encoding == "ascii"
    assert error.object == b"x"
    assert error.start == 0
    assert error.end == 1
    assert error.reason == "why"
    assert error.x == 0

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


class PlainMid(Exception):
    pass


class LateAuthor(Exception):
    def __init__(self, value: int) -> None:
        self.value = value * 3


class BehindPlain(PlainMid, LateAuthor, Struct, frozen=False):
    v: int = 0


def test_an_author_init_behind_a_plain_exception_base_is_not_displaced():
    instance = BehindPlain(5)

    assert instance.value == 15
    assert instance.v == 0


def test_copy_arms_keep_the_source_payload():
    import copy

    import salix

    error = CodeError(7)

    assert salix.replace(error, code=5).args == (5,)
    assert salix.from_mapping(CodeError, {"code": 5}).args == (5,)
    assert copy.copy(error).args == (7,)
    assert copy.deepcopy(error).args == (7,)


class ListError(Exception, Struct, frozen=True):
    items: list


def test_deepcopy_detaches_mutable_payload_items():
    import copy

    error = ListError([1, 2])
    copied = copy.deepcopy(error)

    assert copied.items == [1, 2]
    assert copied.args == ([1, 2],)
    assert copied.args is not error.args
    assert copied.args[0] is not error.args[0]


class MutableCode(Exception, Struct, frozen=False):
    code: int
    detail: str = ""


def test_replace_carries_the_payload_through_non_leading_changes():
    import salix

    error = MutableCode(7)

    assert salix.replace(error, detail="e").args == (7, "e")
    assert salix.replace(error).args == (7,)


class OwnInit(Exception, Struct, frozen=False):
    x: int = 0

    def __init__(self, *args, **kwargs):
        if args:
            self.x = args[0]

        if "x" in kwargs:
            self.x = kwargs["x"]


def test_replace_reconstructs_an_author_init_with_the_field_keywords():
    import salix

    replaced = salix.replace(OwnInit(3), x=5)

    assert replaced.x == 5
    assert replaced.args == ()


class NewRefuser(Exception):
    def __new__(cls, *args, **kwargs):
        raise TypeError("refused by the author")


class Refused(NewRefuser, Struct, frozen=False):
    x: int = 0


def test_an_inherited_author_new_keeps_its_type_error():
    with pytest.raises(TypeError, match="refused by the author"):
        Refused(1)


class OwnNew(Exception, Struct, frozen=False):
    x: int = 0

    def __init__(self, value: int) -> None:
        self.x = value

    def __new__(cls, *args, **kwargs):
        instance = super().__new__(cls, *args, **kwargs)
        instance.built_by_new = True
        return instance


def test_an_author_new_on_the_own_init_arm_runs():
    assert OwnNew(1).built_by_new is True


class Factory(Exception, Struct, frozen=False):
    x: int = 0

    def __new__(cls, *args, **kwargs):
        return object()


def test_an_author_new_returning_a_non_instance_is_refused():
    with pytest.raises(TypeError, match="is not safe"):
        Factory(1)


class BaseFactory(Exception):
    def __new__(cls, *args, **kwargs):
        return object()


class FactoryDerived(BaseFactory, Struct, frozen=False):
    x: int = 0

    def __init__(self, *args, **kwargs):
        pass


def test_a_base_author_new_returning_a_non_instance_is_refused():
    with pytest.raises(TypeError, match="is not safe"):
        FactoryDerived(1)


class SigNone(Exception):
    __signature__ = None


class WithSigNone(SigNone, Struct, frozen=False):
    x: int = 0


def test_a_none_signature_on_an_exception_ancestor_keeps_the_field_signature():
    assert str(inspect.signature(WithSigNone)) == "(x: int = 0)"


class Gaps(Exception, Struct, frozen=True):
    a: int = 1
    b: int = 2


def test_a_gap_default_stays_out_of_args():
    assert Gaps(b=5).args == ()


if sys.version_info >= (3, 11):
    from builtins import ExceptionGroup

    class EG(ExceptionGroup, Struct, frozen=False):
        x: int = 0


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_an_exception_group_struct_constructs_through_the_alloc_fallback():
    """The call shape BaseExceptionGroup.__new__ rejects falls back to the
    allocation with an empty group body: the fields bind, str() and the
    group operations see an empty group."""

    error = EG("boom")

    assert error.x == "boom"
    assert error.args == ("boom",)
    assert str(error) == "boom (0 sub-exception)"


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_the_fallback_group_message_mirrors_the_supplied_value():
    assert str(EG(5)) == "5 (0 sub-exception)"
    assert str(EG(x=5)) == "5 (0 sub-exception)"


if sys.version_info >= (3, 11):
    class HookEG(ExceptionGroup, Struct, frozen=False):
        x: int = 0

        def __post_init__(self) -> None:
            repr(self)


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_the_fallback_arm_writes_args_before_the_hook():
    error = HookEG(1)

    assert error.x == 1
    assert error.args == (1,)


if sys.version_info >= (3, 11):
    from builtins import ExceptionGroup

    class EG2(ExceptionGroup, Struct, frozen=False):
        message: str
        exceptions: list


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_an_exception_group_struct_constructs_through_the_family_new():
    error = EG2("boom", [ValueError()])

    assert error.message == "boom"
    assert len(error.exceptions) == 1
    assert isinstance(error.exceptions[0], ValueError)


class KwNew(Exception, Struct, frozen=False):
    x: int = 0

    def __new__(cls, *args, **kwargs):
        instance = super().__new__(cls, *args, **kwargs)
        instance.seen = kwargs
        return instance


def test_the_family_new_receives_the_call_keywords():
    error = KwNew(x=1)

    assert error.seen == {"x": 1}
    assert error.x == 1


def test_copy_arms_carry_the_family_members():
    import copy

    file_error = FileError(2, "msg")
    syntax_error = SE("hello")
    decode_error = UDE("ascii", b"x", 0, 1, "why")

    for copier in (copy.copy, copy.deepcopy):
        assert copier(file_error).errno == 2
        assert copier(file_error).strerror == "msg"
        assert copier(syntax_error).msg == "hello"
        assert copier(decode_error).encoding == "ascii"


def test_replace_and_from_mapping_run_on_family_owned_structs():
    import salix

    assert salix.replace(FileError(2, "msg"), code=9).code == 9
    assert salix.replace(SE("hello"), detail="d").detail == "d"
    assert salix.from_mapping(FileError, {"code": 9}).code == 9


class FieldNamedAuthor(Exception, Struct, frozen=False):
    msg: str

    def __init__(self, msg):
        self.msg = msg


def test_replace_on_a_field_named_author_parameter_binds_once():
    import salix

    assert salix.replace(FieldNamedAuthor("a"), msg="b").msg == "b"


if sys.version_info >= (3, 11):
    class NewMixin:
        def __new__(cls, *args, **kwargs):
            return super().__new__(cls)

    class EGX(ExceptionGroup, NewMixin, Struct, frozen=False):
        x: int = 0


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_a_new_defining_mixin_does_not_disable_the_group_fallback():
    error = EGX("boom")

    assert error.x == "boom"
    assert str(error) == "boom (0 sub-exception)"


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_group_copy_arms_carry_the_members():
    import copy

    import salix

    error = EG("boom")

    assert str(copy.copy(error)) == "boom (0 sub-exception)"
    assert str(copy.deepcopy(error)) == "boom (0 sub-exception)"
    assert str(salix.replace(error, x=2)) == "boom (0 sub-exception)"
    assert str(salix.from_mapping(EG, {"x": 2})) == "2 (0 sub-exception)"


class BodyNew(Struct, frozen=False):
    a: int
    b: int = 5

    def __new__(cls, *args, **kwargs):
        return super().__new__(cls)

    def __init__(self, a):
        self.a = a


def test_a_body_new_keeps_the_defaults_and_the_required_validation():
    instance = BodyNew(1)

    assert instance.a == 1
    assert instance.b == 5

    with pytest.raises(TypeError):
        BodyNew()


class Uninstantiable(Exception, Struct, frozen=False):
    x: int = 0
    __new__ = None


def test_a_none_new_is_the_cannot_create_refusal():
    with pytest.raises(TypeError, match="cannot create"):
        Uninstantiable(1)


class MutableFileError(OSError, Struct, frozen=False):
    code: int = 0


def test_copy_arms_keep_family_field_mutations():
    import copy

    import salix

    error = MutableFileError(2, "msg")
    error.code = 9

    assert copy.copy(error).code == 9
    assert copy.deepcopy(error).code == 9
    assert salix.replace(error).code == 9
    assert salix.replace(error, code=5).code == 5


class SubclassBase(Struct, frozen=False):
    a: int
    b: int = 5

    def __init__(self, a):
        self.a = a


class SubclassNew(SubclassBase):
    def __new__(cls, *args, **kwargs):
        return super().__new__(cls)


class SubclassInit(SubclassNew):
    def __init__(self, a):
        self.a = a


class SubclassPlain(SubclassNew):
    pass


def test_subclasses_of_own_init_structs_construct():
    assert SubclassNew(1).b == 5
    assert SubclassInit(1).a == 1
    assert SubclassPlain(1).a == 1


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_from_mapping_carries_the_group_body():
    import salix

    error = salix.from_mapping(EG2, {"message": "m", "exceptions": [ValueError("q")]})

    assert len(error.exceptions) == 1
    assert str(error) == "m (1 sub-exception)"


class FrozenNoneNew(Exception, Struct, frozen=True):
    __new__ = None


def test_a_frozen_none_new_struct_defines_but_does_not_construct():
    assert FrozenNoneNew.__name__ == "FrozenNoneNew"

    with pytest.raises(TypeError, match="cannot create"):
        FrozenNoneNew()


class PlainNoneNew(Struct):
    x: int = 0
    __new__ = None


def test_a_plain_none_new_struct_refuses_construction():
    with pytest.raises(TypeError, match="cannot create"):
        PlainNoneNew(5)


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_from_mapping_refuses_a_non_sequence_group_body():
    import salix

    with pytest.raises(TypeError):
        salix.from_mapping(EG2, {"message": "m", "exceptions": 5})


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_keyword_construction_carries_the_group_body():
    error = EG2(message="m", exceptions=[ValueError("q")])

    assert len(error.exceptions) == 1
    assert str(error) == "m (1 sub-exception)"


if sys.version_info >= (3, 11):
    class OwnInitGroup(ExceptionGroup, Struct, frozen=False):
        message: str
        exceptions: list

        def __init__(self, message, exceptions):
            self.message = message
            self.exceptions = exceptions


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_replace_runs_on_an_own_init_group_struct():
    import salix

    replaced = salix.replace(OwnInitGroup("m", [ValueError()]), message="n")

    assert replaced.message == "n"
    assert str(replaced) == "m (1 sub-exception)"


def test_a_from_mapping_built_family_struct_copies_without_fabricated_members():
    import copy

    import salix

    error = salix.from_mapping(FileError, {"code": 9})

    assert error.args == ()
    assert copy.copy(error).errno is None
    assert copy.deepcopy(error).errno is None
    assert salix.replace(error, code=5).errno is None


def test_a_replaced_from_mapping_built_family_struct_formats_and_pickles():
    import pickle

    import salix

    error = salix.from_mapping(FileError, {"code": 9})
    replaced = salix.replace(error, code=5)

    assert replaced.errno is None
    assert replaced.args == ()
    assert str(replaced) == str(error)
    assert repr(replaced) == repr(error)
    assert pickle.loads(pickle.dumps(replaced)).errno is None


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_keyword_construction_validates_the_group_body():
    for body in (["hello"], "xyz"):
        with pytest.raises(ValueError, match="Item 0 of second argument"):
            EG2(message="m", exceptions=body)


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_from_mapping_validates_the_group_body():
    import salix

    with pytest.raises(ValueError, match="Item 0 of second argument"):
        salix.from_mapping(EG2, {"message": "m", "exceptions": ["hello"]})


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_replace_changes_the_raised_group_body():
    import salix

    for group in (EG2("m", [ValueError("q")]), OwnInitGroup("m", [ValueError("q")])):
        replaced = salix.replace(group, exceptions=[ValueError("z")])

        assert str(replaced) == "m (1 sub-exception)"
        assert replaced.subgroup(ValueError).exceptions[0].args == ("z",)

        try:
            raise replaced
        except ExceptionGroup as caught:
            assert caught.exceptions[0].args == ("z",)


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup exists from 3.11")
def test_the_group_message_comes_from_the_bound_field_not_the_call_order():
    error = EG2(exceptions=[ValueError("q")], message="m")

    assert error.message == "m"
    assert str(error) == "m (1 sub-exception)"
