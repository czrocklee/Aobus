"""Project-managed compiler-cache setup and activation.

Local development uses ccache while CI supplies sccache through its pinned
GitHub Action. This module owns their shared capacity and launcher policy; it
never adds a compiler-cache wrapper directory to PATH.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import ntpath
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from collections.abc import Mapping, MutableMapping, Sequence
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
MANIFEST_PATH = PROJECT_ROOT / "script" / "ao" / "compiler-cache.json"
CONFIG_SCHEMA_VERSION = 1
_SIZE_PATTERN = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([kmgt](?:i?b)?)?\s*$", re.IGNORECASE)
_MANAGED_LAUNCHER_MARKERS = {
    "C": "AOBUS_MANAGED_C_COMPILER_LAUNCHER",
    "CXX": "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER",
}


class CompilerCacheError(RuntimeError):
    """A compiler-cache contract, setup, or activation failure."""


@dataclass(frozen=True)
class CachePolicy:
    default_size: str
    local_provider: str
    local_minimum_version: str
    linux_version: str
    macos_formula: str
    windows_version: str
    windows_url: str
    windows_sha256: str
    windows_member: str
    ci_provider: str
    ci_version: str
    ci_action: str


def _required_string(record: Mapping[str, object], key: str) -> str:
    value = record.get(key)
    if not isinstance(value, str) or not value:
        raise CompilerCacheError(f"compiler-cache contract field {key!r} must be a non-empty string")
    return value


def load_policy(path: Path = MANIFEST_PATH) -> CachePolicy:
    """Read and strictly validate the shared compiler-cache contract."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CompilerCacheError(f"cannot read compiler-cache contract {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schemaVersion") != 1:
        raise CompilerCacheError("compiler-cache contract has an unsupported schemaVersion")
    local = document.get("local")
    ci = document.get("ci")
    if not isinstance(local, dict) or not isinstance(ci, dict):
        raise CompilerCacheError("compiler-cache contract must define local and ci objects")
    windows = local.get("windows")
    if not isinstance(windows, dict):
        raise CompilerCacheError("compiler-cache contract must define local.windows")
    policy = CachePolicy(
        default_size=_required_string(document, "defaultCacheSize"),
        local_provider=_required_string(local, "provider"),
        local_minimum_version=_required_string(local, "minimumVersion"),
        linux_version=_required_string(local, "linuxVersion"),
        macos_formula=_required_string(local, "macosFormula"),
        windows_version=_required_string(windows, "version"),
        windows_url=_required_string(windows, "archiveUrl"),
        windows_sha256=_required_string(windows, "archiveSha256"),
        windows_member=_required_string(windows, "archiveMember"),
        ci_provider=_required_string(ci, "provider"),
        ci_version=_required_string(ci, "version"),
        ci_action=_required_string(ci, "action"),
    )
    if policy.local_provider != "ccache" or policy.ci_provider != "sccache":
        raise CompilerCacheError("compiler-cache contract names unsupported providers")
    if not re.fullmatch(r"[0-9a-f]{64}", policy.windows_sha256):
        raise CompilerCacheError("compiler-cache Windows archiveSha256 must be lowercase SHA-256")
    size_bytes(policy.default_size)
    for version in (policy.local_minimum_version, policy.linux_version, policy.windows_version):
        if _version_tuple(version) is None:
            raise CompilerCacheError(f"compiler-cache contract has an invalid ccache version: {version!r}")
    return policy


def _version_tuple(version: str) -> tuple[int, int, int] | None:
    if re.fullmatch(r"\d+\.\d+(?:\.\d+)?", version) is None:
        return None
    parts = [int(part) for part in version.split(".")]
    padded = parts + [0] * (3 - len(parts))
    return padded[0], padded[1], padded[2]


def version_at_least(version: str, minimum: str) -> bool:
    actual = _version_tuple(version)
    required = _version_tuple(minimum)
    return actual is not None and required is not None and actual >= required


def size_bytes(value: str) -> int:
    """Parse ccache sizes, whose bare numbers are GiB and SI prefixes are decimal."""
    match = _SIZE_PATTERN.fullmatch(value)
    if match is None:
        raise CompilerCacheError(f"invalid compiler-cache size: {value!r}")
    try:
        number = Decimal(match.group(1))
    except InvalidOperation as exc:
        raise CompilerCacheError(f"invalid compiler-cache size: {value!r}") from exc
    suffix = (match.group(2) or "").lower().removesuffix("b")
    if not suffix:
        result = int(number * 1024**3)
        if result > 2**64 - 1:
            raise CompilerCacheError(f"compiler-cache size is too large: {value!r}")
        return result
    binary = suffix.endswith("i")
    prefix = suffix.removesuffix("i")
    power = {"k": 1, "m": 2, "g": 3, "t": 4}[prefix]
    result = int(number * ((1024 if binary else 1000) ** power))
    if result > 2**64 - 1:
        raise CompilerCacheError(f"compiler-cache size is too large: {value!r}")
    return result


def larger_size(*values: str) -> str:
    """Return the largest configured size; ccache spells unlimited as zero."""
    parsed = [(size_bytes(value), value) for value in values if value]
    if unlimited := next((value for size, value in parsed if size == 0), None):
        return unlimited
    return max(parsed)[1]


def state_root(
    *,
    environ: Mapping[str, str] | None = None,
    system: str | None = None,
) -> Path:
    environment = os.environ if environ is None else environ
    if configured := environment.get("AOBUS_STATE_ROOT"):
        return Path(configured)
    host = platform.system() if system is None else system
    if host == "Windows":
        if local_app_data := environment.get("LOCALAPPDATA"):
            return Path(local_app_data) / "Aobus"
        return Path.home() / "AppData" / "Local" / "Aobus"
    if host == "Darwin":
        return Path(environment.get("HOME", str(Path.home()))) / "Library" / "Caches" / "Aobus"
    if xdg_cache := environment.get("XDG_CACHE_HOME"):
        return Path(xdg_cache) / "Aobus"
    return Path(environment.get("HOME", str(Path.home()))) / ".cache" / "Aobus"


def config_path(root: Path) -> Path:
    return root / "config" / "compiler-cache.json"


def cache_directory(root: Path, *, system: str) -> Path:
    if system in {"Linux", "Darwin"}:
        return root / "ccache"
    return root / "cache" / "ccache"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _atomic_write(path: Path, data: bytes, *, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == data:
        if executable:
            path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if executable:
            temporary.chmod(0o755)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _write_config(path: Path, record: Mapping[str, object]) -> None:
    _atomic_write(path, (json.dumps(record, indent=2, sort_keys=True) + "\n").encode())


def _read_config(path: Path) -> dict[str, object] | None:
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    return record if isinstance(record, dict) else None


def _executable_version(executable: Path) -> str:
    try:
        result = subprocess.run(
            [str(executable), "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise CompilerCacheError(f"cannot run compiler cache {executable}: {exc}") from exc
    match = re.search(r"\bccache version ([0-9][^\s]*)", result.stdout)
    if match is None:
        raise CompilerCacheError(f"cannot identify ccache version from {executable}")
    return match.group(1)


def _resolve_posix_ccache(policy: CachePolicy, *, system: str, environ: Mapping[str, str]) -> Path:
    if system == "Darwin":
        brew = environ.get("AOBUS_HOMEBREW") or shutil.which("brew", path=environ.get("PATH"))
        if brew is None:
            raise CompilerCacheError("Homebrew is unavailable; install it from https://brew.sh/ first")
        try:
            subprocess.run([brew, "install", policy.macos_formula], check=True)
            prefix = subprocess.run(
                [brew, "--prefix", policy.macos_formula], check=True, capture_output=True, text=True
            ).stdout.strip()
        except (OSError, subprocess.SubprocessError) as exc:
            raise CompilerCacheError(f"Homebrew could not install {policy.macos_formula}: {exc}") from exc
        candidate = Path(prefix) / "bin" / "ccache"
        if candidate.is_file() and not version_at_least(_executable_version(candidate), policy.local_minimum_version):
            try:
                subprocess.run([brew, "upgrade", policy.macos_formula], check=True)
            except (OSError, subprocess.SubprocessError) as exc:
                raise CompilerCacheError(f"Homebrew could not upgrade {policy.macos_formula}: {exc}") from exc
    else:
        located = shutil.which("ccache", path=environ.get("PATH"))
        if located is None:
            raise CompilerCacheError("the pinned Linux shell does not provide ccache")
        candidate = Path(located)
    if not candidate.is_file():
        raise CompilerCacheError(f"ccache executable is missing: {candidate}")
    return candidate.resolve()


def _download(url: str, destination: Path) -> None:
    with urllib.request.urlopen(url, timeout=120) as response, destination.open("wb") as stream:
        shutil.copyfileobj(response, stream)


def install_windows_archive(
    archive: Path,
    destination: Path,
    *,
    expected_sha256: str,
    member_name: str,
) -> str:
    """Verify one official ZIP and atomically install its exact executable member."""
    actual = _sha256(archive)
    if actual != expected_sha256:
        raise CompilerCacheError(f"compiler-cache archive checksum mismatch: expected {expected_sha256}, got {actual}")
    try:
        with zipfile.ZipFile(archive) as bundle:
            matches = [entry for entry in bundle.infolist() if entry.filename == member_name]
            if len(matches) != 1 or matches[0].is_dir():
                raise CompilerCacheError(f"compiler-cache archive does not contain exactly {member_name!r}")
            mode = matches[0].external_attr >> 16
            if stat.S_ISLNK(mode):
                raise CompilerCacheError("compiler-cache executable archive member must not be a symbolic link")
            data = bundle.read(matches[0])
    except zipfile.BadZipFile as exc:
        raise CompilerCacheError(f"compiler-cache archive is not a valid ZIP: {archive}") from exc
    if not data:
        raise CompilerCacheError("compiler-cache executable archive member is empty")
    _atomic_write(destination, data, executable=True)
    return hashlib.sha256(data).hexdigest()


def _install_windows_ccache(policy: CachePolicy, root: Path) -> Path:
    destination = root / "tools" / "compiler-cache" / f"ccache-{policy.windows_version}" / "ccache.exe"
    existing = _read_config(config_path(root))
    if (
        destination.is_file()
        and isinstance(existing, dict)
        and existing.get("version") == policy.windows_version
        and existing.get("executableSha256") == _sha256(destination)
    ):
        return destination
    downloads = root / "cache" / "compiler-cache" / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / Path(policy.windows_url).name
    if not archive.is_file() or _sha256(archive) != policy.windows_sha256:
        descriptor, temporary_name = tempfile.mkstemp(prefix=f".{archive.name}.", dir=downloads)
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            try:
                _download(policy.windows_url, temporary)
            except OSError as exc:
                raise CompilerCacheError(f"cannot download compiler cache from {policy.windows_url}: {exc}") from exc
            if _sha256(temporary) != policy.windows_sha256:
                raise CompilerCacheError("downloaded compiler-cache archive failed SHA-256 verification")
            os.replace(temporary, archive)
        finally:
            temporary.unlink(missing_ok=True)
    install_windows_archive(
        archive,
        destination,
        expected_sha256=policy.windows_sha256,
        member_name=policy.windows_member,
    )
    return destination


def _configured_max_size(executable: Path, environment: Mapping[str, str]) -> str | None:
    try:
        result = subprocess.run(
            [str(executable), "--get-config", "max_size"],
            env=dict(environment),
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
        value = result.stdout.strip()
        size_bytes(value)
        return value
    except (OSError, subprocess.SubprocessError, CompilerCacheError):
        return None


def _validate_local_version(version: str, policy: CachePolicy, system: str) -> None:
    if not version_at_least(version, policy.local_minimum_version):
        raise CompilerCacheError(
            f"ccache {version} is installed; Aobus requires {policy.local_minimum_version} or newer"
        )
    pinned = {"Linux": policy.linux_version, "Windows": policy.windows_version}.get(system)
    if pinned is not None and version != pinned:
        raise CompilerCacheError(f"ccache {version} is installed; Aobus requires exactly {pinned} on {system}")


def setup_local(
    *,
    environ: MutableMapping[str, str] | None = None,
    system: str | None = None,
) -> dict[str, str]:
    """Install or resolve local ccache, then persist Aobus-local activation."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    try:
        policy = load_policy()
        root = state_root(environ=environment, system=host)
        executable = (
            _install_windows_ccache(policy, root)
            if host == "Windows"
            else _resolve_posix_ccache(policy, system=host, environ=environment)
        )
        actual_version = _executable_version(executable)
        _validate_local_version(actual_version, policy, host)
        directory = Path(environment.get("CCACHE_DIR") or cache_directory(root, system=host))
        probe_environment = {**environment, "CCACHE_DIR": str(directory)}
        # Read the directory's persistent setting without an ambient capacity
        # override masking a larger cache the developer already configured.
        probe_environment.pop("CCACHE_MAXSIZE", None)
        candidates = [policy.default_size]
        previous = _read_config(config_path(root))
        if isinstance(previous, dict) and isinstance(previous.get("cacheSize"), str):
            try:
                size_bytes(str(previous["cacheSize"]))
            except CompilerCacheError:
                pass
            else:
                candidates.append(str(previous["cacheSize"]))
        if configured := _configured_max_size(executable, probe_environment):
            candidates.append(configured)
        cache_size = larger_size(*candidates)
        record = {
            "schemaVersion": CONFIG_SCHEMA_VERSION,
            "provider": policy.local_provider,
            "version": actual_version,
            "executable": str(executable),
            "executableSha256": _sha256(executable),
            "cacheSize": cache_size,
        }
        updates = local_environment(record, root=root, system=host, environ=environment)
        if host == "Windows":
            # Provision the saved configuration even when this invocation
            # selects an explicit wrapper instead of the managed one.
            _copy_wrapper(executable, _local_windows_wrapper(root, actual_version))
        _write_config(config_path(root), record)
        environment.update(updates)
        return updates
    except OSError as exc:
        raise CompilerCacheError(f"compiler-cache setup failed: {exc}") from exc


def _trusted_local_config(root: Path, policy: CachePolicy, system: str) -> dict[str, object] | None:
    path = config_path(root)
    try:
        path.stat()
    except FileNotFoundError:
        return None
    record = _read_config(path)
    if record is None:
        raise CompilerCacheError(f"saved compiler-cache configuration is unreadable or invalid: {path}")
    if (
        record.get("schemaVersion") != CONFIG_SCHEMA_VERSION
        or record.get("provider") != policy.local_provider
        or not isinstance(record.get("version"), str)
        or not isinstance(record.get("cacheSize"), str)
        or not isinstance(record.get("executable"), str)
        or not isinstance(record.get("executableSha256"), str)
    ):
        raise CompilerCacheError(f"saved compiler-cache configuration is incompatible or invalid: {path}")
    _validate_local_version(str(record["version"]), policy, system)
    executable = Path(str(record["executable"]))
    size_bytes(str(record["cacheSize"]))
    if not executable.is_file() or _sha256(executable) != record["executableSha256"]:
        raise CompilerCacheError(f"recorded compiler-cache executable is missing or changed: {executable}")
    return record


def _copy_wrapper(executable: Path, wrapper: Path) -> None:
    try:
        data = executable.read_bytes()
        _atomic_write(wrapper, data, executable=True)
    except OSError as exc:
        raise CompilerCacheError(f"cannot prepare compiler-cache wrapper {wrapper}: {exc}") from exc


def _local_windows_wrapper(root: Path, version: str) -> Path:
    return (root / "tools" / "compiler-cache" / f"ccache-{version}" / "msbuild" / "cl.exe").resolve()


def local_environment(
    record: Mapping[str, object],
    *,
    root: Path,
    system: str,
    environ: Mapping[str, str],
    project_root: Path = PROJECT_ROOT,
) -> dict[str, str]:
    """Generate managed local settings without replacing explicit overrides."""
    executable = Path(str(record["executable"])).resolve()
    updates: dict[str, str] = {}
    defaults = {
        "CCACHE_DIR": str(cache_directory(root, system=system)),
        "CCACHE_MAXSIZE": str(record["cacheSize"]),
        "CCACHE_COMPRESS": "1",
    }
    if system != "Windows":
        defaults["CCACHE_BASEDIR"] = str(project_root)
    for key, value in defaults.items():
        if not environ.get(key):
            updates[key] = value
    for language, marker in _MANAGED_LAUNCHER_MARKERS.items():
        key = f"CMAKE_{language}_COMPILER_LAUNCHER"
        if key not in environ:
            updates[key] = str(executable)
            updates[marker] = "1"
    if system == "Windows" and not environ.get("AOBUS_MSBUILD_CL_TOOL_EXE"):
        updates["AOBUS_MSBUILD_CL_TOOL_EXE"] = str(_local_windows_wrapper(root, str(record["version"])))
    return updates


def activate_local(
    *,
    environ: MutableMapping[str, str] | None = None,
    system: str | None = None,
) -> bool:
    """Activate a previously configured local cache without provisioning tools."""
    environment = os.environ if environ is None else environ
    host = platform.system() if system is None else system
    try:
        policy = load_policy()
        root = state_root(environ=environment, system=host)
        record = _trusted_local_config(root, policy, host)
        if record is None:
            return False
        updates = local_environment(record, root=root, system=host, environ=environment)
        if wrapper_name := updates.get("AOBUS_MSBUILD_CL_TOOL_EXE"):
            wrapper = Path(wrapper_name)
            if not wrapper.is_file() or _sha256(wrapper) != record["executableSha256"]:
                raise CompilerCacheError(f"compiler-cache wrapper is missing or changed: {wrapper}")
    except (OSError, CompilerCacheError) as exc:
        portal = "ao.bat" if host == "Windows" else "./ao"
        raise CompilerCacheError(f"{exc}; run {portal} setup compiler-cache") from exc
    environment.update(updates)
    return True


def ci_environment(
    *,
    environ: Mapping[str, str],
    system: str,
    state: Path,
) -> dict[str, str]:
    """Generate CI adapter settings while leaving GHA backend credentials untouched."""
    policy = load_policy()
    supplied = environ.get("SCCACHE_PATH")
    if not supplied:
        raise CompilerCacheError("sccache action did not provide SCCACHE_PATH")
    executable = Path(supplied)
    if system == "Windows" and not executable.is_file() and Path(f"{supplied}.exe").is_file():
        executable = Path(f"{supplied}.exe")
    if not executable.is_file():
        raise CompilerCacheError(f"sccache action did not provide a valid executable: {supplied}")
    executable = executable.resolve()
    updates: dict[str, str] = {"SCCACHE_PATH": str(executable)}
    for key in ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER"):
        if key not in environ:
            updates[key] = str(executable)
            language = "CXX" if "CXX" in key else "C"
            updates[_MANAGED_LAUNCHER_MARKERS[language]] = "1"
    if not environ.get("SCCACHE_CACHE_SIZE"):
        updates["SCCACHE_CACHE_SIZE"] = policy.default_size
    if system == "Windows" and not environ.get("AOBUS_MSBUILD_CL_TOOL_EXE"):
        wrapper = state / "tools" / "compiler-cache" / f"sccache-{policy.ci_version}" / "msbuild" / "cl.exe"
        _copy_wrapper(executable, wrapper)
        updates["AOBUS_MSBUILD_CL_TOOL_EXE"] = str(wrapper.resolve())
    return updates


def _cmake_cache(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return {}
    values = {}
    for line in lines:
        key, separator, value = line.partition("=")
        if separator and ":" in key and not key.startswith(("//", "#")):
            values[key.split(":", 1)[0]] = value
    return values


def _same_launcher(first: str | None, second: str) -> bool:
    """Compare CMake launcher paths while tolerating Windows slash spelling."""
    if first == second:
        return True
    if first is None or ";" in first or ";" in second:
        return False
    windows_path = re.compile(r"^(?:[A-Za-z]:[\\/]|\\\\)")
    if windows_path.match(first) and windows_path.match(second):
        return ntpath.normcase(ntpath.normpath(first)) == ntpath.normcase(ntpath.normpath(second))
    return False


def cmake_launcher_arguments(
    environ: Mapping[str, str] | None = None,
    *,
    build_dir: Path | None = None,
) -> list[str]:
    """Return only the launcher changes needed for one CMake configure."""
    environment = os.environ if environ is None else environ
    cache = _cmake_cache(build_dir / "CMakeCache.txt") if build_dir is not None else {}
    arguments: list[str] = []
    for language in ("C", "CXX"):
        key = f"CMAKE_{language}_COMPILER_LAUNCHER"
        marker = _MANAGED_LAUNCHER_MARKERS[language]
        desired = environment.get(key)
        managed = bool(desired) and environment.get(marker) == "1"
        cached_marker = cache.get(marker) == "ON"
        if desired is not None:
            cached = cache.get(key)
            # A launcher in the CMake cache without our marker was configured
            # directly by the developer, including an empty value to disable it.
            if managed and key in cache and not cached_marker:
                continue
            if not _same_launcher(cached, desired):
                arguments.append(f"-D{key}={desired}")
            if managed and not cached_marker:
                arguments.append(f"-D{marker}:BOOL=ON")
            elif not managed and cached_marker:
                arguments.extend(("-U", marker))
        elif cached_marker:
            arguments.extend(("-U", key, "-U", marker))
    return arguments


def _write_github_environment(path: Path, updates: Mapping[str, str]) -> None:
    for key, value in updates.items():
        if "\n" in value or "\r" in value:
            raise CompilerCacheError(f"compiler-cache environment value for {key} contains a newline")
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        for key, value in updates.items():
            stream.write(f"{key}={value}\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Aobus compiler-cache policy helper")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("ci-contract", help="emit GitHub output fields for the pinned CI provider")
    activate = subparsers.add_parser("activate-ci", help="adapt action-provided sccache to Aobus launchers")
    activate.add_argument("--github-env", type=Path, required=True)
    activate.add_argument("--state-root", type=Path, required=True)
    activate.add_argument("--platform", choices=("Linux", "Darwin", "Windows"), required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        policy = load_policy()
        if args.command == "ci-contract":
            print(f"version={policy.ci_version}")
            return 0
        updates = ci_environment(environ=os.environ, system=args.platform, state=args.state_root)
        _write_github_environment(args.github_env, updates)
        return 0
    except (CompilerCacheError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
