"""Single source of truth for platform build profiles, presets, and build trees."""

import hashlib
import ntpath
import os
import re
import sys
import time
import uuid
from collections.abc import Mapping
from dataclasses import dataclass, replace
from pathlib import Path

from .paths import PROJECT_ROOT


def linux_build_root(*, environ: Mapping[str, str] | None = None) -> Path:
    """Return the Linux build root, host-overridable via AOBUS_BUILD_ROOT."""
    environment = os.environ if environ is None else environ
    if override := environment.get("AOBUS_BUILD_ROOT"):
        return Path(override)
    return Path("/tmp/build")


BUILD_ROOT = linux_build_root()

_CHECKOUT_ID_FILE = "aobus-checkout-id"
_CHECKOUT_KEY_LENGTH = 12


def windows_state_root(*, environ: Mapping[str, str] | None = None) -> Path:
    """Return the native Windows state root shared by Aobus checkouts."""
    environment = os.environ if environ is None else environ
    if override := environment.get("AOBUS_STATE_ROOT"):
        return Path(override)
    if local_app_data := environment.get("LOCALAPPDATA"):
        return Path(local_app_data) / "Aobus"
    return Path.home() / "AppData" / "Local" / "Aobus"


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
    """Return a stable, path-sensitive key for a native Windows checkout."""
    environment = os.environ if environ is None else environ
    create = os.name == "nt" if create_id is None else create_id
    path_module = ntpath if os_name == "nt" else os.path
    normalized_root = path_module.normcase(path_module.abspath(str(project_root)))
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

PRESETS = {
    "debug": "linux-debug",
    "release": "linux-release",
    "pgo1": "linux-pgo-profile",
    "pgo2": "linux-pgo-optimize",
    "profile": "profile",
}

WINDOWS_PRESETS = {
    "debug": "windows-debug",
    "release": "windows-release",
}

TIDY_PRESETS = {
    "linux": "linux-debug",
    "windows": "windows-tidy",
}

FLAVORS = tuple(PRESETS)


@dataclass(frozen=True)
class PlatformProfile:
    name: str
    build_root: Path
    presets: dict[str, str]
    executable_suffix: str
    apps: tuple[str, ...]
    default_suites: tuple[str, ...]
    all_suites: tuple[str, ...]
    compiler: str


LINUX_PROFILE = PlatformProfile(
    name="linux",
    build_root=BUILD_ROOT,
    presets=PRESETS,
    executable_suffix="",
    apps=("cli", "tui", "gtk"),
    default_suites=("core", "gtk"),
    all_suites=("core", "tui", "cli", "gtk", "integration", "council", "tooling", "lint"),
    compiler="gcc",
)

WINDOWS_PROFILE = PlatformProfile(
    name="windows",
    build_root=WINDOWS_BUILD_ROOT,
    presets=WINDOWS_PRESETS,
    executable_suffix=".exe",
    apps=("cli", "tui"),
    default_suites=("core", "tui"),
    all_suites=("core", "tui", "cli", "integration", "tooling"),
    compiler="msvc",
)

DEFAULT_DEBUG_DIR = BUILD_ROOT / "debug"
COVERAGE_DIR = BUILD_ROOT / "coverage"


def platform_profile(os_name: str | None = None) -> PlatformProfile:
    """Return the native development profile for an ``os.name`` value."""
    if (os.name if os_name is None else os_name) == "nt":
        return replace(WINDOWS_PROFILE, build_root=windows_build_root())
    if os_name is None and sys.platform != "linux":
        raise RuntimeError(
            f"Aobus native development supports Linux and Windows; there is no profile for {sys.platform!r}."
        )
    return replace(LINUX_PROFILE, build_root=linux_build_root())


def flavors(os_name: str | None = None) -> tuple[str, ...]:
    return tuple(platform_profile(os_name).presets)


def tidy_preset(os_name: str | None = None) -> str:
    """Return the configure preset for the native clang-tidy toolchain."""
    return TIDY_PRESETS[platform_profile(os_name).name]


def tidy_dir(os_name: str | None = None) -> Path:
    """Return the dedicated native clang-tidy build tree."""
    if override := os.environ.get("BUILD_DIR"):
        return Path(override)
    profile = platform_profile(os_name)
    name = "windows-tidy" if profile.name == "windows" else "debug-clang-tidy"
    return profile.build_root / name


def analyze_dir(os_name: str | None = None) -> Path:
    """Return the dedicated native clang-analyzer build tree."""
    if override := os.environ.get("BUILD_DIR"):
        return Path(override)
    profile = platform_profile(os_name)
    name = "windows-analyzer" if profile.name == "windows" else "debug-clang-analyzer"
    return profile.build_root / name


def preset(flavor: str, *, os_name: str | None = None) -> str:
    return platform_profile(os_name).presets[flavor]


def suffix(*, clang: bool = False, asan: bool = False, tsan: bool = False) -> str:
    return ("-clang" if clang else "") + ("-asan" if asan else "") + ("-tsan" if tsan else "")


def build_dir(
    flavor: str,
    *,
    clang: bool = False,
    asan: bool = False,
    tsan: bool = False,
    os_name: str | None = None,
) -> Path:
    """Resolve the native build tree for a flavor.

    The BUILD_DIR environment variable always wins, which lets callers redirect builds into
    dedicated host-persistent trees. Linux PGO steps share one tree so step 2 can consume the
    profile data of step 1. Each Windows flavor has one tree shared by builds and tests.
    """
    if override := os.environ.get("BUILD_DIR"):
        return Path(override)

    profile = platform_profile(os_name)
    if profile.name == "windows":
        name = preset(flavor, os_name="nt")
        return profile.build_root / f"{name}{suffix(clang=clang, asan=asan, tsan=tsan)}"

    if flavor in ("pgo1", "pgo2"):
        return profile.build_root / f"pgo{suffix(clang=clang)}"
    return profile.build_root / f"{flavor}{suffix(clang=clang, asan=asan, tsan=tsan)}"


def executable(path: Path, os_name: str | None = None) -> Path:
    """Add the native executable suffix to a target path."""
    suffix_value = platform_profile(os_name).executable_suffix
    return path.with_name(f"{path.name}{suffix_value}") if suffix_value else path
