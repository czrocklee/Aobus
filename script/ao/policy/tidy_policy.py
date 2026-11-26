"""Source, ownership and frontend clang-tidy policy shared by the portal and Bazel actions.

``ao check lint`` selects native compile owners; cacheable actions replay their actual
Bazel contexts using these file modes, header folders, MSVC argument exclusions,
framework exceptions and stale-suppression rules. Both sides also rank the
consumer of an uncovered header and defer another native platform's sources
through the same component and ownership policy. The module imports only the
standard library and check policy so actions can declare a small, stable input set.
"""

import fnmatch
import os
import re
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
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


def component_parts(parent: tuple[str, ...]) -> tuple[str, ...] | None:
    """Map a public/private source directory to its actual implementation component."""
    if parent[:2] == ("include", "ao"):
        return ("lib", *parent[2:])
    if parent[:4] == ("app", "include", "ao", "rt"):
        return ("app", "runtime", *parent[4:])
    if parent[:3] == ("app", "include", "ao"):
        return ("app", *parent[3:])
    if len(parent) >= 5 and parent[0] == "app" and parent[2:4] == ("include", "ao"):
        return ("app", parent[1], *parent[5:])
    if parent and parent[0] in {"app", "lib", "test", "tool"}:
        return parent
    return None


def common_prefix(left: Sequence[str], right: Sequence[str]) -> int:
    """Return the length of the shared leading directory parts."""
    count = 0
    for first, second in zip(left, right, strict=False):
        if first != second:
            break
        count += 1
    return count


def consumer_rank(item: str, owner: str, source: str) -> tuple[bool, int, str, str]:
    """Rank one consumer: same component first, then the longest shared prefix."""
    item_component = component_parts(PurePosixPath(item).parent.parts)
    foreign = item_component is None or component_parts(PurePosixPath(source).parent.parts) != item_component
    return (
        foreign,
        -common_prefix(PurePosixPath(item).parts, PurePosixPath(source).parts),
        owner,
        source,
    )


NATIVE_PLATFORMS = ("linux", "macos", "windows")


def platform_incompatible(path: str, platform: str) -> bool:
    """Identify native ownership by OS, not by absence of a compile command.

    The platform is explicit so the portal's ambient profile and a declared
    graph action defer exactly the same paths. Callers own their
    unknown-platform contract: this policy refuses a platform it cannot
    classify.
    """
    if platform not in NATIVE_PLATFORMS:
        raise ValueError(f"unsupported native platform: {platform}")
    parts = tuple(part.casefold() for part in PurePosixPath(path).parts)
    joined = "/".join(parts)
    stem = PurePosixPath(path).stem.casefold()
    owned_by_windows = (
        parts[:2] in {("app", "windows"), ("app", "windows-winui")}
        or parts[:2] == ("test", "windows")
        or parts[:2] == ("test", "winui")
        or "wasapi" in joined
        or "win32" in joined
        or "winui" in joined
        or stem.endswith("windows")
        or joined == "tool/lint/aobusclangtidymain.cpp"
    )
    owned_by_linux = (
        parts[:2] == ("app", "linux-gtk")
        or parts[:5] == ("app", "include", "ao", "systemmedia", "linux")
        or parts[:3] == ("app", "systemmedia", "linux")
        or parts[:2] in {("test", "linux"), ("test", "linux-gtk")}
        or parts[:3] == ("test", "systemmedia", "linux")
        or "alsa" in joined
        or "pipewire" in joined
        or stem.endswith("linux")
    )
    owned_by_macos = (
        parts[:2] == ("app", "macos-appkit")
        or parts[:2] == ("test", "macos")
        or parts[:2] == ("test", "macos-appkit")
        or "coreaudio" in joined
        or "corefoundation" in joined
        or stem.endswith("macos")
    )
    if platform == "linux":
        return owned_by_windows or owned_by_macos
    if platform == "windows":
        return owned_by_linux or owned_by_macos or stem.endswith("posix")
    return owned_by_windows or owned_by_linux


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
