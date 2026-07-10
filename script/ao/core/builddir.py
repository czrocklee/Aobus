"""Single source of truth for platform build profiles, presets, and build trees."""

import os
from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT

BUILD_ROOT = Path("/tmp/build")
WINDOWS_BUILD_ROOT = PROJECT_ROOT / "out" / "build"

PRESETS = {
    "debug": "linux-debug",
    "release": "linux-release",
    "pgo1": "linux-pgo-profile",
    "pgo2": "linux-pgo-optimize",
    "profile": "profile",
}

WINDOWS_PRESETS = {
    "debug": "windows-tui-debug",
    "release": "windows-tui-release",
}

WINDOWS_TEST_PRESETS = {
    "debug": "windows-tui-debug-tests",
    "release": "windows-tui-release-tests",
}

TIDY_PRESETS = {
    "linux": "linux-debug",
    "windows": "windows-tidy",
}

FLAVORS = tuple(PRESETS)


@dataclass(frozen=True)
class PlatformProfile:
    name: str
    build_root: Path
    presets: dict[str, str]
    test_presets: dict[str, str]
    executable_suffix: str
    apps: tuple[str, ...]
    default_suites: tuple[str, ...]
    all_suites: tuple[str, ...]
    compiler: str


LINUX_PROFILE = PlatformProfile(
    name="linux",
    build_root=BUILD_ROOT,
    presets=PRESETS,
    test_presets=PRESETS,
    executable_suffix="",
    apps=("cli", "tui", "gtk"),
    default_suites=("core", "gtk"),
    all_suites=("core", "tui", "cli", "gtk", "integration", "council", "tooling", "lint"),
    compiler="gcc",
)

WINDOWS_PROFILE = PlatformProfile(
    name="windows",
    build_root=WINDOWS_BUILD_ROOT,
    presets=WINDOWS_PRESETS,
    test_presets=WINDOWS_TEST_PRESETS,
    executable_suffix=".exe",
    apps=("tui",),
    default_suites=("core", "tui"),
    # The Python tooling gate depends on the Nix-pinned Ruff and mypy tools.
    # Keep the native Windows gate reproducible from the documented Visual
    # Studio + vcpkg prerequisites instead of depending on ambient PATH tools.
    all_suites=("core", "tui", "integration"),
    compiler="msvc",
)

DEFAULT_DEBUG_DIR = BUILD_ROOT / "debug"
COVERAGE_DIR = BUILD_ROOT / "coverage"


def platform_profile(os_name: str | None = None) -> PlatformProfile:
    """Return the native development profile for an ``os.name`` value."""
    return WINDOWS_PROFILE if (os.name if os_name is None else os_name) == "nt" else LINUX_PROFILE


def flavors(os_name: str | None = None) -> tuple[str, ...]:
    return tuple(platform_profile(os_name).presets)


def tidy_preset(os_name: str | None = None) -> str:
    """Return the configure preset for the native clang-tidy toolchain."""
    return TIDY_PRESETS[platform_profile(os_name).name]


def tidy_dir(os_name: str | None = None) -> Path:
    """Return the dedicated native clang-tidy build tree."""
    profile = platform_profile(os_name)
    name = "windows-tidy" if profile.name == "windows" else "debug-clang-tidy"
    return profile.build_root / name


def analyze_dir(os_name: str | None = None) -> Path:
    """Return the dedicated native clang-analyzer build tree."""
    profile = platform_profile(os_name)
    name = "windows-analyzer" if profile.name == "windows" else "debug-clang-analyzer"
    return profile.build_root / name


TIDY_DIR = tidy_dir()
ANALYZE_DIR = analyze_dir()


def preset(flavor: str, *, with_tests: bool = False, os_name: str | None = None) -> str:
    profile = platform_profile(os_name)
    presets = profile.test_presets if with_tests else profile.presets
    return presets[flavor]


def suffix(*, clang: bool = False, asan: bool = False, tsan: bool = False) -> str:
    return ("-clang" if clang else "") + ("-asan" if asan else "") + ("-tsan" if tsan else "")


def build_dir(
    flavor: str,
    *,
    clang: bool = False,
    asan: bool = False,
    tsan: bool = False,
    with_tests: bool = False,
    os_name: str | None = None,
) -> Path:
    """Resolve the native build tree for a flavor.

    The BUILD_DIR environment variable always wins, which lets callers redirect builds into
    dedicated host-persistent trees. Linux PGO steps share one tree so step 2 can consume the
    profile data of step 1. Windows keeps application-only and test-enabled trees separate.
    """
    if override := os.environ.get("BUILD_DIR"):
        return Path(override)

    profile = platform_profile(os_name)
    if profile.name == "windows":
        name = preset(flavor, with_tests=with_tests, os_name="nt")
        return profile.build_root / f"{name}{suffix(clang=clang, asan=asan, tsan=tsan)}"

    if flavor in ("pgo1", "pgo2"):
        return profile.build_root / f"pgo{suffix(clang=clang)}"
    return profile.build_root / f"{flavor}{suffix(clang=clang, asan=asan, tsan=tsan)}"


def executable(path: Path, os_name: str | None = None) -> Path:
    """Add the native executable suffix to a target path."""
    suffix_value = platform_profile(os_name).executable_suffix
    return path.with_name(f"{path.name}{suffix_value}") if suffix_value else path
