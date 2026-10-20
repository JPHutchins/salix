import copy
import pickle
import sysconfig
from collections.abc import Callable

import pytest
from pyrsistent import pmap, pvector

from salix import Struct, replace


def immutables_map(entries: dict[int, int]) -> object:
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        pytest.skip("importing immutables re-enables the GIL on a free-threaded build")

    return pytest.importorskip("immutables").Map(entries)


PERSISTENT_COLLECTIONS = [
    pytest.param(lambda: pvector(range(5)), id="pvector"),
    pytest.param(lambda: pmap({i: i for i in range(5)}), id="pmap"),
    pytest.param(lambda: immutables_map({i: i for i in range(5)}), id="immutables_map"),
]


class Holder(Struct):
    label: str
    items: object


@pytest.mark.parametrize("build", PERSISTENT_COLLECTIONS)
def test_replace_with_an_updated_collection_leaves_the_source_as_it_was(build: Callable[[], object]):
    source = Holder("label", build())
    replaced = replace(source, items=source.items.set(2, -1))

    assert source.items == build()
    assert replaced.items == build().set(2, -1)
    assert replaced.label is source.label


@pytest.mark.parametrize("build", PERSISTENT_COLLECTIONS)
def test_a_struct_holding_a_persistent_collection_compares_and_hashes_by_value(
    build: Callable[[], object],
):
    assert Holder("label", build()) == Holder("label", build())
    assert hash(Holder("label", build())) == hash(Holder("label", build()))


@pytest.mark.parametrize("build", PERSISTENT_COLLECTIONS)
def test_a_persistent_collection_field_survives_pickle_copy_and_deepcopy(build: Callable[[], object]):
    source = Holder("label", build())

    assert pickle.loads(pickle.dumps(source)) == source
    assert copy.copy(source).items is source.items
    assert copy.deepcopy(source) == source


@pytest.mark.parametrize("build", PERSISTENT_COLLECTIONS)
def test_a_persistent_collection_default_is_shared_rather_than_copied(build: Callable[[], object]):
    class WithDefault(Struct):
        items: object = build()

    assert WithDefault().items is WithDefault().items
