import argparse
import importlib
import json
import subprocess
import sys
import unittest
from collections.abc import Iterable, Mapping
from pathlib import Path
from types import TracebackType
from typing import Literal, NamedTuple

CPYTHON_TAG = "v3.14.6"
CPYTHON_COMMIT = "c63aec69bd59c55314c06c23f4c22c03de76fe45"
CPYTHON_MINOR = (3, 14)
CPYTHON_REPOSITORY = "https://github.com/python/cpython.git"
SPARSE_PATHS = ("/Lib/test/__init__.py", "/Lib/test/support/", "/Lib/test/test_dataclasses/")

HERE = Path(__file__).resolve().parent
REPOSITORY_ROOT = HERE.parent.parent
CHECKOUT = HERE / ".cpython" / CPYTHON_TAG
EXPECTED = HERE / f"conformance-{CPYTHON_TAG}.json"

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
    detail: str


class Difference(NamedTuple):
    test_id: str
    expected: str
    actual: str


def first_line(error: ExceptionInformation) -> str:
    return (
        ""
        if error[0] is None
        else f"{error[0].__name__}: {next(iter(str(error[1]).splitlines()), '')}"
    )


class RecordingResult(unittest.TestResult):
    def __init__(self) -> None:
        super().__init__()
        self.outcomes: list[Outcome] = []

    def addSuccess(self, test: unittest.TestCase) -> None:
        super().addSuccess(test)
        self.outcomes.append(Outcome(test.id(), "pass", ""))

    def addFailure(self, test: unittest.TestCase, err: ExceptionInformation) -> None:
        super().addFailure(test, err)
        self.outcomes.append(Outcome(test.id(), "fail", first_line(err)))

    def addError(self, test: unittest.TestCase, err: ExceptionInformation) -> None:
        super().addError(test, err)
        self.outcomes.append(Outcome(test.id(), "error", first_line(err)))

    def addSkip(self, test: unittest.TestCase, reason: str) -> None:
        super().addSkip(test, reason)
        self.outcomes.append(Outcome(test.id(), "skip", reason))

    def addExpectedFailure(self, test: unittest.TestCase, err: ExceptionInformation) -> None:
        super().addExpectedFailure(test, err)
        self.outcomes.append(Outcome(test.id(), "expected_failure", first_line(err)))

    def addUnexpectedSuccess(self, test: unittest.TestCase) -> None:
        super().addUnexpectedSuccess(test)
        self.outcomes.append(Outcome(test.id(), "unexpected_success", ""))

    def addSubTest(
        self,
        test: unittest.TestCase,
        subtest: unittest.TestCase,
        err: ExceptionInformation | None,
    ) -> None:
        super().addSubTest(test, subtest, err)
        if err is not None and err[0] is not None:
            self.outcomes.append(
                Outcome(
                    test.id(),
                    "fail" if issubclass(err[0], test.failureException) else "error",
                    first_line(err),
                )
            )


def worst_by_test(outcomes: Iterable[Outcome]) -> dict[str, Outcome]:
    return {
        outcome.test_id: outcome
        for outcome in sorted(outcomes, key=lambda outcome: SEVERITY[outcome.status])
    }


def fetch_cpython_tests() -> None:
    if CHECKOUT.exists():
        return
    git = ("git", "-c", "advice.detachedHead=false", "-C", str(CHECKOUT))
    CHECKOUT.mkdir(parents=True)
    for command in (
        (*git, "init", "--quiet"),
        (*git, "remote", "add", "origin", CPYTHON_REPOSITORY),
        (*git, "sparse-checkout", "set", "--no-cone", *SPARSE_PATHS),
        (*git, "fetch", "--quiet", "--depth", "1", "--filter=blob:none", "origin", CPYTHON_COMMIT),
        (*git, "checkout", "--quiet", "FETCH_HEAD"),
    ):
        subprocess.run(command, check=True)


def run_suite() -> dict[str, Outcome]:
    sys.path[:0] = [str(CHECKOUT / "Lib"), str(REPOSITORY_ROOT)]
    from _shim import install

    install()
    result = RecordingResult()
    unittest.TestLoader().loadTestsFromModule(
        importlib.import_module("test.test_dataclasses")
    ).run(result)
    return worst_by_test(result.outcomes)


def differences(expected: Mapping[str, str], actual: Mapping[str, Outcome]) -> list[Difference]:
    observed = {test_id: outcome.status for test_id, outcome in actual.items()}
    return [
        Difference(test_id, expected.get(test_id, "absent"), observed.get(test_id, "absent"))
        for test_id in sorted(expected.keys() | observed.keys())
        if expected.get(test_id) != observed.get(test_id)
    ]


def summary(outcomes: Mapping[str, Outcome]) -> str:
    counts = {
        status: sum(outcome.status == status for outcome in outcomes.values())
        for status in SEVERITY
    }
    return ", ".join(f"{count} {status}" for status, count in counts.items() if count)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", action="store_true")
    arguments = parser.parse_args()
    if sys.version_info[:2] != CPYTHON_MINOR:
        print(f"{CPYTHON_TAG}'s tests run on Python {CPYTHON_MINOR}, not {sys.version_info[:2]}")
        return 2
    fetch_cpython_tests()
    outcomes = run_suite()
    print(f"CPython {CPYTHON_TAG} test_dataclasses through the shim: {summary(outcomes)}")
    if arguments.record:
        EXPECTED.write_text(
            json.dumps({test_id: outcome.status for test_id, outcome in sorted(outcomes.items())}, indent=1)
            + "\n"
        )
        return 0
    changed = differences(json.loads(EXPECTED.read_text()), outcomes)
    for difference in changed:
        detail = outcomes[difference.test_id].detail if difference.test_id in outcomes else ""
        print(
            f"{difference.test_id}: expected {difference.expected}, got {difference.actual}"
            + (f" ({detail})" if detail else "")
        )
    return 1 if changed else 0


if __name__ == "__main__":
    raise SystemExit(main())
