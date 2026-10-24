import re
from collections import Counter
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
USE = re.compile(r"""^\s*(?:-\s+)?\{?\s*["']?uses["']?\s*:\s*["']?([^\s"',}]+)""", re.MULTILINE)
USE_KEY = re.compile(r"""(?:^|[\s{,])["']?uses["']?\s*:""", re.MULTILINE)
COMMENT = re.compile(r"(?:^|\s)#.*$", re.MULTILINE)
DIGEST = re.compile(r"[0-9a-f]{40}")
CODE_REVIEW = "JPHutchins/code-review/.github/workflows/review-reusable.yaml"

reads_the_workflows = pytest.mark.skipif(
    not (ROOT / ".github").is_dir(), reason=".github is not beside these tests"
)


def remote_uses(text: str) -> list[tuple[str, str]]:
    return [
        (action, reference)
        for action, _, reference in (use.partition("@") for use in USE.findall(COMMENT.sub("", text)))
        if not action.startswith("./")
    ]


def unread_uses(text: str) -> int:
    uncommented = COMMENT.sub("", text)

    return len(USE_KEY.findall(uncommented)) - len(USE.findall(uncommented))


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
def test_every_uses_key_in_the_workflows_is_read() -> None:
    assert {
        str(workflow.relative_to(ROOT)): unread_uses(workflow.read_text(encoding="utf-8"))
        for workflow in (ROOT / ".github").rglob("*.y*ml")
        if unread_uses(workflow.read_text(encoding="utf-8"))
    } == {}


@reads_the_workflows
def test_the_code_review_workflow_is_still_called() -> None:
    assert CODE_REVIEW in {action for action, _ in workflow_uses()}


@pytest.mark.parametrize(
    ("text", "read"),
    [
        (f"      - uses: actions/checkout@{'a' * 40} # v7\n", [("actions/checkout", "a" * 40)]),
        ("    uses: owner/repository/.github/workflows/reusable.yaml@v1\n", [("owner/repository/.github/workflows/reusable.yaml", "v1")]),
        (f"      # - uses: actions/checkout@{'a' * 40}\n", []),
        ("      - uses: ./.github/actions/local\n", []),
        (f'      - uses: "actions/checkout@{"a" * 40}"\n', [("actions/checkout", "a" * 40)]),
        (f'      - "uses": actions/checkout@{"a" * 40}\n', [("actions/checkout", "a" * 40)]),
        (f"      - uses : actions/checkout@{'a' * 40}\n", [("actions/checkout", "a" * 40)]),
        (f"      - {{uses: actions/checkout@{'a' * 40}}}\n", [("actions/checkout", "a" * 40)]),
        (f"      - uses: actions/checkout@{'a' * 40}# v7\n", [("actions/checkout", f"{'a' * 40}#")]),
    ],
    ids=["step", "reusable-workflow", "commented-out", "local-action", "quoted-value", "quoted-key", "spaced-key", "flow-mapping", "comment-without-space"],
)
def test_a_use_is_read_from_a_step_or_job_line_but_not_from_a_comment_or_a_local_path(
    text: str, read: list[tuple[str, str]]
) -> None:
    assert remote_uses(text) == read


@pytest.mark.parametrize(
    ("text", "unread"),
    [
        (f"      - uses: actions/checkout@{'a' * 40} # v7\n", 0),
        (f"      # - uses: actions/checkout@{'a' * 40}\n", 0),
        ("      - {name: Checkout, uses: actions/checkout@v4}\n", 1),
        ("      - name: Checkout\n        with: {ref: main, uses: x}\n", 1),
    ],
    ids=["read", "commented-out", "flow-mapping-after-another-key", "uses-inside-a-with-mapping"],
)
def test_a_uses_key_the_matcher_cannot_read_is_counted(text: str, unread: int) -> None:
    assert unread_uses(text) == unread
