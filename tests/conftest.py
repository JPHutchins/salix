import importlib.util
import sys
from pathlib import Path

if importlib.util.find_spec("salix") is None:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

if sys.version_info < (3, 12):
    collect_ignore = ["test_generics_pep695.py"]
