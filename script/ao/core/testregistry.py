"""Helpers for keeping C++ test files registered with CMake."""

import re
from pathlib import Path

from .paths import PROJECT_ROOT

CMAKE_TEST_SOURCE_RE = re.compile(r"[A-Za-z0-9_./+-]+Test\.cpp")
LINT_FIXTURE_PARTS = ("integration", "lint", "fixture")


def real_test_sources(root: Path = PROJECT_ROOT) -> list[str]:
    test_root = root / "test"
    sources: list[str] = []

    for path in test_root.rglob("*Test.cpp"):
        relative = path.relative_to(test_root)
        if relative.parts[: len(LINT_FIXTURE_PARTS)] == LINT_FIXTURE_PARTS:
            continue
        sources.append(relative.as_posix())

    return sorted(sources)


def registered_test_sources(root: Path = PROJECT_ROOT) -> list[str]:
    cmake_file = root / "test" / "CMakeLists.txt"
    sources: set[str] = set()

    for line in cmake_file.read_text(encoding="utf-8").splitlines():
        line_without_comment = line.split("#", 1)[0]
        for token in CMAKE_TEST_SOURCE_RE.findall(line_without_comment):
            sources.add(_normalize_cmake_source(token))

    return sorted(sources)


def unregistered_test_sources(root: Path = PROJECT_ROOT) -> list[str]:
    real_sources = set(real_test_sources(root))
    registered_sources = set(registered_test_sources(root))
    return sorted(f"test/{source}" for source in real_sources - registered_sources)


def _normalize_cmake_source(source: str) -> str:
    normalized = source.lstrip("./")
    if "/test/" in normalized:
        return normalized.split("/test/", 1)[1]
    if normalized.startswith("test/"):
        return normalized.removeprefix("test/")
    return normalized
