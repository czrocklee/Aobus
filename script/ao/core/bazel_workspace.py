"""CMake-free provider selection and disposable Bazel development workspaces."""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import urllib.request
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

from . import builddir, dependency_policy
from .paths import PROJECT_ROOT

TOOLCHAIN = PROJECT_ROOT / "script/ao/bazel-toolchain.json"
POLICY = PROJECT_ROOT / "config/bazel/dependencies.json"
# The root BUILD owns the default suite aggregate; subsystem suites stay explicit conveniences.
DEFAULT_TEST_TARGETS = ("//:tests",)
_INPUT_PROBE: Callable[[Path], None] | None = None


def _probe(path: Path) -> Path:
    """Record existence/symlink decisions for the repository resolver, including absence."""
    if _INPUT_PROBE is not None:
        _INPUT_PROBE(path.absolute())
    return path


class BazelWorkspaceError(RuntimeError):
    """An unavailable or ungoverned native input prevents Bazel execution."""


def _json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise BazelWorkspaceError(f"expected a JSON object: {path}")
    return value


def _load_policy() -> dict[str, Any]:
    policy = _json(POLICY)
    if policy.get("schema") != 3:
        raise BazelWorkspaceError("unsupported Bazel dependency policy schema")
    pending: list[Any] = [policy]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            if value.keys() & {
                "version",
                "versions",
                "provider_version",
                "status_version",
                "status_versions",
                "metadata_versions",
            }:
                raise BazelWorkspaceError("dependency version policy belongs to the existing contract/provider inputs")
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    return policy


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write_json(path: Path, value: object) -> None:
    content = (json.dumps(value, indent=2, sort_keys=True) + "\n").replace("\n", os.linesep).encode("utf-8")
    try:
        if path.read_bytes() == content:
            return
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def native_platform() -> str:
    return builddir.platform_profile().name


def _source_root() -> str:
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


def workspace_path(path: str | None, config: str) -> Path:
    selected = Path(path or os.environ.get("BUILD_DIR") or builddir.platform_profile().build_root / f"bazel-{config}")
    selected = selected.resolve()
    source_root = PROJECT_ROOT.resolve()
    if selected.is_relative_to(source_root) or source_root.is_relative_to(selected):
        raise BazelWorkspaceError("Bazel --path must be outside and must not contain the source checkout")
    if (selected / "CMakeCache.txt").exists():
        raise BazelWorkspaceError(f"{selected} is a CMake tree; select a separate Bazel --path")
    return selected


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


def _tool_version(executable: Path, expected: str) -> None:
    result = subprocess.run(
        [str(executable), "--version"], env=child_environment(), capture_output=True, text=True, check=False
    )
    accepted = {f"bazel {expected}"}
    if executable.resolve().is_relative_to("/nix/store"):
        # Nixpkgs' source build has no release Git metadata, but retains the exact upstream version.
        accepted.add(f"bazel {expected}- (@non-git)")
    if result.returncode or result.stdout.strip() not in accepted:
        raise BazelWorkspaceError(f"{executable} must report bazel {expected}; run ao bazel setup")


def _host_machine() -> str:
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


def _artifact_key() -> str:
    system = {"linux": "linux", "windows": "windows", "macos": "darwin"}[native_platform()]
    machine = _host_machine().lower()
    machine = {"amd64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
    return f"{system}-{machine}"


def _managed_tool(lock: dict[str, Any]) -> Path:
    if override := os.environ.get("AOBUS_STATE_ROOT"):
        state = Path(override)
    elif os.name == "nt":
        state = builddir.windows_state_root()
    else:
        state = Path.home() / "Library/Caches/Aobus"
    suffix = ".exe" if os.name == "nt" else ""
    return state / "tools/bazel" / str(lock["version"]) / f"bazel{suffix}"


def _publish_tool(download: Path, executable: Path, expected_sha256: str) -> None:
    descriptor, name = tempfile.mkstemp(prefix=".bazel-stage-", dir=executable.parent)
    os.close(descriptor)
    staged = Path(name)
    try:
        shutil.copyfile(download, staged)
        if _sha256(staged) != expected_sha256:
            raise BazelWorkspaceError("staged Bazel SHA-256 does not match bazel-toolchain.json")
        staged.chmod(0o755)
        if executable.is_file():
            if _sha256(executable) != expected_sha256:
                raise BazelWorkspaceError(f"managed Bazel SHA-256 mismatch: {executable}")
            return
        try:
            os.replace(staged, executable)
        except OSError:
            # Another setup may have published and started the same executable;
            # Windows cannot replace a running binary. Reuse only verified bytes.
            if not executable.is_file() or _sha256(executable) != expected_sha256:
                raise
    finally:
        staged.unlink(missing_ok=True)


def bazel_executable(*, setup: bool = False) -> Path:
    lock = _json(TOOLCHAIN)
    if lock.get("schemaVersion") != 1 or (PROJECT_ROOT / ".bazelversion").read_text().strip() != lock["version"]:
        raise BazelWorkspaceError("Bazel pin files disagree")
    if override := os.environ.get("AOBUS_BAZEL"):
        executable = Path(override).absolute()
    elif native_platform() == "linux":
        found = shutil.which("bazel")
        if not found or not Path(found).resolve().is_relative_to("/nix/store"):
            raise BazelWorkspaceError("Bazel is missing from the pinned Nix shell; re-enter through ./ao")
        executable = Path(found).absolute()
    else:
        executable = _managed_tool(lock)
        artifact = lock["artifacts"].get(_artifact_key())
        if artifact is None:
            raise BazelWorkspaceError(f"no pinned Bazel artifact for {_artifact_key()}")
        if not executable.is_file():
            if not setup:
                raise BazelWorkspaceError("managed Bazel is missing; run ao bazel setup")
            executable.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="aobus-bazel-") as temporary:
                download = Path(temporary) / "bazel"
                urllib.request.urlretrieve(artifact["url"], download)
                if _sha256(download) != artifact["sha256"]:
                    raise BazelWorkspaceError("downloaded Bazel SHA-256 does not match bazel-toolchain.json")
                _publish_tool(download, executable, artifact["sha256"])
        if _sha256(executable) != artifact["sha256"]:
            raise BazelWorkspaceError(f"managed Bazel SHA-256 mismatch: {executable}")
    _tool_version(executable, lock["version"])
    return executable


def _executable(name: str, *, nix: bool = False, path: str | None = None) -> str:
    found = shutil.which(name) if path is None else shutil.which(name, path=path)
    if _INPUT_PROBE is not None:
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
                    _probe(Path(os.path.join(os.curdir, filename)))
        # Inventory a superset, not a second executable-selection engine. Exact
        # lexical equality alone proves later groups cannot affect which's result.
        cutoff_safe = all(not os.path.dirname(filename) for filename in filenames)
        for directory in directories:
            candidates = [os.path.join(directory, filename) for filename in filenames]
            for candidate in candidates:
                _probe(Path(candidate))
            if cutoff_safe and found in candidates:
                break
        if found:
            _probe(Path(found))
    if not found or (nix and not Path(found).resolve().is_relative_to("/nix/store")):
        raise BazelWorkspaceError(f"{name} is unavailable in the selected native environment")
    # Preserve argv[0]: Nix multicall tools and Clang drivers dispatch by basename.
    return str(Path(found).absolute())


def _evidence(paths: list[Path]) -> list[dict[str, str]]:
    return [{"path": str(path.resolve()), "sha256": _sha256(path)} for path in paths]


def _nix_root(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_relative_to("/nix/store") or len(resolved.parts) < 4:
        raise BazelWorkspaceError(f"provider path is outside the pinned Nix store: {path}")
    return Path(*resolved.parts[:4])


def _nix_dependency(report: dict[str, Any], source: str) -> tuple[Path, str, str, list[Path]]:
    """Trust report-owned output membership, not the local store DB's deriver records."""
    message = f"{source} output identity is unavailable in the Nix dependency report; re-enter through ./ao"
    dependencies = report.get("dependencies")
    entry = dependencies.get(source) if isinstance(dependencies, dict) else None
    if not isinstance(entry, dict):
        raise BazelWorkspaceError(message)

    def store_path(value: object, *, derivation: bool = False) -> Path:
        # Reject nested/normalized spellings before _nix_root can promote them.
        if not isinstance(value, str) or not re.fullmatch(r"/nix/store/[A-Za-z0-9+._?=-]+", value):
            raise BazelWorkspaceError(message)
        path = Path(value)
        if value.rsplit("/", 1)[1] in {".", ".."} or (
            derivation and (not value.endswith(".drv") or path.name == ".drv")
        ):
            raise BazelWorkspaceError(message)
        return path

    version = entry.get("version")
    if not isinstance(version, str) or not version or any(c in version for c in "\r\n\x00"):
        raise BazelWorkspaceError(message)
    selected = store_path(entry.get("storePath"))
    derivation = store_path(entry.get("drvPath"), derivation=True)
    reported = entry.get("outputs")
    if not isinstance(reported, dict) or not reported:
        raise BazelWorkspaceError(message)
    declared = []
    for name, value in reported.items():
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_+-]*", name):
            raise BazelWorkspaceError(message)
        declared.append(store_path(value))
    # Optional outputs need not be installed. Validate every spelling first,
    # then retain the existing canonical-root and existing-directory behavior.
    outputs = sorted({_nix_root(path) for path in declared if path.is_dir()})
    root = _nix_root(selected)
    if root not in outputs:
        raise BazelWorkspaceError(message)
    return root, version, derivation.as_posix(), outputs


def _package_groups(policy: dict[str, Any], *, host: str | None = None) -> dict[str, list[dict[str, Any]]]:
    groups = policy["packages"]
    if set(groups) != {"native", "test"} or any(not isinstance(value, list) for value in groups.values()):
        raise BazelWorkspaceError("unsupported Bazel dependency groups")
    result = {group: value for group, value in groups.items()}
    names = [package["name"] for packages in result.values() for package in packages]
    if len(names) != len(set(names)) or set(names) - {
        "icu",
        "icu_uc",
        "cli11",
        "spdlog",
        "ftxui",
        "mimalloc",
        "stb",
        "date_tz",
        "giomm",
        "gtkmm",
        "boost_process",
        "lmdb",
        "rapidyaml",
        "boost",
        "gsl_lite",
        "fast_float",
        "xxhash",
        "lexy",
        "catch2",
        "flac",
        "alac",
        "fdk_aac",
        "mpg123",
        "opus",
        "alsa",
        "pipewire",
        "udev",
        "fakeit",
        "xtest",
    }:
        raise BazelWorkspaceError("unknown or duplicate Bazel dependency package")
    host = native_platform() if host is None else host
    if host not in {"linux", "macos", "windows"}:
        raise BazelWorkspaceError(f"unsupported dependency platform: {host}")
    # These filters follow actual OS-specific audio, GTK, MPRIS and time-zone consumers.
    # No arbitrary per-package platform policy is supported by this projection.
    for group, packages in result.items():
        for package in packages:
            if package["name"] == "giomm":
                if package.get("nix") != "glibmm" or package.get("nix_inputs") != ["glib", "libsigcxx"]:
                    raise BazelWorkspaceError("giomm requires exactly the glibmm/glib/libsigcxx Nix inputs")
            elif package["name"] == "gtkmm":
                if (
                    package.get("nix") != "gtkmm"
                    or package.get("modules") != ["gtkmm-4.0", "cairo-ft", "cairo-fc"]
                    or package.get("nix_inputs")
                    != [
                        "gtk4",
                        "glibmm",
                        "glib",
                        "libsigcxx",
                        "cairo",
                        "cairomm",
                        "pango",
                        "pangomm",
                        "gdk-pixbuf",
                        "graphene",
                        "harfbuzz",
                        "freetype",
                        "fontconfig",
                        "vulkan-headers",
                        "vulkan-loader",
                    ]
                ):
                    raise BazelWorkspaceError("gtkmm requires its closed GTK/glibmm rendering SDK Nix inputs")
            elif package["name"] == "xtest":
                if (
                    package.get("nix") != "libXtst"
                    or package.get("modules") != ["xtst", "x11", "xi"]
                    or package.get("nix_inputs") != ["libX11", "libXi", "xorgproto"]
                ):
                    raise BazelWorkspaceError(
                        "xtest requires exactly the libXtst/libX11/libXi/xorgproto Linux test interface"
                    )
            elif "nix_inputs" in package:
                raise BazelWorkspaceError(f"unsupported additional Nix dependency inputs: {package['name']}")
            if package["name"] == "boost_process" and (
                package.get("nix") != "boost"
                or package.get("modules") != ["boost_process"]
                or package.get("vcpkg") != ["boost-process", "boost-filesystem"]
            ):
                raise BazelWorkspaceError("boost_process requires the selected Boost Process SDK")
            platforms = package.get("platforms")
            if package["name"] in {
                "alsa",
                "pipewire",
                "udev",
                "giomm",
                "gtkmm",
                "boost_process",
                "date_tz",
                "xtest",
            }:
                required = {
                    "date_tz": ["macos"],
                    "boost_process": ["linux", "macos", "windows"],
                    "xtest": ["linux"],
                }.get(package["name"], ["linux"])
                expected_group = "test" if package["name"] == "xtest" else "native"
                if group != expected_group or platforms != required:
                    raise BazelWorkspaceError(f"{package['name']} dependency platform contract must be {required}")
            elif platforms is not None:
                raise BazelWorkspaceError(f"unsupported package platform filter: {package['name']}")
        result[group] = [package for package in packages if host in package.get("platforms", [host])]
    return result


def _linux_provider(
    policy: dict[str, Any], *, identity_path: Path
) -> tuple[dict[str, Any], dict[str, str], dict[str, list[Path]]]:
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise BazelWorkspaceError("AOBUS_NIX_DEPENDENCY_REPORT is missing; enter the pinned shell through ./ao")
    report = _json(report_path)
    pin = _json(PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise BazelWorkspaceError("Nix dependency report does not match nixpkgs.json; re-enter through ./ao")
    packages = [package for group in _package_groups(policy, host="linux").values() for package in group]
    versions, expected_roots, derivers = {}, {}, {}
    outputs: dict[str, list[Path]] = {}
    package_outputs: dict[str, list[Path]] = {}
    input_identities: dict[str, dict[str, Any]] = {}
    for package in packages:
        name, source = package["name"], package["nix"]
        root, version, derivation, selected = _nix_dependency(report, source)
        versions[name] = version
        expected_roots[name] = root
        derivers[name] = derivation
        package_outputs[name] = selected
        outputs[name] = selected
        extra = package.get("nix_inputs", [])
        for input_name in extra:
            input_root, input_version, input_deriver, input_outputs = _nix_dependency(report, input_name)
            input_identities[input_name] = {
                "storePath": str(input_root),
                "version": input_version,
                "deriver": input_deriver,
                "outputs": [str(path) for path in input_outputs],
            }
            outputs[name] = sorted(set(outputs[name]) | set(input_outputs))
    roots = {root for selected in outputs.values() for root in selected}
    pc_dirs = {
        str(root / directory)
        for root in roots
        for directory in ("lib/pkgconfig", "share/pkgconfig")
        if (root / directory).is_dir()
    }
    library_dirs = {str(root / "lib") for root in roots if (root / "lib").is_dir()}
    pkg_config = _executable("pkg-config", nix=True)
    # Detect stale shell metadata before isolating the generator's lookup roots.
    for package in packages:
        for module in package["modules"]:
            result = subprocess.run(
                [pkg_config, "--variable=pcfiledir", module], capture_output=True, text=True, check=False
            )
            if result.returncode:
                continue  # Only the bounded complete fallbacks below may handle absent metadata.
            path = Path(result.stdout.strip())
            if not path.is_absolute() or _nix_root(path) not in outputs[package["name"]]:
                raise BazelWorkspaceError(
                    f"{module} metadata does not match the Nix report output {expected_roots[package['name']]}"
                )
            pc_dirs.add(str(path))
    _write_json(
        identity_path,
        {
            "schema": 1,
            "resolver": "nix",
            "nixpkgsRevision": pin["rev"],
            "inputs": input_identities,
            "packages": {
                name: {
                    "storePath": str(root),
                    "outputs": [str(p) for p in package_outputs[name]],
                    "deriver": derivers[name],
                    "version": versions[name],
                }
                for name, root in expected_roots.items()
            },
        },
    )
    evidence_paths = [report_path, identity_path, PROJECT_ROOT / "nixpkgs.json"]
    return (
        {
            "provider": {"name": "nix", "version": pin["rev"], "evidence": _evidence(evidence_paths)},
            "allowed_roots": sorted(str(root) for root in roots) + [str(p.resolve()) for p in evidence_paths],
            "library_dirs": sorted(library_dirs),
            "pkg_config": {"executable": pkg_config, "pc_dirs": sorted(pc_dirs)},
            "elf": {"readelf": _executable("readelf", nix=True)},
        },
        versions,
        outputs,
    )


def _verify_linux_resolution(manifest: dict[str, Any], expected: dict[str, list[Path]]) -> None:
    for package in manifest["packages"]:
        name = package["name"]
        paths = [*package["interface"]["header_dirs"]]
        paths.extend(library["file"] for library in package["resolved_libraries"] if library["kind"] != "system")
        for value in paths:
            if _nix_root(Path(value)) not in expected[name]:
                raise BazelWorkspaceError(
                    f"resolved library/header {value} does not match Nix report outputs {expected[name]}"
                )
    roots = {root for outputs in expected.values() for root in outputs}
    for module in manifest["modules"].values():
        metadata = Path(module["metadata"])
        if _nix_root(metadata) not in roots:
            raise BazelWorkspaceError(f"resolved metadata {metadata} does not match the Nix report")


def vcpkg_triplets() -> tuple[str, str]:
    """Return the native (target, host) vcpkg triplets, matching the native CMake presets."""
    host = native_platform()
    if host == "windows":
        return "x64-windows-static-md", "x64-windows"
    if host == "macos":
        # The macOS launcher selects the project triplet from the machine architecture.
        if triplet := os.environ.get("AOBUS_VCPKG_TRIPLET"):
            return triplet, triplet
        raise BazelWorkspaceError("macOS vcpkg selection requires AOBUS_VCPKG_TRIPLET from the ./ao launcher")
    raise BazelWorkspaceError("Linux dependencies come from the pinned Nix shell, not vcpkg")


def vcpkg_prefix(explicit: str | None, tree: Path | None) -> Path:
    """Validate an explicit triplet prefix, or select the one provisioned in the Bazel tree."""
    if explicit:
        prefix = Path(explicit).resolve()
    elif tree is None:
        raise BazelWorkspaceError("native dependency resolution requires the prepared vcpkg prefix")
    else:
        prefix = tree / "vcpkg_installed" / vcpkg_triplets()[0]
        if not (prefix.parent / "vcpkg/status").is_file():
            raise BazelWorkspaceError(
                f"no vcpkg provider in {prefix.parent}; run `ao bazel provision` with the same --config/--path, "
                "or pass --vcpkg-prefix <installed/triplet>"
            )
    if not (prefix / "include").is_dir() or not (prefix.parent / "vcpkg/status").is_file():
        raise BazelWorkspaceError(f"not an installed vcpkg triplet prefix: {prefix}")
    return prefix.resolve()


def _native_path(path: Path, intended_root: Path) -> Path:
    """Check against a lexical root anchored at the canonical triplet prefix."""
    resolved = _probe(path).resolve()
    if not resolved.is_relative_to(intended_root):
        raise BazelWorkspaceError(
            f"native provider path escapes intended configuration/root {intended_root}: {path} -> {resolved}"
        )
    return resolved


def _native_config_directories(prefix: Path, config: str) -> tuple[Path, Path]:
    selected = prefix.resolve() / "debug" if config == "debug" else prefix.resolve()
    # Do not resolve the intended roots: a Debug directory alias to Release
    # must fail before the adapter canonicalizes its allowed-root inventory.
    library_dir, runtime_dir = selected / "lib", selected / "bin"
    return _native_path(library_dir, library_dir), _native_path(runtime_dir, runtime_dir)


def _native_provider(
    prefix: Path, config: str, host: str, *, identity_path: Path, policy: dict[str, Any] | None = None
) -> tuple[dict[str, Any], dict[str, str]]:
    prefix = prefix.resolve()
    if host == "windows" and prefix.name != "x64-windows-static-md":
        raise BazelWorkspaceError("Windows target dependencies require the x64-windows-static-md vcpkg triplet")
    include = _native_path(prefix / "include", prefix / "include")
    library_dir, runtime_dir = _native_config_directories(prefix, config)
    if not include.is_dir() or not library_dir.is_dir():
        raise BazelWorkspaceError(f"native provider headers/library directory is missing: {prefix}: {config}")
    pc_dir = _native_path(library_dir / "pkgconfig", library_dir)
    status = prefix.parent / "vcpkg/status"
    # A status database can contain host-tool triplets alongside target ports.
    paragraphs = status.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n\n")
    selected = "\n\n".join(p for p in paragraphs if f"Architecture: {prefix.name}" in p.splitlines())
    packages = dependency_policy.parse_vcpkg_status(selected)
    policy = _load_policy() if policy is None else policy
    groups = _package_groups(policy, host=host)
    manifest = _json(PROJECT_ROOT / "vcpkg.json")
    requests = [*manifest["dependencies"], *manifest["features"]["tests"]["dependencies"]]
    boost_ports = {request if isinstance(request, str) else request["name"] for request in requests}
    boost_ports = {name for name in boost_ports if name.startswith("boost-")}
    versions: dict[str, str] = {}
    selected_ports: set[str] = set()
    for package in (package for group in groups.values() for package in group):
        ports = package["vcpkg"]
        if package["name"] == "boost":
            ports = sorted(set(ports) | boost_ports | {name for name in packages if name.startswith("boost-")})
        if package["name"] == "boost_process" and host == "windows":
            # The static Windows Process closure links the producer's compiled
            # dependency archives; the shared descriptor keeps its two-port
            # header contract while each archive's port owns status and headers.
            reviewed = (
                policy["library_fallbacks"].get("boost_process", {}).get("windows", {})
                if isinstance(policy.get("library_fallbacks"), dict)
                else {}
            )
            closure = reviewed.get("libraries") if isinstance(reviewed, dict) else None
            if (
                not isinstance(closure, list)
                or not closure
                or any(
                    not isinstance(library, str) or not re.fullmatch(r"boost_[a-z0-9_]+", library)
                    for library in closure
                )
            ):
                raise BazelWorkspaceError("Windows Boost Process requires its reviewed compiled closure libraries")
            ports = [
                *ports,
                *(port for port in (library.replace("_", "-") for library in closure) if port not in ports),
            ]
        for name in ports:
            if name not in packages:
                raise BazelWorkspaceError(f"{name} is not installed for {prefix.name}: {status}")
            selected_ports.add(name)
        versions[package["name"]] = packages[ports[0]].upstream_version
        if package["name"] == "boost" and any(packages[name].upstream_version != versions["boost"] for name in ports):
            raise BazelWorkspaceError("installed Boost header ports have inconsistent upstream versions")
        if package["name"] == "boost_process" and any(
            packages[port].upstream_version != versions["boost_process"] for port in ports
        ):
            raise BazelWorkspaceError("installed Boost Process ports have inconsistent upstream versions")
    for request in requests:
        if not isinstance(request, dict) or request["name"] not in selected_ports:
            continue
        installed = packages[request["name"]]
        required = {"core", *request.get("features", [])}
        actual = set(installed.features)
        if not required.issubset(actual) or (request.get("default-features") is False and actual != required):
            raise BazelWorkspaceError(
                f"{request['name']} features {sorted(actual)} do not match vcpkg.json request {sorted(required)}"
            )
    _write_json(
        identity_path,
        {
            "schema": 1,
            "prefix": str(prefix.resolve()),
            "triplet": prefix.name,
            "provenance": "Installed status/current manifest checked; original registry/build provenance unverified",
            "status_sha256": _sha256(status),
            "packages": {
                name: {"version": packages[name].version, "features": list(packages[name].features)}
                for name in sorted(selected_ports)
            },
        },
    )
    identity_files = [status, identity_path, PROJECT_ROOT / "vcpkg.json", PROJECT_ROOT / "vcpkg-configuration.json"]
    if host == "windows":
        host_prefix = prefix.parent / "x64-windows"
        _native_path(host_prefix, host_prefix)
        host_status = "\n\n".join(p for p in paragraphs if "Architecture: x64-windows" in p.splitlines())
        pkgconf_package = dependency_policy.parse_vcpkg_status(host_status).get("pkgconf")
        if pkgconf_package is None:
            raise BazelWorkspaceError("Windows metadata queries require installed x64-windows pkgconf")
        info = prefix.parent / "vcpkg/info"
        ownership = _native_path(
            info / f"pkgconf_{pkgconf_package.upstream_version}_{pkgconf_package.triplet}.list", info
        )
        if not ownership.is_file():
            raise BazelWorkspaceError(f"pkgconf host ownership manifest is missing: {ownership}")
        owned = ownership.read_text(encoding="utf-8").splitlines()
        directory = host_prefix / "tools/pkgconf"
        _native_path(directory, directory)
        tool_directory = "x64-windows/tools/pkgconf"
        runtime = sorted(
            {
                Path(value).name
                for value in owned
                if Path(value).parent.as_posix() == tool_directory and Path(value).suffix.lower() == ".dll"
            }
            | {path.name for path in directory.iterdir() if path.suffix.lower() == ".dll"}
        )
        for name in ("pkgconf.exe", *runtime):
            path = _native_path(directory / name, directory)
            if not path.is_file() or f"{tool_directory}/{name}" not in owned:
                raise BazelWorkspaceError(f"missing/unowned Windows pkgconf host input: {path}")
            identity_files.append(path)
        identity_files.append(ownership)
        pkgconf = str(directory / "pkgconf.exe")
    else:
        pkgconf = _executable("pkgconf")
    if not Path(pkgconf).is_file():
        raise BazelWorkspaceError(f"native pkgconf is missing: {pkgconf}")
    return {
        "provider": {
            "name": "vcpkg",
            "version": f"{prefix.resolve()}:{_sha256(status)}",
            "evidence": _evidence(identity_files),
        },
        "allowed_roots": list(
            dict.fromkeys(
                [str(include), str(library_dir), str(Path(pkgconf).resolve())]
                + ([str(runtime_dir)] if runtime_dir.is_dir() else [])
                + [str(p.resolve()) for p in identity_files]
            )
        ),
        "library_roots": [str(library_dir)],
        "runtime_roots": [str(runtime_dir)] if runtime_dir.is_dir() else [],
        "library_dirs": [str(library_dir)],
        "pkg_config": {"executable": pkgconf, "pc_dirs": [str(pc_dir)]},
    }, versions


def _patch(ident: str, spec: dict[str, Any], package: str, **fields: Any) -> dict[str, Any]:
    return {
        "id": ident,
        "when": {
            "package": package,
            "platform": spec["platform"],
            "config": spec["config"],
            "provider": spec["provider"]["name"],
            "provider_version": spec["provider"]["version"],
        },
        **fields,
    }


def _windows_icu_patch(
    spec: dict[str, Any], policy: dict[str, Any], prefix: Path, *, package: str = "icu"
) -> dict[str, Any]:
    if prefix.name != "x64-windows-static-md":
        raise BazelWorkspaceError(f"unsupported Windows static ICU provider: {prefix}")
    values = {"debug_suffix": "d" if spec["config"] == "Debug" else ""}
    libraries = [name.format(**values) for name in policy["libraries"][package]]
    patch = _windows_static_link_patch(spec, package, libraries, policy["system_libraries"])
    assert patch is not None
    return patch


def _metadata_flags(spec: dict[str, Any], option: str, *, package: str | None = None) -> list[str]:
    environment = {
        "PATH": os.defpath,
        "PKG_CONFIG_PATH": "",
        "PKG_CONFIG_LIBDIR": os.pathsep.join(spec["pkg_config"]["pc_dirs"]),
        "PKG_CONFIG_ALLOW_SYSTEM_CFLAGS": "1",
        "PKG_CONFIG_ALLOW_SYSTEM_LIBS": "1",
        "LC_ALL": "C",
    }
    for key in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP"):
        if key in os.environ:
            environment[key] = os.environ[key]
    selected = next((p for p in spec["packages"] if p["name"] == package), spec["packages"][0])
    static = ["--static"] if selected.get("linkage", spec["linkage"]) == "static" else []
    return shlex.split(
        subprocess.run(
            [spec["pkg_config"]["executable"], *static, option, *selected["modules"]],
            env=environment,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    )


def _windows_static_link_patch(
    spec: dict[str, Any], name: str, libraries: list[str], system_libraries: list[str]
) -> dict[str, Any] | None:
    """Translate only reviewed trailing Windows SDK imports, retaining archive order."""
    if (
        name not in {"icu", "icu_uc", "mimalloc", "flac", "fdk_aac", "mpg123", "opus"}
        or spec["platform"] != "windows"
        or spec["config"] not in {"Debug", "Release"}
        or spec["provider"]["name"] != "vcpkg"
        or spec["linkage"] != "static"
        or not set(system_libraries) <= set(spec["system_libraries"])
    ):
        raise BazelWorkspaceError(f"unsupported Windows static metadata interface: {name}")
    flags = _metadata_flags(spec, "--libs", package=name)
    actual = [flag.removeprefix("-l") for flag in flags if flag.startswith("-l")]
    if actual != [*libraries, *system_libraries]:
        raise BazelWorkspaceError(f"unreviewed Windows static library sequence/interleaving: {name}: {actual}")
    if not system_libraries:
        return None
    # The typed adapter deliberately rejects mixed static/system imports. The
    # reviewed metadata puts these SDK libraries after every provider archive,
    # so named linkopts lose no archive/system interleaving. Do not generalize
    # this translation to unknown metadata or remove the adapter's order guard.
    present = [{"name": library, "kind": "static"} for library in libraries]
    removed = [{"name": library, "kind": "system"} for library in system_libraries]
    added = {"linkopts": [library + ".lib" for library in system_libraries]}
    return _patch(
        f"windows-{name}-system-links",
        spec,
        name,
        expect={"present": {"libraries": [*present, *removed]}, "missing": added, "unmet_requirements": []},
        remove={"libraries": removed},
        add=added,
        satisfy=[],
    )


def _macos_icu_runtime_patch(
    spec: dict[str, Any], policy: dict[str, Any], *, package: str = "icu"
) -> dict[str, Any] | None:
    flags = _metadata_flags(spec, "--libs", package=package)
    implicit = {f"-l{name}" for name in policy["libraries"]}
    saw_implicit = False
    for flag in flags:
        if flag in implicit:
            saw_implicit = True
        elif saw_implicit and flag.startswith("-l"):
            raise BazelWorkspaceError("Darwin ICU system/provider library interleaving is unsupported")
    removed = [{"name": name, "kind": "system"} for name in policy["libraries"] if f"-l{name}" in flags]
    if not removed:
        return None
    return _patch(
        f"macos-{package}-implicit-runtime",
        spec,
        package,
        expect={"present": {"libraries": removed}, "missing": {}, "unmet_requirements": []},
        remove={"libraries": removed},
        add={},
        satisfy=[],
    )


def _macos_audio_runtime_patch(spec: dict[str, Any], name: str, policy: dict[str, Any]) -> dict[str, Any]:
    if (
        name not in {"flac", "fdk_aac", "mpg123", "opus"}
        or spec["platform"] != "macos"
        or spec["config"] not in {"Debug", "Release"}
        or spec["provider"]["name"] != "vcpkg"
        or spec["linkage"] != "static"
    ):
        raise BazelWorkspaceError(f"unsupported macOS codec spec: {name}")
    flags = _metadata_flags(spec, "--libs", package=name)
    libraries = [flag.removeprefix("-l") for flag in flags if flag.startswith("-l")]
    if libraries != policy["libraries"]:
        raise BazelWorkspaceError(f"unreviewed macOS codec metadata library sequence: {name}: {libraries}")
    # Apple-targeting clang++ supplies libc++; libSystem supplies math. FLAC's
    # reviewed -lm is interleaved, so retain the relative order of its archives.
    present = [
        {"name": library, "kind": "system" if library in policy["implicit"] else "static"} for library in libraries
    ]
    removed = [library for library in present if library["name"] in policy["implicit"]]
    return _patch(
        f"macos-{name}-implicit-runtime",
        spec,
        name,
        expect={"present": {"libraries": present}, "missing": {}, "unmet_requirements": []},
        remove={"libraries": removed},
        add={},
        satisfy=[],
    )


def _catch_fallback(spec: dict[str, Any], policy: dict[str, Any], prefix: Path) -> dict[str, Any]:
    values = {"debug_suffix": "d" if spec["config"] == "Debug" else ""}
    libraries = []
    library_dir, _ = _native_config_directories(prefix, spec["config"].lower())
    _native_path(Path(spec["library_dirs"][0]), library_dir)
    for index, template in enumerate(policy[spec["platform"]]):
        file = _native_path(Path(spec["library_dirs"][0]) / template.format(**values), library_dir)
        if not file.is_file():
            raise BazelWorkspaceError(f"Catch2 metadata and fallback archive are missing: {file}")
        libraries.append({"name": f"catch2_{index}", "kind": "static", "file": str(file)})
    return _patch(
        "catch2-static-fallback",
        spec,
        "catch2",
        expect={"present": {}, "missing": {}, "unmet_requirements": ["manual_interface"]},
        add={
            "header_dirs": [str(_native_path(prefix / "include", prefix.resolve() / "include"))],
            "libraries": libraries,
        },
        satisfy=["manual_interface"],
    )


def _has_metadata(spec: dict[str, Any], modules: list[str]) -> bool:
    present = []
    for module in modules:
        matches = {
            path.resolve()
            for directory in spec["pkg_config"]["pc_dirs"]
            if _probe(path := Path(directory) / f"{module}.pc").is_file()
        }
        if len(matches) > 1:
            raise BazelWorkspaceError(f"ambiguous pkg-config metadata: {module}")
        present.append(bool(matches))
    if any(present) and not all(present):
        raise BazelWorkspaceError(f"incomplete pkg-config module group: {modules}")
    return bool(present) and all(present)


def _rapidyaml_version_header(include: Path) -> Path:
    candidates = [include / "c4/yml/version.hpp", include / "ryml/c4/yml/version.hpp"]
    markers = [path for path in candidates if _probe(path).is_file()]
    if len(markers) != 1:
        raise BazelWorkspaceError("RapidYaml version header is missing/ambiguous")
    return markers[0]


def _header_root(roots: list[Path], marker: str) -> Path:
    matches = {root / "include" for root in roots if _probe(root / "include" / marker).is_file()}
    if len(matches) != 1:
        raise BazelWorkspaceError(f"missing/ambiguous governed header {marker}: {sorted(map(str, matches))}")
    return matches.pop()


def _verify_header_version(path: Path, version: str, macros: list[str], *, encoded: bool = False) -> None:
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


def _compile_interface_patch(spec: dict[str, Any], name: str, interface: dict[str, Any]) -> dict[str, Any] | None:
    if not interface:
        return None
    flags = _metadata_flags(spec, "--cflags", package=name)
    # Normalize the split forms accepted by the artifact adapter as well.
    for option in ("-I", "-isystem", "-D"):
        flags = [
            token + flags[index + 1] if token == option and index + 1 < len(flags) else token
            for index, token in enumerate(flags)
        ]
    # The adapter canonicalizes include paths before checking patch predicates.
    # Compare the same identities here, including pkg-config's ../ spellings.
    headers = {
        Path(token[len(option) :]).resolve()
        for token in flags
        for option in ("-I", "-isystem")
        if token.startswith(option) and len(token) > len(option)
    }
    missing = {}
    for field, values in interface.items():
        if field == "header_dirs":
            absent = [value for value in values if Path(value).resolve() not in headers]
        elif field == "defines":
            absent = [value for value in values if "-D" + value not in flags]
        elif field == "copts":
            absent = [value for value in values if value not in flags]
        else:
            continue
        if absent:
            missing[field] = absent
    if not missing:
        return None
    return _patch(
        f"{name}-compile-interface",
        spec,
        name,
        expect={"present": {}, "missing": missing, "unmet_requirements": []},
        add=missing,
        satisfy=[],
    )


def _manual_patch(spec: dict[str, Any], name: str, interface: dict[str, Any]) -> dict[str, Any]:
    return _patch(
        f"{name}-complete-interface",
        spec,
        name,
        expect={"present": {}, "missing": interface, "unmet_requirements": ["manual_interface"]},
        add=interface,
        satisfy=["manual_interface"],
    )


def _metadata_versions(name: str, version: str, modules: list[str], host: str) -> dict[str, str]:
    """Compare exact producer spellings, derived from the selected provider release."""
    if name == "mimalloc":
        match = re.fullmatch(r"(\d+\.\d+)\.\d+", version)
        module = "mimalloc"
    elif host == "linux" and name == "alac":
        match = re.fullmatch(r"(\d+\.\d+\.\d+)(?:-unstable-\d{4}-\d{2}-\d{2})?", version)
        module = "alac"
    elif host == "linux" and name == "udev":
        match = re.fullmatch(r"(\d+)(?:\.\d+)*", version)
        module = "libudev"
    else:
        return {module: version for module in modules}
    if match is None or modules != [module]:
        raise BazelWorkspaceError(f"unsupported metadata version format/module: {name}: {host}: {version}")
    return {module: match[1]}


def _library_interface(
    package: dict[str, Any],
    roots: list[Path],
    config: str,
    host: str,
    policy: dict[str, Any],
) -> dict[str, Any]:
    """Project a complete structural interface; absent metadata never invokes CMake."""
    name = package["name"]
    reviewed = policy["library_fallbacks"].get(name, {}).get(host)
    if reviewed is None:
        raise BazelWorkspaceError(f"no reviewed {host} artifact interface for {name}")
    marker = {
        "lmdb": "lmdb.h",
        "xxhash": "xxhash.h",
        "rapidyaml": "ryml.hpp",
        "alac": "alac/ALACDecoder.h",
        "ftxui": "ftxui/component/component.hpp",
        "spdlog": "spdlog/spdlog.h",
        "mimalloc": "mimalloc.h",
        "date_tz": "date/tz.h",
        "cli11": "CLI/Version.hpp",
    }[name]
    if name == "rapidyaml" and not any(_probe(root / "include/ryml.hpp").is_file() for root in roots):
        marker = "ryml/ryml.hpp"
    include = _header_root(roots, marker)
    if host != "linux":
        _native_path(include, include.parent.resolve() / "include")
    header_dirs = [str(include)]
    if marker.startswith("ryml/"):
        header_dirs.append(str(include / "ryml"))
    kind = reviewed["linkage"]
    if kind != "static":
        raise BazelWorkspaceError(f"unsupported reviewed linkage: {name}: {host}: {kind}")
    if host == "windows" and name == "mimalloc":
        header = _native_path(include / "mimalloc.h", include)
        if not re.search(
            r"#if !defined\(MI_SHARED_LIB\)\s*\n\s*#define mi_decl_export\s*\n\s*#elif defined\(MI_SHARED_LIB_EXPORT\)",
            header.read_text(encoding="utf-8"),
        ):
            raise BazelWorkspaceError(f"static mimalloc requires its non-importing installed header: {header}")
    libraries = []
    for library in reviewed["libraries"]:
        # Debug spellings are bounded provider layouts, not a search path.
        # Reject old shared layouts rather than interpreting them as static.
        windows_static = host == "windows"
        if name in {"cli11", "spdlog", "ftxui", "mimalloc", "date_tz"}:
            suffix = ("d" if name == "spdlog" else "-debug" if name == "mimalloc" else "") if config == "debug" else ""
            spellings = [library + suffix]
        else:
            spellings = (
                [library, library + "d"] if config == "debug" and host != "linux" and not windows_static else [library]
            )
        candidates: set[Path] = set()
        for root in roots:
            selected = root / "debug" if config == "debug" and host != "linux" else root
            library_dir = _native_config_directories(root, config)[0] if host != "linux" else root / "lib"
            if windows_static:
                debug_spelling = library + ("-debug" if name == "mimalloc" else "d")
                alternate = library if config == "debug" and name in {"spdlog", "mimalloc"} else debug_spelling
                unexpected = [selected / "lib" / (alternate + ".lib")]
                for spelling in (library, debug_spelling):
                    unexpected.extend(
                        [selected / "lib" / (spelling + ".dll.lib"), selected / "bin" / (spelling + ".dll")]
                    )
                if name == "mimalloc":
                    unexpected.append(selected / "bin/mimalloc-redirect.dll")
                if any(_probe(path).exists() or path.is_symlink() for path in unexpected):
                    raise BazelWorkspaceError(f"unsupported static provider shape: {name}: {selected}: {library}")
            for spelling in spellings:
                file = selected / "lib" / (spelling + ".lib" if host == "windows" else "lib" + spelling + ".a")
                if _probe(file).is_file():
                    candidates.add(_native_path(file, library_dir) if host != "linux" else file.resolve())
        if len(candidates) != 1:
            raise BazelWorkspaceError(
                f"missing/ambiguous reviewed {host} {config} {library} artifact: {sorted(map(str, candidates))}"
            )
        file = candidates.pop()
        if not any(file.is_relative_to(root.resolve()) for root in roots):
            raise BazelWorkspaceError(f"artifact escapes selected provider: {file}")
        logical_name = spellings[0] if name in {"spdlog", "mimalloc"} else library
        libraries.append({"name": logical_name, "kind": kind, "file": str(file)})
    result: dict[str, Any] = {
        "header_dirs": header_dirs,
        "defines": reviewed.get("defines", []),
        "libraries": libraries,
    }
    if reviewed.get("linkopts"):
        result["linkopts"] = reviewed["linkopts"]
    if reviewed.get("runtime"):
        raise BazelWorkspaceError(f"unsupported extra frontend runtime interface: {name}: {host}")
    return result


def _linux_boost_process_interface(spec: dict[str, Any], roots: list[Path], version: str) -> dict[str, Any]:
    """The detached launcher consumes compiled Process v2 with Boost.Filesystem, never std::filesystem."""
    if spec["platform"] != "linux" or spec["provider"]["name"] != "nix" or spec["linkage"] != "shared":
        raise BazelWorkspaceError("Boost Process adapter requires the Linux shared Nix SDK")
    include = _header_root(roots, "boost/version.hpp")

    def owned(path: Path) -> Path:
        resolved = _probe(path).resolve()
        if not path.is_file() or not any(resolved.is_relative_to(root) for root in roots):
            raise BazelWorkspaceError(f"Boost Process input escapes/missing from selected Boost outputs: {path}")
        return resolved

    _verify_header_version(owned(include / "boost/version.hpp"), version, ["BOOST_VERSION"], encoded=True)
    for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp"):
        owned(include / "boost/process/v2" / marker)
    configuration = owned(include / "boost/process/v2/detail/config.hpp").read_text(encoding="utf-8")
    if any(
        marker not in configuration
        for marker in ("BOOST_PROCESS_V2_DECL", "BOOST_PROCESS_DYN_LINK", "namespace filesystem = boost::filesystem;")
    ):
        raise BazelWorkspaceError("Boost Process requires the installed compiled v2 Boost.Filesystem API")
    owned(include / "boost/filesystem/path.hpp")
    libraries = []
    for name, symbol in (
        ("boost_process", "boost::process::v2::process_environment::do_setup("),
        ("boost_filesystem", "boost::filesystem::path::"),
    ):
        candidates = {owned(path) for root in roots if _probe(path := root / "lib" / ("lib" + name + ".so")).is_file()}
        if len(candidates) != 1:
            raise BazelWorkspaceError(f"missing/ambiguous shared Boost Process artifact: {name}")
        file = candidates.pop()
        result = subprocess.run(
            [spec["elf"]["readelf"], "--dynamic", "--dyn-syms", "--wide", "--demangle", str(file)],
            env=child_environment(),
            capture_output=True,
            text=True,
            check=False,
        )
        sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", result.stdout)
        exported = re.search(r"\b(?:GLOBAL|WEAK)\s+DEFAULT\s+\d+\s+" + re.escape(symbol), result.stdout)
        if result.returncode or sonames != [f"lib{name}.so.{version}"] or exported is None:
            raise BazelWorkspaceError(f"Boost Process shared library ABI/version mismatch: {file}")
        libraries.append({"name": name, "kind": "shared", "file": str(file)})
    return {
        "header_dirs": [str(include)],
        "defines": [
            "BOOST_PROCESS_NO_LIB",
            "BOOST_PROCESS_DYN_LINK",
            "BOOST_FILESYSTEM_NO_LIB",
            "BOOST_FILESYSTEM_DYN_LINK",
        ],
        "libraries": libraries,
    }


def _static_boost_process_headers(prefix: Path, version: str) -> Path:
    """Shared static Process v2 headers over compiled Boost.Filesystem, never std::filesystem."""
    include = _native_path(_header_root([prefix], "boost/version.hpp"), prefix.resolve() / "include")

    def owned(path: Path) -> Path:
        resolved = _probe(path).resolve()
        if not path.is_file() or not resolved.is_relative_to(include):
            raise BazelWorkspaceError(f"Boost Process input escapes/missing from the selected vcpkg prefix: {path}")
        return resolved

    _verify_header_version(owned(include / "boost/version.hpp"), version, ["BOOST_VERSION"], encoded=True)
    for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp"):
        owned(include / "boost/process/v2" / marker)
    configuration = owned(include / "boost/process/v2/detail/config.hpp").read_text(encoding="utf-8")
    if any(
        marker not in configuration
        for marker in ("BOOST_PROCESS_V2_DECL", "BOOST_PROCESS_DYN_LINK", "namespace filesystem = boost::filesystem;")
    ):
        raise BazelWorkspaceError("Boost Process requires the installed compiled v2 Boost.Filesystem API")
    owned(include / "boost/filesystem/path.hpp")
    filesystem_configuration = owned(include / "boost/filesystem/config.hpp")
    # Narrow match of the installed config's static default branch: the same
    # branch the compiled archive and the CMake static export consume.
    if (
        re.search(
            r"#if !defined\(BOOST_FILESYSTEM_DYN_LINK\) && !defined\(BOOST_FILESYSTEM_STATIC_LINK\) && "
            r"!defined\(BOOST_ALL_DYN_LINK\) && !defined\(BOOST_ALL_STATIC_LINK\)\s*\n"
            r"#define BOOST_FILESYSTEM_STATIC_LINK\b",
            filesystem_configuration.read_text(encoding="utf-8"),
        )
        is None
    ):
        raise BazelWorkspaceError(
            "static Boost Process requires the installed Boost.Filesystem static-link config: "
            f"{filesystem_configuration}"
        )
    return include


def _macos_boost_process_interface(
    spec: dict[str, Any], prefix: Path, config: str, version: str, policy: dict[str, Any]
) -> dict[str, Any]:
    """AppKit consumes compiled Process v2 as static archives over Boost.Filesystem, never dylibs."""
    if spec["platform"] != "macos" or spec["provider"]["name"] != "vcpkg" or spec["linkage"] != "static":
        raise BazelWorkspaceError("macOS Boost Process adapter requires the reviewed static vcpkg SDK")
    reviewed = policy["library_fallbacks"].get("boost_process", {}).get("macos")
    if (
        not isinstance(reviewed, dict)
        or reviewed.get("linkage") != "static"
        or reviewed.get("libraries") != ["boost_process", "boost_filesystem"]
        or not isinstance(reviewed.get("defines"), list)
        or not reviewed["defines"]
    ):
        raise BazelWorkspaceError("macOS Boost Process requires its reviewed two-archive static interface")
    # The static archive was compiled against Boost.Filesystem with its default
    # STATIC_LINK branch; DYN_LINK, USE_STD_FS, standalone or a filesystem
    # version pin would compile consumers to a different ABI/semantics.
    forbidden = {
        "BOOST_PROCESS_DYN_LINK",
        "BOOST_FILESYSTEM_DYN_LINK",
        "BOOST_PROCESS_USE_STD_FS",
        "BOOST_PROCESS_V2_STANDALONE",
        "BOOST_FILESYSTEM_VERSION",
    }
    if forbidden & {define.split("=", 1)[0] for define in reviewed["defines"]}:
        raise BazelWorkspaceError(
            "macOS Boost Process consumer definitions must remain static without a filesystem version pin"
        )
    include = _static_boost_process_headers(prefix, version)
    library_dir, _ = _native_config_directories(prefix, config)
    libraries = []
    for name in reviewed["libraries"]:
        # Exact configuration archive spelling; no dylib, CMake export or ambient route.
        file = _native_path(library_dir / ("lib" + name + ".a"), library_dir)
        if not file.is_file():
            raise BazelWorkspaceError(f"missing/ambiguous reviewed macos {config} {name} artifact: {file}")
        # The .a suffix alone is not provenance: require the ar magic so a
        # same-configuration dylib alias or non-archive cannot pass as static.
        with file.open("rb") as archive:
            if archive.read(8) != b"!<arch>\n":
                raise BazelWorkspaceError(f"reviewed macos {config} {name} artifact is not a static ar archive: {file}")
        libraries.append({"name": name, "kind": "static", "file": str(file)})
    return {"header_dirs": [str(include)], "defines": list(reviewed["defines"]), "libraries": libraries}


def _windows_coff_object_member(archive: Any, file: Path, name: str, config: str, member: int) -> str:
    """Classify one member; its Version separates BigObj objects from short imports."""
    if member < 20:
        raise BazelWorkspaceError(f"truncated reviewed windows {config} {name} object member: {file}")
    header = archive.read(20)
    if len(header) != 20:
        raise BazelWorkspaceError(f"truncated reviewed windows {config} {name} object member: {file}")
    if header[0:4] == b"\x00\x00\xff\xff":
        # Short imports and BigObj share the signature. BigObj additionally
        # requires its full 56-byte header, Version >= 2 and the COFF UUID.
        version = int.from_bytes(header[4:6], "little")
        if version == 0:
            # A valid AMD64 short import object is an import-library member;
            # unknown machines stay rejected, never classified.
            if int.from_bytes(header[6:8], "little") != 0x8664:
                raise BazelWorkspaceError(f"unsupported reviewed windows {config} {name} import member machine: {file}")
            return "interface"
        if version < 2:
            raise BazelWorkspaceError(f"unsupported reviewed windows {config} {name} anonymous COFF member: {file}")
        if member < 56:
            raise BazelWorkspaceError(f"truncated reviewed windows {config} {name} BigObj header: {file}")
        header += archive.read(36)
        if (
            len(header) != 56
            or int.from_bytes(header[6:8], "little") != 0x8664
            or header[12:28] != bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
        ):
            raise BazelWorkspaceError(f"malformed reviewed windows {config} {name} BigObj header: {file}")
        sections = int.from_bytes(header[44:48], "little")
        header_size = 56
    else:
        if int.from_bytes(header[0:2], "little") != 0x8664:
            raise BazelWorkspaceError(f"reviewed windows {config} {name} archive member is not an x64 object: {file}")
        sections = int.from_bytes(header[2:4], "little")
        header_size = 20
        if int.from_bytes(header[16:18], "little") != 0:
            raise BazelWorkspaceError(f"malformed reviewed windows {config} {name} object member: {file}")
    if sections < 1 or header_size + 40 * sections > member:
        raise BazelWorkspaceError(f"malformed reviewed windows {config} {name} object member: {file}")
    for _ in range(sections):
        section = archive.read(40)
        if len(section) != 40:
            raise BazelWorkspaceError(f"truncated reviewed windows {config} {name} object sections: {file}")
        section_name = section[0:8].rstrip(b"\x00")
        if section_name == b".idata" or section_name.startswith(b".idata$"):
            # Full import-descriptor objects are import-library payloads too.
            return "interface"
    return "static"


def _windows_coff_archive_kind(file: Path, name: str, config: str) -> str:
    """Classify a reviewed .lib archive as 'static' or 'interface' by validating every payload member.

    Archive magic and the .lib suffix are shared by both shapes; only the bounded
    member inventory classifies. 'static' holds native object members only;
    'interface' holds import-library members, possibly mixed with objects. This
    classifies payload shape, not full ABI/build provenance; the installed port
    status, owned filenames and the actual native link remain required evidence.
    """
    with file.open("rb") as archive:
        if archive.read(8) != b"!<arch>\n":
            raise BazelWorkspaceError(f"reviewed windows {config} {name} artifact is not a static COFF archive: {file}")
        size = file.stat().st_size
        offset, classified, imported = 8, 0, False
        while offset < size:
            archive.seek(offset)
            header = archive.read(60)
            if len(header) != 60 or header[58:60] != b"`\n":
                raise BazelWorkspaceError(f"malformed reviewed windows {config} {name} archive member: {file}")
            digits = header[48:58].decode("ascii", "replace").strip()
            if not digits.isdecimal():
                raise BazelWorkspaceError(f"malformed reviewed windows {config} {name} archive member size: {file}")
            member = int(digits)
            end = offset + 60 + member
            if end > size or end + (member & 1) > size:
                raise BazelWorkspaceError(f"truncated reviewed windows {config} {name} archive member: {file}")
            # Linker symbol and long-name index members carry no object payload.
            if header[0:16].decode("ascii", "replace").rstrip(" ") not in {"/", "//", "/SYM64/"}:
                archive.seek(offset + 60)
                if _windows_coff_object_member(archive, file, name, config, member) == "interface":
                    imported = True
                classified += 1
            offset = end + (member & 1)
        if not classified:
            raise BazelWorkspaceError(f"no native object members in reviewed windows {config} {name} archive: {file}")
        return "interface" if imported else "static"


def _windows_static_coff_archive(file: Path, name: str, config: str) -> None:
    """The Boost Process guard: reviewed archives hold native objects, never import-library members."""
    if _windows_coff_archive_kind(file, name, config) == "interface":
        raise BazelWorkspaceError(f"reviewed windows {config} {name} artifact is an import library: {file}")


def _windows_boost_process_interface(
    spec: dict[str, Any], prefix: Path, config: str, version: str, policy: dict[str, Any]
) -> dict[str, Any]:
    """WinUI consumes compiled Process v2 as static archives over Boost.Filesystem, never import libraries."""
    if spec["platform"] != "windows" or spec["provider"]["name"] != "vcpkg" or spec["linkage"] != "static":
        raise BazelWorkspaceError("Windows Boost Process adapter requires the reviewed static vcpkg SDK")
    reviewed = policy["library_fallbacks"].get("boost_process", {}).get("windows")
    if (
        not isinstance(reviewed, dict)
        or reviewed.get("linkage") != "static"
        or not isinstance(reviewed.get("libraries"), list)
        or not reviewed["libraries"]
        or len(set(reviewed["libraries"])) != len(reviewed["libraries"])
        or reviewed["libraries"][0] != "boost_process"
        or "boost_filesystem" not in reviewed["libraries"][1:]
        or any(
            not isinstance(library, str) or not re.fullmatch(r"boost_[a-z0-9_]+", library)
            for library in reviewed["libraries"]
        )
        or not isinstance(reviewed.get("defines"), list)
        or not reviewed["defines"]
    ):
        raise BazelWorkspaceError("Windows Boost Process requires its reviewed static archive closure interface")
    # The compiled archives follow the producer's static branch; DYN_LINK,
    # USE_STD_FS, standalone or a filesystem version pin would compile
    # consumers to a different ABI/semantics.
    forbidden = {
        "BOOST_PROCESS_DYN_LINK",
        "BOOST_FILESYSTEM_DYN_LINK",
        "BOOST_PROCESS_USE_STD_FS",
        "BOOST_PROCESS_V2_STANDALONE",
        "BOOST_FILESYSTEM_VERSION",
    }
    if forbidden & {define.split("=", 1)[0] for define in reviewed["defines"]}:
        raise BazelWorkspaceError(
            "Windows Boost Process consumer definitions must remain static without a filesystem version pin"
        )
    include = _static_boost_process_headers(prefix, version)
    components = version.split(".")
    if len(components) < 2 or not all(component.isdecimal() for component in components[:2]):
        raise BazelWorkspaceError(f"unsupported Windows Boost Process provider version: {version}")
    debug = "-gd" if config == "debug" else ""
    alternate = "" if debug else "-gd"
    library_dir, _ = _native_config_directories(prefix, config)
    # The installed producer toolset tag is observed per install and required
    # coherent across the closure; refresh has no independent selected-compiler
    # metadata, so native validation owns the compiler/toolset binding.
    toolset: str | None = None
    libraries = []
    for name in reviewed["libraries"]:
        # Discover the exact owned archive through the reviewed producer name
        # pattern (component, version from the shared status, mt[-gd], x64);
        # the vc tag is captured, never pinned, duplicated or guessed here.
        tag = f"-x64-{components[0]}_{components[1]}.lib"
        selected = re.compile(rf"{re.escape(name)}-vc(?P<toolset>[0-9]+)-mt{re.escape(debug)}{tag}")
        rejected = (
            re.compile(rf"{re.escape(name)}-vc[0-9]+-mt-s(?:gd)?{tag}"),
            re.compile(rf"{re.escape(name)}-vc[0-9]+-mt{re.escape(alternate)}{tag}"),
        )
        candidates: list[tuple[Path, str]] = []
        for entry in sorted(library_dir.iterdir()):
            if rejected[0].fullmatch(entry.name) or rejected[1].fullmatch(entry.name):
                raise BazelWorkspaceError(
                    f"unsupported static provider shape in reviewed windows {config} layout: {entry}"
                )
            match = selected.fullmatch(entry.name)
            if match is not None:
                candidates.append((_probe(entry), match.group("toolset")))
        if len(candidates) != 1:
            raise BazelWorkspaceError(
                f"missing/ambiguous reviewed windows {config} {name} artifact: "
                f"{sorted(str(path) for path, _ in candidates)}"
            )
        file = _native_path(candidates[0][0], library_dir)
        if toolset is None:
            toolset = candidates[0][1]
        elif candidates[0][1] != toolset:
            raise BazelWorkspaceError(
                "incoherent installed producer toolset ABI across the reviewed Windows Process closure: "
                f"{toolset} != {candidates[0][1]}"
            )
        _windows_static_coff_archive(file, name, config)
        libraries.append({"name": name, "kind": "static", "file": str(file)})
    result: dict[str, Any] = {
        "header_dirs": [str(include)],
        "defines": list(reviewed["defines"]),
        "libraries": libraries,
    }
    if reviewed.get("linkopts"):
        result["linkopts"] = list(reviewed["linkopts"])
    return result


def _lexy_interface(roots: list[Path], host: str, version: str, evidence: Path) -> dict[str, Any]:
    """Complete header-only interface; provider metadata owns its version."""
    marker = "lexy/_detail/config.hpp"
    include = _header_root(roots, marker)
    header = include / marker
    if host != "linux":
        _native_path(include, include.parent.resolve() / "include")
        _native_path(header, include)
    if "#define LEXY_DETAIL_CONFIG_HPP_INCLUDED" not in header.read_text(encoding="utf-8"):
        raise BazelWorkspaceError(f"unrecognized lexy configuration header: {header}")
    _write_json(
        evidence,
        {
            "package": "lexy",
            "provider_version": version,
            "header": _evidence([header])[0],
            "version_evidence": "Provider report/status only; installed header has no upstream version macro",
            "interface": "Complete include root; header-only, no defines, compile flags or link inputs",
        },
    )
    return {"header_dirs": [str(include)]}


def _stb_interface(roots: list[Path], host: str, version: str) -> dict[str, Any]:
    marker = "stb/stb_image_resize2.h" if host == "linux" else "stb_image_resize2.h"
    include = _header_root(roots, marker)
    header = include / marker
    if host != "linux":
        overlay = _json(PROJECT_ROOT / "cmake/vcpkg-ports/stb/vcpkg.json")
        if version != overlay["version-string"]:
            raise BazelWorkspaceError(f"stb does not match the governed overlay: {version}")
        _native_path(header, include)
    # The pinned Nix recipe/vcpkg overlay owns the fixed source and its hash.
    # Check the consumed API, not another source revision or release-banner pin.
    text = header.read_text(encoding="utf-8")
    if not text.startswith("/* stb_image_resize2 - ") or any(
        re.search(r"\b" + symbol + r"\b", text) is None for symbol in ("stbir_resize_uint8_srgb", "STBIR_RGBA")
    ):
        raise BazelWorkspaceError(f"stb resize2 interface is missing: {header}")
    return {"header_dirs": [str(header.parent)]}


def _linux_gperf() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise BazelWorkspaceError("gperf requires the pinned Nix dependency report; re-enter through ./ao")
    report, pin = _json(report_path), _json(PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise BazelWorkspaceError("Nix dependency report does not match nixpkgs.json; re-enter through ./ao")
    root, version, derivation, outputs = _nix_dependency(report, "gperf")
    executable = root / "bin/gperf"
    if not executable.is_file() or _nix_root(executable) not in outputs:
        raise BazelWorkspaceError(f"gperf executable does not match the Nix report output: {executable}")
    return (
        {
            "provider": "nix",
            "root": str(root),
            "version": version,
            "deriver": derivation,
            "nixpkgsRevision": pin["rev"],
        },
        executable,
        [],
        [report_path, PROJECT_ROOT / "nixpkgs.json"],
    )


def _native_gperf(prefix: Path, host: str) -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    manifest = PROJECT_ROOT / "vcpkg.json"
    requests = [
        request
        for request in _json(manifest)["dependencies"]
        if isinstance(request, dict) and request["name"] == "gperf"
    ]
    if len(requests) != 1 or requests[0].get("host") is not True:
        raise BazelWorkspaceError("gperf must be a host:true port in vcpkg.json")
    status = prefix.resolve().parent / "vcpkg/status"
    # The host tool can live in a sibling triplet. Windows selects its reviewed
    # host role explicitly; other native layouts must remain unambiguous.
    paragraphs = status.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n\n")
    selected = "\n\n".join(
        p
        for p in paragraphs
        if "Package: gperf" in p.splitlines() and (host != "windows" or "Architecture: x64-windows" in p.splitlines())
    )
    try:
        packages = dependency_policy.parse_vcpkg_status(selected)
    except dependency_policy.DependencyPolicyError as exc:
        raise BazelWorkspaceError(f"ambiguous installed gperf host identity: {status}: {exc}") from exc
    if "gperf" not in packages:
        raise BazelWorkspaceError(f"gperf host port is not installed: {status}")
    package = packages["gperf"]
    evidence = [status, manifest, PROJECT_ROOT / "vcpkg-configuration.json"]
    if host == "windows" and (prefix.name != "x64-windows-static-md" or package.triplet != "x64-windows"):
        raise BazelWorkspaceError("Windows gperf requires the installed x64-windows host tool")
    if host not in {"windows", "macos"} or not re.fullmatch(r"[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)+", package.triplet):
        raise BazelWorkspaceError(f"unsupported gperf host layout: {host}: {package.triplet}")
    host_prefix = prefix.resolve().parent / package.triplet
    _native_path(host_prefix, host_prefix)
    directory = host_prefix / "tools/gperf"
    _native_path(directory, directory)
    executable = directory / ("gperf.exe" if host == "windows" else "gperf")
    _native_path(executable, directory)
    if not executable.is_file():
        raise BazelWorkspaceError(f"installed gperf host executable is missing: {executable}")
    runtime = sorted(path for path in directory.iterdir() if path.suffix.lower() == ".dll")
    if host == "windows":
        info = prefix.resolve().parent / "vcpkg/info"
        ownership = _native_path(info / f"gperf_{package.upstream_version}_{package.triplet}.list", info)
        if not ownership.is_file():
            raise BazelWorkspaceError(f"gperf host ownership manifest is missing: {ownership}")
        owned = ownership.read_text(encoding="utf-8").splitlines()
        if any(f"x64-windows/tools/gperf/{path.name}" not in owned for path in [executable, *runtime]):
            raise BazelWorkspaceError("unowned Windows gperf host-tool input")
        evidence.append(ownership)
    for path in runtime:
        _native_path(path, directory)
        if host != "windows" or not path.is_file():
            raise BazelWorkspaceError(f"unsupported adjacent gperf runtime: {path}")
    return (
        {
            "provider": "vcpkg",
            "root": str(host_prefix),
            "triplet": package.triplet,
            "host_machine": _host_machine(),
            "version": package.upstream_version,
            "status_version": package.version,
            "features": list(package.features),
            "provenance": (
                "Installed host-port status/current manifest checked; original registry/build provenance unverified"
            ),
        },
        executable,
        runtime,
        evidence,
    )


def _linux_dbus() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    """Only the already selected private-bus test tool; no ambient PATH lookup."""
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    report, pin = _json(report_path), _json(PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise BazelWorkspaceError("D-Bus test tool requires the selected pinned Nix report")
    _, version, derivation, outputs = _nix_dependency(report, "dbus")
    candidates = {path.resolve(): path for root in outputs if _probe(path := root / "bin/dbus-daemon").is_file()}
    if len(candidates) != 1:
        raise BazelWorkspaceError("missing/ambiguous dbus-daemon in selected Nix outputs")
    resolved, executable = next(iter(candidates.items()))
    if _nix_root(resolved) not in outputs:
        raise BazelWorkspaceError("dbus-daemon escapes selected Nix outputs")
    runtime = sorted({path.resolve() for root in outputs for path in (root / "lib").glob("libdbus-1.so*")})
    if any(not path.is_file() or _nix_root(path) not in outputs for path in runtime):
        raise BazelWorkspaceError("D-Bus runtime escapes selected Nix outputs")
    return (
        {
            "provider": "nix",
            "version": version,
            "deriver": derivation,
            "outputs": [str(path) for path in outputs],
            "nixpkgsRevision": pin["rev"],
            "runtime_contract": "Installed immutable Nix SDK; not full OS/runtime hermeticity",
        },
        executable,
        runtime,
        [report_path, PROJECT_ROOT / "nixpkgs.json"],
    )


def _linux_gresource() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    """Select the resource compiler and its GLib runtime from report-owned outputs only."""
    if native_platform() != "linux":
        raise BazelWorkspaceError("GLib resource compiler requires the Linux Nix SDK")
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise BazelWorkspaceError("GLib resource compiler requires the pinned Nix dependency report")
    report, pin = _json(report_path), _json(PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise BazelWorkspaceError("GLib resource compiler requires the selected pinned Nix report")
    root, version, derivation, outputs = _nix_dependency(report, "glib")
    candidates = {
        path.resolve(): path for output in outputs if _probe(path := output / "bin/glib-compile-resources").is_file()
    }
    if len(candidates) != 1:
        raise BazelWorkspaceError("missing/ambiguous glib-compile-resources in selected Nix outputs")
    resolved, executable = next(iter(candidates.items()))
    if _nix_root(resolved) not in outputs or not os.access(executable, os.X_OK):
        raise BazelWorkspaceError("glib-compile-resources escapes/is not executable in selected Nix outputs")
    runtime = []
    for name in ("gio", "glib", "gobject"):
        matches = {
            path.resolve()
            for output in outputs
            for path in (output / "lib").glob(f"lib{name}-2.0.so*")
            if re.fullmatch(r"lib" + name + r"-2\.0\.so(?:\.\d+)*", path.name)
        }
        if len(matches) != 1:
            raise BazelWorkspaceError(f"missing/ambiguous resource compiler GLib runtime: {name}")
        path = matches.pop()
        if not path.is_file() or _nix_root(path) not in outputs:
            raise BazelWorkspaceError("resource compiler runtime escapes selected GLib outputs")
        runtime.append(path)
    result = subprocess.run(
        [str(executable), "--version"],
        env=child_environment(),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode or result.stdout.strip() != version:
        raise BazelWorkspaceError(f"glib-compile-resources does not report GLib version {version}")
    return (
        {
            "provider": "nix",
            "root": str(root),
            "version": version,
            "deriver": derivation,
            "outputs": [str(path) for path in outputs],
            "nixpkgsRevision": pin["rev"],
            "runtime_contract": "Installed immutable GLib SDK; not full OS/runtime hermeticity",
        },
        executable,
        sorted(runtime),
        [report_path, PROJECT_ROOT / "nixpkgs.json"],
    )


def _catalog_sdk(tree: Path, prefix: Path | None, host: str) -> tuple[dict[str, str], list[Path], list[Path]]:
    """Installed tool locations plus explicit SDK bytes, not a hermetic runtime claim."""
    evidence = [PROJECT_ROOT / "dependency-contract.json"]
    if prefix is None:
        report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
        report, pin = _json(report_path), _json(PROJECT_ROOT / "nixpkgs.json")
        if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
            raise BazelWorkspaceError("catalog tools require the selected pinned Nix report")
        _, version, _, outputs = _nix_dependency(report, "icu")
        evidence.extend([report_path, PROJECT_ROOT / "nixpkgs.json"])
    else:
        if host == "windows" and prefix.resolve().name != "x64-windows-static-md":
            raise BazelWorkspaceError("Windows catalog tools require the x64-windows-static-md ICU tools provider")
        outputs = [prefix.resolve()]
        status = prefix.parent / "vcpkg/status"
        paragraphs = status.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n\n")
        selected = "\n\n".join(p for p in paragraphs if f"Architecture: {prefix.name}" in p.splitlines())
        package = dependency_policy.parse_vcpkg_status(selected).get("icu")
        if package is None or set(package.features) != {"core", "tools"}:
            raise BazelWorkspaceError("catalog tools require installed ICU core/tools features")
        version = package.upstream_version
        evidence.extend([status, PROJECT_ROOT / "vcpkg.json", PROJECT_ROOT / "vcpkg-configuration.json"])
        if host == "windows":
            info = prefix.resolve().parent / "vcpkg/info"
            ownership = _native_path(info / f"icu_{package.upstream_version}_{package.triplet}.list", info)
            if not ownership.is_file():
                raise BazelWorkspaceError(f"ICU tool ownership manifest is missing: {ownership}")
            owned = ownership.read_text(encoding="utf-8").splitlines()
            tool_directory = "x64-windows-static-md/tools/icu/bin/"
            if any(tool_directory + name + ".exe" not in owned for name in ("genrb", "pkgdata")) or any(
                value.startswith(tool_directory) and value.lower().endswith(".dll") for value in owned
            ):
                raise BazelWorkspaceError("unowned/unsupported Windows static ICU tools manifest")
            evidence.append(ownership)
    contract = dependency_policy.load_contract()
    if not dependency_policy.effective_policy(contract, "icu", host).accepts(version):
        raise BazelWorkspaceError(f"ICU {version} does not satisfy dependency-contract.json")
    components = version.split(".")
    if len(components) not in {2, 3}:
        raise BazelWorkspaceError(f"unsupported ICU provider version format: {version}")
    include = _header_root(outputs, "unicode/uvernum.h")
    header = include / "unicode/uvernum.h"
    if prefix is None:
        if _nix_root(header.resolve()) not in outputs:
            raise BazelWorkspaceError(f"ICU header escapes selected Nix outputs: {header}")
    else:
        _native_path(header, prefix.resolve() / "include")
    macros = ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]
    if len(components) == 3:
        macros.append("U_ICU_VERSION_PATCHLEVEL_NUM")
    _verify_header_version(header, version, macros)
    evidence.append(header)
    tools = {"python": str(Path(sys.executable).absolute())}
    artifacts = [Path(sys.executable)]
    for name in ("genrb", "pkgdata"):
        candidates = (
            [root / "bin" / name for root in outputs]
            if prefix is None
            else [prefix / "tools/icu/bin" / (name + (".exe" if host == "windows" else ""))]
        )
        matches = {path.resolve(): path for path in candidates if _probe(path).is_file()}
        if len(matches) != 1:
            raise BazelWorkspaceError(f"missing/ambiguous selected ICU tool: {name}")
        resolved, executable = next(iter(matches.items()))
        if prefix is None:
            if _nix_root(resolved) not in outputs:
                raise BazelWorkspaceError(f"ICU tool escapes selected Nix outputs: {executable}")
        else:
            _native_path(executable, prefix.resolve() / "tools/icu/bin")
        tools[name] = str(executable.absolute())
        artifacts.append(executable)
    # Original installed paths retain native loader adjacency/RPATH. Declare
    # the selected ICU runtime bytes as inputs without copying a whole prefix.
    directories = [root / "lib" for root in outputs] if prefix is None else [prefix / "tools/icu/bin"]
    for directory in directories:
        if not _probe(directory).is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if (prefix is None and (".so" in path.name or path.suffix == ".dylib")) or (
                prefix is not None and path.suffix.lower() == ".dll" and path.name.lower().startswith("icu")
            ):
                if prefix is None:
                    if _nix_root(path) not in outputs:
                        raise BazelWorkspaceError(f"ICU runtime escapes selected Nix outputs: {path}")
                else:
                    _native_path(path, directory)
                if not path.is_file():
                    raise BazelWorkspaceError(f"missing ICU tool runtime: {path}")
                artifacts.append(path)
    if host == "windows":
        assert prefix is not None
        directory = prefix.resolve() / "tools/icu/bin"
        _native_path(directory, directory)
        # This reviewed triplet installs static ICU tools, separately from its
        # target archives. Status, ownership and the executable version admit
        # this producer layout; system/CRT runtime remains trusted SDK state.
        if any(path.suffix.lower() == ".dll" for path in directory.iterdir()):
            raise BazelWorkspaceError(f"unsupported DLL-bearing Windows static ICU tool layout: {directory}")
    # ICU can print its valid version with usage and exit1 for -V. Match the
    # selected provider's version, as the existing CMake tool admission does.
    version_query = subprocess.run(
        [tools["genrb"], "-V"],
        env=child_environment(),
        capture_output=True,
        text=True,
        check=False,
    )
    reported = re.findall(r"\bICU version ([^\s)]+)", version_query.stdout + version_query.stderr)
    if reported != [version]:
        raise BazelWorkspaceError(f"genrb executable does not report ICU version {version}")
    stdlib = Path(sysconfig.get_path("stdlib"))
    extension_root = sysconfig.get_config_var("DESTSHARED")
    if extension_root is not None and not isinstance(extension_root, str):
        raise BazelWorkspaceError("invalid selected Python native-extension root")
    extensions = Path(sys.base_prefix) / "DLLs" if host == "windows" else Path(extension_root or stdlib / "lib-dynload")
    if not stdlib.is_dir():
        raise BazelWorkspaceError(f"selected Python standard library is missing: {stdlib}")
    pending = [stdlib]
    seen = set()
    while pending:
        directory = pending.pop()
        if directory.resolve() in seen:
            raise BazelWorkspaceError(f"Python standard library directory alias/cycle: {directory}")
        seen.add(directory.resolve())
        for path in sorted(directory.iterdir()):
            if path.name in {"site-packages", "dist-packages", "__pycache__"}:
                continue
            if path.is_dir():
                pending.append(path)
            elif path.suffix == ".py":
                artifacts.append(path)
    if extensions.is_dir():
        artifacts.extend(
            path for path in sorted(extensions.iterdir()) if path.is_file() and path.suffix in {".so", ".pyd", ".dll"}
        )
    if host == "windows":
        artifacts.extend(path for path in sorted(Path(sys.base_prefix).glob("*.dll")) if path.is_file())
        system_root = os.environ.get("SYSTEMROOT") or os.environ.get("SystemRoot")
        if not system_root or not Path(system_root).is_absolute():
            raise BazelWorkspaceError("catalog actions require the selected Windows SystemRoot")
        tools["SystemRoot"] = system_root
        if user_profile := os.environ.get("USERPROFILE"):
            if not Path(user_profile).is_absolute():
                raise BazelWorkspaceError("native Python USERPROFILE must be an absolute path")
            tools["UserProfile"] = user_profile
    else:
        libdir, library = sysconfig.get_config_var("LIBDIR"), sysconfig.get_config_var("LDLIBRARY")
        if (
            isinstance(libdir, str)
            and isinstance(library, str)
            and libdir
            and library
            and (path := _probe(Path(libdir) / library)).is_file()
        ):
            artifacts.append(path)
        framework_name = sysconfig.get_config_var("PYTHONFRAMEWORK")
        if host == "macos" and framework_name:
            if not isinstance(framework_name, str):
                raise BazelWorkspaceError("invalid selected Python framework runtime name")
            framework = _probe(Path(sys.base_prefix) / framework_name)
            if not framework.is_file():
                raise BazelWorkspaceError(f"selected Python framework runtime is missing: {framework}")
            artifacts.append(framework)
    identity_path = tree / "catalog-sdk.json"
    _write_json(
        identity_path,
        {
            "icu_version": version,
            "tools": tools,
            "python_version": sys.version,
            "python_base_prefix": sys.base_prefix,
            "stdlib": str(stdlib),
            "extensions": str(extensions),
            "runtime_contract": (
                "Installed native SDK locations; declared payload bytes, not full OS/runtime hermeticity"
            ),
        },
    )
    artifacts.append(identity_path)
    return tools, sorted(set(artifacts)), evidence


def refresh_dependencies(tree: Path, config: str, *, group: str, prefix: str | None = None) -> Path:
    """Resolve governed interfaces; repository callers own materialization and invalidation."""
    from .bazel_deps import _Adapter

    host = native_platform()
    policy = _load_policy()
    groups = _package_groups(policy, host=host)
    if group == "catalog":
        # The real host catalog compiler needs ICU only, in its own compilation
        # mode. Keep the target repository's Debug/Release guards unchanged.
        packages = [package for package in groups["native"] if package["name"] == "icu"]
        if len(packages) != 1:
            raise BazelWorkspaceError("catalog compiler requires the governed ICU package")
        policy = {**policy, "packages": {"native": packages, "test": []}}
        groups = {"catalog": packages}
    elif group not in groups:
        raise BazelWorkspaceError(f"unsupported dependency group: {group}")
    provider_prefix = None if host == "linux" else vcpkg_prefix(prefix, None)
    expected_nix_outputs: dict[str, list[Path]] = {}
    if provider_prefix is None:
        base, versions, expected_nix_outputs = _linux_provider(policy, identity_path=tree / "provider-identity.json")
    else:
        base, versions = _native_provider(
            provider_prefix, config, host, identity_path=tree / "provider-identity.json", policy=policy
        )
    identity = _json(tree / "provider-identity.json")
    contract = dependency_policy.load_contract()
    for package in (package for group in groups.values() for package in group):
        source = package["nix"]
        if source in contract["dependencies"] and not dependency_policy.effective_policy(
            contract, source, host
        ).accepts(versions[package["name"]]):
            raise BazelWorkspaceError(f"{source} {versions[package['name']]} does not satisfy dependency-contract.json")
    common_evidence = _evidence([POLICY, PROJECT_ROOT / "dependency-contract.json"])
    base["provider"]["evidence"].extend(common_evidence)
    base["allowed_roots"].extend(item["path"] for item in common_evidence)
    generated = tree / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    for selected_group, packages in groups.items():
        if group != selected_group:
            continue
        spec = {
            **base,
            "schema": 1,
            "platform": host,
            "config": config.title(),
            "linkage": "static" if selected_group == "test" or host in {"macos", "windows"} else "shared",
            "system_libraries": {
                "linux": ["pthread", "m", "dl"],
                "macos": ["c++", "pthread", "m"],
                "windows": [
                    "Shell32",
                    "kernel32",
                    "user32",
                    "gdi32",
                    "winspool",
                    "shell32",
                    "ole32",
                    "oleaut32",
                    "comdlg32",
                    "advapi32",
                    "psapi",
                    "bcrypt",
                    "shlwapi",
                    "ntdll",
                    "ws2_32",
                    "Synchronization",
                    "mswsock",
                    "uuid",
                ]
                if selected_group != "test"
                else [],
            }[host],
            "packages": [],
        }
        patches = []
        for package in packages:
            name = package["name"]
            roots = (
                [provider_prefix]
                if provider_prefix is not None
                else [Path(value) for value in identity["packages"][name]["outputs"]]
            )
            selected: dict[str, Any] = {"name": name, "modules": package["modules"].copy()}
            spec["packages"].append(selected)
            if name in {"icu", "icu_uc", "catch2"}:
                marker, macros = {
                    "icu": ("unicode/uvernum.h", ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]),
                    "icu_uc": ("unicode/uvernum.h", ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]),
                    "catch2": (
                        "catch2/catch_version_macros.hpp",
                        ["CATCH_VERSION_MAJOR", "CATCH_VERSION_MINOR", "CATCH_VERSION_PATCH"],
                    ),
                }[name]
                include = _header_root(roots, marker)
                _verify_header_version(include / marker, versions[name], macros)
                if host == "windows" and name in {"icu", "icu_uc"}:
                    header = _native_path(include / "unicode/utypes.h", include)
                    static_api = (
                        r"#elif 1[ \t]*\n"
                        + "".join(
                            r"#define " + macro + r"[ \t]*\n"
                            for macro in (
                                "U_DATA_API",
                                "U_COMMON_API",
                                "U_I18N_API",
                                "U_LAYOUT_API",
                                "U_LAYOUTEX_API",
                                "U_IO_API",
                                "U_TOOLUTIL_API",
                            )
                        )
                        + r"#elif defined\(U_COMMON_IMPLEMENTATION\)"
                    )
                    if not re.search(static_api, header.read_text(encoding="utf-8")):
                        raise BazelWorkspaceError(f"static ICU requires its non-importing installed header: {header}")
                    platform_header = _native_path(include / "unicode/platform.h", include)
                    if not re.search(
                        r"#elif 1[ \t]*\n#\s*define U_EXPORT[ \t]*\n",
                        platform_header.read_text(encoding="utf-8"),
                    ):
                        raise BazelWorkspaceError(f"static ICU requires its installed export header: {platform_header}")
                selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                continue
            if name == "ftxui" or (name == "cli11" and host != "linux"):
                selected["linkage"] = "static"
            metadata = _has_metadata(spec, selected["modules"])
            if name == "stb":
                patches.append(_manual_patch(spec, name, _stb_interface(roots, host, versions[name])))
                continue
            if name in {"giomm", "gtkmm"}:
                if not metadata:
                    raise BazelWorkspaceError(f"{name} requires complete governed pkg-config metadata")
                if name == "gtkmm":
                    # Cairomm's public headers expose both Cairo font backends,
                    # which gtkmm metadata does not request on this SDK.
                    selected["expected_versions"] = {
                        module: identity["inputs"]["cairo"]["version"]
                        if module in {"cairo-ft", "cairo-fc"}
                        else versions[name]
                        for module in selected["modules"]
                    }
                else:
                    selected["expected_versions"] = {
                        module: identity["inputs"]["glib"]["version"] if module == "gio-unix-2.0" else versions[name]
                        for module in selected["modules"]
                    }
                continue
            if name == "xtest":
                if not metadata:
                    raise BazelWorkspaceError("xtest requires complete governed pkg-config metadata")
                # XTest is a shared X11 extension library; the test group's
                # otherwise-static default would look for archives Nix does not install.
                selected["linkage"] = "shared"
                selected["expected_versions"] = {
                    "xtst": versions[name],
                    "x11": identity["inputs"]["libX11"]["version"],
                    "xi": identity["inputs"]["libXi"]["version"],
                }
                continue
            if name == "boost_process":
                if host == "linux":
                    interface = _linux_boost_process_interface(spec, roots, versions[name])
                    if metadata:
                        selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                    else:
                        selected["modules"] = []
                        patches.append(_manual_patch(spec, name, interface))
                    continue
                # The installed compiled ports ship no pkg-config metadata; a
                # discovered module is unreviewed for this static interface and
                # must fail instead of being consumed as ambient metadata.
                if metadata:
                    platform_name = "macOS" if spec["platform"] == "macos" else spec["platform"]
                    raise BazelWorkspaceError(f"unreviewed {platform_name} Boost Process pkg-config metadata")
                assert provider_prefix is not None
                interface = (
                    _macos_boost_process_interface(spec, provider_prefix, config, versions[name], policy)
                    if host == "macos"
                    else _windows_boost_process_interface(spec, provider_prefix, config, versions[name], policy)
                )
                selected["modules"] = []
                selected["linkage"] = "static"
                patches.append(_manual_patch(spec, name, interface))
                continue
            if name in {"spdlog", "ftxui", "mimalloc", "date_tz"}:
                if name == "spdlog":
                    include = _header_root(roots, "spdlog/version.h")
                    _verify_header_version(
                        include / "spdlog/version.h",
                        versions[name],
                        ["SPDLOG_VER_MAJOR", "SPDLOG_VER_MINOR", "SPDLOG_VER_PATCH"],
                    )
                elif name == "mimalloc":
                    include = _header_root(roots, "mimalloc.h")
                    values = re.findall(
                        r"^\s*#\s*define\s+MI_MALLOC_VERSION\s+(\d+)\b",
                        (include / "mimalloc.h").read_text(encoding="utf-8"),
                        re.MULTILINE,
                    )
                    components = versions[name].split(".")
                    if (
                        len(values) != 1
                        or len(components) != 3
                        or not all(value.isdecimal() for value in components)
                        or int(values[0])
                        != sum(int(value) * scale for value, scale in zip(components, (10000, 100, 1), strict=True))
                    ):
                        raise BazelWorkspaceError("mimalloc header version does not match the selected provider")
                interface = {}
                if host != "linux":
                    interface = _library_interface(
                        package,
                        roots,
                        config,
                        host,
                        policy,
                    )
                    selected["linkage"] = interface["libraries"][0]["kind"]
                elif not metadata:
                    raise BazelWorkspaceError(f"{name} requires complete governed pkg-config metadata")
                if not metadata:
                    selected["modules"] = []
                    patches.append(_manual_patch(spec, name, interface))
                else:
                    selected["expected_versions"] = _metadata_versions(name, versions[name], selected["modules"], host)
                    patch = _compile_interface_patch(spec, name, interface)
                    if patch is not None:
                        patches.append(patch)
                    if host == "windows" and name == "mimalloc":
                        patch = _windows_static_link_patch(
                            spec,
                            name,
                            [item["name"] for item in interface["libraries"]],
                            [value.removesuffix(".lib") for value in interface["linkopts"]],
                        )
                        if patch is not None:
                            patches.append(patch)
                    if host == "windows" and name == "spdlog":
                        reviewed = policy["windows_spdlog_threads_remove"]
                        removed = {"linkopts": reviewed["linkopts"]}
                        patches.append(
                            _patch(
                                "windows-spdlog-native-threads",
                                spec,
                                name,
                                expect={"present": removed, "missing": {}, "unmet_requirements": []},
                                remove=removed,
                                add={},
                                satisfy=[],
                            )
                        )
                    if host == "macos" and name == "mimalloc":
                        reviewed = policy["macos_mimalloc_runtime_remove"]
                        patch = _macos_icu_runtime_patch(spec, reviewed, package=name)
                        if patch is not None:
                            patches.append(patch)
                continue
            if name in {"lmdb", "xxhash"}:
                marker, macros = {
                    "lmdb": ("lmdb.h", ["MDB_VERSION_MAJOR", "MDB_VERSION_MINOR", "MDB_VERSION_PATCH"]),
                    "xxhash": ("xxhash.h", ["XXH_VERSION_MAJOR", "XXH_VERSION_MINOR", "XXH_VERSION_RELEASE"]),
                }[name]
                include = _header_root(roots, marker)
                _verify_header_version(include / marker, versions[name], macros)
            if name == "lexy":
                evidence_path = tree / "lexy-identity.json"
                lexy_interface = _lexy_interface(roots, host, versions[name], evidence_path)
                header_evidence = _evidence([evidence_path, Path(lexy_interface["header_dirs"][0]) / package["header"]])
                spec["provider"] = {
                    **spec["provider"],
                    "evidence": [*spec["provider"]["evidence"], *header_evidence],
                }
                spec["allowed_roots"] = [*spec["allowed_roots"], str(evidence_path.resolve())]
                patches.append(_manual_patch(spec, name, lexy_interface))
                continue
            if "header" in package:
                if name == "fakeit":
                    reviewed = policy["header_fallbacks"].get(name, {}).get(host)
                    if reviewed is None:
                        raise BazelWorkspaceError(f"unsupported FakeIt header interface: {host}")
                    include = _header_root(roots, reviewed.get("header", package["header"]))
                    fakeit_interface = {"header_dirs": [str(include / reviewed["include"])]}
                    if metadata:
                        selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                        patch = _compile_interface_patch(spec, name, fakeit_interface)
                        if patch is not None:
                            patches.append(patch)
                    else:
                        selected["modules"] = []
                        patches.append(_manual_patch(spec, name, fakeit_interface))
                    continue
                include = _header_root(roots, package["header"])
                marker, macros = {
                    "boost": ("boost/version.hpp", ["BOOST_VERSION"]),
                    "gsl_lite": (package["header"], ["gsl_lite_MAJOR", "gsl_lite_MINOR", "gsl_lite_PATCH"]),
                    "cli11": ("CLI/Version.hpp", ["CLI11_VERSION_MAJOR", "CLI11_VERSION_MINOR", "CLI11_VERSION_PATCH"]),
                    "fast_float": (
                        "fast_float/float_common.h",
                        ["FASTFLOAT_VERSION_MAJOR", "FASTFLOAT_VERSION_MINOR", "FASTFLOAT_VERSION_PATCH"],
                    ),
                }[name]
                _verify_header_version(include / marker, versions[name], macros, encoded=name == "boost")
                interface = {
                    "header_dirs": [str(include)],
                    **policy["header_fallbacks"].get(name, {}).get(host, {}),
                }
                if name == "cli11" and host != "linux":
                    interface = _library_interface(
                        package,
                        roots,
                        config,
                        host,
                        policy,
                    )
                if metadata:
                    selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                    patch = _compile_interface_patch(spec, name, interface)
                    if patch is not None:
                        patches.append(patch)
                    if name == "cli11" and host == "windows":
                        # The reviewed compiled port omits CommandLineToArgvW's
                        # SDK import from pkg-config. Keep it on CLI11 itself.
                        links = {"linkopts": interface["linkopts"]}
                        patches.append(
                            _patch(
                                "cli11-windows-system-link",
                                spec,
                                name,
                                expect={
                                    "present": {"libraries": [{"name": "CLI11", "kind": "static"}]},
                                    "missing": links,
                                    "unmet_requirements": [],
                                },
                                add=links,
                                satisfy=[],
                            )
                        )
                else:
                    selected["modules"] = []
                    patches.append(_manual_patch(spec, name, interface))
                continue
            # These codec interfaces come from metadata, never a partial fallback.
            if name in {"flac", "fdk_aac", "mpg123", "opus"}:
                if not metadata:
                    raise BazelWorkspaceError(f"{name} pkg-config metadata is missing; no reviewed fallback for {host}")
                interface = {}
                if name == "opus":
                    # Upstream metadata exposes <opus.h>; Aobus consumes <opus/opus.h>.
                    interface["header_dirs"] = [str(_header_root(roots, "opus/opus.h"))]
                if host == "windows":
                    reviewed = policy["windows_audio"][name]
                    assert provider_prefix is not None
                    selected["linkage"] = "static"
                    patch = _windows_static_link_patch(
                        spec, name, reviewed["libraries"], reviewed.get("system_libraries", [])
                    )
                    if patch is not None:
                        patches.append(patch)
                    if name == "flac":
                        include = _header_root(roots, "FLAC/export.h")
                        header = _native_path(include / "FLAC/export.h", include)
                        if not re.search(
                            r"#if 1 && !\(defined\(DLL_EXPORT\)\)\s*\n\s*#define FLAC_API\s*\n",
                            header.read_text(encoding="utf-8"),
                        ):
                            raise BazelWorkspaceError(
                                f"static FLAC requires its non-importing installed header: {header}"
                            )
                elif host == "macos":
                    patches.append(
                        _macos_audio_runtime_patch(
                            spec,
                            name,
                            policy["macos_audio_runtime_remove"][name],
                        )
                    )
            elif host != "linux" or name == "rapidyaml":
                if name == "alac" and not metadata and _has_metadata(spec, ["libalac"]):
                    raise BazelWorkspaceError("unreviewed ALAC metadata alias; complete fallback requires absence")
                # Linkage is reviewed policy; artifacts and runtimes must all exist.
                interface = _library_interface(
                    package,
                    roots,
                    config,
                    host,
                    policy,
                )
                selected["linkage"] = interface["libraries"][0]["kind"]
            else:
                interface = {}
            if metadata:
                selected["expected_versions"] = _metadata_versions(name, versions[name], selected["modules"], host)
                patch = _compile_interface_patch(spec, name, interface)
                if patch is not None:
                    patches.append(patch)
            else:
                if not interface.get("libraries"):
                    raise BazelWorkspaceError(f"{name} pkg-config metadata is missing; no reviewed fallback for {host}")
                selected["modules"] = []
                patches.append(_manual_patch(spec, name, interface))
            if name == "rapidyaml":
                include = _header_root(
                    roots,
                    "ryml.hpp" if any(_probe(r / "include/ryml.hpp").is_file() for r in roots) else "ryml/ryml.hpp",
                )
                _verify_header_version(
                    _rapidyaml_version_header(include),
                    versions[name],
                    ["RYML_VERSION_MAJOR", "RYML_VERSION_MINOR", "RYML_VERSION_PATCH"],
                )
                if provider_prefix is not None:
                    c4version = identity["packages"]["c4core"]["version"].split("#", maxsplit=1)[0]
                    _verify_header_version(
                        include / "c4/version.hpp",
                        c4version,
                        ["C4CORE_VERSION_MAJOR", "C4CORE_VERSION_MINOR", "C4CORE_VERSION_PATCH"],
                    )
        if selected_group in {"native", "catalog"}:
            for package in packages:
                name = package["name"]
                if name not in {"icu", "icu_uc"}:
                    continue
                if host == "windows":
                    assert provider_prefix is not None
                    patches.append(_windows_icu_patch(spec, policy["windows_icu"], provider_prefix, package=name))
                elif host == "macos":
                    patch = _macos_icu_runtime_patch(spec, policy["macos_icu_runtime_remove"], package=name)
                    if patch is not None:
                        patches.append(patch)
        if selected_group == "test" and provider_prefix is not None:
            if host == "windows":
                bindir = provider_prefix / "debug/bin" if config == "debug" else provider_prefix / "bin"
                if list(bindir.glob("Catch2*.dll")):
                    raise BazelWorkspaceError("initial Bazel tests require static Catch2/Catch2Main, not Catch2 DLLs")
            pc = Path(base["pkg_config"]["pc_dirs"][0]) / "catch2-with-main.pc"
            if not _probe(pc).is_file():
                spec["packages"][0] = {"name": "catch2", "modules": []}
                patches.append(_catch_fallback(spec, policy["catch2_fallback"], provider_prefix))
            elif host == "windows":
                removed = {}
                for field, option in (("linkopts", "--libs"), ("copts", "--cflags")):
                    flags = _metadata_flags(spec, option)
                    tokens = [token for token in policy["windows_catch2_remove"] if token in flags]
                    if tokens:
                        removed[field] = tokens
                if removed:
                    # Only reviewed translations are eligible, and generated guards assert exact presence.
                    patches.append(
                        _patch(
                            "windows-catch2-pthread",
                            spec,
                            "catch2",
                            expect={"present": removed, "missing": {}, "unmet_requirements": []},
                            remove=removed,
                            add={},
                            satisfy=[],
                        )
                    )
        from .bazel_headers import vcpkg_header_inventory

        if provider_prefix is not None:
            spec["allowed_roots"].append(str(provider_prefix.parent / "vcpkg/info"))
        adapter = _Adapter(spec, {"schema": 1, "patches": patches})
        adapter.prepare()
        for name, prepared_interface, _ in adapter.packages:
            if host == "windows":
                if (
                    adapter.package_linkages[name] != "static"
                    or prepared_interface.runtime
                    or any(library.kind != "static" for library in prepared_interface.libraries)
                ):
                    raise BazelWorkspaceError(f"Windows target dependencies require a static-only interface: {name}")
                shared_defines = {
                    "mimalloc": {"MI_SHARED_LIB", "MI_SHARED_LIB_EXPORT"},
                    "xxhash": {"XXH_SHARED_LIBRARY", "XXH_IMPORT", "XXH_EXPORT"},
                    "mpg123": {"LINK_MPG123_DLL", "BUILD_MPG123_DLL"},
                    "flac": {"DLL_EXPORT", "FLAC_API_EXPORTS"},
                }.get(name, set())
                if any(define.split("=", 1)[0] in shared_defines for define in prepared_interface.defines):
                    raise BazelWorkspaceError(f"shared-only consumer definition on Windows static interface: {name}")
            if (
                name == "cli11"
                and host != "linux"
                and (
                    "CLI11_COMPILE" not in prepared_interface.defines
                    or adapter.package_linkages[name] != "static"
                    or [library.name for library in prepared_interface.libraries] != ["CLI11"]
                    or (
                        host == "windows"
                        and prepared_interface.linkopts != policy["library_fallbacks"]["cli11"][host]["linkopts"]
                    )
                )
            ):
                raise BazelWorkspaceError(
                    "native CLI11 requires its compiled static CLI11_COMPILE interface and reviewed system links"
                )
            if name == "boost_process" and host == "linux":
                required = {
                    "BOOST_PROCESS_NO_LIB",
                    "BOOST_PROCESS_DYN_LINK",
                    "BOOST_FILESYSTEM_NO_LIB",
                    "BOOST_FILESYSTEM_DYN_LINK",
                }
                if (
                    set(prepared_interface.defines) != required
                    or adapter.package_linkages[name] != "shared"
                    or [library.name for library in prepared_interface.libraries]
                    != ["boost_process", "boost_filesystem"]
                    or prepared_interface.copts
                    or prepared_interface.linkopts
                    or prepared_interface.runtime
                    or prepared_interface.header_dirs
                    != [str(_header_root(expected_nix_outputs[name], "boost/version.hpp"))]
                ):
                    raise BazelWorkspaceError("Boost Process requires its shared Boost.Filesystem ABI interface")
            if name == "boost_process" and host == "macos":
                assert provider_prefix is not None
                # Static consumption mirrors the CMake export interface: STATIC_LINK
                # plus NO_LIB, never DYN_LINK, USE_STD_FS, STANDALONE or a
                # BOOST_FILESYSTEM_VERSION that would skew consumer semantics.
                required = {
                    "BOOST_PROCESS_STATIC_LINK",
                    "BOOST_PROCESS_NO_LIB",
                    "BOOST_FILESYSTEM_STATIC_LINK",
                    "BOOST_FILESYSTEM_NO_LIB",
                }
                if (
                    set(prepared_interface.defines) != required
                    or adapter.package_linkages[name] != "static"
                    or [library.name for library in prepared_interface.libraries]
                    != ["boost_process", "boost_filesystem"]
                    or any(library.kind != "static" for library in prepared_interface.libraries)
                    or prepared_interface.copts
                    or prepared_interface.linkopts
                    or prepared_interface.runtime
                    or prepared_interface.header_dirs != [str((provider_prefix / "include").resolve())]
                ):
                    raise BazelWorkspaceError("macOS Boost Process requires its static Boost.Filesystem ABI interface")
            if name == "boost_process" and host == "windows":
                assert provider_prefix is not None
                # Static consumption mirrors the producer interface: the reviewed
                # policy owns the archive closure and its Windows SDK tail;
                # native COFF/ABI acceptance belongs to the native gates.
                reviewed = policy["library_fallbacks"]["boost_process"]["windows"]
                if (
                    set(prepared_interface.defines) != set(reviewed["defines"])
                    or adapter.package_linkages[name] != "static"
                    or [library.name for library in prepared_interface.libraries] != reviewed["libraries"]
                    or any(library.kind != "static" for library in prepared_interface.libraries)
                    or prepared_interface.copts
                    or prepared_interface.linkopts != reviewed.get("linkopts", [])
                    or prepared_interface.runtime
                    or prepared_interface.header_dirs != [str((provider_prefix / "include").resolve())]
                ):
                    raise BazelWorkspaceError(
                        "Windows Boost Process requires its reviewed static Boost.Filesystem ABI interface"
                    )
            if name == "spdlog":
                required = {"SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT"}
                if adapter.package_linkages[name] == "shared":
                    required.add("SPDLOG_SHARED_LIB")
                forbidden = {"SPDLOG_FMT_EXTERNAL", "SPDLOG_HEADER_ONLY"}
                if host == "windows" and adapter.package_linkages[name] == "static":
                    forbidden.add("SPDLOG_SHARED_LIB")
                if not required <= set(prepared_interface.defines) or any(
                    define.split("=", 1)[0] in forbidden for define in prepared_interface.defines
                ):
                    raise BazelWorkspaceError("spdlog must expose its compiled std::format ABI without external fmt")
            if name == "ftxui" and [library.name for library in prepared_interface.libraries] != [
                "ftxui-component",
                "ftxui-dom",
                "ftxui-screen",
            ]:
                raise BazelWorkspaceError("FTXUI requires the complete component/dom/screen archive interface")
        if provider_prefix is not None:
            package_ports = {package["name"]: package["vcpkg"] for package in packages}
            for name, resolved_interface, _ in adapter.packages:
                ports = package_ports[name]
                if name in {"boost", "boost_process"}:
                    # The Process v2 header closure and its Boost transitive
                    # headers come from the selected coherent Boost ports, never
                    # from an unowned whole-triplet include.
                    ports = [port for port in identity["packages"] if port.startswith("boost-")]
                inventory = vcpkg_header_inventory(
                    provider_prefix,
                    {port: identity["packages"][port]["version"].split("#", 1)[0] for port in ports},
                    [Path(path) for path in resolved_interface.header_dirs],
                )
                adapter.header_files[name] = list(inventory.files)
                for path in inventory.manifests:
                    adapter.record(path, "header_ownership")
        output = generated / selected_group
        plan: dict[Path, tuple[Path, str]] = {}
        manifest = adapter.build(output, plan=plan)
        _write_json(output / "manifest.json", manifest)
        _write_json(
            output / "repository.json",
            {
                "links": [
                    {"source": str(source), "destination": path.as_posix()} for path, (source, _) in plan.items()
                ],
                "watch_files": sorted(
                    {
                        *[
                            entry["path"]
                            for entry in manifest["inputs"]
                            if set(entry["roles"]) - {"header", "library", "runtime"}
                        ],
                        *map(str, adapter.aliases),
                        *map(str, adapter.probes),
                    }
                ),
                "watch_directories": list(map(str, [*adapter.libdirs, *adapter.pcdirs])),
            },
        )
        if host == "linux":
            _verify_linux_resolution(
                manifest, {package["name"]: expected_nix_outputs[package["name"]] for package in packages}
            )
    return generated


def _linux_toolchain(generated: Path) -> list[dict[str, str]]:
    """Project the pinned Nix SDK into a rule-based native C/C++ toolchain."""
    wrapper = _linux_cc_wrapper()
    tools, declared_roots = wrapper.get("tools"), wrapper.get("builtinRoots")
    roles = {"clang", "clang++", "ar", "strip", "objcopy"}
    if (
        not isinstance(tools, dict)
        or set(tools) != roles
        or any(not isinstance(value, str) for value in tools.values())
        or not isinstance(declared_roots, list)
        or not declared_roots
        or any(not isinstance(value, str) for value in declared_roots)
    ):
        raise BazelWorkspaceError("Nix compiler SDK metadata is missing; re-enter the pinned shell")
    roots = []
    for value in declared_roots:
        root = Path(value).resolve()
        if not root.is_dir() or root != _nix_root(root):
            raise BazelWorkspaceError(f"invalid Nix compiler built-in root: {value}")
        roots.append(root)
    links: list[dict[str, str]] = []
    labels: dict[str, str] = {}
    for role, value in sorted(tools.items()):
        source = Path(value)
        if not source.is_absolute() or not source.is_file() or not os.access(source, os.X_OK):
            raise BazelWorkspaceError(f"Nix compiler tool is missing or not executable: {value}")
        _nix_root(source)
        destination = "tools/" + source.name
        if destination in labels.values():
            raise BazelWorkspaceError(f"duplicate Nix compiler tool basename: {source.name}")
        labels[role] = destination
        # LLVM multicall tools and compiler wrappers dispatch on argv[0].
        # Keep the selected lexical basename even when the source is a symlink.
        links.append({"source": str(source), "destination": destination})
    probe = subprocess.run(
        [tools["clang++"], "-E", "-x", "c++", "-std=c++26", "-no-canonical-prefixes", "-", "-v"],
        input="",
        env={"PATH": ""},
        capture_output=True,
        text=True,
        check=True,
    )
    start, end = "#include <...> search starts here:", "End of search list."
    if probe.stderr.count(start) != 1 or probe.stderr.count(end) != 1:
        raise BazelWorkspaceError("Nix compiler did not report its built-in include directories")
    directories = [
        line.strip() for line in probe.stderr.split(start, 1)[1].split(end, 1)[0].splitlines() if line.strip()
    ]
    if not directories:
        raise BazelWorkspaceError("Nix compiler reported an empty built-in include interface")
    for value in directories:
        directory = Path(value)
        if (
            not directory.is_absolute()
            or not directory.is_dir()
            or not any(directory.resolve().is_relative_to(root) for root in roots)
        ):
            raise BazelWorkspaceError(f"compiler include is outside its declared Nix SDK roots: {value}")
    machine = _host_machine().lower()
    if machine not in {"x86_64", "aarch64"}:
        raise BazelWorkspaceError(f"unsupported Linux compiler CPU: {machine}")
    unoptimized = linux_hardening_environment("debug")
    metadata = {
        "TOOLS": labels,
        "BUILTIN_INCLUDE_DIRECTORIES": directories,
        "HARDENING_ENVIRONMENT": {
            "dbg": unoptimized,
            "opt": linux_hardening_environment("release"),
            "fastbuild": unoptimized,
        },
        "CONSTRAINT_CPU": machine,
    }
    target = generated / "toolchain"
    target.mkdir(parents=True, exist_ok=True)
    (target / "native_paths.bzl").write_text(
        "# Governed Nix metadata; compilation-mode behavior belongs to the toolchain.\n"
        + "\n".join(name + " = " + json.dumps(value, sort_keys=True) for name, value in metadata.items())
        + "\n",
        encoding="utf-8",
    )
    (target / "BUILD.bazel").write_text(
        'load("@aobus//bazel:linux_toolchain.bzl", "linux_toolchain")\n'
        'load(":native_paths.bzl", "BUILTIN_INCLUDE_DIRECTORIES", "CONSTRAINT_CPU", "HARDENING_ENVIRONMENT", "TOOLS")\n'
        '\npackage(default_visibility = ["//visibility:public"])\n\n'
        "linux_toolchain(\n"
        "    tools = TOOLS,\n"
        "    builtin_include_directories = BUILTIN_INCLUDE_DIRECTORIES,\n"
        "    hardening_environment = HARDENING_ENVIRONMENT,\n"
        "    constraint_cpu = CONSTRAINT_CPU,\n"
        ")\n",
        encoding="utf-8",
    )
    return links


def _macos_toolchain(generated: Path) -> None:
    llvm = Path(os.environ.get("AOBUS_LLVM_ROOT", ""))
    shim = Path(os.environ.get("AOBUS_LIBCXX_EXPECTED_SHIM", ""))
    if not llvm.is_absolute() or not (llvm / "bin/clang++").is_file() or not shim.is_absolute():
        raise BazelWorkspaceError("native LLVM/shim is missing; invoke through ./ao on macOS")
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()
    if not Path(sdk).is_dir():
        raise BazelWorkspaceError(f"macOS SDK is missing: {sdk}")
    machine = platform.machine().lower()
    if machine not in {"x86_64", "arm64"}:
        raise BazelWorkspaceError(f"unsupported macOS CPU: {machine}")
    linker = _macos_linker()
    deployment = _json(PROJECT_ROOT / "script/ao/macos-toolchain.json")["deploymentTarget"]
    values = {
        "LLVM": str(llvm),
        "LLVM_REAL": str(llvm.resolve()),
        "SDK": str(Path(sdk).resolve()),
        "SHIM": str(shim),
        "LINKER": str(linker),
        "DEPLOYMENT": deployment,
        "CPU": machine,
        "CONSTRAINT_CPU": "x86_64" if machine == "x86_64" else "aarch64",
    }
    output = generated / "toolchain"
    output.mkdir(parents=True, exist_ok=True)
    (output / "native_paths.bzl").write_text(
        "".join(f"{key} = {json.dumps(value, ensure_ascii=False)}\n" for key, value in values.items()), encoding="utf-8"
    )
    (output / "BUILD.bazel").write_text(
        'load("@aobus//bazel:macos_cc_config.bzl", "macos_toolchain")\n'
        'load(":native_paths.bzl", "CONSTRAINT_CPU", "CPU", "DEPLOYMENT", "LINKER", '
        '"LLVM", "LLVM_REAL", "SDK", "SHIM")\n'
        '\npackage(default_visibility = ["//visibility:public"])\n\n'
        "macos_toolchain(\n"
        "    llvm = LLVM,\n"
        "    llvm_real = LLVM_REAL,\n"
        "    sdk = SDK,\n"
        "    shim = SHIM,\n"
        "    linker = LINKER,\n"
        "    deployment = DEPLOYMENT,\n"
        "    cpu = CPU,\n"
        "    constraint_cpu = CONSTRAINT_CPU,\n"
        ")\n",
        encoding="utf-8",
    )


def _windows_toolchain(generated: Path, compiler_environment: object) -> list[dict[str, str]]:
    """Project the selected local MSVC SDK, not a relocatable compiler installation."""
    required = {"INCLUDE", "LIB", "PATH", "TMP", "VCToolsInstallDir", "WindowsSdkDir", "WindowsSDKVersion"}
    if not isinstance(compiler_environment, dict) or any(
        not isinstance(compiler_environment.get(name), str) or not compiler_environment[name].strip()
        for name in required
    ):
        raise BazelWorkspaceError(
            "Windows compiler_environment requires nonempty INCLUDE, LIB, PATH, TMP and SDK metadata"
        )
    environment = compiler_environment
    vc = Path(environment["VCToolsInstallDir"])
    sdk = Path(environment["WindowsSdkDir"])
    version = environment["WindowsSDKVersion"].rstrip("\\/")
    if (
        not vc.is_absolute()
        or not sdk.is_absolute()
        or not version
        or Path(version).name != version
        or version in {".", ".."}
        or not _probe(vc).is_dir()
        or not _probe(sdk).is_dir()
    ):
        raise BazelWorkspaceError("selected Windows VC/SDK roots or SDK version are invalid")
    if not Path(environment["TMP"]).is_absolute() or not _probe(Path(environment["TMP"])).is_dir():
        raise BazelWorkspaceError("selected Windows compiler TMP is not an existing absolute directory")
    # Exact SDK interfaces only: neither a parent prefix nor a provider header
    # directory may become a compiler built-in and bypass Bazel's header checks.
    include_roots = [vc / "include", vc / "atlmfc/include"]
    include_roots.extend(sdk / "Include" / version / part for part in ("ucrt", "shared", "um", "winrt", "cppwinrt"))
    if [parent.name.casefold() for parent in vc.parents[:3]] == ["msvc", "tools", "vc"]:
        include_roots.append(vc.parents[2] / "Auxiliary/VS/include")
    lib_roots = [vc / "lib/x64", vc / "atlmfc/lib/x64"]
    lib_roots.extend(sdk / "Lib" / version / part / "x64" for part in ("ucrt", "um"))
    # Optional selected NETFX SDK metadata is authority for two exact interfaces,
    # never a version inferred from arbitrary INCLUDE/LIB entries or a new pin.
    netfx_value = environment.get("NETFXSDKDir", "")
    if not isinstance(netfx_value, str):
        raise BazelWorkspaceError("selected NETFXSDKDir metadata must be a string")
    if netfx_value:
        netfx = Path(netfx_value)
        if (
            not netfx.is_absolute()
            or netfx.parent != sdk.parent / "NETFXSDK"
            or not _probe(netfx).is_dir()
            or netfx.resolve() != netfx.absolute()
            or netfx.is_relative_to(PROJECT_ROOT)
        ):
            raise BazelWorkspaceError(
                "selected NETFXSDKDir must be an installed version root under WindowsSdkDir.parent/NETFXSDK"
            )
        include_roots.append(netfx / "include/um")
        lib_roots.append(netfx / "lib/um/x64")
    includes = []
    for name, allowed in (("INCLUDE", include_roots), ("LIB", lib_roots)):
        directories = [Path(value) for value in environment[name].split(";") if value]
        if not directories:
            raise BazelWorkspaceError(f"selected Windows {name} has no SDK directories")
        for directory in directories:
            if (
                not directory.is_absolute()
                or not _probe(directory).is_dir()
                or directory not in allowed
                or directory.resolve() != directory.absolute()
            ):
                raise BazelWorkspaceError(f"Windows {name} is not an exact selected VC/SDK directory: {directory}")
        if name == "INCLUDE":
            includes = directories
    tools: dict[str, str] = {}
    inputs: dict[str, str] = {}
    links = []
    tool_directory = vc / "bin/Hostx64/x64"
    if tool_directory.resolve() != tool_directory.absolute():
        raise BazelWorkspaceError("selected MSVC tool directory must retain its installed SDK location")
    for role in ("cl", "link", "lib", "ml64"):
        # Use the existing PATH selection/probes, then reject tools from another
        # installation. Execute installed tools; projected bytes are inputs only.
        source = Path(_executable(role + ".exe", path=environment["PATH"]))
        if (
            not source.is_absolute()
            or source.parent != tool_directory
            or source.resolve() != source.absolute()
            or not _probe(source).is_file()
        ):
            raise BazelWorkspaceError(f"selected {role}.exe is outside VCToolsInstallDir bin/Hostx64/x64: {source}")
        destination = "tools/" + source.name
        if destination.casefold() in {value.casefold() for value in inputs.values()}:
            raise BazelWorkspaceError(f"duplicate Windows compiler tool destination: {destination}")
        tools[role] = source.as_posix()
        inputs[role] = destination
        links.append({"source": str(source), "destination": destination})
    target = generated / "toolchain"
    target.mkdir(parents=True, exist_ok=True)
    metadata = {
        "TOOLS": tools,
        "TOOL_INPUTS": inputs,
        "BUILTIN_INCLUDE_DIRECTORIES": [directory.as_posix() for directory in includes],
        "MSVC_ENVIRONMENT": {name: environment[name] for name in ("INCLUDE", "LIB", "PATH", "TMP")},
    }
    (target / "native_paths.bzl").write_text(
        "# Selected installed MSVC SDK; native DLL/SDK runtime remains trusted local state.\n"
        + "\n".join(
            name + " = " + json.dumps(value, sort_keys=True, ensure_ascii=False) for name, value in metadata.items()
        )
        + "\n",
        encoding="utf-8",
    )
    (target / "BUILD.bazel").write_text(
        'load("@aobus//bazel:windows_toolchain.bzl", "windows_toolchain")\n'
        'load(":native_paths.bzl", "BUILTIN_INCLUDE_DIRECTORIES", "MSVC_ENVIRONMENT", "TOOLS", "TOOL_INPUTS")\n'
        '\npackage(default_visibility = ["//visibility:public"])\n\n'
        "windows_toolchain(\n"
        "    tools = TOOLS,\n"
        "    tool_inputs = TOOL_INPUTS,\n"
        "    builtin_include_directories = BUILTIN_INCLUDE_DIRECTORIES,\n"
        "    msvc_environment = MSVC_ENVIRONMENT,\n"
        ")\n",
        encoding="utf-8",
    )
    return links


def _macos_linker() -> Path:
    selected = subprocess.run(["xcrun", "--find", "ld"], capture_output=True, text=True, check=True).stdout.strip()
    path = Path(selected)
    if not path.is_absolute() or not _probe(path).is_file():
        raise BazelWorkspaceError(f"selected macOS linker is missing: {selected}")
    return path.resolve(strict=True)


def _windows_mt(environment: Mapping[str, str] | None = None) -> Path:
    environment = os.environ if environment is None else environment
    sdk = Path(environment.get("WindowsSdkDir", ""))
    version = environment.get("WindowsSDKVersion", "").rstrip("\\").rstrip("/")
    tool = sdk / "bin" / version / "x64/mt.exe"
    if not sdk.is_absolute() or not version or not tool.is_file():
        raise BazelWorkspaceError(f"selected Windows SDK manifest tool is missing: {tool}")
    return tool


def _toolchain_identity(generated: Path, host: str, *, compiler_environment: Mapping[str, str] | None = None) -> None:
    files: set[Path] = set()
    context: dict[str, str] = {"platform": host, "machine": _host_machine()}

    def tree(directory: Path) -> None:
        if not directory.is_absolute() or not _probe(directory).is_dir():
            raise BazelWorkspaceError(f"native toolchain input directory is missing: {directory}")
        files.update(path for path in directory.rglob("*") if path.is_file())

    if host == "macos":
        llvm = Path(os.environ["AOBUS_LLVM_ROOT"])
        executables = [llvm / "bin" / name for name in ("clang", "clang++", "llvm-ar", "llvm-libtool-darwin")]
        for path in executables:
            if not path.is_file():
                raise BazelWorkspaceError(f"native toolchain input is missing: {path}")
            files.add(path)
        linker = _macos_linker()
        files.add(linker)
        # Bind the SDK selected by the same xcrun installation as the explicit
        # linker. SDK/version markers alone miss in-place header/stub changes.
        selected = subprocess.run(
            ["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True
        ).stdout.strip()
        sdk = Path(selected).resolve(strict=True)
        settings = [sdk / name for name in ("SDKSettings.json", "SDKSettings.plist")]
        if not any(_probe(path).is_file() for path in settings):
            raise BazelWorkspaceError(f"macOS SDK identity is missing: {sdk}")
        files.update(path for path in settings if _probe(path).is_file())
        tree(sdk / "usr/include")
        local_include = sdk / "usr/local/include"
        if _probe(local_include).is_dir():
            tree(local_include)
        for directory in (sdk / "usr/lib", sdk / "System/Library", sdk / "Library/Frameworks"):
            if _probe(directory).is_dir():
                files.update(
                    path
                    for path in directory.rglob("*")
                    if path.is_file()
                    and (
                        path.suffix in {".tbd", ".modulemap", ".apinotes"}
                        or {"Headers", "PrivateHeaders"}.intersection(path.relative_to(directory).parts)
                    )
                )
        tree(llvm / "include/c++/v1")
        resource = subprocess.run(
            [str(llvm / "bin/clang++"), "--no-default-config", "-print-resource-dir"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        tree(Path(resource))
        # The driver passes this plugin explicitly even without -flto.
        lto = llvm / "lib/libLTO.dylib"
        if not _probe(lto).is_file():
            raise BazelWorkspaceError(f"native compiler runtime is missing: {lto}")
        files.add(lto)
        tree(Path(os.environ["AOBUS_LIBCXX_EXPECTED_SHIM"]))
        context["deployment"] = _json(PROJECT_ROOT / "script/ao/macos-toolchain.json")["deploymentTarget"]
    elif host == "windows":
        environment = os.environ if compiler_environment is None else compiler_environment
        files.add(_windows_mt(environment))
        tool_directories = set()
        for name in ("cl.exe", "link.exe", "lib.exe", "ml64.exe"):
            path = Path(_executable(name, path=environment["PATH"]))
            files.add(path)
            tool_directories.add(path.parent)
        for directory in tool_directories:
            tree(directory)
        for name in ("INCLUDE", "LIB"):
            for value in environment[name].split(";"):
                if value:
                    tree(Path(value))
        for name in ("VCToolsVersion", "WindowsSDKVersion", "UCRTVersion"):
            context[name] = environment.get(name, "")
        sdk_root = Path(environment.get("WindowsSdkDir", ""))
        version = context["WindowsSDKVersion"].rstrip("\\").rstrip("/")
        for relative in ("um/Windows.h", "ucrt/corecrt.h"):
            marker = sdk_root / "Include" / version / relative
            if not marker.is_file():
                raise BazelWorkspaceError(f"Windows SDK identity is missing: {marker}")
            files.add(marker)
    else:
        wrapper = _linux_cc_wrapper()
        tools = wrapper.get("tools", {})
        if not isinstance(tools, dict) or not tools:
            raise BazelWorkspaceError("Nix compiler tools are missing; re-enter the pinned shell")
        files.update(Path(value) for value in tools.values())
        context["wrapper_suffix"] = wrapper["suffixSalt"]
        context["builtin_roots"] = json.dumps(wrapper.get("builtinRoots", []), sort_keys=True)
    identity = {
        "schema": 1,
        "context": context,
        "files": [
            {"path": str(path), "realpath": str(path.resolve()), "sha256": _sha256(path)} for path in sorted(files)
        ],
    }
    _write_json(generated / "toolchain/identity.json", identity)


def _linux_cc_wrapper() -> dict[str, Any]:
    """Read compiler metadata from the same pinned Nix report as dependencies."""
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise BazelWorkspaceError("Nix hardening policy is missing; enter the pinned shell through ./ao")
    report = _json(report_path)
    pin = _json(PROJECT_ROOT / "nixpkgs.json")
    wrapper = report.get("ccWrapper", {})
    if not isinstance(wrapper, dict):
        raise BazelWorkspaceError("Nix hardening policy does not match the pinned shell; re-enter through ./ao")
    suffix = wrapper.get("suffixSalt")
    flags = wrapper.get("hardeningEnable")
    if (
        report.get("schemaVersion") != 1
        or report.get("nixpkgsRevision") != pin["rev"]
        or not isinstance(suffix, str)
        or not re.fullmatch(r"[A-Za-z0-9_]+", suffix)
        or not isinstance(flags, list)
        or any(not isinstance(flag, str) or not re.fullmatch(r"[a-z][a-z0-9]*", flag) for flag in flags)
    ):
        raise BazelWorkspaceError("Nix hardening policy does not match the pinned shell; re-enter through ./ao")
    return wrapper


def linux_hardening_environment(config: str) -> dict[str, str]:
    """Describe a mode's Nix policy for toolchain generation and verification."""
    wrapper = _linux_cc_wrapper()
    suffix, flags = wrapper["suffixSalt"], wrapper["hardeningEnable"]
    if config not in {"debug", "release"}:
        raise BazelWorkspaceError(f"unsupported hardening configuration: {config}")
    # Fortify makes the Nix wrapper inject -O2 and requires optimized libc
    # headers. Keep the development Debug contract explicitly unoptimized.
    selected = flags if config == "release" else [flag for flag in flags if flag not in {"fortify", "fortify3"}]
    return {"NIX_HARDENING_ENABLE_" + suffix: " ".join(selected)}


def _git_bash() -> Path | None:
    """Return the MSYS bash that Git for Windows installs beside its git executable."""
    if git := shutil.which("git"):
        for root in Path(git).resolve().parents[:3]:
            if (bash := root / "usr/bin/bash.exe").is_file():
                return bash
    return None


def prepare_workspace(
    tree: Path,
    config: str,
    *,
    prefix: str | None = None,
    lint_configuration: Path | None = None,
    lint_consumers: Path | None = None,
) -> Path:
    """Write launch configuration, not a dependency snapshot or freshness stamp."""
    host = native_platform()
    prepared = tree / "configuration"
    prepared.mkdir(parents=True, exist_ok=True)
    request = {
        "schema": 1,
        "host": host,
        "config": config,
        "prefix": None if host == "linux" else str(vcpkg_prefix(prefix, tree)),
        # The WinUI quality probe needs the original checkout, not Bazel's execution root or a compiler view.
        "source": _source_root(),
    }
    environment = child_environment()
    # os.environ is case-insensitive on Windows, but dict(os.environ) is not:
    # enumeration uppercases SDK/toolset names such as WindowsSdkDir.
    lookup = {key.casefold() if host == "windows" else key: value for key, value in environment.items()}
    if host == "windows":
        # Capture the original selected environment before the repository rule
        # replaces resolver scratch TMP. The watched request is the only channel.
        request["compiler_environment"] = {
            name: lookup.get(name.casefold(), "")
            for name in (
                "INCLUDE",
                "LIB",
                "PATH",
                "TMP",
                "VCToolsInstallDir",
                "WindowsSdkDir",
                "WindowsSDKVersion",
                "VCToolsVersion",
                "UCRTVersion",
                "NETFXSDKDir",
            )
        }
    _write_json(prepared / "request.json", request)
    selected = {
        "AOBUS_BAZEL_CONFIGURATION": (prepared / "request.json").as_posix(),
        "AOBUS_BAZEL_PYTHON": Path(sys.executable).absolute().as_posix(),
    }
    for name in (
        "PATH",
        "PATHEXT",
        "SYSTEMROOT",
        "SYSTEMDRIVE",
        "COMSPEC",
        "PROCESSOR_ARCHITECTURE",
        "PROCESSOR_ARCHITEW6432",
        "USERPROFILE",
        "HOMEDRIVE",
        "HOMEPATH",
        "HOME",
        "LOCALAPPDATA",
        "APPDATA",
        "AOBUS_STATE_ROOT",
        "XDG_CACHE_HOME",
        "WINDIR",
        "AOBUS_NIX_DEPENDENCY_REPORT",
        "AOBUS_NUGET_PACKAGES_DIR",
        "PKG_CONFIG_PATH",
        "PKG_CONFIG_LIBDIR",
        "AOBUS_LLVM_ROOT",
        "AOBUS_LLVM_SDK_ROOT",
        "AOBUS_LLVM_SDK_CACHE_ROOT",
        "AOBUS_LINT_LLVM_CONFIG",
        "AOBUS_LINT_CLANG_TIDY",
        "AOBUS_LINT_CLANG",
        "AOBUS_LINT_CLANGXX",
        "AOBUS_LINT_XCRUN",
        "AOBUS_LINT_CLANG_ROOTS",
        "VSINSTALLDIR",
        "AOBUS_LIBCXX_EXPECTED_SHIM",
        "DEVELOPER_DIR",
        "SDKROOT",
        "VCToolsVersion",
        "VCToolsInstallDir",
        "WindowsSdkDir",
        "WindowsSDKVersion",
        "UCRTVersion",
        "BAZEL_VC",
        "BAZEL_VC_FULL_VERSION",
        "BAZEL_SH",
    ):
        key = name.casefold() if host == "windows" else name
        if key in lookup:
            selected[name] = lookup[key]
    # Bazel's Windows shell toolchain needs a bash, which the checkout's Git
    # for Windows provides unless the caller selects another one.
    if host == "windows" and "BAZEL_SH" not in selected and (bash := _git_bash()):
        selected["BAZEL_SH"] = str(bash)
    if host == "windows" and "AOBUS_NUGET_PACKAGES_DIR" not in selected:
        from .bazel_provision import restored_nuget_packages

        # An explicit selection wins; otherwise use the managed cache once it is completely restored.
        if packages := restored_nuget_packages(environment):
            selected["AOBUS_NUGET_PACKAGES_DIR"] = str(packages)
    # This metadata selector is for ready SDK discovery, never compiler flags.
    # child_environment intentionally keeps it out of action execution.
    if roots := os.environ.get("NIXPKGS_CMAKE_PREFIX_PATH"):
        selected["NIXPKGS_CMAKE_PREFIX_PATH"] = roots
    if lint_configuration is not None:
        selected["AOBUS_BAZEL_LINT_CONFIGURATION"] = lint_configuration.absolute().as_posix()
    if lint_consumers is not None:
        selected["AOBUS_BAZEL_LINT_CONSUMERS"] = lint_consumers.absolute().as_posix()
    state = (
        Path(os.environ["AOBUS_STATE_ROOT"])
        if os.environ.get("AOBUS_STATE_ROOT")
        else (
            builddir.windows_state_root()
            if host == "windows"
            else Path.home() / "Library/Caches/Aobus"
            if host == "macos"
            else Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "Aobus"
        )
    )
    cache = state / "cache/bazel"
    # Downloads use Bazel's default content-addressed repository cache under the
    # shared output root; the repository rc keeps the contents cache disabled.
    lines = [
        "# Generated launch configuration; repository rules own dependency refresh.",
        "startup " + json.dumps("--output_user_root=" + (state / "bazel").as_posix(), ensure_ascii=False),
        "startup " + json.dumps("--output_base=" + (tree / "state/output").as_posix(), ensure_ascii=False),
        "startup --max_idle_secs=900",
        "common --experimental_install_base_gc_max_age=0",
        "build " + json.dumps("--config=" + config, ensure_ascii=False),
        "build " + json.dumps("--symlink_prefix=" + tree.as_posix() + "/bazel-", ensure_ascii=False),
        "build " + json.dumps("--disk_cache=" + cache.as_posix(), ensure_ascii=False),
        "build --experimental_disk_cache_gc_max_size=50G",
    ]
    lines.extend(
        "common " + json.dumps("--repo_env=" + key + "=" + value, ensure_ascii=False)
        for key, value in sorted(selected.items())
    )
    content = ("\n".join(lines) + "\n").encode("utf-8")
    rc = prepared / "aobus.bazelrc"
    if not rc.is_file() or rc.read_bytes() != content:
        rc.write_bytes(content)
    return prepared


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


def execute(
    executable: Path,
    tree: Path,
    generated: Path,
    action: str,
    config: str,
    targets: list[str],
    *,
    jobs: int = 2,
    remote_cache: str | None = None,
) -> int:
    require_supported_source()
    analysis = action in {"query", "cquery", "aquery"}
    argv = [str(executable), f"--bazelrc={generated / 'aobus.bazelrc'}", "fetch" if action == "deps" else action]
    if action not in {"query", "deps"}:
        argv.append(f"--jobs={jobs}")
    if action == "deps":
        argv.extend(
            f"--repo=@{name}"
            for name in ("aobus_native_deps", "aobus_test_deps", "aobus_native_tools", "aobus_toolchain")
        )
    if remote_cache:
        argv.append(f"--remote_cache={remote_cache}")
    default_targets = list(DEFAULT_TEST_TARGETS)
    effective_targets = targets
    if action != "deps":
        if not targets and action not in {"build", "test"}:
            raise BazelWorkspaceError(f"Bazel {action} requires an explicit expression or target")
        options_only = targets and all(value.startswith("-") for value in targets)
        effective_targets = (
            (default_targets + targets) if options_only and action in {"build", "test"} else targets or default_targets
        )
        argv.extend(effective_targets)
    if action == "run":
        # Interactive frontends must inherit the terminal, not pipes or the build log.
        print(f"Bazel run ({config})", flush=True, file=sys.stderr)
        return subprocess.run(argv, cwd=PROJECT_ROOT, env=child_environment(), check=False).returncode
    log = tree / "build.log"
    print(f"Bazel {action} ({config}); output: {log}", flush=True, file=sys.stderr if analysis else sys.stdout)
    if analysis:
        result = subprocess.run(argv, cwd=PROJECT_ROOT, env=child_environment(), capture_output=True)
        log.write_bytes(result.stdout + result.stderr)
        sys.stdout.write(result.stdout.decode("utf-8", errors="replace"))
        if result.returncode:
            sys.stderr.write(result.stderr.decode("utf-8", errors="replace"))
    else:
        with log.open("wb") as sink:
            result = subprocess.run(
                argv, cwd=PROJECT_ROOT, env=child_environment(), stdout=sink, stderr=subprocess.STDOUT
            )
    status = result.returncode
    if status and not analysis:
        print(log.read_text(encoding="utf-8", errors="replace"))
    return status
