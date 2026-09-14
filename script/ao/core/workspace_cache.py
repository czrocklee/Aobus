"""Per-build ownership and CMake inputs for shared-workspace compiler caching."""

from __future__ import annotations

import ctypes
import hashlib
import json
import ntpath
import os
import platform
import tempfile
from collections.abc import Iterator, Mapping, MutableMapping
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

from . import builddir, compiler_cache
from .paths import PROJECT_ROOT, absolute_path

PROFILE_SCHEMA_VERSION = 1
SOURCE_ALIAS_NAME = "source"
DEBUG_ROOT = "/aobus/build"
MAP_FILE_NAME = "aobus-workspace-cache.json"
WINDOWS_MAP_FILE_NAME = "aobus-windows-workspace-cache.json"
WINDOWS_SOURCE_VIEW = "AOBUS_WINDOWS_SOURCE_VIEW"
WINDOWS_BUILD_VIEW = "AOBUS_WINDOWS_BUILD_VIEW"
WINDOWS_BUILD_PHYSICAL_ROOT = "AOBUS_WINDOWS_BUILD_PHYSICAL_ROOT"
_CMAKE_MODULE = Path("cmake/SharedWorkspaceCache.cmake")
_WINDOWS_CMAKE_MODULE = Path("cmake/SharedWorkspaceCacheWindows.cmake")
_WINDOWS_CONTEXT_COMMANDS = frozenset({"build", "check", "run", "test", "tidy", "analyze", "hygiene", "format", "perf"})
_PROFILE_CACHE_KEYS = (
    "AOBUS_SHARED_WORKSPACES",
    "AOBUS_MANAGED_SHARED_WORKSPACES",
    "AOBUS_SHARED_WORKSPACE_BUILD_DIR",
    "AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS",
    "AOBUS_SHARED_WORKSPACE_MODULE_SHA256",
    "AOBUS_SHARED_WORKSPACE_POLICY_FINGERPRINT",
    "AOBUS_SHARED_WORKSPACE_NAMESPACE",
    "AOBUS_SHARED_WORKSPACE_CCACHE",
    "AOBUS_SHARED_WORKSPACE_MAP_FILE",
)
_WINDOWS_PROFILE_CACHE_KEYS = (
    "AOBUS_WINDOWS_SHARED_WORKSPACES",
    "AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_SOURCE",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_BUILD",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_MODULE_SHA256",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_POLICY_FINGERPRINT",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_NAMESPACE",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_CCACHE",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_MSBUILD_WRAPPER",
    "AOBUS_WINDOWS_SHARED_WORKSPACE_MAP_FILE",
)


class WorkspaceCacheError(RuntimeError):
    """A shared-workspace cache layout or ownership failure."""


def _validate_supported_profile(environment: Mapping[str, str], unsupported_reason: str | None) -> None:
    if environment.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) == "1" and unsupported_reason:
        raise WorkspaceCacheError(
            f"shared-workspace compiler caching does not support {unsupported_reason}; rerun this command with "
            f"{compiler_cache.SHARED_WORKSPACES_OVERRIDE}=0"
        )


def command_uses_context(args: object, *, system: str | None = None) -> bool:
    """Require saved-profile validation or native compiler views for this command."""
    host = platform.system() if system is None else system
    command = getattr(args, "command", "")
    if command == "test" and getattr(args, "suite", "") == "tooling":
        return False
    if getattr(args, "asan", False) or getattr(args, "tsan", False):
        return True
    if command in {"format", "hygiene", "tidy"}:
        selected = getattr(args, "_resolved_sources", None)
        if selected is not None:
            from .gitfiles import CPP_SUFFIXES

            return host == "Windows" and any(str(path).endswith(CPP_SUFFIXES) for path in selected)
    return host == "Windows" and (
        command in _WINDOWS_CONTEXT_COMMANDS
        or (command == "deps" and getattr(args, "deps_action", "") in {"report", "verify"})
    )


@dataclass(frozen=True)
class WorkspaceCache:
    source_dir: Path
    cmake_arguments: tuple[str, ...]
    enabled: bool
    map_file: Path | None = None
    compiler_build_dir: Path | None = None


def _absolute(path: Path) -> Path:
    return Path(os.path.abspath(path))


def _contains(parent: Path, child: Path) -> bool:
    return parent == child or child.is_relative_to(parent)


def _module_digest(project_root: Path, module: Path = _CMAKE_MODULE) -> str:
    path = project_root / module
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        raise WorkspaceCacheError(f"cannot read shared-workspace CMake policy {path}: {exc}") from exc


def _windows_resolved_path(path: Path) -> str:
    resolved = os.path.realpath(path)
    if resolved.lower().startswith("\\\\?\\unc\\"):
        resolved = "\\\\" + resolved[8:]
    elif resolved.startswith("\\\\?\\"):
        resolved = resolved[4:]
    return ntpath.normpath(resolved)


def _windows_path_identity(path: Path) -> str:
    return ntpath.normcase(_windows_resolved_path(path))


def _windows_raw_target(path: Path) -> str:
    identity = _windows_resolved_path(path)
    if identity.startswith("\\\\"):
        return "\\??\\UNC\\" + identity[2:]
    return "\\??\\" + identity


def _windows_kernel32() -> ctypes.CDLL:
    from ctypes import wintypes

    kernel32: ctypes.CDLL = vars(ctypes)["WinDLL"]("kernel32", use_last_error=True)
    kernel32.CreateMutexW.argtypes = (wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR)
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.ReleaseMutex.argtypes = (wintypes.HANDLE,)
    kernel32.ReleaseMutex.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.QueryDosDeviceW.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD)
    kernel32.QueryDosDeviceW.restype = wintypes.DWORD
    kernel32.DefineDosDeviceW.argtypes = (wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR)
    kernel32.DefineDosDeviceW.restype = wintypes.BOOL
    return kernel32


def _windows_is_local_system() -> bool:
    from ctypes import wintypes

    kernel32 = _windows_kernel32()
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    security = vars(ctypes)["WinDLL"]("advapi32", use_last_error=True)
    security.OpenProcessToken.argtypes = (wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE))
    security.OpenProcessToken.restype = wintypes.BOOL
    security.GetTokenInformation.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    )
    security.GetTokenInformation.restype = wintypes.BOOL
    security.IsWellKnownSid.argtypes = (wintypes.LPVOID, ctypes.c_int)
    security.IsWellKnownSid.restype = wintypes.BOOL
    token = wintypes.HANDLE()
    if not security.OpenProcessToken(kernel32.GetCurrentProcess(), 0x8, ctypes.byref(token)):
        raise WorkspaceCacheError("cannot inspect the Windows process identity")
    try:
        size = wintypes.DWORD()
        security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
        if not size.value:
            raise WorkspaceCacheError("cannot determine the Windows token user size")
        data = ctypes.create_string_buffer(size.value)
        if not security.GetTokenInformation(token, 1, data, size, ctypes.byref(size)):
            raise WorkspaceCacheError("cannot read the Windows token user")
        sid = ctypes.cast(data, ctypes.POINTER(wintypes.LPVOID))[0]
        return bool(security.IsWellKnownSid(sid, 22))  # WinLocalSystemSid
    finally:
        kernel32.CloseHandle(token)


@contextmanager
def _windows_mapping_mutex() -> Iterator[None]:

    kernel32 = _windows_kernel32()
    handle = kernel32.CreateMutexW(None, False, "Local\\AobusSharedWorkspaceDriveReservation-v1")
    if not handle:
        raise WorkspaceCacheError(f"cannot create Windows drive-reservation mutex: {vars(ctypes)['get_last_error']()}")
    try:
        status = kernel32.WaitForSingleObject(handle, 30_000)
        if status not in {0, 0x80}:
            raise WorkspaceCacheError("timed out reserving Windows shared-workspace drive letters")
        try:
            yield
        finally:
            kernel32.ReleaseMutex(handle)
    finally:
        kernel32.CloseHandle(handle)


def _windows_drive_target(drive: str) -> str | None:

    kernel32 = _windows_kernel32()
    buffer = ctypes.create_unicode_buffer(32_768)
    if kernel32.QueryDosDeviceW(drive, buffer, len(buffer)):
        return buffer.value
    error = vars(ctypes)["get_last_error"]()
    if error in {2, 3}:
        return None
    raise WorkspaceCacheError(f"cannot inspect Windows drive {drive}: error {error}")


def _windows_define_drive(drive: str, target: str) -> None:

    if not _windows_kernel32().DefineDosDeviceW(0x1 | 0x8, drive, target):
        raise WorkspaceCacheError(f"cannot map Windows drive {drive}: error {vars(ctypes)['get_last_error']()}")


def _windows_remove_drive(drive: str, target: str) -> None:

    if not _windows_kernel32().DefineDosDeviceW(0x1 | 0x2 | 0x4 | 0x8, drive, target):
        raise WorkspaceCacheError(
            f"cannot remove owned Windows drive {drive}: error {vars(ctypes)['get_last_error']()}"
        )


@contextmanager
def command_context(
    args: object,
    *,
    environ: dict[str, str] | None = None,
    system: str | None = None,
    project_root: Path = PROJECT_ROOT,
) -> Iterator[None]:
    """Validate profile options, then own any fixed Windows compiler views."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    tooling_only = getattr(args, "command", "") == "test" and getattr(args, "suite", "") == "tooling"
    if not tooling_only and (getattr(args, "asan", False) or getattr(args, "tsan", False)):
        _validate_supported_profile(environment, "sanitizer builds")
    if host != "Windows" or environment.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) != "1":
        yield
        return
    if not command_uses_context(args, system=host):
        yield
        return
    if _windows_is_local_system():
        raise WorkspaceCacheError(
            "Windows shared-workspace drive views are unavailable under LocalSystem; "
            "rerun with AOBUS_SHARED_WORKSPACES=0"
        )
    if environment.get("CL") or environment.get("_CL_"):
        raise WorkspaceCacheError(
            "Windows shared-workspace caching requires CL and _CL_ to be unset; rerun with AOBUS_SHARED_WORKSPACES=0"
        )

    explicit = getattr(args, "path", None) or environment.get("BUILD_DIR")
    selected: Path | None = None
    if explicit:
        selected = Path(os.path.abspath(str(explicit)))
        build_root = selected.parent
        if selected == build_root:
            raise WorkspaceCacheError("a Windows shared-workspace build path cannot be a filesystem root")
    else:
        build_root = builddir.windows_build_root(environ=environment, project_root=project_root)
    source_identity = _windows_path_identity(project_root)
    build_identity = _windows_path_identity(build_root)
    if any(character in value for value in (source_identity, build_identity) for character in ";\r\n"):
        raise WorkspaceCacheError(
            "Windows shared-workspace source and build paths cannot contain semicolons or newlines"
        )
    if build_identity.startswith("\\\\"):
        raise WorkspaceCacheError(
            "Windows shared-workspace build trees must use local storage; "
            "rerun with AOBUS_SHARED_WORKSPACES=0 for this build path"
        )
    try:
        common = ntpath.commonpath((source_identity, build_identity))
    except ValueError:
        common = ""
    if common in {source_identity, build_identity}:
        raise WorkspaceCacheError(
            f"Windows shared-workspace B: root {build_root} overlaps source tree {project_root}; "
            "B: maps the parent of an explicit build path so sibling build trees remain reachable. "
            "Select a build path whose parent is disjoint from the source tree"
        )
    try:
        build_root.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise WorkspaceCacheError(f"cannot prepare Windows shared-workspace build root {build_root}: {exc}") from exc

    source_target = _windows_raw_target(project_root)
    build_target = _windows_raw_target(build_root)
    mapped: list[tuple[str, str]] = []
    previous = {
        key: environment.get(key)
        for key in (
            WINDOWS_SOURCE_VIEW,
            WINDOWS_BUILD_VIEW,
            WINDOWS_BUILD_PHYSICAL_ROOT,
            "CCACHE_BASEDIR",
            "CCACHE_HASHDIR",
            "CCACHE_SLOPPINESS",
            "CCACHE_NAMESPACE",
        )
    }
    active_error: BaseException | None = None
    cleanup_errors: list[str] = []
    dispatched = False
    try:
        with _windows_mapping_mutex():
            for drive, target in (("S:", source_target), ("B:", build_target)):
                occupied = _windows_drive_target(drive)
                if occupied is not None:
                    raise WorkspaceCacheError(
                        f"Windows shared-workspace drive {drive} is already in use; "
                        "rerun with AOBUS_SHARED_WORKSPACES=0 or sign out of an interrupted SSH logon"
                    )
                _windows_define_drive(drive, target)
                mapped.append((drive, target))
                if _windows_drive_target(drive) != target:
                    raise WorkspaceCacheError(f"Windows shared-workspace drive {drive} did not retain its owned target")
        environment.update(
            {
                WINDOWS_SOURCE_VIEW: "S:/",
                WINDOWS_BUILD_VIEW: "B:/",
                WINDOWS_BUILD_PHYSICAL_ROOT: str(build_root),
                "CCACHE_BASEDIR": "",
                "CCACHE_HASHDIR": "true",
                "CCACHE_SLOPPINESS": "",
            }
        )
        # Replacement belongs to do_build's lock; no-build consumers still reuse
        # the existing tree even if their arguments include --clean.
        replacing_tree = (
            getattr(args, "command", "") in {"build", "check", "run", "perf"}
            and getattr(args, "clean", False)
            and not getattr(args, "no_build", False)
        )
        if selected is not None and (selected / "CMakeCache.txt").is_file() and not replacing_tree:
            owner = _load_windows_map_file(selected / WINDOWS_MAP_FILE_NAME)
            _validate_windows_tree(
                absolute_path(selected),
                absolute_path(project_root),
                compiler_cache.read_cmake_cache(selected / "CMakeCache.txt"),
                environment,
                owner,
            )
        dispatched = True
        yield
    except BaseException as exc:
        active_error = exc
        raise
    finally:
        for key, value in previous.items():
            if value is None:
                environment.pop(key, None)
            else:
                environment[key] = value
        if dispatched and isinstance(active_error, KeyboardInterrupt):
            active_error.add_note(
                "Windows shared-workspace drives were left mapped because interrupted child processes may "
                "still be using them; sign out of this SSH logon before retrying"
            )
        else:
            with _windows_mapping_mutex():
                for drive, target in reversed(mapped):
                    try:
                        if _windows_drive_target(drive) == target:
                            _windows_remove_drive(drive, target)
                        else:
                            cleanup_errors.append(f"owned Windows shared-workspace drive {drive} changed during use")
                    except WorkspaceCacheError as exc:
                        cleanup_errors.append(str(exc))
        if cleanup_errors:
            message = "; ".join(cleanup_errors)
            if active_error is not None:
                active_error.add_note(f"Windows shared-workspace cleanup also failed: {message}")
            else:
                raise WorkspaceCacheError(message)


def policy_fingerprint(project_root: Path = PROJECT_ROOT, *, module_sha256: str | None = None) -> str:
    """Hash every stable input that can affect normalized compiler output."""
    record = {
        "schemaVersion": PROFILE_SCHEMA_VERSION,
        "sourceAlias": SOURCE_ALIAS_NAME,
        "debugRoot": DEBUG_ROOT,
        "flags": (
            "-fdebug-prefix-map=<build>=/aobus/build",
            "-ffile-prefix-map=<build>/=",
        ),
        "cmakeModuleSha256": module_sha256 or _module_digest(project_root),
    }
    encoded = json.dumps(record, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _namespace(prefix: str, fingerprint: str) -> str:
    if any(character in prefix for character in ";\r\n"):
        raise WorkspaceCacheError("the configured ccache namespace cannot contain a semicolon or newline")
    suffix = f"aobus-shared-{fingerprint}"
    return f"{prefix}:{suffix}" if prefix else suffix


def _windows_policy_fingerprint(module_sha256: str) -> str:
    record = {
        "schemaVersion": 1,
        "sourceView": "S:/",
        "buildView": "B:/",
        "compilerFlags": ["/experimental:deterministic"],
        "ccache": {"base_dir": "", "hash_dir": True, "sloppiness": ""},
        "cmakeModuleSha256": module_sha256,
    }
    encoded = json.dumps(record, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _read_map_file(path: Path) -> dict[str, object] | None:
    if path.is_symlink():
        raise WorkspaceCacheError(f"workspace-cache ownership file must not be a symbolic link: {path}")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise WorkspaceCacheError(f"workspace-cache ownership file is invalid: {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schemaVersion") != PROFILE_SCHEMA_VERSION:
        raise WorkspaceCacheError(f"workspace-cache ownership file is incompatible: {path}")
    return document


def _load_map_file(path: Path) -> dict[str, object] | None:
    document = _read_map_file(path)
    if document is not None:
        for key in ("buildDirectory", "sourceAlias", "originalSourceRoot"):
            if not isinstance(document.get(key), str):
                raise WorkspaceCacheError(f"workspace-cache ownership file has no valid {key}: {path}")
    return document


def _load_windows_map_file(path: Path) -> dict[str, object] | None:
    document = _read_map_file(path)
    if document is not None and document.get("profile") != "windows-session-v1":
        raise WorkspaceCacheError(f"workspace-cache ownership file is incompatible: {path}")
    return document


def _write_map_file(path: Path, document: Mapping[str, object]) -> None:
    try:
        data = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode()
        if path.is_file() and path.read_bytes() == data:
            return
        descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    except OSError as exc:
        raise WorkspaceCacheError(f"cannot write workspace-cache mapping {path}: {exc}") from exc


def _validate_ownership(
    build_dir: Path,
    source_alias: Path,
    project_root: Path,
    cache: Mapping[str, str],
    owner: Mapping[str, object] | None,
) -> None:
    if owner is not None:
        expected = {
            "buildDirectory": str(build_dir),
            "sourceAlias": str(source_alias),
            "originalSourceRoot": str(project_root),
        }
        for key, value in expected.items():
            if owner[key] != value:
                raise WorkspaceCacheError(
                    f"workspace-cache ownership mismatch for {key} in {build_dir / MAP_FILE_NAME}: "
                    f"expected {value!r}, recorded {owner[key]!r}"
                )

    cmake_home = cache.get("CMAKE_HOME_DIRECTORY")
    if cmake_home:
        recorded = _absolute(Path(cmake_home))
        if recorded == source_alias:
            if owner is None:
                raise WorkspaceCacheError(
                    f"cannot trust configured source alias {source_alias}: the tree has no workspace-cache "
                    "ownership record"
                )
        elif recorded.resolve(strict=False) != project_root:
            raise WorkspaceCacheError(
                f"build tree {build_dir} belongs to CMake source {cmake_home}, not {project_root}"
            )


def _prepare_source_alias(build_dir: Path, project_root: Path, cache: Mapping[str, str]) -> tuple[Path, Path]:
    source_alias = build_dir / SOURCE_ALIAS_NAME
    map_file = build_dir / MAP_FILE_NAME
    owner = _load_map_file(map_file)
    _validate_ownership(build_dir, source_alias, project_root, cache, owner)

    if source_alias.is_symlink():
        if source_alias.resolve(strict=False) != project_root:
            raise WorkspaceCacheError(
                f"source alias {source_alias} points to {source_alias.resolve(strict=False)}, not {project_root}; "
                "select another build tree or clean it explicitly"
            )
    elif source_alias.exists():
        raise WorkspaceCacheError(
            f"source alias path {source_alias} already exists and is not a symbolic link; "
            "select another build tree or clean it explicitly"
        )
    else:
        try:
            source_alias.symlink_to(project_root, target_is_directory=True)
        except FileExistsError:
            if not source_alias.is_symlink() or source_alias.resolve(strict=False) != project_root:
                raise WorkspaceCacheError(f"source alias {source_alias} appeared with different ownership") from None
        except OSError as exc:
            raise WorkspaceCacheError(f"cannot create source alias {source_alias}: {exc}") from exc
    return source_alias, map_file


def validated_source_roots(build_dir: Path, *, project_root: Path = PROJECT_ROOT) -> tuple[Path, ...]:
    """Return physical and owned alias spellings for first-party source paths."""
    build = absolute_path(build_dir)
    source = absolute_path(project_root)
    if platform.system() == "Windows" and os.environ.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) == "1":
        for key in (WINDOWS_SOURCE_VIEW, WINDOWS_BUILD_VIEW, WINDOWS_BUILD_PHYSICAL_ROOT):
            if not os.environ.get(key):
                raise WorkspaceCacheError("Windows shared-workspace drive views were not prepared by the portal")
        cache = compiler_cache.read_cmake_cache(build / "CMakeCache.txt")
        owner = _load_windows_map_file(build / WINDOWS_MAP_FILE_NAME)
        _validate_windows_tree(build, source, cache, os.environ, owner)
        if (
            cache.get("AOBUS_WINDOWS_SHARED_WORKSPACES") != "ON"
            or cache.get("AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES") != "ON"
        ):
            raise WorkspaceCacheError(f"Windows shared-workspace tree is not managed by the portal: {build}")
        return source, Path(_windows_view_path(os.environ[WINDOWS_SOURCE_VIEW]))
    source_alias = build / SOURCE_ALIAS_NAME
    map_file = build / MAP_FILE_NAME
    cache = compiler_cache.read_cmake_cache(build / "CMakeCache.txt")
    cmake_home = cache.get("CMAKE_HOME_DIRECTORY")
    cached_source_alias = cache.get("AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS")
    configured_from_alias = cmake_home is not None and cmake_home != "" and _absolute(Path(cmake_home)) == source_alias
    managed_profile = (
        cache.get("AOBUS_SHARED_WORKSPACES") == "ON" and cache.get("AOBUS_MANAGED_SHARED_WORKSPACES") == "ON"
    )
    if not configured_from_alias and not managed_profile:
        return (source,)
    if managed_profile and (cached_source_alias is None or _absolute(Path(cached_source_alias)) != source_alias):
        raise WorkspaceCacheError(f"managed source alias does not match this build tree: {cached_source_alias!r}")

    owner = _load_map_file(map_file)
    if owner is None:
        raise WorkspaceCacheError(f"configured source alias has no workspace-cache ownership file: {map_file}")
    _validate_ownership(build, source_alias, source, cache, owner)
    if not source_alias.is_symlink() or source_alias.resolve(strict=False) != source:
        raise WorkspaceCacheError(f"configured source alias is missing or changed: {source_alias}")
    return source, source_alias


def _disabled_arguments(cache: Mapping[str, str]) -> tuple[str, ...]:
    if cache.get("AOBUS_MANAGED_SHARED_WORKSPACES") != "ON":
        return ()
    arguments: list[str] = []
    for key in _PROFILE_CACHE_KEYS:
        arguments.extend(("-U", key))
    return tuple(arguments)


def _enabled_arguments(cache: Mapping[str, str], values: Mapping[str, str]) -> tuple[str, ...]:
    arguments: list[str] = []
    for key, value in values.items():
        if cache.get(key) != value:
            value_type = (
                "BOOL"
                if key
                in {
                    "AOBUS_SHARED_WORKSPACES",
                    "AOBUS_MANAGED_SHARED_WORKSPACES",
                    "AOBUS_WINDOWS_SHARED_WORKSPACES",
                    "AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES",
                }
                else "STRING"
            )
            arguments.append(f"-D{key}:{value_type}={value}")
    return tuple(arguments)


def _windows_disabled_arguments(cache: Mapping[str, str]) -> tuple[str, ...]:
    if cache.get("AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES") != "ON":
        return ()
    return tuple(argument for key in _WINDOWS_PROFILE_CACHE_KEYS for argument in ("-U", key))


def _windows_view_build_dir(build: Path, environment: Mapping[str, str]) -> Path:
    root_value = environment.get(WINDOWS_BUILD_PHYSICAL_ROOT)
    view_value = environment.get(WINDOWS_BUILD_VIEW)
    if not root_value or not view_value:
        raise WorkspaceCacheError("Windows shared-workspace drive views were not prepared by the portal")
    root = ntpath.normcase(ntpath.normpath(os.path.abspath(root_value)))
    physical = ntpath.normcase(ntpath.normpath(os.path.abspath(build)))
    try:
        relative = ntpath.relpath(physical, root)
    except ValueError as exc:
        raise WorkspaceCacheError(
            f"Windows build tree {build} is outside the fixed B: mapping root {root_value}"
        ) from exc
    if relative == ntpath.pardir or relative.startswith(ntpath.pardir + ntpath.sep):
        raise WorkspaceCacheError(f"Windows build tree {build} is outside the fixed B: mapping root {root_value}")
    return Path(_windows_view_path(ntpath.join(view_value, relative)))


def _windows_view_path(path: str | Path) -> str:
    return ntpath.normpath(str(path)).replace("\\", "/")


def canonical_portal_path(
    path: Path,
    *,
    environ: Mapping[str, str] | None = None,
    system: str | None = None,
    project_root: Path = PROJECT_ROOT,
) -> Path:
    """Map active compiler-facing Windows views to authoritative portal paths."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    if host != "Windows" or environment.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) != "1":
        return absolute_path(path)

    spelling = _windows_view_path(path)
    mappings: list[tuple[str, Path]] = []
    if source_view := environment.get(WINDOWS_SOURCE_VIEW):
        mappings.append((_windows_view_path(source_view).rstrip("/"), absolute_path(project_root)))
    if (build_view := environment.get(WINDOWS_BUILD_VIEW)) and (
        build_root := environment.get(WINDOWS_BUILD_PHYSICAL_ROOT)
    ):
        mappings.append((_windows_view_path(build_view).rstrip("/"), absolute_path(Path(build_root))))
    for view, physical in mappings:
        if spelling.casefold() == view.casefold() or spelling.casefold().startswith(view.casefold() + "/"):
            relative = spelling[len(view) :].lstrip("/")
            return absolute_path(physical / Path(relative))
    return absolute_path(path)


def compiler_source_path(
    path: Path,
    *,
    environ: Mapping[str, str] | None = None,
    system: str | None = None,
    project_root: Path = PROJECT_ROOT,
) -> Path:
    """Return an owned compiler-facing source spelling for the active Windows view."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    physical = absolute_path(path)
    if host != "Windows" or environment.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) != "1":
        return physical
    source_view = environment.get(WINDOWS_SOURCE_VIEW)
    if not source_view:
        raise WorkspaceCacheError("Windows shared-workspace source view was not prepared by the portal")
    try:
        relative = physical.relative_to(absolute_path(project_root))
    except ValueError:
        return physical
    return Path(_windows_view_path(ntpath.join(source_view, *relative.parts)))


def _windows_expected_owner(
    build: Path,
    source: Path,
    environment: Mapping[str, str],
) -> dict[str, object]:
    return {
        "schemaVersion": PROFILE_SCHEMA_VERSION,
        "profile": "windows-session-v1",
        "physicalSourceRoot": _windows_path_identity(source),
        "physicalBuildDirectory": _windows_path_identity(build),
        "physicalBuildRoot": _windows_path_identity(Path(environment[WINDOWS_BUILD_PHYSICAL_ROOT])),
        "sourceView": _windows_view_path(environment[WINDOWS_SOURCE_VIEW]),
        "buildView": _windows_view_path(_windows_view_build_dir(build, environment)),
    }


def _validate_windows_tree(
    build: Path,
    source: Path,
    cache: Mapping[str, str],
    environment: Mapping[str, str],
    owner: Mapping[str, object] | None,
) -> None:
    map_file = build / WINDOWS_MAP_FILE_NAME
    if owner is None:
        raise WorkspaceCacheError(f"Windows shared-workspace tree has no ownership file: {map_file}")
    for key, value in _windows_expected_owner(build, source, environment).items():
        if owner.get(key) != value:
            raise WorkspaceCacheError(
                f"Windows workspace-cache ownership mismatch for {key} in {map_file}: "
                f"expected {value!r}, recorded {owner.get(key)!r}"
            )
    cmake_home = cache.get("CMAKE_HOME_DIRECTORY")
    source_view = environment[WINDOWS_SOURCE_VIEW]
    if cmake_home and ntpath.normcase(ntpath.normpath(cmake_home)) != ntpath.normcase(ntpath.normpath(source_view)):
        raise WorkspaceCacheError(
            f"existing Windows build tree {build} was configured from {cmake_home}, not the fixed {source_view} view"
        )


def compiler_build_dir(
    build_dir: Path,
    *,
    environ: Mapping[str, str] | None = None,
    system: str | None = None,
) -> Path:
    """Return the compiler-facing spelling of a physical build tree."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    build = absolute_path(build_dir)
    if host == "Windows" and environment.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) == "1":
        return _windows_view_build_dir(build, environment)
    return build


def validate_consumer(build_dir: Path, *, project_root: Path = PROJECT_ROOT) -> None:
    """Check existing Windows tree ownership before no-build consumers use it."""
    if platform.system() != "Windows":
        return
    cache = compiler_cache.read_cmake_cache(build_dir / "CMakeCache.txt")
    effective = os.environ.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) == "1"
    if not effective:
        if cache.get("AOBUS_WINDOWS_SHARED_WORKSPACES") == "ON":
            raise WorkspaceCacheError("this Windows tree requires the shared-workspace profile in a fresh SSH logon")
        return
    _validate_windows_tree(
        absolute_path(build_dir),
        absolute_path(project_root),
        cache,
        os.environ,
        _load_windows_map_file(build_dir / WINDOWS_MAP_FILE_NAME),
    )
    if cache.get("AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES") != "ON" or not cache.get("CMAKE_HOME_DIRECTORY"):
        raise WorkspaceCacheError("this Windows build tree has not been configured with the managed fixed views")


def _prepare_windows(
    build: Path,
    source: Path,
    cache: Mapping[str, str],
    environment: Mapping[str, str],
) -> WorkspaceCache:
    source_view_value = environment.get(WINDOWS_SOURCE_VIEW)
    if not source_view_value:
        raise WorkspaceCacheError("Windows shared-workspace source view was not prepared by the portal")
    source_view = Path(source_view_value)
    compiler_build = _windows_view_build_dir(build, environment)
    executable_value = environment.get(compiler_cache.SHARED_WORKSPACES_CCACHE)
    if not executable_value or not Path(executable_value).is_file():
        raise WorkspaceCacheError(
            "Windows shared-workspace mode has no verified ccache executable; run setup compiler-cache"
        )
    wrapper_value = environment.get("AOBUS_MSBUILD_CL_TOOL_EXE")
    managed_wrapper = environment.get(compiler_cache.SHARED_WORKSPACES_MSBUILD_WRAPPER)
    if not wrapper_value or managed_wrapper != wrapper_value or not Path(wrapper_value).is_file():
        raise WorkspaceCacheError(
            "Windows shared-workspace mode requires the verified managed MSBuild wrapper; "
            "rerun with AOBUS_SHARED_WORKSPACES=0 to preserve an explicit wrapper"
        )

    map_file = build / WINDOWS_MAP_FILE_NAME
    owner = _load_windows_map_file(map_file)
    if (build / "CMakeCache.txt").is_file() and owner is None:
        raise WorkspaceCacheError(
            f"existing Windows build tree {build} has no fixed-view ownership record; "
            "select a new tree or clean it explicitly"
        )
    expected_owner = _windows_expected_owner(build, source, environment)
    if owner is not None:
        _validate_windows_tree(build, source, cache, environment, owner)

    try:
        build.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise WorkspaceCacheError(f"cannot prepare Windows shared-workspace build tree {build}: {exc}") from exc
    module_sha256 = _module_digest(source, _WINDOWS_CMAKE_MODULE)
    fingerprint = _windows_policy_fingerprint(module_sha256)
    namespace = _namespace(environment.get(compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX, ""), fingerprint)
    document = {
        **expected_owner,
        "policyFingerprint": fingerprint,
        "namespace": namespace,
        "debuggerSourceMap": {"from": "S:/", "to": _windows_resolved_path(source)},
    }
    _write_map_file(map_file, document)
    values = {
        "AOBUS_WINDOWS_SHARED_WORKSPACES": "ON",
        "AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES": "ON",
        "AOBUS_WINDOWS_SHARED_WORKSPACE_SOURCE": _windows_view_path(source_view),
        "AOBUS_WINDOWS_SHARED_WORKSPACE_BUILD": _windows_view_path(compiler_build),
        "AOBUS_WINDOWS_SHARED_WORKSPACE_MODULE_SHA256": module_sha256,
        "AOBUS_WINDOWS_SHARED_WORKSPACE_POLICY_FINGERPRINT": fingerprint,
        "AOBUS_WINDOWS_SHARED_WORKSPACE_NAMESPACE": namespace,
        "AOBUS_WINDOWS_SHARED_WORKSPACE_CCACHE": _windows_view_path(Path(executable_value).resolve()),
        "AOBUS_WINDOWS_SHARED_WORKSPACE_MSBUILD_WRAPPER": _windows_view_path(Path(wrapper_value).resolve()),
        "AOBUS_WINDOWS_SHARED_WORKSPACE_MAP_FILE": _windows_view_path(map_file),
    }
    if isinstance(environment, MutableMapping):
        environment["CCACHE_NAMESPACE"] = namespace
    return WorkspaceCache(
        source_view,
        _enabled_arguments(cache, values),
        True,
        map_file,
        compiler_build,
    )


def prepare(
    build_dir: Path,
    *,
    project_root: Path = PROJECT_ROOT,
    environ: Mapping[str, str] | None = None,
    system: str | None = None,
    unsupported_reason: str | None = None,
) -> WorkspaceCache:
    """Prepare one locked build tree and return its lexical CMake source and owned arguments."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    build = absolute_path(build_dir)
    source = absolute_path(project_root)
    cache = compiler_cache.read_cmake_cache(build / "CMakeCache.txt")
    effective = environment.get(compiler_cache.SHARED_WORKSPACES_EFFECTIVE) == "1"

    _validate_supported_profile(environment, unsupported_reason)
    if not effective:
        if host == "Windows" and cache.get("AOBUS_WINDOWS_SHARED_WORKSPACES") == "ON":
            raise WorkspaceCacheError(
                "this Windows tree requires fixed drive views; enable shared workspaces in a fresh SSH logon "
                "or select an ordinary build tree"
            )
        source_dir = source
        source_alias = build / SOURCE_ALIAS_NAME
        if cache.get("CMAKE_HOME_DIRECTORY") and _absolute(Path(cache["CMAKE_HOME_DIRECTORY"])) == source_alias:
            source_dir = validated_source_roots(build, project_root=source)[-1]
        disabled = _windows_disabled_arguments(cache) if host == "Windows" else _disabled_arguments(cache)
        return WorkspaceCache(source_dir, disabled, False)
    if host == "Windows":
        return _prepare_windows(build, source, cache, environment)
    if host not in {"Linux", "Darwin"}:
        raise WorkspaceCacheError("shared-workspace compiler caching is supported only on Linux and macOS")
    if _contains(source, build) or _contains(build, source):
        raise WorkspaceCacheError(
            f"shared-workspace compiler caching requires disjoint source and build trees: {source} and {build}"
        )
    if ";" in str(build):
        raise WorkspaceCacheError("shared-workspace build paths cannot contain semicolons")
    executable_value = environment.get(compiler_cache.SHARED_WORKSPACES_CCACHE)
    if not executable_value or not Path(executable_value).is_file():
        raise WorkspaceCacheError("shared-workspace mode has no verified ccache executable; run setup compiler-cache")

    try:
        build.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise WorkspaceCacheError(f"cannot prepare shared-workspace build tree {build}: {exc}") from exc
    source_alias, map_file = _prepare_source_alias(build, source, cache)
    module_sha256 = _module_digest(source)
    fingerprint = policy_fingerprint(source, module_sha256=module_sha256)
    namespace = _namespace(environment.get(compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX, ""), fingerprint)
    document = {
        "schemaVersion": PROFILE_SCHEMA_VERSION,
        "policyFingerprint": fingerprint,
        "namespace": namespace,
        "buildDirectory": str(build),
        "sourceAlias": str(source_alias),
        "originalSourceRoot": str(source),
        "debuggerSourceMap": {"from": DEBUG_ROOT, "to": str(build)},
    }
    _write_map_file(map_file, document)
    values = {
        "AOBUS_SHARED_WORKSPACES": "ON",
        "AOBUS_MANAGED_SHARED_WORKSPACES": "ON",
        "AOBUS_SHARED_WORKSPACE_BUILD_DIR": str(build),
        "AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS": str(source_alias),
        "AOBUS_SHARED_WORKSPACE_MODULE_SHA256": module_sha256,
        "AOBUS_SHARED_WORKSPACE_POLICY_FINGERPRINT": fingerprint,
        "AOBUS_SHARED_WORKSPACE_NAMESPACE": namespace,
        "AOBUS_SHARED_WORKSPACE_CCACHE": str(Path(executable_value).resolve()),
        "AOBUS_SHARED_WORKSPACE_MAP_FILE": str(map_file),
    }
    return WorkspaceCache(source_alias, _enabled_arguments(cache, values), True, map_file, build)
