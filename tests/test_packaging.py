import importlib.metadata
import os
import pathlib

import pytest

import salix

pytestmark = pytest.mark.skipif(
    os.environ.get("SALIX_REQUIRE_INSTALLED") != "1",
    reason="this leg imports the working tree on purpose",
)


def test_the_imported_module_is_the_file_the_distribution_installed():
    """Exact rather than path-shaped: --target installs nowhere near site-packages."""

    distribution = importlib.metadata.distribution("salix")
    installed = {
        pathlib.Path(str(distribution.locate_file(entry))).resolve()
        for entry in distribution.files or ()
    }

    assert pathlib.Path(salix.__file__).resolve() in installed


def test_the_import_did_not_come_from_the_working_tree():
    location = pathlib.Path(salix.__file__).resolve()

    assert not (location.parent / "setup.py").exists(), location
    assert not (location.parent / "build_config.py").exists(), location
