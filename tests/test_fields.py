import copy
import gc
import pickle
import sys
import types
import weakref
from dataclasses import InitVar
from typing import Annotated, ClassVar, Generic, TypeVar

import pytest
from platforms import printed_by_a_fresh_interpreter, starts_a_subprocess

from salix import MISSING, Field, Struct, fields

META = type(Struct)
T = TypeVar("T")

ATTRIBUTES = (
    "name",
    "type",
    "default",
    "default_factory",
    "init",
    "repr",
    "hash",
    "compare",
    "metadata",
    "kw_only",
    "annotated_metadata",
)


class Point(Struct):
    x: int
    y: str = "y"


class Spatial(Point):
    z: float = 0.0


class Described(Struct):
    plain: int
    described: Annotated[str, "the help", 3] = "d"


class WithSpecialForms(Struct):
    kept: int
    limit: ClassVar[int] = 10
    seed: InitVar[int] = 0

    def __post_init__(self, seed: int) -> None:
        pass


class Box(Struct):
    items: list = []  # noqa: RUF012


class Ok(Struct, Generic[T]):
    value: T


class CodeError(Exception, Struct, frozen=True):
    code: int
    detail: str = ""


class Holder:
    pass


def names(class_or_instance: object) -> tuple[str, ...]:
    return tuple(record.name for record in fields(class_or_instance))


def test_a_class_and_its_instance_read_the_same_records():
    assert fields(Point) is fields(Point(1))


def test_the_records_follow_the_constructor_order_with_base_fields_first():
    assert names(Spatial) == ("x", "y", "z") == Spatial._struct_fields_


def test_a_field_without_a_default_reads_missing_and_a_default_is_the_stored_object():
    assert [record.default for record in fields(Spatial)] == [MISSING, "y", 0.0]
    assert fields(Box)[0].default is Box._struct_defaults_[0]


def test_the_flags_report_a_field_salix_has_no_options_for():
    for record in fields(Spatial):
        assert record.default_factory is MISSING
        assert record.init is True
        assert record.repr is True
        assert record.hash is None
        assert record.compare is True
        assert record.kw_only is False


def test_metadata_is_an_empty_read_only_mapping():
    metadata = fields(Point)[0].metadata

    assert type(metadata) is types.MappingProxyType
    assert metadata == {}

    with pytest.raises(TypeError):
        metadata["help"] = "written"  # type: ignore[index]


def test_the_annotated_extras_are_the_annotated_metadata_and_the_type_is_the_base():
    plain, described = fields(Described)

    assert (plain.type, plain.annotated_metadata) == (int, ())
    assert (described.type, described.annotated_metadata) == (str, ("the help", 3))
    assert described.annotated_metadata is Described._struct_metadata_[1]


def test_class_variables_and_init_variables_are_not_fields():
    assert names(WithSpecialForms) == ("kept",)


def test_the_struct_base_has_no_fields():
    assert fields(Struct) == ()


@pytest.mark.parametrize("not_a_struct", [1, object, object(), META, int, Ok[int]])
def test_anything_but_a_struct_class_or_instance_is_refused(not_a_struct: object):
    with pytest.raises(TypeError, match=r"fields\(\) expects a struct class or instance"):
        fields(not_a_struct)  # type: ignore[arg-type]


def test_the_refusal_names_the_class_or_the_instance_s_type():
    with pytest.raises(TypeError, match=r"not the class int$"):
        fields(int)  # type: ignore[arg-type]

    with pytest.raises(TypeError, match=r"not an instance of int$"):
        fields(1)  # type: ignore[arg-type]


def test_a_hook_reading_fields_during_class_creation_is_refused_and_caches_nothing():
    refusals: list[str] = []

    class Base(Struct):
        def __init_subclass__(cls, **keywords: object) -> None:
            super().__init_subclass__(**keywords)

            try:
                fields(cls)
            except TypeError as refusal:
                refusals.append(str(refusal))

    class Late(Base):
        x: int
        y: str = "y"

    assert refusals == ["fields() cannot read 'Late' until its class has finished being created"]
    assert [(record.name, record.default) for record in fields(Late)] == [("x", MISSING), ("y", "y")]


def test_a_generic_field_reads_its_type_variable():
    assert fields(Ok)[0].type is T


def test_an_exception_struct_reads_its_fields():
    assert names(CodeError) == ("code", "detail")
    assert [record.default for record in fields(CodeError(7))] == [MISSING, ""]


@pytest.mark.parametrize("attribute", ATTRIBUTES)
def test_every_attribute_is_read_only(attribute: str):
    record = fields(Point)[0]

    with pytest.raises(AttributeError):
        setattr(record, attribute, None)

    with pytest.raises(AttributeError):
        delattr(record, attribute)


def test_neither_a_field_nor_missing_can_be_instantiated():
    with pytest.raises(TypeError, match=r"cannot create 'salix\.Field' instances"):
        Field()

    with pytest.raises(TypeError, match=r"cannot create 'salix\._MissingType' instances"):
        type(MISSING)()


def test_replace_derives_a_changed_field_and_leaves_the_original():
    original = fields(Point)[0]
    changed = original.__replace__(type=str, default=1)

    assert (changed.name, changed.type, changed.default) == ("x", str, 1)
    assert (original.type, original.default) == (int, MISSING)
    assert fields(Point)[0] is original


@pytest.mark.skipif(sys.version_info < (3, 13), reason="copy.replace arrived in 3.13")
def test_copy_replace_reaches_the_field_replace():
    assert copy.replace(fields(Point)[0], name="renamed").name == "renamed"


def test_replace_refuses_an_unknown_name_and_a_positional():
    record = fields(Point)[0]

    with pytest.raises(TypeError, match="unexpected keyword argument 'nope'"):
        record.__replace__(nope=1)

    with pytest.raises(TypeError, match="takes no positional arguments"):
        record.__replace__(1)  # type: ignore[call-arg]


def test_missing_is_itself_through_copy_and_pickle():
    assert repr(MISSING) == "MISSING"
    assert copy.copy(MISSING) is MISSING
    assert copy.deepcopy(MISSING) is MISSING
    assert pickle.loads(pickle.dumps(MISSING)) is MISSING


def test_the_repr_names_every_attribute_in_order():
    assert repr(fields(Point)[0]) == (
        "Field(name='x', type=<class 'int'>, default=MISSING, default_factory=MISSING, "
        "init=True, repr=True, hash=None, compare=True, metadata=mappingproxy({}), "
        "kw_only=False, annotated_metadata=())"
    )


def test_records_read_inside_a_delegate_do_not_outlive_the_settle():
    read_before_the_settle: list[list[object]] = []

    class Reading(META):
        def __new__(metacls, name, bases, namespace, **keywords):
            built = super().__new__(metacls, name, bases, namespace, **keywords)
            read_before_the_settle.append([record.default for record in fields(built)])

            return built

    class Base(Struct, metaclass=Reading):
        x: int

    built = META("Built", (Base,), {"__annotations__": {"y": int}, "y": 42})

    assert read_before_the_settle[-1] == [MISSING, MISSING]
    assert [record.default for record in fields(built)] == [MISSING, 42]


def test_a_class_its_records_reach_back_to_is_collected():
    holder = Holder()
    built = META("Cyclic", (Struct,), {"__annotations__": {"held": object}, "held": holder})
    holder.cls = built  # type: ignore[attr-defined]
    fields(built)
    collected = weakref.ref(built)

    del built, holder
    gc.collect()

    assert collected() is None


@starts_a_subprocess
def test_fields_still_answer_after_the_module_leaves_sys_modules():
    assert printed_by_a_fresh_interpreter("""
        import gc, sys
        import salix
        fields, missing = salix.fields, salix.MISSING
        class Point(salix.Struct):
            x: int
        del sys.modules['salix'], salix
        gc.collect()
        print(fields(Point)[0].default is missing)
    """) == "True"


@starts_a_subprocess
def test_reading_fields_imports_nothing():
    assert printed_by_a_fresh_interpreter("""
        import sys
        before = set(sys.modules)
        import salix
        class Point(salix.Struct):
            x: int
            y: str = 'y'
        salix.fields(Point)
        salix.fields(Point(1))
        print(sorted(set(sys.modules) - before))
    """) == "['salix']"
