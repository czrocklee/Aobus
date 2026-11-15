"""Shared native analysis scope, source ownership and verified SDK descriptors.

Bazel owns compile commands, header inputs and generated artifacts. This module
never configures a product, reads a CMake cache or invents a compile database.
"""

import argparse
import os
import sys
from dataclasses import dataclass
from functools import cache
from pathlib import Path

from . import builddir, gitfiles, windows_llvm_sdk
from .jobs import default_workers
from .paths import absolute_path
from .proc import die
from .tidypolicy import windows_analysis_arguments as windows_analysis_arguments

LLVM_SDK_MANIFEST = windows_llvm_sdk.MANIFEST
LLVM_SDK_COMPLETION_MARKER = windows_llvm_sdk.MARKER


@cache
def windows_llvm_sdk_manifest() -> windows_llvm_sdk.Manifest:
    try:
        return windows_llvm_sdk.read_manifest(LLVM_SDK_MANIFEST)
    except windows_llvm_sdk.WindowsLlvmSdkError as error:
        raise die(str(error)) from None


def default_jobs() -> int:
    return default_workers()


def add_scope_arguments(parser: argparse.ArgumentParser, verb: str) -> None:
    parser.add_argument("files", nargs="*", metavar="file", help=f"explicit files to {verb}")
    parser.add_argument("--all", action="store_true", help=f"{verb} every source in the project folders")
    parser.add_argument(
        "--folder", action="append", default=[], metavar="<dir>", help="all files under directory (repeatable)"
    )
    parser.add_argument(
        "--commit", metavar="<rev>", help="changed files since revision plus working tree and untracked"
    )
    parser.add_argument("--check", metavar="<name>", help="run only the specified check")
    parser.add_argument("--debug", action="store_true", help="show analysis evidence/configuration")
    parser.add_argument("-o", "--output", metavar="<file>", help="write diagnostics to file")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs(), help="parallel native analysis workers")
    parser.add_argument("-p", "--path", metavar="<dir>", help="external Bazel analysis tree (honors BUILD_DIR)")


def resolve_scope(
    args: argparse.Namespace,
    all_folders: list[str],
    label: str,
    *,
    suffixes: tuple[str, ...] = gitfiles.CPP_SUFFIXES,
) -> tuple[list[str], bool]:
    if (prepared := getattr(args, "_resolved_sources", None)) is not None:
        return list(prepared), bool(args.files)
    if args.files:
        return list(args.files), True
    if args.all:
        print(f"{label} all sources in: {' '.join(all_folders)}", file=sys.stderr)
        files = gitfiles.find_sources(all_folders, suffixes=suffixes)
        if suffixes == gitfiles.SOURCE_SUFFIXES:
            files = sorted(set(files).union(gitfiles.root_bazel_files()))
    elif args.folder:
        print(f"{label} folders: {' '.join(args.folder)}", file=sys.stderr)
        files = gitfiles.find_sources(args.folder, suffixes=suffixes)
    else:
        base = gitfiles.diff_base(args.commit)
        print(f"No files specified — using git diff {base}..HEAD + working tree + staged + untracked", file=sys.stderr)
        files = gitfiles.changed_files(base, suffixes=suffixes)
    return files, False


@dataclass(frozen=True)
class TidyToolchain:
    """A caller-verified checker host and resources, not a product build cache."""

    clang_tidy: str
    plugin: Path | None
    resource_dir: Path | None
    expected_llvm_version: str | None = None
    sdk_root: Path | None = None


def clang_tool(_build_dir: Path, name: str, *, os_name: str | None = None) -> str:
    """Select a ready LLVM SDK tool; custom checker hosts require Bazel admission."""
    if builddir.platform_profile(os_name).name != "windows":
        return name
    if name == "clang-tidy":
        raise die("the Aobus checker host must come from the verified Bazel lint context")
    manifest = windows_llvm_sdk_manifest()
    try:
        root, _cached = windows_llvm_sdk.ready_root(os.environ, manifest)
    except windows_llvm_sdk.WindowsLlvmSdkError as error:
        raise die(str(error)) from error
    tool = root / "bin" / f"{name}.exe"
    if not tool.is_file():
        raise die(f"pinned LLVM SDK tool not found: {tool}")
    return str(tool)


def _is_platform_incompatible(path: Path, root: Path) -> bool:
    """Identify ownership by native OS, not by absence of a compile command."""
    try:
        parts = tuple(part.casefold() for part in absolute_path(path).relative_to(absolute_path(root)).parts)
    except ValueError:
        return False
    joined = "/".join(parts)
    stem = path.stem.casefold()
    profile = builddir.platform_profile().name
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
        or parts[:2] == ("test", "linux-gtk")
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
    if profile == "linux":
        return owned_by_windows or owned_by_macos
    if profile == "windows":
        return owned_by_linux or owned_by_macos or stem.endswith("posix")
    if profile == "macos":
        return owned_by_windows or owned_by_linux
    return False


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
