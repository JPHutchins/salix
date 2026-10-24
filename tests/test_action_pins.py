import re
from collections import Counter
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
USE = re.compile(r"""\s*(?:-\s+)?uses:\s+([^\s#"']+)(?:\s+#.*)?""")
NAMES_USES = re.compile(r"\buses\b")
WHOLE_LINE_COMMENT = re.compile(r"\s*#.*")
DIGEST = re.compile(r"[0-9a-f]{40}")
CODE_REVIEW = "JPHutchins/code-review/.github/workflows/review-reusable.yaml"

reads_the_workflows = pytest.mark.skipif(
    not (ROOT / ".github").is_dir(), reason=".github is not beside these tests"
)


def remote_uses(text: str) -> list[tuple[str, str]]:
    return [
        (action, reference)
        for action, _, reference in (
            use.group(1).partition("@") for use in map(USE.fullmatch, text.splitlines()) if use
        )
        if not action.startswith("./")
    ]


def lines_outside_the_grammar(text: str) -> list[str]:
    return [
        line
        for line in text.splitlines()
        if NAMES_USES.search(line) and not USE.fullmatch(line) and not WHOLE_LINE_COMMENT.fullmatch(line)
    ]


def workflow_uses() -> list[tuple[str, str]]:
    return [use for workflow in (ROOT / ".github").rglob("*.y*ml") for use in remote_uses(workflow.read_text(encoding="utf-8"))]


@reads_the_workflows
def test_each_action_is_pinned_to_one_digest_across_the_workflows_and_actions() -> None:
    actions = Counter(action for action, reference in set(workflow_uses()) if DIGEST.fullmatch(reference))

    assert "DeterminateSystems/nix-installer-action" in actions
    assert [action for action, digests in actions.items() if digests > 1] == []


@reads_the_workflows
def test_every_remote_use_is_pinned_by_digest() -> None:
    assert [(action, reference) for action, reference in workflow_uses() if not DIGEST.fullmatch(reference)] == []


@reads_the_workflows
def test_every_line_naming_uses_in_the_workflows_is_a_block_key_or_a_whole_line_comment() -> None:
    assert {
        str(workflow.relative_to(ROOT)): lines_outside_the_grammar(workflow.read_text(encoding="utf-8"))
        for workflow in (ROOT / ".github").rglob("*.y*ml")
        if lines_outside_the_grammar(workflow.read_text(encoding="utf-8"))
    } == {}


@reads_the_workflows
def test_the_review_workflow_calls_code_review() -> None:
    assert CODE_REVIEW in {
        action
        for action, _ in remote_uses((ROOT / ".github" / "workflows" / "review.yaml").read_text(encoding="utf-8"))
    }


@pytest.mark.parametrize(
    ("text", "read"),
    [
        (f"      - uses: actions/checkout@{'a' * 40} # v7\n", [("actions/checkout", "a" * 40)]),
        ("    uses: owner/repository/.github/workflows/reusable.yaml@v1\n", [("owner/repository/.github/workflows/reusable.yaml", "v1")]),
        (f"      # - uses: actions/checkout@{'a' * 40}\n", []),
        ("      - uses: ./.github/actions/local\n", []),
        (f"      - uses: actions/checkout@{'a' * 40},nightly\n", [("actions/checkout", f"{'a' * 40},nightly")]),
        (f"      - uses: actions/checkout@{'a' * 40}\r      - uses: actions/cache@v4\r", [("actions/checkout", "a" * 40), ("actions/cache", "v4")]),
    ],
    ids=["step", "reusable-workflow", "commented-out", "local-action", "reference-with-a-comma", "carriage-return-lines"],
)
def test_a_use_is_read_whole_from_a_block_key_line_but_not_from_a_comment_or_a_local_path(
    text: str, read: list[tuple[str, str]]
) -> None:
    assert remote_uses(text) == read


@pytest.mark.parametrize(
    ("text", "outside"),
    [
        (f"      - uses: actions/checkout@{'a' * 40} # v7\n", []),
        (f"      # - uses: actions/checkout@{'a' * 40}\n", []),
        ("      - name: Reuse the cache\n", []),
        (f'      - uses: "actions/checkout@{"a" * 40}"\n', [f'      - uses: "actions/checkout@{"a" * 40}"']),
        ("      - 'uses': actions/checkout@v4\n", ["      - 'uses': actions/checkout@v4"]),
        ("      - uses : actions/checkout@v4\n", ["      - uses : actions/checkout@v4"]),
        ('      - {name: "Issue #1", uses: actions/checkout@v4}\n', ['      - {name: "Issue #1", uses: actions/checkout@v4}']),
        ("    steps: [uses: actions/checkout@v4]\n", ["    steps: [uses: actions/checkout@v4]"]),
        (f"      - uses: actions/checkout@{'a' * 40}# v7\n", [f"      - uses: actions/checkout@{'a' * 40}# v7"]),
        ("      - run: grep -v 'uses:' workflow.yaml\n", ["      - run: grep -v 'uses:' workflow.yaml"]),
        ("      - uses: actions/checkout@v4\r      - {uses: actions/cache@v4}\r", ["      - {uses: actions/cache@v4}"]),
    ],
    ids=[
        "block-key",
        "whole-line-comment",
        "no-uses",
        "quoted-value",
        "quoted-key",
        "spaced-key",
        "flow-mapping-after-a-quoted-hash",
        "flow-sequence",
        "comment-without-space",
        "free-text",
        "carriage-return-lines",
    ],
)
def test_a_line_naming_uses_outside_the_block_key_grammar_is_reported(text: str, outside: list[str]) -> None:
    assert lines_outside_the_grammar(text) == outside
