import argparse
import importlib
import json
import platform
import shutil
import subprocess
import sys
import unittest
from collections.abc import Iterable, Mapping
from pathlib import Path
from types import TracebackType
from typing import Literal, NamedTuple

CPYTHON_REPOSITORY = "https://github.com/python/cpython.git"
SPARSE_PATHS = ("/Lib/test/__init__.py", "/Lib/test/support/", "/Lib/test/test_dataclasses/")
FETCHED_MARKER = ".fetched"

HERE = Path(__file__).resolve().parent
REPOSITORY_ROOT = HERE.parent.parent

Status = Literal["pass", "skip", "expected_failure", "fail", "unexpected_success", "error"]
SEVERITY: Mapping[Status, int] = {
    "pass": 0,
    "skip": 1,
    "expected_failure": 2,
    "fail": 3,
    "unexpected_success": 4,
    "error": 5,
}

ExceptionInformation = (
    tuple[type[BaseException], BaseException, TracebackType] | tuple[None, None, None]
)


class Outcome(NamedTuple):
    test_id: str
    status: Status
    exception: str
    detail: str

    def recorded(self) -> str:
        return f"{self.status}: {self.exception}" if self.exception else self.status


class Difference(NamedTuple):
    test_id: str
    expected: str
    actual: str


def failure_outcome(test_id: str, status: Status, error: ExceptionInformation) -> Outcome:
    return Outcome(
        test_id,
        status,
        "" if error[0] is None else error[0].__name__,
        "" if error[1] is None else next(iter(str(error[1]).splitlines()), ""),
    )


class RecordingResult(unittest.TestResult):
    def __init__(self) -> None:
        super().__init__()
        self.outcomes: list[Outcome] = []

    def addSuccess(self, test: unittest.TestCase) -> None:
        super().addSuccess(test)
        self.outcomes.append(Outcome(test.id(), "pass", "", ""))

    def addFailure(self, test: unittest.TestCase, err: ExceptionInformation) -> None:
        super().addFailure(test, err)
        self.outcomes.append(failure_outcome(test.id(), "fail", err))

    def addError(self, test: unittest.TestCase, err: ExceptionInformation) -> None:
        super().addError(test, err)
        self.outcomes.append(failure_outcome(test.id(), "error", err))

    def addSkip(self, test: unittest.TestCase, reason: str) -> None:
        super().addSkip(test, reason)
        self.outcomes.append(Outcome(test.id(), "skip", "", reason))

    def addExpectedFailure(self, test: unittest.TestCase, err: ExceptionInformation) -> None:
        super().addExpectedFailure(test, err)
        self.outcomes.append(failure_outcome(test.id(), "expected_failure", err))

    def addUnexpectedSuccess(self, test: unittest.TestCase) -> None:
        super().addUnexpectedSuccess(test)
        self.outcomes.append(Outcome(test.id(), "unexpected_success", "", ""))

    def addSubTest(
        self,
        test: unittest.TestCase,
        subtest: unittest.TestCase,
        err: ExceptionInformation | None,
    ) -> None:
        super().addSubTest(test, subtest, err)
        if err is not None and err[0] is not None:
            self.outcomes.append(
                failure_outcome(
                    test.id(),
                    "fail" if issubclass(err[0], test.failureException) else "error",
                    err,
                )
            )


def worst_by_test(outcomes: Iterable[Outcome]) -> dict[str, Outcome]:
    return {
        outcome.test_id: outcome
        for outcome in sorted(outcomes, key=lambda outcome: SEVERITY[outcome.status])
    }


def fetched(checkout: Path, tag: str) -> bool:
    marker = checkout / FETCHED_MARKER
    return marker.exists() and marker.read_text() == tag


def fetch_cpython_tests(checkout: Path, tag: str) -> None:
    if fetched(checkout, tag):
        return
    shutil.rmtree(checkout, ignore_errors=True)
    checkout.mkdir(parents=True)
    git = ("git", "-c", "advice.detachedHead=false", "-C", str(checkout))
    for command in (
        (*git, "init", "--quiet"),
        (*git, "remote", "add", "origin", CPYTHON_REPOSITORY),
        (*git, "sparse-checkout", "set", "--no-cone", *SPARSE_PATHS),
        (*git, "fetch", "--quiet", "--depth", "1", "--filter=blob:none", "origin", f"refs/tags/{tag}"),
        (*git, "checkout", "--quiet", "FETCH_HEAD"),
    ):
        subprocess.run(command, check=True)
    (checkout / FETCHED_MARKER).write_text(tag)


def run_suite(checkout: Path) -> dict[str, Outcome]:
    sys.path[:0] = [str(checkout / "Lib"), str(REPOSITORY_ROOT), str(HERE)]
    from _shim import install

    install()
    result = RecordingResult()
    unittest.TestLoader().loadTestsFromModule(
        importlib.import_module("test.test_dataclasses")
    ).run(result)
    return worst_by_test(result.outcomes)


def recorded(outcomes: Mapping[str, Outcome]) -> dict[str, str]:
    return {
        test_id: outcome.recorded()
        for test_id, outcome in sorted(outcomes.items())
        if outcome.status != "skip"
    }


def differences(expected: Mapping[str, str], actual: Mapping[str, str]) -> list[Difference]:
    return [
        Difference(test_id, expected.get(test_id, "absent"), actual.get(test_id, "absent"))
        for test_id in sorted(expected.keys() | actual.keys())
        if expected.get(test_id) != actual.get(test_id)
    ]


def summary(outcomes: Mapping[str, Outcome]) -> str:
    counts = {
        status: sum(outcome.status == status for outcome in outcomes.values())
        for status in SEVERITY
    }
    return ", ".join(f"{count} {status}" for status, count in counts.items() if count)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("--record", action="store_true")
    arguments = parser.parse_args()
    tag = f"v{arguments.version}"
    expected_file = HERE / f"conformance-{tag}.json"
    if platform.python_version() != arguments.version:
        print(f"{tag}'s tests run on Python {arguments.version}, not {platform.python_version()}")
        return 2
    if not arguments.record and not expected_file.exists():
        print(f"{expected_file.name} does not exist; run with --record to create it")
        return 2
    checkout = HERE / ".cpython" / tag
    fetch_cpython_tests(checkout, tag)
    outcomes = run_suite(checkout)
    print(f"CPython {tag} test_dataclasses through the shim: {summary(outcomes)}")
    expected = json.loads(expected_file.read_text()) if expected_file.exists() else {}
    changed = differences(expected, recorded(outcomes))
    for difference in changed:
        detail = outcomes[difference.test_id].detail if difference.test_id in outcomes else ""
        print(
            f"{difference.test_id}: expected {difference.expected}, got {difference.actual}"
            + (f" ({detail})" if detail else "")
        )
    if arguments.record:
        expected_file.write_text(json.dumps(recorded(outcomes), indent=1) + "\n")
        return 0
    return 1 if changed else 0


if __name__ == "__main__":
    raise SystemExit(main())
