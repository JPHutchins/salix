from __future__ import annotations


def c_string(binary: bytes, start: int, region: tuple[int, int]) -> str:
    low, high = region

    if not low <= start < high:
        raise ValueError(f"the string at {start} is outside its region {low}..{high}")

    terminator = binary.find(b"\0", start, high)

    if terminator < 0:
        raise ValueError(f"the string at {start} has no terminator before {high}")

    return binary[start:terminator].decode()
