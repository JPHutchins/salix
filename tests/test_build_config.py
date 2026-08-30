import pytest

from build_config import _project_version


def test_the_version_reads_from_the_project_table():
    assert (
        _project_version('[project]\nname = "x"\nversion = "1.2.3"\n\n[tool.y]\nversion = "9.9.9"\n')
        == "1.2.3"
    )


def test_the_version_survives_crlf_line_endings():
    assert _project_version('[project]\r\nversion = "1.2.3"\r\n') == "1.2.3"


def test_the_version_survives_a_commented_header():
    assert _project_version('[project]  # the one\nversion = "1.2.3"\n') == "1.2.3"


def test_a_single_quoted_version_reads_the_same():
    assert _project_version("[project]\nversion = '1.2.3'\n") == "1.2.3"


def test_a_missing_version_is_loud():
    with pytest.raises(SystemExit, match="no version"):
        _project_version("[project]\nname = 'x'\n")


def test_a_missing_project_table_is_loud():
    with pytest.raises(SystemExit, match="no \\[project\\]"):
        _project_version("[tool.y]\nversion = '1.2.3'\n")
