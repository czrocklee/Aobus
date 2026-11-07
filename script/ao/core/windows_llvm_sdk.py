"""Read the pinned Windows LLVM SDK contract shared with cmake/LlvmSdk.cmake.

The provisioner, the CMake lint tree and the Bazel lint SDK import all evaluate
one manifest, cache precedence and completion marker here. The lint repository
rule runs its helper as a script, so this module imports only the standard library.
"""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from pathlib import Path
from typing import NamedTuple

MANIFEST = Path(__file__).resolve().parents[1] / "windows-llvm-sdk.json"
MARKER = ".aobus-llvm-sdk-complete"


class WindowsLlvmSdkError(ValueError):
    """The manifest or a selected SDK root does not satisfy the pinned contract."""


class Manifest(NamedTuple):
    version: str
    archive_sha256: str
    # The listed files plus the Clang resource include of the pinned major.
    required_files: tuple[str, ...]


def read_manifest(path: Path) -> Manifest:
    """Validate the manifest and derive its required files as the CMake module does."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError) as error:
        raise WindowsLlvmSdkError(f"cannot read the Windows LLVM SDK manifest {path}: {error}") from None
    except json.JSONDecodeError as error:
        raise WindowsLlvmSdkError(f"the Windows LLVM SDK manifest {path} is not valid JSON: {error}") from None
    version = value.get("version") if isinstance(value, dict) else None
    archive_sha256 = value.get("archiveSha256") if isinstance(value, dict) else None
    required_files = value.get("requiredFiles") if isinstance(value, dict) else None
    if (
        not isinstance(version, str)
        or re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+", version) is None
        or not isinstance(archive_sha256, str)
        or re.fullmatch(r"[0-9a-f]{64}", archive_sha256) is None
        or not isinstance(required_files, list)
        or not required_files
        or any(
            not isinstance(entry, str)
            or re.fullmatch(r"[A-Za-z0-9_.+-]+(/[A-Za-z0-9_.+-]+)*", entry) is None
            or any(part in (".", "..") for part in entry.split("/"))
            for entry in required_files
        )
    ):
        raise WindowsLlvmSdkError(
            f"the Windows LLVM SDK manifest {path} needs a '<major>.<minor>.<patch>' "
            "'version', a 64-digit lowercase-hex 'archiveSha256', and a non-empty "
            "'requiredFiles' array of portable relative SDK paths "
            "(slash-separated letters, digits, '_', '.', '+', '-'; no '.' or '..' components)"
        )
    return Manifest(
        version,
        archive_sha256,
        (*required_files, f"lib/clang/{version.split('.', 1)[0]}/include/stddef.h"),
    )


def marker_text(manifest: Manifest) -> str:
    """Return the completion marker that a verified automatic extraction writes."""
    return f"version={manifest.version}\nsha256={manifest.archive_sha256}\n"


def _absolute(value: str, label: str) -> Path:
    path = Path(value)
    if not value or not path.is_absolute():
        raise WindowsLlvmSdkError(f"{label} must select an absolute existing native path")
    return path


def cache_root(environment: Mapping[str, str], manifest: Manifest) -> Path:
    """Return the automatic SDK root, using cmake/LlvmSdk.cmake's cache precedence."""
    cache = environment.get("AOBUS_LLVM_SDK_CACHE_ROOT", "")
    if not cache:
        if state := environment.get("AOBUS_STATE_ROOT", ""):
            cache = str(_absolute(state, "AOBUS_STATE_ROOT") / "cache/llvm")
        elif local := environment.get("LOCALAPPDATA", ""):
            cache = str(_absolute(local, "LOCALAPPDATA") / "Aobus/cache/llvm")
    if not cache:
        raise WindowsLlvmSdkError(
            "No ready Windows LLVM SDK selected; set AOBUS_LLVM_SDK_ROOT or its native cache root"
        )
    return _absolute(cache, "LLVM SDK cache root") / "toolchains" / f"llvm-{manifest.version}-x86_64-windows-msvc"


def ready_root(environment: Mapping[str, str], manifest: Manifest) -> tuple[Path, bool]:
    """Select a complete SDK and report whether it is the automatic cache.

    An explicit AOBUS_LLVM_SDK_ROOT is pre-provisioned and needs only its files;
    the automatic cache also needs the marker of a verified extraction.
    """
    explicit = environment.get("AOBUS_LLVM_SDK_ROOT", "")
    if explicit:
        root, cached = _absolute(explicit, "AOBUS_LLVM_SDK_ROOT"), False
    else:
        root, cached = cache_root(environment, manifest), True
    for relative in (*manifest.required_files, *((MARKER,) if cached else ())):
        if not (root / relative).is_file():
            raise WindowsLlvmSdkError(
                f"Windows LLVM SDK file is missing: {root / relative}; provision it with `ao bazel provision`"
            )
    if cached:
        marker = root / MARKER
        try:
            # Match the native owner's text identity, including Windows CRLF files.
            content = marker.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            raise WindowsLlvmSdkError(f"Windows LLVM SDK completion marker is not valid UTF-8: {marker}") from None
        if content != marker_text(manifest):
            raise WindowsLlvmSdkError(f"Windows LLVM SDK completion marker does not match the manifest: {marker}")
    return root, cached
