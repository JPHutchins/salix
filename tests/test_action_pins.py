import re
from collections import Counter
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
PINNED_USE = re.compile(r"uses:\s*([\w.-]+/[\w./-]+)@([0-9a-f]{40})")

pytestmark = pytest.mark.skipif(
    not (ROOT / ".github").is_dir(), reason=".github is not beside these tests"
)


def pinned_uses() -> set[tuple[str, str]]:
    return {
        use
        for workflow in (ROOT / ".github").rglob("*.y*ml")
        for use in PINNED_USE.findall(workflow.read_text(encoding="utf-8"))
    }


def test_each_action_is_pinned_to_one_digest_across_the_workflows_and_actions() -> None:
    actions = Counter(action for action, _ in pinned_uses())

    assert "DeterminateSystems/nix-installer-action" in actions
    assert [action for action, digests in actions.items() if digests > 1] == []
