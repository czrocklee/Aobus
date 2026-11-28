"""Shared native analysis scope, source ownership and verified SDK descriptors.

Bazel owns compile commands, header inputs and generated artifacts. This module
classifies source paths and selects ready SDK tools.
"""

import os
from functools import cache
from pathlib import Path

from ao.core.jobs import default_workers
from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.core.proc import die
from ao.policy import windows_llvm_sdk
from ao.policy.lint_ownership import NATIVE_PLATFORMS, platform_incompatible
from ao.policy.lint_ownership import component_parts as component_parts
from ao.policy.tidy_policy import mode_buckets
from ao.policy.tidy_policy import windows_analysis_arguments as windows_analysis_arguments
from ao.portal import build_dir

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


def classify_existing(files: list[str], explicit: bool, *, require_checkout: bool = False) -> dict[str, list[Path]]:
    """Bucket files by tidy mode; a frozen checkout scope must not lose paths."""
    root = absolute_path(PROJECT_ROOT)
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    seen: set[Path] = set()
    relatives: dict[str, Path] = {}
    outside: list[Path] = []
    for name in files:
        if "\x00" in name:
            raise ValueError("lint path contains a NUL byte")
        path = absolute_path(PROJECT_ROOT / name)
        if path in seen:
            continue
        try:
            relative = path.relative_to(root).as_posix()
        except ValueError:
            if require_checkout:
                raise ValueError(f"invalid lint paths (outside the checkout: {name})") from None
            relative = None
        if not path.is_file():
            if require_checkout:
                raise ValueError(f"invalid lint paths (missing: {name})")
            continue
        seen.add(path)
        if relative is None:
            outside.append(path)
        else:
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
