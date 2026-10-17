import importlib.util
import sys
from collections.abc import Generator
from pathlib import Path

import pytest
from pytest_markdown_docs.plugin import MarkdownDocstringCodeModule

if importlib.util.find_spec("salix") is None:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

if sys.version_info < (3, 12):
    collect_ignore = ["test_generics_pep695.py"]


@pytest.hookimpl(wrapper=True)
def pytest_collect_file() -> Generator[None, list[pytest.Collector], list[pytest.Collector]]:
    collected = yield
    return [collector for collector in collected if not isinstance(collector, MarkdownDocstringCodeModule)]
