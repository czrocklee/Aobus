"""Discover an already prepared, mode-neutral repository-rule host environment.

This helper never installs native SDKs, ports or Python tooling. Automatic native
SDK selection is host state, not a fully watched registry: after changing that
selection, run ``bazel fetch --force --configure``.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import stat
import subprocess
import sys
from collections.abc import Iterator, Mapping
from pathlib import Path, PureWindowsPath
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parents[3]
_NIX_STORE = Path("/nix/store")
ENVIRONMENT_KEYS = (
    "PATH",
    "PATHEXT",
    "SYSTEMROOT",
    "SYSTEMDRIVE",
    "COMSPEC",
    "WINDIR",
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
    "AOBUS_NIX_DEPENDENCY_REPORT",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_LIBDIR",
    "AOBUS_LLVM_ROOT",
    "AOBUS_LIBCXX_EXPECTED_SHIM",
    "AOBUS_VCPKG_TRIPLET",
    "DEVELOPER_DIR",
    "SDKROOT",
    "CC",
    "CXX",
    "MACOSX_DEPLOYMENT_TARGET",
    "AOBUS_NUGET_PACKAGES_DIR",
    "AOBUS_LLVM_SDK_ROOT",
    "AOBUS_LLVM_SDK_CACHE_ROOT",
    "AOBUS_LINT_LLVM_CONFIG",
    "AOBUS_LINT_CLANG_TIDY",
    "AOBUS_LINT_CLANG",
    "AOBUS_LINT_CLANGXX",
    "AOBUS_LINT_XCRUN",
    "AOBUS_LINT_CLANG_ROOTS",
    "NIXPKGS_CMAKE_PREFIX_PATH",
    "BAZEL_SH",
    "BAZEL_VC",
    "BAZEL_VC_FULL_VERSION",
    "BAZEL_WINSDK_FULL_VERSION",
    "VSINSTALLDIR",
    "VCToolsInstallDir",
    "WindowsSdkDir",
    "WindowsSDKVersion",
    "VCToolsVersion",
    "UCRTVersion",
    "NETFXSDKDir",
)
_COMPILER_KEYS = (
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
_INPUT_KEYS = (
    *ENVIRONMENT_KEYS,
    *_COMPILER_KEYS,
    "AOBUS_BAZEL_VCPKG_PREFIX",
    "VCPKG_ROOT",
    "ProgramFiles(x86)",
    "AOBUS_HOMEBREW",
    "AOBUS_PYTHON",
)


class EnvironmentError(RuntimeError):
    """A selected native input is unavailable or invalid; no fallback is allowed."""


class _Inventory:
    def __init__(self) -> None:
        self.files: set[str] = set()
        self.directories: set[str] = set()

    def probe(self, path: Path) -> None:
        path = path.absolute()
        self.files.update((str(path), str(path.resolve())))
        for parent in path.parents:
            try:
                linked = stat.S_ISLNK(parent.lstat().st_mode)
            except FileNotFoundError:
                continue
            if linked:
                self.files.update((str(parent), str(parent.resolve())))

    def require(self, path: Path, *, directory: bool = False) -> Path:
        self.probe(path)
        try:
            mode = path.stat().st_mode
        except FileNotFoundError:
            raise EnvironmentError(
                f"selected native input is missing: {path}; prepare it outside Bazel fetch"
            ) from None
        if not (stat.S_ISDIR(mode) if directory else stat.S_ISREG(mode)):
            raise EnvironmentError(f"selected native input has the wrong file type: {path}")
        if directory:
            self.directories.update((str(path.absolute()), str(path.resolve())))
        return path

    def optional_file(self, path: Path) -> bool:
        self.probe(path)
        try:
            path.stat()
        except FileNotFoundError:
            return False
        self.require(path)
        return True


def _mapping(value: object) -> dict[str, str]:
    if not isinstance(value, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in value.items()):
        raise EnvironmentError("environment must be a string mapping")
    if any("\x00" in k or "\x00" in v for k, v in value.items()):
        raise EnvironmentError("environment contains an invalid key or value")
    return value


def _filter(environment: Mapping[str, str], keys: tuple[str, ...], host: str) -> dict[str, str]:
    lookup = {key.casefold() if host == "windows" else key: value for key, value in environment.items()}
    return {
        key: lookup[key.casefold() if host == "windows" else key]
        for key in keys
        if (key.casefold() if host == "windows" else key) in lookup
    }


def _object(path: Path, inventory: _Inventory) -> dict[str, Any]:
    inventory.require(path)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, UnicodeError):
        raise EnvironmentError(f"invalid JSON input: {path}") from None
    if not isinstance(value, dict):
        raise EnvironmentError(f"expected a JSON object: {path}")
    return value


def _absolute(value: object, label: str) -> Path:
    if not isinstance(value, str) or not value or "\x00" in value or not Path(value).is_absolute():
        raise EnvironmentError(f"{label} must select an absolute native path")
    return Path(value)


@contextlib.contextmanager
def _selected_environment(environment: Mapping[str, str]) -> Iterator[None]:
    previous = dict(os.environ)
    os.environ.clear()
    os.environ.update(environment)
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(previous)


def _query(argv: list[str], environment: Mapping[str, str]) -> str:
    result = subprocess.run(argv, env=dict(environment), capture_output=True, text=True, check=False, timeout=60)
    if result.returncode:
        # Native tool output is never a diagnostic channel.
        raise EnvironmentError(f"native discovery failed: {Path(argv[0]).name}; check the selected SDK outside Bazel")
    return result.stdout.strip()


def windows_compiler_selection(environment: Mapping[str, str]) -> dict[str, str]:
    """Pin rules_cc to an initialized VS environment, rejecting explicit conflicts.

    Bare clients must export the returned selectors before starting Bazel. A
    repository helper's child environment cannot select rules_cc's repository.
    """
    lookup = {key.casefold(): value for key, value in environment.items()}
    for name in ("VSINSTALLDIR", "VCToolsVersion", "WindowsSDKVersion"):
        if not lookup.get(name.casefold(), "").strip().rstrip("\\/"):
            raise EnvironmentError(
                f"Windows Bazel requires initialized {name}; prepare the selected x64 VS environment"
            )
    visual_studio = lookup["vsinstalldir"]
    if not (PureWindowsPath(visual_studio).is_absolute() or Path(visual_studio).is_absolute()):
        raise EnvironmentError("VSINSTALLDIR must select an absolute native path")
    vc_version = lookup["vctoolsversion"].rstrip("\\/")
    sdk_version = lookup["windowssdkversion"].rstrip("\\/")
    if not re.fullmatch(r"\d+\.\d+\.\d+", vc_version):
        raise EnvironmentError("VCToolsVersion must name one full MSVC version")
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", sdk_version):
        raise EnvironmentError("WindowsSDKVersion must name one full Windows SDK version")
    selection = {
        "BAZEL_VC": (PureWindowsPath(visual_studio) / "VC").as_posix(),
        "BAZEL_VC_FULL_VERSION": vc_version,
        "BAZEL_WINSDK_FULL_VERSION": sdk_version,
    }
    for name, value in selection.items():
        explicit = lookup.get(name.casefold())
        if explicit is not None and (
            PureWindowsPath(explicit) != PureWindowsPath(value) if name == "BAZEL_VC" else explicit != value
        ):
            raise EnvironmentError(f"Explicit {name} disagrees with the selected Visual Studio environment")
    return selection


def _windows_compiler_selection(environment: dict[str, str], compiler: dict[str, str], inventory: _Inventory) -> None:
    selection = windows_compiler_selection({**environment, **compiler})
    vc_root = inventory.require(_absolute(selection["BAZEL_VC"], "BAZEL_VC"), directory=True)
    expected = vc_root / "Tools/MSVC" / selection["BAZEL_VC_FULL_VERSION"]
    if Path(compiler["VCToolsInstallDir"]).resolve() != expected.resolve():
        raise EnvironmentError("VCToolsInstallDir disagrees with BAZEL_VC/BAZEL_VC_FULL_VERSION")
    # Require the modern, exact-version upstream entrypoint rather than allowing
    # rules_cc to ignore selectors or fall back to another installed VC tree.
    inventory.require(vc_root / "Auxiliary/Build/vcvarsall.bat")


def _compiler_environment(value: object, inventory: _Inventory) -> dict[str, str]:
    selected = _filter(_mapping(value), _COMPILER_KEYS, "windows")
    for key in _COMPILER_KEYS[:8]:
        if not selected.get(key, "").strip():
            raise EnvironmentError(f"Windows compiler_environment requires {key}")
    for key in ("VCToolsInstallDir", "WindowsSdkDir", "TMP"):
        inventory.require(_absolute(selected[key], key), directory=True)
    version = selected["WindowsSDKVersion"].rstrip("\\/")
    if not version or version in {".", ".."} or re.search(r"[\\/]", version):
        raise EnvironmentError("WindowsSDKVersion must name one selected SDK version")
    vc, sdk = Path(selected["VCToolsInstallDir"]), Path(selected["WindowsSdkDir"])
    if vc.name != selected["VCToolsVersion"].rstrip("\\/"):
        raise EnvironmentError("VCToolsVersion disagrees with VCToolsInstallDir")
    ucrt_version = selected.get("UCRTVersion", "").rstrip("\\/")
    if ucrt_version and ucrt_version != version:
        raise EnvironmentError("UCRTVersion disagrees with WindowsSDKVersion")
    for key, required in (
        ("INCLUDE", (vc / "include", sdk / "Include" / version / "um", sdk / "Include" / version / "ucrt")),
        ("LIB", (vc / "lib/x64", sdk / "Lib" / version / "um/x64", sdk / "Lib" / version / "ucrt/x64")),
    ):
        directories = {
            inventory.require(_absolute(entry, key), directory=True).resolve()
            for entry in selected[key].split(";")
            if entry
        }
        if any(path.resolve() not in directories for path in required):
            raise EnvironmentError(
                f"Windows compiler {key} must include the selected x64 MSVC and Windows SDK directories"
            )
    compiler = vc / "bin/Hostx64/x64/cl.exe"
    for entry in selected["PATH"].split(";"):
        directory = _absolute(entry.strip('"'), "Windows compiler PATH entry")
        candidate = directory / "cl.exe"
        if inventory.optional_file(candidate):
            if candidate.resolve() != compiler.resolve():
                raise EnvironmentError(
                    "Windows compiler PATH must select VCToolsInstallDir/bin/Hostx64/x64/cl.exe first"
                )
            break
    else:
        raise EnvironmentError("Windows compiler PATH has no initialized x64 cl.exe")
    for role in ("cl", "link", "lib", "ml64", "dumpbin"):
        inventory.require(vc / "bin/Hostx64/x64" / (role + ".exe"))
    inventory.require(sdk / "bin" / version / "x64/mt.exe")
    inventory.require(sdk / "Include" / version / "um/Windows.h")
    inventory.require(sdk / "Include" / version / "ucrt/corecrt.h")
    for directory in (vc.parent, sdk / "Include", sdk / "Lib", sdk / "bin"):
        inventory.require(directory, directory=True)
    if [parent.name.casefold() for parent in vc.parents[:3]] == ["msvc", "tools", "vc"]:
        auxiliary = vc.parents[2] / "Auxiliary/Build"
        inventory.probe(auxiliary / "Microsoft.VCToolsVersion.default.txt")
        inventory.probe(vc.parents[3] / "Common7/Tools/VsDevCmd.bat")
    return selected


def _macos_environment(environment: dict[str, str], inventory: _Inventory) -> None:
    lock = _object(PROJECT_ROOT / "script/ao/macos-toolchain.json", inventory)
    inventory.require(PROJECT_ROOT / "script/ao/macos-vcpkg-bootstrap.sh")
    if lock.get("schemaVersion") != 1:
        raise EnvironmentError("unsupported macOS toolchain schema")
    llvm = inventory.require(_absolute(environment.get("AOBUS_LLVM_ROOT"), "AOBUS_LLVM_ROOT"), directory=True)
    compiler = inventory.require(llvm / "bin/clang++")
    if _query([str(compiler), "-dumpversion"], environment).split(".", 1)[0] != lock["homebrew"]["llvmMajorVersion"]:
        raise EnvironmentError("selected Clang does not match macos-toolchain.json")
    inventory.require(llvm / "include/c++/v1/__expected/expected.h")
    shim = _absolute(environment.get("AOBUS_LIBCXX_EXPECTED_SHIM"), "AOBUS_LIBCXX_EXPECTED_SHIM")
    inventory.require(shim / "__expected/expected.h")
    inventory.require(_absolute(environment.get("DEVELOPER_DIR"), "DEVELOPER_DIR"), directory=True)
    sdk = inventory.require(_absolute(environment.get("SDKROOT"), "SDKROOT"), directory=True)
    settings = [inventory.optional_file(sdk / name) for name in ("SDKSettings.json", "SDKSettings.plist")]
    if not any(settings):
        raise EnvironmentError("selected macOS SDK has no SDKSettings identity")
    for directory in ("usr/include", "usr/lib"):
        inventory.require(sdk / directory, directory=True)
    for key in ("AOBUS_HOMEBREW", "AOBUS_PYTHON"):
        if key in environment:
            inventory.require(_absolute(environment[key], key))
    inventory.probe(Path("/usr/bin/xcrun"))
    inventory.probe(Path("/var/db/xcode_select_link"))
    selected_sdk = _absolute(_query(["/usr/bin/xcrun", "--show-sdk-path"], environment), "xcrun SDK")
    if selected_sdk.resolve(strict=True) != sdk.resolve(strict=True):
        raise EnvironmentError("xcrun did not retain the explicitly selected SDKROOT; no fallback is allowed")
    inventory.require(_absolute(_query(["/usr/bin/xcrun", "--find", "ld"], environment), "macOS linker"))


def discover_environment(
    *, output: Path, request: Path | None = None, environment: Mapping[str, str] | None = None
) -> dict[str, Any]:
    """Return selected ready inputs; only repository-owned scratch may be created.

    An explicit environment replaces, rather than augments, the prepared child
    environment. Legacy request configuration is deliberately not interpreted.
    """
    host = {"linux": "linux", "darwin": "macos", "win32": "windows"}.get(sys.platform)
    if host is None:
        raise EnvironmentError("unsupported repository bootstrap host")
    supplied = _mapping(dict(os.environ if environment is None else environment))
    selected = _filter(supplied, _INPUT_KEYS, host)
    inventory = _Inventory()
    inventory.require(Path(__file__))
    policy = _object(PROJECT_ROOT / "script/ao/toolchain.json", inventory)
    expected = policy.get("python")
    if (
        policy.get("schemaVersion") != 1
        or not isinstance(expected, str)
        or not re.fullmatch(r"\d+\.\d+\.\d+", expected)
    ):
        raise EnvironmentError("invalid governed Python policy")
    components = 2 if host == "macos" else 3
    if tuple(sys.version_info[:components]) != tuple(map(int, expected.split(".")[:components])):
        raise EnvironmentError("selected Python does not match script/ao/toolchain.json")
    python = inventory.require(_absolute(sys.executable, "selected Python"))
    python_root = inventory.require(Path(sys.base_prefix), directory=True).resolve(strict=True)
    prepared = _object(request, inventory) if request is not None else {}
    source = PROJECT_ROOT
    if request is not None:
        if prepared.get("schema") != 1 or prepared.get("host") != host:
            raise EnvironmentError("legacy request does not match the native host/schema")
        source = _absolute(prepared.get("source"), "legacy request source")
        inventory.require(source, directory=True)
        if source.resolve(strict=True) != PROJECT_ROOT.resolve(strict=True):
            raise EnvironmentError("legacy request source differs from the helper checkout")
    scratch = output.absolute().parent / "_tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    compiler: dict[str, str] | None = None
    if host == "linux":
        if not python.resolve(strict=True).is_relative_to(_NIX_STORE):
            raise EnvironmentError("Linux requires the governed Nix Python, not an ambient interpreter")
        report_path = _absolute(selected.get("AOBUS_NIX_DEPENDENCY_REPORT"), "AOBUS_NIX_DEPENDENCY_REPORT")
        if not report_path.resolve(strict=True).is_relative_to(_NIX_STORE):
            raise EnvironmentError("Linux dependency report must belong to the selected Nix store")
        report = _object(report_path, inventory)
        pin = _object(PROJECT_ROOT / "nixpkgs.json", inventory)
        if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
            raise EnvironmentError("Nix dependency report does not match nixpkgs.json")
    elif host == "windows":
        # The original client tuple, not a child capture/derived export, selects
        # upstream rules_cc. Portal requests already pin it in their launch rc.
        selectors = ("BAZEL_VC", "BAZEL_VC_FULL_VERSION", "BAZEL_WINSDK_FULL_VERSION")
        portal = "compiler_environment" in prepared
        validate_selection = not portal or any(name in selected for name in selectors)
        if validate_selection:
            if any(not selected.get(name, "").strip() for name in selectors):
                raise EnvironmentError(
                    "Windows bare Bazel requires the complete original-client selector tuple: " + ", ".join(selectors)
                )
            # Reject absent or contradictory initialized metadata before probing
            # SDKs. Never initialize latest/current cl in a separate CMD child.
            windows_compiler_selection(
                selected if not portal else {**selected, **_mapping(prepared["compiler_environment"])}
            )
        if portal:
            compiler = _compiler_environment(prepared["compiler_environment"], inventory)
        else:
            selected["TMP"] = str(scratch)
            compiler = _compiler_environment(selected, inventory)
        if validate_selection:
            _windows_compiler_selection(selected, compiler, inventory)
        selected.update({key: value for key, value in compiler.items() if key in ENVIRONMENT_KEYS})
    else:
        _macos_environment(selected, inventory)
        mac_policy = _object(PROJECT_ROOT / "script/ao/macos-toolchain.json", inventory)
        if mac_policy["homebrew"]["pythonMajorMinorVersion"] != ".".join(expected.split(".")[:2]):
            raise EnvironmentError("macOS Python bootstrap and shared Python policy disagree")
    selected.update({key: str(scratch) for key in ("TMP", "TEMP", "TMPDIR")})
    prefix: Path | None = None
    if host != "linux":
        # Reuse the triplet/readiness owner without selecting a portal build tree.
        with _selected_environment(selected):
            sys.path.insert(0, str(PROJECT_ROOT / "script"))
            try:
                from ao.core import bazel_native as native
                from ao.core import bazel_providers as providers
            finally:
                sys.path.pop(0)
            native.require_supported_source()
            with native.capture_probes(inventory.probe):
                explicit = selected.get("AOBUS_BAZEL_VCPKG_PREFIX", prepared.get("prefix"))
                prefix = _absolute(explicit, "AOBUS_BAZEL_VCPKG_PREFIX or legacy request prefix")
                prefix = inventory.require(prefix, directory=True).resolve(strict=True)
                inventory.require(prefix / "include", directory=True)
                inventory.require(prefix.parent / "vcpkg/status")
                prefix = providers.vcpkg_prefix(str(prefix), None)
                if prefix.name != providers.vcpkg_triplets(host=host)[0]:
                    raise EnvironmentError("selected vcpkg prefix does not match the native target triplet")
                if host == "macos":
                    inventory.require(Path(native.executable("pkgconf")))
    for module in tuple(sys.modules.values()):
        filename = getattr(module, "__file__", None)
        if isinstance(filename, str):
            path = Path(filename).absolute()
            if path.is_relative_to(PROJECT_ROOT / "script/ao") or path.resolve().is_relative_to(python_root):
                inventory.probe(path)
                inventory.directories.add(str(path.parent))
    runtime = _filter(selected, ENVIRONMENT_KEYS, host)
    runtime.update({key: str(scratch) for key in ("TMP", "TEMP", "TMPDIR")})
    repository = output.absolute().parent.resolve()
    result: dict[str, Any] = {
        "schema": 1,
        "host": host,
        "python": str(python),
        "source": str(source),
        "prefix": str(prefix) if prefix is not None else None,
        "environment": runtime,
        "watch_files": sorted(path for path in inventory.files if not Path(path).resolve().is_relative_to(repository)),
        "watch_directories": sorted(
            path for path in inventory.directories if not Path(path).resolve().is_relative_to(repository)
        ),
    }
    if compiler is not None:
        result["compiler_environment"] = compiler
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--request", type=Path)
    parser.add_argument("--environment-json")
    args = parser.parse_args(argv)
    staged = args.output.with_name(args.output.name + ".tmp")
    try:
        args.output.unlink(missing_ok=True)
        try:
            environment = None if args.environment_json is None else _mapping(json.loads(args.environment_json))
        except (ValueError, UnicodeError):
            raise EnvironmentError("invalid explicit environment JSON") from None
        result = discover_environment(output=args.output, request=args.request, environment=environment)
        staged.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(staged, args.output)
        return 0
    except KeyboardInterrupt:
        print("Aobus host environment discovery interrupted", file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        # Never print exception arguments from native commands or JSON captures.
        detail = str(error) if isinstance(error, EnvironmentError) else type(error).__name__
        print(f"Aobus host environment discovery failed: {detail}", file=sys.stderr)
        return 1
    finally:
        staged.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
