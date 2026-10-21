from platforms import (
    TESTS_DIRECTORY,
    printed_by_a_fresh_interpreter,
    starts_a_subprocess,
)

pytestmark = starts_a_subprocess

WHERE_THE_TEST_HELPERS_ARE = f"""
    import os
    import sys

    standard_library = sys.path.index(os.path.dirname(os.__file__))
    helpers = sys.path.index({TESTS_DIRECTORY!r}) if {TESTS_DIRECTORY!r} in sys.path else None
    print(
        "absent" if helpers is None else
        "after the standard library" if helpers > standard_library else
        "before the standard library"
    )
"""


def test_a_probe_leaves_the_test_helpers_off_its_path_by_default():
    assert printed_by_a_fresh_interpreter(WHERE_THE_TEST_HELPERS_ARE) == "absent"


def test_a_probe_that_asks_for_the_test_helpers_finds_them_after_the_standard_library():
    assert (
        printed_by_a_fresh_interpreter(WHERE_THE_TEST_HELPERS_ARE, imports_test_helpers=True)
        == "after the standard library"
    )
