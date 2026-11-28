"""Source modes and frontend clang-tidy policy shared by the portal and Bazel actions.

``ao check lint`` selects native compile owners; cacheable actions replay their actual
Bazel contexts using these file modes, header folders, MSVC argument exclusions,
framework exceptions and stale-suppression rules. Consumer routing belongs to
``lint_ownership``; per-file diagnostics do not depend on that selection.
"""

import fnmatch
import os
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType

from ao.policy.tidy_config import mode_disabled_checks

# File types supported by the Bazel lint aspect and the native ownership audit.
TRANSLATION_UNIT_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".mm")
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx")
FRAGMENT_SUFFIXES = (".def",)
LINT_SUFFIXES = (*TRANSLATION_UNIT_SUFFIXES, *HEADER_SUFFIXES, *FRAGMENT_SUFFIXES)

STRICT_HEADER_FOLDERS = ("lib", "app", "include", "tool")
RELAXED_HEADER_FOLDERS = ("test", "include")

# Clang tooling does not consume MSVC code-generation toggles or its warning
# promotion: /WX would turn Clang-only compatibility warnings into hard errors.
# Strip deterministic output and both IPO spellings; workarounds may use /GL-.
WINDOWS_EXCLUDED_COMPILE_ARGUMENTS = (
    "/Zc:preprocessor",
    "/c",
    "/ZW:nostdlib",
    "/WX",
    "/GL",
    "/GL-",
    "/experimental:deterministic",
)

# GCC owns flags with no Clang parser meaning: the Release LTO object-model
# options and the dependency-path canonicalization base flag (GCC needs it for
# reproducible -MD symlink observations; the pinned Clang rejects it as an
# unknown argument). A Clang-native context never carries these spellings, so
# the exact-match projection below never alters a Clang-native replay.
GCC_ANALYSIS_ONLY_ARGUMENTS = ("-flto=auto", "-fno-fat-lto-objects", "-fno-canonical-system-headers")


def gcc_analysis_projection(arguments: Iterable[str]) -> list[str]:
    """Return the native arguments a selected Clang parser may replay.

    Only the admitted GCC-only producer flags are dropped; every semantic
    define, include, workaround macro and warning stays, and the native GCC
    dependency observations keep the full unmodified argv. Nothing is
    rewritten, filtered by prefix or globally suppressed.
    """
    return [argument for argument in arguments if argument not in GCC_ANALYSIS_ONLY_ARGUMENTS]


# Auxiliary WinUI probe sources compile only in their own Visual Studio
# projects, so they share the frontend's compile context and policy.
WINUI_AUXILIARY_SOURCE_PROJECTS = MappingProxyType(
    {
        os.path.normcase(source).replace("\\", "/"): project
        for source, project in {
            "test/helper/WinUiLocalizationProbe.cpp": "app/windows-winui/ao_winui_localization_probe.vcxproj",
            "test/helper/WinUiTrackItemProbe.cpp": "app/windows-winui/ao_winui_track_item_probe.vcxproj",
            "test/winui/probe/WinUiActivityStatusProbe.cpp": "app/windows-winui/ao_winui_activity_status_probe.vcxproj",
            "test/winui/probe/WinUiAutomationProbe.cpp": "app/windows-winui/ao_winui_automation_probe.vcxproj",
            "test/winui/probe/WinUiCreditsProbe.cpp": "app/windows-winui/ao_winui_credits_probe.vcxproj",
            "test/winui/probe/WinUiQualityToolTipProbe.cpp": "app/windows-winui/ao_winui_quality_tooltip_probe.vcxproj",
            "test/winui/probe/WinUiSilentAudio.cpp": "app/windows-winui/ao_winui_automation_probe.vcxproj",
            "test/winui/probe/WinUiSoulProbe.cpp": "app/windows-winui/ao_winui_soul_probe.vcxproj",
            "test/winui/probe/WinUiTrackTableProbe.cpp": "app/windows-winui/ao_winui_track_table_probe.vcxproj",
        }.items()
    }
)
_WINUI_FRONTEND_PREFIX = os.path.normcase("app/windows-winui").replace("\\", "/") + "/"


def classify_relative(relative: str, explicit: bool) -> str:
    """Return the tidy mode of a repository-relative POSIX path.

    Fixtures run only when explicitly named, other ``test/lint`` files are
    lint infrastructure, remaining tests are RELAXED and everything else is
    STRICT. Windows compares paths case-insensitively.
    """
    rel = relative.lower() if os.name == "nt" else relative
    if rel.startswith("test/lint/fixture/"):
        return "STRICT" if explicit else "IGNORE"
    if rel.startswith("test/lint/"):
        return "IGNORE"
    if rel.startswith("test/"):
        return "RELAXED"
    return "STRICT"


def mode_buckets(relatives: Iterable[str], *, explicit: bool = False) -> dict[str, list[str]]:
    """Bucket repository-relative POSIX paths into STRICT/RELAXED tidy modes.

    IGNORE paths drop out; ``classify_relative`` stays the only classification
    authority, and command layers reuse this loop instead of duplicating it.
    """
    buckets: dict[str, list[str]] = {"STRICT": [], "RELAXED": []}
    for relative in relatives:
        mode = classify_relative(relative, explicit)
        if mode in buckets:
            buckets[mode].append(relative)
    return buckets


def winui_auxiliary_context(key: str, auxiliary: Mapping[str, str] = WINUI_AUXILIARY_SOURCE_PROJECTS) -> bool:
    """Return whether a normalized relative key is an auxiliary probe source or its header."""
    return key in auxiliary or (key[-2:].lower() == ".h" and key[:-2] + ".cpp" in auxiliary)


def winui_compile_context(relative: str) -> bool:
    """Return whether a repository-relative path compiles in the WinUI context."""
    key = os.path.normcase(relative).replace("\\", "/")
    return key.startswith(_WINUI_FRONTEND_PREFIX) or winui_auxiliary_context(key)


@dataclass(frozen=True)
class FrontendPolicy:
    """Framework adjustments owned by the frontend that compiles a translation unit."""

    check_suffix: str
    arguments: tuple[str, ...]
    selected_path_only: bool


def frontend_policy(compile_source: str, *, winui: bool, windows: bool) -> FrontendPolicy:
    """Return check and clang-tidy argument adjustments for one compile context.

    GTK widget trees own their children and use printf-style APIs. WinUI
    reports diagnostics only for the selected source or header, whether or not
    the compile command uses a forced include; changed headers receive their
    own header-main analysis. Generated C++/WinRT headers keep schema spelling
    while Windows resolves include paths case-insensitively.
    """
    check_suffix = ""
    arguments: list[str] = []
    if winui and windows:
        arguments.append("--extra-arg-before=-Wno-nonportable-include-path")
    if "linux-gtk/" in compile_source.replace("\\", "/"):
        check_suffix = ",-cppcoreguidelines-owning-memory"
        arguments.append("--extra-arg=-Wno-format-nonliteral")
    return FrontendPolicy(check_suffix=check_suffix, arguments=tuple(arguments), selected_path_only=winui)


def windows_analysis_arguments(resource_dir: Path, *, driver_mode: bool = False) -> tuple[str, ...]:
    """Return the Windows analysis arguments shared by tidy and lint fixtures.

    The VS 18 STL lets Clang classify three-byte records as SIMD-find
    candidates, but its vectorized helper supports only 1/2/4/8-byte elements.
    clang-tidy does not link or execute the TU, so disabling that optional STL
    implementation path preserves the analyzed API. Tidy passes the arguments
    as extra-args-before; ``driver_mode`` returns the same two arguments with
    the clang-cl ``/clang:`` and ``/D`` spellings the fixture runner needs.
    """
    if driver_mode:
        return (f"/clang:-resource-dir={resource_dir}", "/D_USE_STD_VECTOR_ALGORITHMS=0")
    return (f"-resource-dir={resource_dir}", "-D_USE_STD_VECTOR_ALGORITHMS=0")


_NOLINT_OPEN_RE = re.compile(r"\bNOLINT(?:NEXTLINE|BEGIN)?\(([^)]*)\)")


@dataclass(frozen=True)
class StaleNolintSuppression:
    path: Path
    line: int
    check: str


def find_stale_nolint_suppressions(buckets: dict[str, list[Path]]) -> list[StaleNolintSuppression]:
    """Find NOLINT entries naming checks that the file's tidy mode never enables."""
    issues: list[StaleNolintSuppression] = []
    for mode, paths in buckets.items():
        disabled_patterns = [token.removeprefix("-") for token in mode_disabled_checks(mode)]
        for path in paths:
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            for line_number, line in enumerate(lines, start=1):
                for match in _NOLINT_OPEN_RE.finditer(line):
                    checks = (check.strip() for check in match.group(1).split(","))
                    for check in checks:
                        if check and any(fnmatch.fnmatchcase(check, pattern) for pattern in disabled_patterns):
                            issues.append(StaleNolintSuppression(path=path, line=line_number, check=check))
    return issues
