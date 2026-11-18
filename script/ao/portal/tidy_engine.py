"""Shared native analysis scope, source ownership and verified SDK descriptors.

Bazel owns compile commands, header inputs and generated artifacts. This module
never configures a product, reads a CMake cache or invents a compile database.
"""

import argparse
import os
import sys
from functools import cache
from pathlib import Path

from ao.core.jobs import default_workers
from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.core.proc import die
from ao.policy import windows_llvm_sdk
from ao.policy.tidy_policy import NATIVE_PLATFORMS, mode_buckets, platform_incompatible
from ao.policy.tidy_policy import component_parts as component_parts
from ao.policy.tidy_policy import windows_analysis_arguments as windows_analysis_arguments
from ao.portal import build_dir, git_files

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


ALL_FOLDERS = ["lib", "app", "asset", "bazel", "include", "script", "test", "tool"]


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
    suffixes: tuple[str, ...] = git_files.CPP_SUFFIXES,
) -> tuple[list[str], bool]:
    if (prepared := getattr(args, "_resolved_sources", None)) is not None:
        return list(prepared), bool(args.files)
    if args.files:
        return list(args.files), True
    if args.all:
        print(f"{label} all sources in: {' '.join(all_folders)}", file=sys.stderr)
        files = git_files.find_sources(all_folders, suffixes=suffixes)
        if suffixes == git_files.SOURCE_SUFFIXES:
            files = sorted(set(files).union(git_files.root_bazel_files()))
    elif args.folder:
        print(f"{label} folders: {' '.join(args.folder)}", file=sys.stderr)
        files = git_files.find_sources(args.folder, suffixes=suffixes)
    else:
        base = git_files.diff_base(args.commit)
        print(f"No files specified — using git diff {base}..HEAD + working tree + staged + untracked", file=sys.stderr)
        files = git_files.changed_files(base, suffixes=suffixes)
    return files, False


def classify_existing(files: list[str], explicit: bool) -> dict[str, list[Path]]:
    """Bucket selected spellings into tidy modes, keeping foreign roots STRICT."""
    root = absolute_path(PROJECT_ROOT)
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    seen: set[Path] = set()
    relatives: dict[str, Path] = {}
    outside: list[Path] = []
    for name in files:
        path = absolute_path(PROJECT_ROOT / name)
        if not path.is_file() or path in seen:
            continue
        seen.add(path)
        try:
            relative = path.relative_to(root).as_posix()
        except ValueError:
            outside.append(path)
            continue
        relatives[relative] = path
    for mode, names in mode_buckets(relatives, explicit=explicit).items():
        buckets[mode].extend(relatives[name] for name in names)
    # Files outside the checkout stay STRICT, matching the shared classifier.
    buckets["STRICT"].extend(outside)
    return buckets


def clang_tool(_build_dir: Path, name: str, *, os_name: str | None = None) -> str:
    """Select a ready LLVM SDK tool; custom checker hosts require Bazel admission."""
    if build_dir.platform_profile(os_name).name != "windows":
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
    """Identify ownership by native OS, not by absence of a compile command.

    The shared policy owns the classification; this portal wrapper supplies the
    ambient profile and keeps an unknown profile non-deferring instead of
    failing the whole gate.
    """
    try:
        relative = absolute_path(path).relative_to(absolute_path(root))
    except ValueError:
        return False
    profile = build_dir.platform_profile().name
    if profile not in NATIVE_PLATFORMS:
        return False
    return platform_incompatible(relative.as_posix(), profile)
