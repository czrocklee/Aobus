"""Shared CMakeCache.txt tokenizer and readers.

CMake writes cache entries as ``NAME:TYPE=value`` lines; comments start with
``//`` or ``#``. Only the first ``=`` separates the raw value, which may itself
contain ``=``; entry names keep the ``:TYPE`` suffix removed. The ordered reader
replaces undecodable bytes; callers own duplicate resolution, whitespace,
strict decoding, and read-error fallbacks.
"""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path


def parse_cache(text: str) -> Iterator[tuple[str, str]]:
    """Yield ``(name, value)`` pairs in file order, preserving duplicates and raw values."""
    for line in text.splitlines():
        name, separator, value = line.partition("=")
        if separator and ":" in name and not name.startswith(("//", "#")):
            yield name.split(":", 1)[0], value


def read_entries(path: Path) -> list[tuple[str, str]]:
    """Read entries in file order, replacing undecodable bytes with U+FFFD.

    OSError propagates to the caller, which owns the fallback or diagnostic.
    """
    return list(parse_cache(path.read_text(encoding="utf-8", errors="replace")))
