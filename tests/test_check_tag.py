import subprocess
import sys
from pathlib import Path

import pytest

CHECK_TAG = Path(__file__).resolve().parent.parent / "tools" / "check_tag.py"

# wheel-smoke copies tests/ into the nix store on its own, and the release
# invokes the script through `uv run`, which takes 3.14.
pytestmark = [
    pytest.mark.skipif(not CHECK_TAG.exists(), reason="tools/ is not beside these tests"),
    pytest.mark.skipif(sys.version_info < (3, 11), reason="check_tag.py needs tomllib"),
]


def check(tag: str, declared: str, where: Path) -> subprocess.CompletedProcess[str]:
    (where / "pyproject.toml").write_text(f'[project]\nversion = "{declared}"\n')

    return subprocess.run(
        [sys.executable, str(CHECK_TAG), tag],
        cwd=where,
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.mark.parametrize(
    "declared",
    ["1.0.0", "0.1.0", "1.0", "1", "1.0.0rc1", "1.0.0a1", "1.0.0b2", "1.0.0.post1",
     "1.0.0.dev0", "1.0.0rc1.post1.dev2", "1!1.0.0", "0.0.0"],
)
def test_a_canonical_version_matching_its_tag_passes(declared, tmp_path):
    assert check(f"v{declared}", declared, tmp_path).returncode == 0


@pytest.mark.parametrize(
    "declared",
    ["1.0.0-rc1", "1.0.0.rc1", "01.0.0", "1.0.0.RELEASE", "1.0.0-1", "1.0.0.rev1",
     "1.0.0alpha1", "1.0.0RC1", "0!1.0.0", "1.0.0+local", "", "v1.0.0",
     "1.0.0rc01", "1.0.0.post01", "1.0.0.dev01", "1.0.0.dev2.post1", "1.0.0.post1.rc1"],
)
def test_a_version_pypi_would_rename_is_refused(declared, tmp_path):
    result = check(f"v{declared}", declared, tmp_path)

    assert result.returncode != 0
    assert "canonical PEP 440" in result.stderr


def test_a_tag_that_names_another_version_is_refused(tmp_path):
    result = check("v1.0.1", "1.0.0", tmp_path)

    assert result.returncode != 0
    assert "1.0.1" in result.stderr
    assert "1.0.0" in result.stderr


def test_a_tag_without_the_v_is_refused(tmp_path):
    assert check("1.0.0", "1.0.0", tmp_path).returncode != 0


def test_the_pattern_accepts_exactly_what_packaging_calls_canonical():
    """The gate carries a regex instead of importing `packaging`, so that it
    cannot fail on a dependency at the one moment it matters. This is the
    price: the equivalence is asserted here rather than assumed.
    """

    from importlib.util import module_from_spec, spec_from_file_location
    from itertools import product

    from packaging.version import InvalidVersion, Version

    spec = spec_from_file_location("check_tag", CHECK_TAG)
    assert spec is not None and spec.loader is not None
    check_tag = module_from_spec(spec)
    spec.loader.exec_module(check_tag)

    def packaging_calls_it_canonical(version: str) -> bool:
        try:
            return str(Version(version)) == version
        except InvalidVersion:
            return False

    generated = (
        head + pre + post + dev
        for head, pre, post, dev in product(
            ["1", "1.0", "1.0.0", "01.0.0", "1.00", "0!1.0", "1!1.0", "v1.0", "", "x"],
            ["", "a1", "b2", "rc1", "-rc1", ".rc1", "c1", "alpha1", "a01", "RC1"],
            ["", ".post1", "-1", ".post01", ".rev1", "-post1", ".POST1"],
            ["", ".dev0", ".dev", "-dev1", ".DEV0"],
        )
    )

    assert [
        version
        for version in generated
        if bool(check_tag.CANONICAL_VERSION.fullmatch(version))
        != packaging_calls_it_canonical(version)
    ] == []


@pytest.mark.parametrize("declared", ["1.0", "1", "[\"1.0.0\"]", "true"])
def test_a_version_that_is_not_a_string_is_refused_with_a_message(declared, tmp_path):
    (tmp_path / "pyproject.toml").write_text(f"[project]\nversion = {declared}\n")

    result = subprocess.run(
        [sys.executable, str(CHECK_TAG), "v1.0"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode != 0
    assert "must be a string" in result.stderr
    assert "Traceback" not in result.stderr


def test_no_argument_is_refused(tmp_path):
    result = subprocess.run(
        [sys.executable, str(CHECK_TAG)], cwd=tmp_path, capture_output=True, text=True, check=False
    )

    assert result.returncode != 0
    assert "usage" in result.stderr
