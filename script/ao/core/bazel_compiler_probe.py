"""Admit native compiler SDK facts without choosing Bazel graph or mode policy."""

from __future__ import annotations

import json
import os
import platform
import re
import subprocess
from pathlib import Path
from typing import Any

from . import bazel_native as native
from . import bazel_providers as providers


def probe_compiler(host: str) -> dict[str, Any]:
    """Return admitted installed SDK observations and the existing input identity."""
    if host == "linux":
        sdk = _linux_sdk()
    elif host == "macos":
        sdk = _macos_sdk()
    else:
        raise native.BazelWorkspaceError(f"unsupported native toolchain platform: {host}")
    return {
        "platform": host,
        "sdk": sdk,
        "identity": _compiler_identity(host),
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
    xcrun = Path(native.executable("xcrun", path="/usr/bin"))
    sdk = subprocess.run([str(xcrun), "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()
    if not Path(sdk).is_dir():
        raise native.BazelWorkspaceError(f"macOS SDK is missing: {sdk}")
    developer_dir = Path(os.environ.get("DEVELOPER_DIR", ""))
    if not developer_dir.is_absolute() or not native.probe(developer_dir).is_dir():
        raise native.BazelWorkspaceError("selected macOS developer directory is missing")
    machine = platform.machine()
    if machine.lower() not in {"x86_64", "arm64"}:
        raise native.BazelWorkspaceError(f"unsupported macOS CPU: {machine}")
    linker = _macos_tool("ld", xcrun)
    tools = {
        "clang": llvm / "bin/clang",
        "clang++": llvm / "bin/clang++",
        "ar": llvm / "bin/llvm-libtool-darwin",
        "strip": llvm / "bin/llvm-strip",
        "ld": linker,
        "dsymutil": _macos_tool("dsymutil", xcrun),
        "apple_strip": _macos_tool("strip", xcrun),
        "xcrun": xcrun,
    }
    for tool in tools.values():
        if not native.probe(tool).is_file() or not os.access(tool, os.X_OK):
            raise native.BazelWorkspaceError(f"selected macOS compiler tool is missing or not executable: {tool}")
    llvm_version = subprocess.run(
        [str(llvm / "bin/clang++"), "--no-default-config", "-dumpversion"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", llvm_version):
        raise native.BazelWorkspaceError(f"selected Clang reported an invalid version: {llvm_version}")
    deployment = native.read_json(native.PROJECT_ROOT / "script/ao/macos-toolchain.json")["deploymentTarget"]
    return {
        "machine": machine,
        "llvm": str(llvm),
        "llvm_real": str(llvm.resolve()),
        "llvm_version": llvm_version,
        "sdk": str(Path(sdk).resolve()),
        "shim": str(shim),
        "linker": str(linker),
        "developer_dir": str(developer_dir),
        "deployment": deployment,
        "tools": {role: str(tool) for role, tool in tools.items()},
    }


def _macos_tool(name: str, xcrun: Path) -> Path:
    selected = subprocess.run([str(xcrun), "--find", name], capture_output=True, text=True, check=True).stdout.strip()
    path = Path(selected)
    if not path.is_absolute() or not native.probe(path).is_file():
        raise native.BazelWorkspaceError(f"selected macOS {name} is missing: {selected}")
    return path.resolve(strict=True)


def _compiler_identity(host: str) -> dict[str, Any]:
    files: set[Path] = set()
    context: dict[str, str] = {"platform": host, "machine": native.host_machine()}

    def tree(directory: Path) -> None:
        if not directory.is_absolute() or not native.probe(directory).is_dir():
            raise native.BazelWorkspaceError(f"native toolchain input directory is missing: {directory}")
        files.update(path for path in directory.rglob("*") if path.is_file())

    if host == "macos":
        llvm = Path(os.environ["AOBUS_LLVM_ROOT"])
        executables = [
            llvm / "bin" / name for name in ("clang", "clang++", "llvm-ar", "llvm-libtool-darwin", "llvm-strip")
        ]
        for path in executables:
            if not path.is_file():
                raise native.BazelWorkspaceError(f"native toolchain input is missing: {path}")
            files.add(path)
        xcrun = Path(native.executable("xcrun", path="/usr/bin"))
        files.add(xcrun)
        files.update(_macos_tool(name, xcrun) for name in ("ld", "dsymutil", "strip"))
        # Bind the SDK selected by the same xcrun installation as the explicit
        # linker and wrapper postprocessors, including their executable bytes.
        selected = subprocess.run(
            [str(xcrun), "--show-sdk-path"], capture_output=True, text=True, check=True
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
