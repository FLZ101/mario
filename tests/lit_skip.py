"""Validation for explicit, reasoned fixture skips."""

from pathlib import Path


def parse_skip_reason(path: str | Path, value: str, previous: str | None) -> str:
    if previous is not None:
        raise ValueError(f"{path}: duplicate SKIP")
    reason = value.strip()
    if not reason:
        raise ValueError(f"{path}: SKIP requires a nonempty reason")
    return reason
