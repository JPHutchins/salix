import subprocess
import sys
from pathlib import Path

import pytest

SOURCE_TIERS = Path(__file__).resolve().parent.parent / "bench" / "source-tiers"

pytestmark = [
    pytest.mark.skipif(not SOURCE_TIERS.exists(), reason="bench/ is not beside these tests"),
    pytest.mark.skipif(sys.platform == "win32", reason="the harness runs its children with a POSIX PATH"),
]


def interleave(*variants: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SOURCE_TIERS / "interleave.py"), sys.executable, "1", *variants],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )


def test_interleave_tabulates_each_label_against_the_first_variant(tmp_path):
    (tmp_path / "stock.py").write_text('print("work 2.0 ms")\n')
    (tmp_path / "fork.py").write_text('print("work 1.0 ms")\n')
    completed = interleave(f"stock||{tmp_path / 'stock.py'}", f"fork||{tmp_path / 'fork.py'}")

    assert completed.returncode == 0, completed.stderr
    assert any(line.startswith("work") and " 0.50x" in line for line in completed.stdout.splitlines())


def test_interleave_refuses_a_label_one_variant_never_printed(tmp_path):
    (tmp_path / "stock.py").write_text('print("work 2.0 ms")\nprint("extra 3.0 ms")\n')
    (tmp_path / "fork.py").write_text('print("work 1.0 ms")\n')
    completed = interleave(f"stock||{tmp_path / 'stock.py'}", f"fork||{tmp_path / 'fork.py'}")

    assert completed.returncode != 0
    assert "Traceback" not in completed.stderr
    assert "extra (fork)" in completed.stderr


def gate(tmp_path: Path, fork_source: str) -> subprocess.CompletedProcess[str]:
    library = tmp_path / "library"
    (library / "fakelib").mkdir(parents=True)
    (library / "thirdparty.py").write_text("import dataclasses\n")
    (library / "fakelib" / "__init__.py").write_text(fork_source)
    (tmp_path / "root").mkdir()
    return subprocess.run(
        [
            sys.executable,
            str(SOURCE_TIERS / "fork_gate.py"),
            sys.executable,
            str(library),
            str(library / "fakelib"),
            str(tmp_path / "root"),
            "fakelib",
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )


@pytest.mark.skipif(sys.version_info < (3, 11), reason="fork_gate.py needs tomllib")
def test_the_gate_sees_a_fork_import_of_a_module_a_third_party_loaded_first(tmp_path):
    completed = gate(tmp_path, 'import importlib\nimport thirdparty\nimportlib.import_module("data" + "classes")\n')

    assert completed.returncode == 1
    assert "IMPORTED BY THE FORK dataclasses <- fakelib" in completed.stdout
    assert "IMPORTED BY THIRD PARTY thirdparty: dataclasses" in completed.stdout
    assert "GATE FAIL" in completed.stdout


@pytest.mark.skipif(sys.version_info < (3, 11), reason="fork_gate.py needs tomllib")
def test_the_gate_passes_a_fork_whose_only_import_is_a_third_partys(tmp_path):
    completed = gate(tmp_path, "import thirdparty\n")

    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert "IMPORTED BY THIRD PARTY thirdparty: dataclasses" in completed.stdout
    assert "GATE PASS" in completed.stdout
