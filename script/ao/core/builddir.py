"""Native host metadata and checkout-isolated Bazel output/state roots."""

import hashlib
import ntpath
import os
import platform
import re
import sys
import time
import uuid
from collections.abc import Mapping
from dataclasses import dataclass, replace
from pathlib import Path

from .paths import PROJECT_ROOT


def linux_build_root(*, environ: Mapping[str, str] | None = None, project_root: Path = PROJECT_ROOT) -> Path:
    """Return the project-specific Linux build root, host-overridable via AOBUS_BUILD_ROOT."""
    environment = os.environ if environ is None else environ
    base = Path(environment.get("AOBUS_BUILD_ROOT") or "/tmp/build")
    return base / project_root.name


BUILD_ROOT = linux_build_root()

_CHECKOUT_ID_FILE = "aobus-checkout-id"
_CHECKOUT_KEY_LENGTH = 12


def _windows_canonical_path(path: str) -> str | None:
    """Return the final DOS/UNC spelling of a Windows filesystem path."""
    if os.name != "nt":
        return None

    try:
        resolved = os.path.realpath(path)
    except (OSError, ValueError):
        return None
    if resolved.lower().startswith("\\\\?\\unc\\"):
        return "\\\\" + resolved[8:]
    if resolved.startswith("\\\\?\\"):
        return resolved[4:]
    if resolved.startswith("\\\\.\\"):
        return None
    return resolved or None


def state_root(*, environ: Mapping[str, str] | None = None, system: str | None = None) -> Path:
    """Return host-local tool/cache state, preserving an explicit selection."""
    environment = os.environ if environ is None else environ
    if configured := environment.get("AOBUS_STATE_ROOT"):
        return Path(configured)
    host = platform.system() if system is None else system
    if host == "Windows":
        return Path(environment.get("LOCALAPPDATA", str(Path.home() / "AppData/Local"))) / "Aobus"
    if host == "Darwin":
        return Path(environment.get("HOME", str(Path.home()))) / "Library/Caches/Aobus"
    if xdg_cache := environment.get("XDG_CACHE_HOME"):
        return Path(xdg_cache) / "Aobus"
    return Path(environment.get("HOME", str(Path.home()))) / ".cache/Aobus"


def windows_state_root(*, environ: Mapping[str, str] | None = None) -> Path:
    """Return the native Windows state root shared by Aobus checkouts."""
    return state_root(environ=environ, system="Windows")


def _git_directory(project_root: Path) -> Path | None:
    marker = project_root / ".git"
    if marker.is_dir():
        return marker
    if not marker.is_file():
        return None
    try:
        declaration = marker.read_text(encoding="utf-8").strip()
    except OSError:
        return None
    prefix = "gitdir:"
    if not declaration.lower().startswith(prefix):
        return None
    directory = Path(declaration[len(prefix) :].strip())
    if not directory.is_absolute():
        directory = project_root / directory
    return Path(os.path.abspath(directory))


def _read_checkout_id(path: Path) -> str | None:
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return value or None


def _checkout_id(project_root: Path, *, environment: Mapping[str, str], create: bool) -> str:
    if override := environment.get("AOBUS_CHECKOUT_ID"):
        return override

    git_directory = _git_directory(project_root)
    if git_directory is None:
        if not create:
            return "path-only"
        raise RuntimeError(
            f"cannot locate the private Git directory for {project_root}; "
            "set AOBUS_CHECKOUT_ID to a stable value for this checkout"
        )
    path = git_directory / _CHECKOUT_ID_FILE
    if existing := _read_checkout_id(path):
        return existing
    if not create:
        return "path-only"

    candidate = uuid.uuid4().hex
    try:
        with path.open("x", encoding="utf-8") as stream:
            stream.write(f"{candidate}\n")
    except FileExistsError:
        # Another portal may be creating the ID concurrently. Its exclusive
        # create is followed immediately by one short write, so bounded retries
        # avoid selecting a transient path-only key.
        for _ in range(10):
            if existing := _read_checkout_id(path):
                return existing
            time.sleep(0.01)
        raise RuntimeError(
            f"checkout ID remained empty at {path}; set AOBUS_CHECKOUT_ID to a stable value for this checkout"
        ) from None
    except OSError as exc:
        raise RuntimeError(
            f"cannot create the checkout ID at {path}; set AOBUS_CHECKOUT_ID to a stable value for this checkout"
        ) from exc
    return candidate


def windows_checkout_key(
    project_root: Path = PROJECT_ROOT,
    *,
    environ: Mapping[str, str] | None = None,
    create_id: bool | None = None,
    os_name: str = "nt",
) -> str:
    """Return a stable key for a native Windows checkout and its resolved share."""
    environment = os.environ if environ is None else environ
    create = os.name == "nt" if create_id is None else create_id
    path_module = ntpath if os_name == "nt" else os.path
    absolute_root = path_module.abspath(str(project_root))
    resolved_root = _windows_canonical_path(absolute_root) if os_name == "nt" else None
    normalized_root = path_module.normcase(path_module.abspath(resolved_root or absolute_root))
    identity = _checkout_id(project_root, environment=environment, create=create)
    digest = hashlib.sha256(f"{normalized_root}\0{identity}".encode()).hexdigest()[:_CHECKOUT_KEY_LENGTH]
    label = re.sub(r"[^A-Za-z0-9._-]+", "-", project_root.name).strip("-.") or "aobus"
    return f"{label}-{digest}"


def windows_build_root(
    *,
    environ: Mapping[str, str] | None = None,
    project_root: Path = PROJECT_ROOT,
    create_id: bool | None = None,
) -> Path:
    """Return the checkout-isolated Windows build root on local storage."""
    environment = os.environ if environ is None else environ
    base = (
        Path(environment["AOBUS_BUILD_ROOT"])
        if environment.get("AOBUS_BUILD_ROOT")
        else windows_state_root(environ=environment) / "build"
    )
    return base / windows_checkout_key(project_root, environ=environment, create_id=create_id)


# Compatibility snapshot for callers that only need profile metadata. Native
# path selection goes through ``platform_profile()`` and remains dynamic.
WINDOWS_BUILD_ROOT = windows_build_root(create_id=False)


@dataclass(frozen=True)
class PlatformProfile:
    name: str
    build_root: Path
    executable_suffix: str
    apps: tuple[str, ...]
    default_suites: tuple[str, ...]
    all_suites: tuple[str, ...]
    tsan_suites: tuple[str, ...]
    compiler: str


# Backend owns the registrations that need a real device, daemon, or native
# SDK, so it belongs to the unattended Linux and macOS groups. The Linux-only
# system-media suite is conditional on AOBUS_BUILD_SYSTEM_MEDIA, which the
# test command checks against the configured tree before admitting it to an
# automatic group.
LINUX_PROFILE = PlatformProfile(
    name="linux",
    build_root=BUILD_ROOT,
    executable_suffix="",
    apps=("cli", "tui", "gtk"),
    default_suites=("core", "application", "backend", "gtk"),
    all_suites=("core", "application", "backend", "tui", "cli", "gtk", "systemmedia", "tooling", "lint"),
    tsan_suites=("core", "application", "backend", "gtk"),
    compiler="clang",
)

# macOS builds the terminal and command-line frontends with Core Audio output.
# The AppKit desktop is built by native checks; its GUI smoke runs separately.
# Python repository tooling remains owned by Linux and Windows. Native
# clang-tidy integration is supported on Darwin.
MACOS_PROFILE = PlatformProfile(
    name="macos",
    build_root=BUILD_ROOT,
    executable_suffix="",
    apps=("cli", "tui", "appkit"),
    default_suites=("core", "application", "backend", "tui"),
    all_suites=("core", "application", "backend", "tui", "cli", "lint"),
    tsan_suites=("core", "application", "backend"),
    compiler="clang",
)

# The Windows backend suite is compiled by the ordinary graph but joins no
# unattended group: both of its registrations are hidden there, so it stays
# individually selectable for an explicit run, like the WinUI audit.
WINDOWS_PROFILE = PlatformProfile(
    name="windows",
    build_root=WINDOWS_BUILD_ROOT,
    executable_suffix=".exe",
    apps=("cli", "tui", "winui"),
    default_suites=("core", "application", "tui"),
    all_suites=("core", "application", "tui", "cli", "tooling", "lint"),
    tsan_suites=(),
    compiler="msvc",
)


def platform_profile(os_name: str | None = None) -> PlatformProfile:
    """Return the native development profile for an ``os.name`` value."""
    if (os.name if os_name is None else os_name) == "nt":
        return replace(WINDOWS_PROFILE, build_root=windows_build_root())
    # os.name is "posix" for both Linux and macOS, so the two are separated by
    # sys.platform. An explicit os_name still selects the Linux profile, which
    # keeps cross-profile queries working from any host.
    if os_name is None and sys.platform == "darwin":
        return replace(MACOS_PROFILE, build_root=linux_build_root())
    if os_name is None and sys.platform != "linux":
        raise RuntimeError(
            f"Aobus native development supports Linux, macOS, and Windows; there is no profile for {sys.platform!r}."
        )
    return replace(LINUX_PROFILE, build_root=linux_build_root())
