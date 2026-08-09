import importlib.util
import sys
from pathlib import Path

if importlib.util.find_spec("salix") is None:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
