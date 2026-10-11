import sys
from pathlib import Path

out = Path(sys.argv[1])
(out / "conf").mkdir(exist_ok=True)
(out / "conf" / "config.yaml").write_text(
    "defaults:\n  - root\n  - _self_\n\n" + (out / "big.yaml").read_text()
)
