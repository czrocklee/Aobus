"""Admit native compiler SDK facts without choosing Bazel graph or mode policy."""

from __future__ import annotations

import json
import os
import platform
import re
import subprocess
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from . import bazel_native as native
from . import bazel_providers as providers


def probe_compiler(host: str, *, compiler_environment: Mapping[str, str] | None = None) -> dict[str, Any]:
    """Return admitted installed SDK observations and the existing input identity."""
    if host == "linux":
        sdk = _linux_sdk()
    elif host == "macos":
        sdk = _macos_sdk()
    elif host == "windows":
        sdk = _windows_sdk(compiler_environment)
    else:
        raise native.BazelWorkspaceError(f"unsupported native toolchain platform: {host}")
    return {
        "platform": host,
        "sdk": sdk,
        "identity": _compiler_identity(host, compiler_environment=compiler_environment),
    }


def _linux_sdk() -> dict[str, Any]:
    """Observe the pinned Nix SDK's bounded built-ins and selected tools."""
    wrapper = nix_cc_wrapper()
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
        raise native.BazelWorkspaceError("Nix compiler SDK metadata is missing; re-enter the pinned shell")
    roots = []
    for value in declared_roots:
        root = Path(value).resolve()
        if not root.is_dir() or root != providers.nix_root(root):
            raise native.BazelWorkspaceError(f"invalid Nix compiler built-in root: {value}")
        roots.append(root)
    for value in tools.values():
        source = Path(value)
        if not source.is_absolute() or not source.is_file() or not os.access(source, os.X_OK):
            raise native.BazelWorkspaceError(f"Nix compiler tool is missing or not executable: {value}")
        providers.nix_root(source)
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
        raise native.BazelWorkspaceError("Nix compiler did not report its built-in include directories")
    directories = [
        line.strip() for line in probe.stderr.split(start, 1)[1].split(end, 1)[0].splitlines() if line.strip()
    ]
    if not directories:
        raise native.BazelWorkspaceError("Nix compiler reported an empty built-in include interface")
    for value in directories:
        directory = Path(value)
        if (
            not directory.is_absolute()
            or not directory.is_dir()
            or not any(directory.resolve().is_relative_to(root) for root in roots)
        ):
            raise native.BazelWorkspaceError(f"compiler include is outside its declared Nix SDK roots: {value}")
    machine = native.host_machine()
    if machine.lower() not in {"x86_64", "aarch64"}:
        raise native.BazelWorkspaceError(f"unsupported Linux compiler CPU: {machine}")
    return {
        "machine": machine,
        "tools": dict(tools),
        "builtin_include_directories": directories,
        "nix_cc_wrapper": {
            "suffixSalt": wrapper["suffixSalt"],
            "hardeningEnable": wrapper["hardeningEnable"].copy(),
        },
    }


def _macos_sdk() -> dict[str, Any]:
    llvm = Path(os.environ.get("AOBUS_LLVM_ROOT", ""))
    shim = Path(os.environ.get("AOBUS_LIBCXX_EXPECTED_SHIM", ""))
    if not llvm.is_absolute() or not (llvm / "bin/clang++").is_file() or not shim.is_absolute():
        raise native.BazelWorkspaceError("native LLVM/shim is missing; invoke through ./ao on macOS")
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()
    if not Path(sdk).is_dir():
        raise native.BazelWorkspaceError(f"macOS SDK is missing: {sdk}")
    machine = platform.machine()
    if machine.lower() not in {"x86_64", "arm64"}:
        raise native.BazelWorkspaceError(f"unsupported macOS CPU: {machine}")
    linker = _macos_linker()
    deployment = native.read_json(native.PROJECT_ROOT / "script/ao/macos-toolchain.json")["deploymentTarget"]
    return {
        "machine": machine,
        "llvm": str(llvm),
        "llvm_real": str(llvm.resolve()),
        "sdk": str(Path(sdk).resolve()),
        "shim": str(shim),
        "linker": str(linker),
        "deployment": deployment,
    }


def _windows_sdk(compiler_environment: object) -> dict[str, Any]:
    """Observe the selected local MSVC SDK, not a relocatable compiler installation."""
    required = {"INCLUDE", "LIB", "PATH", "TMP", "VCToolsInstallDir", "WindowsSdkDir", "WindowsSDKVersion"}
    if not isinstance(compiler_environment, dict) or any(
        not isinstance(compiler_environment.get(name), str) or not compiler_environment[name].strip()
        for name in required
    ):
        raise native.BazelWorkspaceError(
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
        or not native.probe(vc).is_dir()
        or not native.probe(sdk).is_dir()
    ):
        raise native.BazelWorkspaceError("selected Windows VC/SDK roots or SDK version are invalid")
    if not Path(environment["TMP"]).is_absolute() or not native.probe(Path(environment["TMP"])).is_dir():
        raise native.BazelWorkspaceError("selected Windows compiler TMP is not an existing absolute directory")
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
        raise native.BazelWorkspaceError("selected NETFXSDKDir metadata must be a string")
    if netfx_value:
        netfx = Path(netfx_value)
        if (
            not netfx.is_absolute()
            or netfx.parent != sdk.parent / "NETFXSDK"
            or not native.probe(netfx).is_dir()
            or netfx.resolve() != netfx.absolute()
            or netfx.is_relative_to(native.PROJECT_ROOT)
        ):
            raise native.BazelWorkspaceError(
                "selected NETFXSDKDir must be an installed version root under WindowsSdkDir.parent/NETFXSDK"
            )
        include_roots.append(netfx / "include/um")
        lib_roots.append(netfx / "lib/um/x64")
    includes = []
    for name, allowed in (("INCLUDE", include_roots), ("LIB", lib_roots)):
        directories = [Path(value) for value in environment[name].split(";") if value]
        if not directories:
            raise native.BazelWorkspaceError(f"selected Windows {name} has no SDK directories")
        for directory in directories:
            if (
                not directory.is_absolute()
                or not native.probe(directory).is_dir()
                or directory not in allowed
                or directory.resolve() != directory.absolute()
            ):
                raise native.BazelWorkspaceError(
                    f"Windows {name} is not an exact selected VC/SDK directory: {directory}"
                )
        if name == "INCLUDE":
            includes = directories
    tools: dict[str, str] = {}
    tool_directory = vc / "bin/Hostx64/x64"
    if tool_directory.resolve() != tool_directory.absolute():
        raise native.BazelWorkspaceError("selected MSVC tool directory must retain its installed SDK location")
    for role in ("cl", "link", "lib", "ml64"):
        # Use the existing PATH selection/probes, then reject tools from another
        # installation. Execute installed tools; projected bytes are inputs only.
        source = Path(native.executable(role + ".exe", path=environment["PATH"]))
        if (
            not source.is_absolute()
            or source.parent != tool_directory
            or source.resolve() != source.absolute()
            or not native.probe(source).is_file()
        ):
            raise native.BazelWorkspaceError(
                f"selected {role}.exe is outside VCToolsInstallDir bin/Hostx64/x64: {source}"
            )
        tools[role] = source.as_posix()
    return {
        "target_architecture": tool_directory.name,
        "tools": tools,
        "builtin_include_directories": [directory.as_posix() for directory in includes],
        "environment": {name: environment[name] for name in ("INCLUDE", "LIB", "PATH", "TMP")},
    }


def _macos_linker() -> Path:
    selected = subprocess.run(["xcrun", "--find", "ld"], capture_output=True, text=True, check=True).stdout.strip()
    path = Path(selected)
    if not path.is_absolute() or not native.probe(path).is_file():
        raise native.BazelWorkspaceError(f"selected macOS linker is missing: {selected}")
    return path.resolve(strict=True)


def windows_mt(environment: Mapping[str, str] | None = None) -> Path:
    environment = os.environ if environment is None else environment
    sdk = Path(environment.get("WindowsSdkDir", ""))
    version = environment.get("WindowsSDKVersion", "").rstrip("\\").rstrip("/")
    tool = sdk / "bin" / version / "x64/mt.exe"
    if not sdk.is_absolute() or not version or not tool.is_file():
        raise native.BazelWorkspaceError(f"selected Windows SDK manifest tool is missing: {tool}")
    return tool


def _compiler_identity(host: str, *, compiler_environment: Mapping[str, str] | None = None) -> dict[str, Any]:
    files: set[Path] = set()
    context: dict[str, str] = {"platform": host, "machine": native.host_machine()}

    def tree(directory: Path) -> None:
        if not directory.is_absolute() or not native.probe(directory).is_dir():
            raise native.BazelWorkspaceError(f"native toolchain input directory is missing: {directory}")
        files.update(path for path in directory.rglob("*") if path.is_file())

    if host == "macos":
        llvm = Path(os.environ["AOBUS_LLVM_ROOT"])
        executables = [llvm / "bin" / name for name in ("clang", "clang++", "llvm-ar", "llvm-libtool-darwin")]
        for path in executables:
            if not path.is_file():
                raise native.BazelWorkspaceError(f"native toolchain input is missing: {path}")
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
        if not any(native.probe(path).is_file() for path in settings):
            raise native.BazelWorkspaceError(f"macOS SDK identity is missing: {sdk}")
        files.update(path for path in settings if native.probe(path).is_file())
        tree(sdk / "usr/include")
        local_include = sdk / "usr/local/include"
        if native.probe(local_include).is_dir():
            tree(local_include)
        for directory in (sdk / "usr/lib", sdk / "System/Library", sdk / "Library/Frameworks"):
            if native.probe(directory).is_dir():
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
        if not native.probe(lto).is_file():
            raise native.BazelWorkspaceError(f"native compiler runtime is missing: {lto}")
        files.add(lto)
        tree(Path(os.environ["AOBUS_LIBCXX_EXPECTED_SHIM"]))
        context["deployment"] = native.read_json(native.PROJECT_ROOT / "script/ao/macos-toolchain.json")[
            "deploymentTarget"
        ]
    elif host == "windows":
        environment = os.environ if compiler_environment is None else compiler_environment
        files.add(windows_mt(environment))
        tool_directories = set()
        for name in ("cl.exe", "link.exe", "lib.exe", "ml64.exe"):
            path = Path(native.executable(name, path=environment["PATH"]))
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
                raise native.BazelWorkspaceError(f"Windows SDK identity is missing: {marker}")
            files.add(marker)
    else:
        wrapper = nix_cc_wrapper()
        tools = wrapper.get("tools", {})
        if not isinstance(tools, dict) or not tools:
            raise native.BazelWorkspaceError("Nix compiler tools are missing; re-enter the pinned shell")
        files.update(Path(value) for value in tools.values())
        context["wrapper_suffix"] = wrapper["suffixSalt"]
        context["builtin_roots"] = json.dumps(wrapper.get("builtinRoots", []), sort_keys=True)
    return {
        "schema": 1,
        "context": context,
        "files": [
            {"path": str(path), "realpath": str(path.resolve()), "sha256": native.sha256(path)}
            for path in sorted(files)
        ],
    }


def nix_cc_wrapper() -> dict[str, Any]:
    """Read validated raw compiler metadata, without constructing action environments."""
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise native.BazelWorkspaceError("Nix hardening policy is missing; enter the pinned shell through ./ao")
    report = native.read_json(report_path)
    pin = native.read_json(native.PROJECT_ROOT / "nixpkgs.json")
    wrapper = report.get("ccWrapper", {})
    if not isinstance(wrapper, dict):
        raise native.BazelWorkspaceError("Nix hardening policy does not match the pinned shell; re-enter through ./ao")
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
        raise native.BazelWorkspaceError("Nix hardening policy does not match the pinned shell; re-enter through ./ao")
    return wrapper
