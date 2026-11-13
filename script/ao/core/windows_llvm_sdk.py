"""Read the pinned Windows LLVM SDK contract.

The provisioner and the Bazel lint SDK import both evaluate one manifest,
cache precedence and completion marker here. The lint repository
rule runs its helper as a script, so this module imports only the standard library.
"""

from __future__ import annotations

import json
from collections.abc import Mapping
from pathlib import Path
from typing import NamedTuple

MANIFEST = Path(__file__).resolve().parents[1] / "windows-llvm-sdk.json"
MARKER = ".aobus-llvm-sdk-complete"


class WindowsLlvmSdkError(ValueError):
    """The manifest or a selected SDK root does not satisfy the pinned contract."""


class WindowsLlvmSdkSelectedFileMissing(WindowsLlvmSdkError):
    """A selected SDK root is present but a required file or marker is absent.

    The selected root and precedence are already authoritative; the exact absent
    file lets an importer report a not-yet-provisioned SDK.
    """

    def __init__(self, message: str, path: Path):
        super().__init__(message)
        self.path = path


class Manifest(NamedTuple):
    version: str
    archive_sha256: str
    # The listed files plus the Clang resource include of the pinned major.
    required_files: tuple[str, ...]


def read_manifest(path: Path) -> Manifest:
    """Read the governed manifest and derive its required SDK files."""
    value = json.loads(path.read_text(encoding="utf-8"))
    version = value["version"]
    return Manifest(
        version,
        value["archiveSha256"],
        (*value["requiredFiles"], f"lib/clang/{version.split('.', 1)[0]}/include/stddef.h"),
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
    """Return the automatic SDK root using the established cache override precedence."""
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


def _require_selected_file(path: Path) -> None:
    """Report a genuinely absent selected file as a not-yet-provisioned SDK."""
    if not path.exists():
        raise WindowsLlvmSdkSelectedFileMissing(
            f"Windows LLVM SDK file is missing: {path}; provision it with `ao deps provision`", path
        )


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
        _require_selected_file(root / relative)
    if cached:
        marker = root / MARKER
        # A native owner may write the marker with Windows CRLF line endings.
        if marker.read_bytes().replace(b"\r\n", b"\n") != marker_text(manifest).encode():
            raise WindowsLlvmSdkError(f"Windows LLVM SDK completion marker does not match the manifest: {marker}")
    return root, cached
