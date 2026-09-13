"""Conservative MSBuild tracking for a verified, managed Windows ccache wrapper."""

from __future__ import annotations

import ctypes
import hashlib
import ntpath
import os
import re
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path

TRACKING_ENV = "AOBUS_MSBUILD_CACHE_TRACKING"
_REQUIRED_CONFIG = frozenset(
    {
        "cache_dir",
        "temporary_dir",
        "debug",
        "debug_dir",
        "log_file",
        "stats_log",
        "remote_storage",
        "prefix_command",
        "prefix_command_cpp",
    }
)


def parse_config(output: str) -> dict[str, str]:
    """Read effective ccache values, including config-file and default origins."""
    values: dict[str, str] = {}
    for line in output.splitlines():
        match = re.fullmatch(r"\(.+\) ([a-z0-9_]+) = (.*)", line)
        if match is None or match[1] in values:
            raise ValueError("unrecognized ccache configuration output")
        values[match[1]] = match[2]
    if not _REQUIRED_CONFIG <= values.keys():
        raise ValueError("ccache configuration omits tracking-relevant settings")
    if values["debug"] not in {"true", "false"}:
        raise ValueError("ccache configuration has an unknown debug setting")
    return values


def _normalized(path: str) -> str:
    if path[:8].upper() == "\\\\?\\UNC\\":
        path = "\\\\" + path[8:]
    elif path.startswith("\\\\?\\"):
        path = path[4:]
    drive, tail = ntpath.splitdrive(path)
    if drive.startswith("\\\\") and not tail:
        path += "\\"
        tail = "\\"
    if not drive or not tail.startswith(("\\", "/")) or "\x00" in path:
        raise ValueError("tracking paths must be absolute Windows paths")
    return ntpath.normcase(ntpath.normpath(path))


def _under(path: str, root: str) -> bool:
    try:
        return ntpath.commonpath((path, root)) == root
    except ValueError:
        return False


def unsafe_reason(
    config: Mapping[str, str],
    *,
    excluded_roots: Sequence[str],
    project_roots: Sequence[str],
) -> str | None:
    """Classify already-normalized physical or lexical path spellings."""
    if not _REQUIRED_CONFIG <= config.keys():
        return "ccache configuration omits tracking-relevant settings"
    if config["remote_storage"] or config["prefix_command"] or config["prefix_command_cpp"]:
        return "remote storage or compiler prefixes have unverified tracking outputs"
    paths = [config["cache_dir"], config["temporary_dir"]]
    if not all(paths):
        return "ccache does not expose its effective cache and temporary directories"
    if config["debug"] not in {"true", "false"}:
        return "ccache has an unknown debug setting"
    if config["debug"] == "true":
        if not config["debug_dir"]:
            return "ccache debug output has no explicit protected directory"
        paths.append(config["debug_dir"])
    paths.extend(config[key] for key in ("log_file", "stats_log") if config[key])
    try:
        protected = tuple(_normalized(path) for path in excluded_roots)
        projects = tuple(_normalized(path) for path in project_roots)
        if not protected or not projects:
            return "native tracker exclusions or project/build paths are unavailable"
        for value in paths:
            path = _normalized(value)
            if not any(_under(path, root) for root in protected):
                return "ccache writes outside verified native tracker exclusions"
            if any(_under(path, root) or _under(root, path) for root in projects):
                return "ccache writable paths overlap a source or build tree"
    except ValueError as exc:
        return str(exc)
    return None


def native_excluded_roots(environ: Mapping[str, str]) -> tuple[str, ...]:
    """Resolve native special folders instead of trusting AppData env aliases."""
    if os.name != "nt":
        raise OSError("native Windows tracker folders are unavailable")
    buffer = ctypes.create_unicode_buffer(32768)
    # CSIDL_LOCAL_APPDATA; SHGetFolderPath resolves the actual Windows known folder.
    if vars(ctypes)["windll"].shell32.SHGetFolderPathW(None, 28, None, 0, buffer) != 0:
        raise OSError("cannot resolve the native LocalAppData folder")
    roots = [buffer.value]
    # GetTempPath reads this process environment. Only trust it for children that
    # inherit the same TEMP/TMP values; LocalAppData remains independently known.
    if all(environ.get(key) == os.environ.get(key) for key in ("TEMP", "TMP")):
        length = vars(ctypes)["windll"].kernel32.GetTempPathW(len(buffer), buffer)
        if not 0 < length < len(buffer):
            raise OSError("cannot resolve the native temporary folder")
        roots.append(buffer.value)
    return tuple(roots)


def tracking_mode(
    *,
    executable: Path,
    managed_wrapper: Path,
    expected_sha256: str,
    environ: Mapping[str, str],
    source_root: Path,
    build_roots: Sequence[Path],
    compiler_roots: Sequence[Path],
) -> tuple[str, str | None]:
    """Return runtime flag and fallback reason without changing cache settings."""
    try:
        selected = environ.get("AOBUS_MSBUILD_CL_TOOL_EXE", "")
        if not selected or _normalized(selected) != _normalized(str(managed_wrapper)):
            return "0", "the selected MSBuild wrapper is not the verified managed ccache"
        if not build_roots:
            return "0", "selected Windows build paths are unavailable"
        if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
            return "0", "the managed ccache digest is unavailable"
        for path in (executable, managed_wrapper):
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if digest != expected_sha256:
                return "0", "the managed ccache executable or wrapper changed"
        protected = native_excluded_roots(environ)
        if not compiler_roots or any(
            _under(_normalized(str(path)), _normalized(root)) for path in compiler_roots for root in protected
        ):
            return "0", "compiler source or build paths are inside native tracker exclusions"
        process = subprocess.run(
            [str(executable), "--show-config"],
            cwd=source_root,
            env=dict(environ),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=5,
            check=False,
        )
        if process.returncode:
            return "0", "ccache could not report its effective configuration"
        config = parse_config(process.stdout)
        projects = [str(source_root), *(str(path) for path in build_roots)]
        if reason := unsafe_reason(config, excluded_roots=protected, project_roots=projects):
            return "0", reason
        # Require the same protection after resolving junctions and drive aliases.
        # This only classifies cache ownership; compiler-visible paths stay intact.
        physical = dict(config)
        for key in ("cache_dir", "temporary_dir", "debug_dir", "log_file", "stats_log"):
            if physical[key]:
                physical[key] = os.path.realpath(physical[key])
        reason = unsafe_reason(
            physical,
            excluded_roots=tuple(os.path.realpath(path) for path in protected),
            project_roots=tuple(os.path.realpath(path) for path in projects),
        )
        return ("0", reason) if reason else ("1", None)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        return "0", f"MSBuild cache tracking could not be verified: {exc}"
