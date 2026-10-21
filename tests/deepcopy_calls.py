import copy
import sys
from collections.abc import Callable
from types import FrameType
from typing import TypeVar

T = TypeVar("T")


def deepcopy_calls_from_outside_copy(operation: Callable[[], object]) -> int:
    return result_and_deepcopy_calls_from_outside_copy(operation)[1]


def result_and_deepcopy_calls_from_outside_copy(operation: Callable[[], T]) -> tuple[T, int]:
    deepcopy_code = copy.deepcopy.__code__
    copy_globals = copy.__dict__
    calls = 0

    def profile(frame: FrameType, event: str, argument: object) -> None:
        nonlocal calls
        if (
            event == "call"
            and frame.f_code is deepcopy_code
            and (frame.f_back is None or frame.f_back.f_globals is not copy_globals)
        ):
            calls += 1

    outer_profile = sys.getprofile()
    sys.setprofile(profile)
    try:
        result = operation()
    finally:
        sys.setprofile(outer_profile)

    return result, calls
