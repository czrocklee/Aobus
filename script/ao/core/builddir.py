"""Single source of truth for build flavors, CMake presets, and build tree locations."""

import os
from pathlib import Path

BUILD_ROOT = Path("/tmp/build")

PRESETS = {
    "debug": "linux-debug",
    "release": "linux-release",
    "pgo1": "linux-pgo-profile",
    "pgo2": "linux-pgo-optimize",
    "profile": "profile",
}

FLAVORS = tuple(PRESETS)

DEFAULT_DEBUG_DIR = BUILD_ROOT / "debug"
TIDY_DIR = BUILD_ROOT / "debug-clang-tidy"
ANALYZE_DIR = BUILD_ROOT / "debug-clang-analyzer"
COVERAGE_DIR = BUILD_ROOT / "coverage"


def suffix(*, clang: bool = False, asan: bool = False, tsan: bool = False) -> str:
    return ("-clang" if clang else "") + ("-asan" if asan else "") + ("-tsan" if tsan else "")


def build_dir(flavor: str, *, clang: bool = False, asan: bool = False, tsan: bool = False) -> Path:
    """Resolve the build tree for a flavor.

    The BUILD_DIR environment variable always wins, which lets callers redirect builds into
    dedicated host-persistent trees.
    Both PGO steps share one tree so step 2 can consume the profile data of step 1.
    """
    if override := os.environ.get("BUILD_DIR"):
        return Path(override)
    if flavor in ("pgo1", "pgo2"):
        return BUILD_ROOT / f"pgo{suffix(clang=clang)}"
    return BUILD_ROOT / f"{flavor}{suffix(clang=clang, asan=asan, tsan=tsan)}"
