"""Native Bazel input admission and scoped discovery observations.

Selection, path checks and read inventories are shared by repository probes and
portal launchers; this module never chooses a build tree or provisions an SDK.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import platform
import re
import shutil
import sys
from collections.abc import Callable, Iterator, Mapping
from contextlib import contextmanager
from contextvars import ContextVar
from pathlib import Path
from typing import Any

from .paths import PROJECT_ROOT as PROJECT_ROOT

_PROBE: ContextVar[Callable[[Path], None] | None] = ContextVar("bazel_input_probe", default=None)


class BazelWorkspaceError(RuntimeError):
    """An unavailable or ungoverned native input prevents Bazel execution."""


@contextmanager
def capture_probes(observer: Callable[[Path], None] | None) -> Iterator[None]:
    """Observe lexical input decisions for this scope; restore an outer observer."""
    token = _PROBE.set(observer)
    try:
        yield
    finally:
        _PROBE.reset(token)


def probe(path: Path) -> Path:
    """Record existence/symlink decisions, including missing selected candidates."""
    if (observer := _PROBE.get()) is not None:
        observer(path.absolute())
    return path


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise BazelWorkspaceError(f"expected a JSON object: {path}")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    content = (json.dumps(value, indent=2, sort_keys=True) + "\n").replace("\n", os.linesep).encode("utf-8")
    try:
        if path.read_bytes() == content:
            return
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def native_platform() -> str:
    # Host detection must not select build trees or create checkout-local state.
    host = {"linux": "linux", "darwin": "macos", "win32": "windows"}.get(sys.platform)
    if host is None:
        raise BazelWorkspaceError(f"unsupported native Bazel host: {sys.platform}")
    return host


def source_root() -> str:
    """The original checkout spelling, never Bazel's execution root or a compiler view."""
    path = PROJECT_ROOT
    if os.name == "nt":
        resolved = os.path.realpath(path)
        if resolved.lower().startswith("\\\\?\\unc\\"):
            resolved = "\\\\" + resolved[8:]
        elif resolved.startswith("\\\\?\\"):
            resolved = resolved[4:]
        path = Path(resolved)
    return path.as_posix()


def child_environment(environ: Mapping[str, str] | None = None) -> dict[str, str]:
    """Keep the compiler, not Nix's implicit all-dependencies compiler flags."""
    environment = dict(os.environ if environ is None else environ)
    for key in list(environment):
        if key.startswith(("NIX_CFLAGS_", "NIX_LDFLAGS", "CCACHE_", "SCCACHE_")) or key in {
            "CPATH",
            "C_INCLUDE_PATH",
            "CPLUS_INCLUDE_PATH",
            "LIBRARY_PATH",
            "CMAKE_PREFIX_PATH",
            "NIXPKGS_CMAKE_PREFIX_PATH",
            "CMAKE_INCLUDE_PATH",
            "CMAKE_LIBRARY_PATH",
            "CFLAGS",
            "CXXFLAGS",
            "CPPFLAGS",
            "LDFLAGS",
        }:
            del environment[key]
    return environment


def host_machine() -> str:
    if os.name != "nt":
        return platform.machine()

    # SYSTEM_INFO uses fixed-width DWORD/WORD fields and pointer-width addresses/masks.
    # Read only the native ISA; platform.machine()/uname() can query Windows through WMI.
    class ProcessorArchitecture(ctypes.Structure):
        _fields_ = [("processor_architecture", ctypes.c_uint16), ("reserved", ctypes.c_uint16)]

    class ProcessorInfo(ctypes.Union):
        _anonymous_ = ("architecture",)
        _fields_ = [("oem_id", ctypes.c_uint32), ("architecture", ProcessorArchitecture)]

    class SystemInfo(ctypes.Structure):
        _anonymous_ = ("processor",)
        _fields_ = [
            ("processor", ProcessorInfo),
            ("page_size", ctypes.c_uint32),
            ("minimum_application_address", ctypes.c_void_p),
            ("maximum_application_address", ctypes.c_void_p),
            ("active_processor_mask", ctypes.c_size_t),
            ("number_of_processors", ctypes.c_uint32),
            ("processor_type", ctypes.c_uint32),
            ("allocation_granularity", ctypes.c_uint32),
            ("processor_level", ctypes.c_uint16),
            ("processor_revision", ctypes.c_uint16),
        ]

    kernel32: ctypes.CDLL = vars(ctypes)["WinDLL"]("kernel32")
    get_native_system_info = kernel32.GetNativeSystemInfo
    get_native_system_info.argtypes = (ctypes.POINTER(SystemInfo),)
    get_native_system_info.restype = None
    info = SystemInfo()
    get_native_system_info(ctypes.byref(info))
    architecture = info.processor_architecture
    machines = {0: "x86", 9: "AMD64", 12: "ARM64"}
    if architecture not in machines:
        raise BazelWorkspaceError(f"unsupported native Windows processor architecture: {architecture}")
    return machines[architecture]


def executable(name: str, *, nix: bool = False, path: str | None = None) -> str:
    found = shutil.which(name) if path is None else shutil.which(name, path=path)
    if _PROBE.get() is not None:
        dirname, basename = os.path.split(name)
        if dirname:
            directories = [dirname]
        else:
            search_path = os.environ.get("PATH") if path is None else path
            if search_path is None:
                confstr = getattr(os, "confstr", None)
                try:
                    search_path = confstr("CS_PATH") if confstr is not None else os.defpath
                except (AttributeError, ValueError):
                    search_path = os.defpath
            directories = (search_path or "").split(os.pathsep)
        filenames = [basename]
        if sys.platform == "win32":
            pathext = os.environ.get("PATHEXT") or ".COM;.EXE;.BAT;.CMD;.VBS;.JS;.WS;.MSC"
            filenames.extend(basename + ext.rstrip(".") for ext in pathext.split(os.pathsep) if ext)
            if not dirname:
                # Cover optional implicit cwd without duplicating WinAPI policy.
                # This conservative group must never establish a PATH cutoff.
                for filename in filenames:
                    probe(Path(os.path.join(os.curdir, filename)))
        # Inventory a superset, not a second executable-selection engine. Exact
        # lexical equality alone proves later groups cannot affect which's result.
        cutoff_safe = all(not os.path.dirname(filename) for filename in filenames)
        for directory in directories:
            candidates = [os.path.join(directory, filename) for filename in filenames]
            for candidate in candidates:
                probe(Path(candidate))
            if cutoff_safe and found in candidates:
                break
        if found:
            probe(Path(found))
    if not found or (nix and not Path(found).resolve().is_relative_to("/nix/store")):
        raise BazelWorkspaceError(f"{name} is unavailable in the selected native environment")
    # Preserve argv[0]: Nix multicall tools and Clang drivers dispatch by basename.
    return str(Path(found).absolute())


def evidence(paths: list[Path]) -> list[dict[str, str]]:
    return [{"path": str(path.resolve()), "sha256": sha256(path)} for path in paths]


def native_path(path: Path, intended_root: Path) -> Path:
    """Check against a lexical root anchored at the canonical triplet prefix."""
    resolved = probe(path).resolve()
    if not resolved.is_relative_to(intended_root):
        raise BazelWorkspaceError(
            f"native provider path escapes intended configuration/root {intended_root}: {path} -> {resolved}"
        )
    return resolved


def header_root(roots: list[Path], marker: str) -> Path:
    matches = {root / "include" for root in roots if probe(root / "include" / marker).is_file()}
    if len(matches) != 1:
        raise BazelWorkspaceError(f"missing/ambiguous governed header {marker}: {sorted(map(str, matches))}")
    return matches.pop()


def verify_header_version(path: Path, version: str, macros: list[str], *, encoded: bool = False) -> None:
    text = path.read_text(encoding="utf-8")
    values = []
    for macro in macros:
        matches = re.findall(r"^\s*#\s*define\s+" + re.escape(macro) + r"\s+(\d+)\b", text, re.MULTILINE)
        if len(matches) != 1:
            raise BazelWorkspaceError(f"missing/ambiguous version macro {macro}: {path}")
        values.append(int(matches[0]))
    actual = (
        f"{values[0] // 100000}.{values[0] // 100 % 1000}.{values[0] % 100}" if encoded else ".".join(map(str, values))
    )
    if actual != version:
        raise BazelWorkspaceError(f"header version mismatch: {path}: {actual} != provider {version}")


def require_supported_source() -> None:
    """Reject the unsupported Windows network source layout before invoking Bazel."""
    if native_platform() == "windows":
        source = str(PROJECT_ROOT.resolve()).replace("/", "\\")
        network = (
            source.lower().startswith("\\\\?\\unc\\") if source.startswith("\\\\?\\") else source.startswith("\\\\")
        )
        if network:
            raise BazelWorkspaceError(
                "Windows Bazel commands require a local source checkout; UNC/mapped-network checkouts are not "
                "supported. Use the existing CMake commands for this shared checkout."
            )
