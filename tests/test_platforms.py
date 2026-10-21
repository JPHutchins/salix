import pytest
from platforms import printed_by_a_fresh_interpreter, starts_a_subprocess

pytestmark = starts_a_subprocess


@pytest.mark.parametrize(("imports_test_helpers", "printed"), [(False, "False"), (True, "True")])
def test_a_probe_finds_the_test_helpers_only_when_it_asks_for_them(imports_test_helpers: bool, printed: str):
    assert printed_by_a_fresh_interpreter(
        """
        import importlib.util
        print(importlib.util.find_spec("deepcopy_calls") is not None)
        """,
        imports_test_helpers=imports_test_helpers,
    ) == printed
