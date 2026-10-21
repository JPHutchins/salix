from __future__ import annotations

import bisect
import functools
import re

TERMINATOR = re.compile(b"\0")


def c_string(binary: bytes, start: int, region: tuple[int, int]) -> str:
    low, high = region
    terminators = terminator_offsets(binary, low, high)
    terminator = bisect.bisect_left(terminators, start)

    if not low <= start < high or terminator == len(terminators):
        raise ValueError(f"the string at {start} has no terminator before {high}")

    return binary[start : terminators[terminator]].decode()


@functools.lru_cache(maxsize=4)
def terminator_offsets(binary: bytes, low: int, high: int) -> list[int]:
    return [match.start() for match in TERMINATOR.finditer(binary, low, high)]
