"""Admit native compiler SDK facts without choosing Bazel graph or mode policy."""

from __future__ import annotations

import hashlib
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
    """Return admitted SDK selection and its platform-specific action metadata."""
    if host == "linux":
        sdk = _linux_sdk()
        identity = _compiler_identity(host)
    elif host == "macos":
        sdk = _macos_sdk()
        identity = {"schema": 2, "platform": host, "sdk": sdk}
    else:
        raise native.BazelWorkspaceError(f"unsupported native toolchain platform: {host}")
    return {"platform": host, "sdk": sdk, "identity": identity}


def _linux_sdk() -> dict[str, Any]:
    """Observe the pinned Nix SDK's bounded built-ins and selected tools."""
    wrapper = nix_cc_wrapper()
    tools = wrapper.get("tools")
    if not isinstance(tools, dict) or set(tools) != {"clang", "clang++", "ar", "strip", "objcopy", "gcov"}:
        raise native.BazelWorkspaceError("Nix compiler SDK metadata is missing; re-enter the pinned shell")
    for value in tools.values():
        source = Path(value)
        if not source.is_absolute() or not source.is_file() or not os.access(source, os.X_OK):
            raise native.BazelWorkspaceError(f"Nix compiler tool is missing or not executable: {value}")
        providers.nix_root(source)
    sanitizer_include = wrapper.get("sanitizerIncludeDirectory")
    if not isinstance(sanitizer_include, str):
        raise native.BazelWorkspaceError("Nix compiler sanitizer interface is missing; re-enter the pinned shell")
    probe = subprocess.run(
        [tools["clang++"], "-E", "-x", "c++", "-std=c++26", "-no-canonical-prefixes", "-", "-v"],
        input="",
        env={"PATH": ""},
        capture_output=True,
        text=True,
        check=True,
    )
    start, end = "#include <...> search starts here:", "End of search list."
    directories = [
        line.strip() for line in probe.stderr.split(start, 1)[1].split(end, 1)[0].splitlines() if line.strip()
    ]
    resource_probe = subprocess.run(
        [tools["clang++"], "-print-resource-dir"],
        env={"PATH": ""},
        capture_output=True,
        text=True,
        check=True,
    )
    resource = Path(resource_probe.stdout.strip())
    machine = native.host_machine()
    if machine.lower() not in {"x86_64", "aarch64"}:
        raise native.BazelWorkspaceError(f"unsupported Linux compiler CPU: {machine}")
    return {
        "machine": machine,
        "tools": dict(tools),
        "builtin_include_directories": directories,
        "compiler_resource_directory": str(resource),
        "sanitizer_include_directory": sanitizer_include,
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
    selected_sdk = Path(sdk).resolve(strict=True)
    settings = [native.probe(Path(sdk) / name).is_file() for name in ("SDKSettings.json", "SDKSettings.plist")]
    if not any(settings):
        raise native.BazelWorkspaceError(f"selected macOS SDK has no SDKSettings metadata: {selected_sdk}")
    sdk_version = subprocess.run(
        [str(xcrun), "--sdk", str(selected_sdk), "--show-sdk-version"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    sdk_build_version = subprocess.run(
        [str(xcrun), "--sdk", str(selected_sdk), "--show-sdk-build-version"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    liblto = llvm / "lib/libLTO.dylib"
    for path in (liblto, shim / "__expected/expected.h"):
        if not native.probe(path).is_file():
            raise native.BazelWorkspaceError(f"selected macOS compiler input is missing: {path}")
    deployment = native.read_json(native.PROJECT_ROOT / "script/ao/macos-toolchain.json")["deploymentTarget"]
    return {
        "machine": machine,
        "llvm": str(llvm),
        "llvm_real": str(llvm.resolve()),
        "llvm_version": llvm_version,
        "sdk": str(selected_sdk),
        "sdk_version": sdk_version,
        "sdk_build_version": sdk_build_version,
        "liblto": str(liblto),
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
    """Retain the Linux Nix compiler inventory used by coverage and concepts."""
    if host != "linux":
        raise native.BazelWorkspaceError(f"compiler byte inventory is Linux-only: {host}")
    context: dict[str, str] = {"platform": host, "machine": native.host_machine()}
    wrapper = nix_cc_wrapper()
    tools = wrapper.get("tools", {})
    if not isinstance(tools, dict) or not tools:
        raise native.BazelWorkspaceError("Nix compiler tools are missing; re-enter the pinned shell")
    files = {Path(value) for value in tools.values()}
    context["wrapper_suffix"] = wrapper["suffixSalt"]
    context["builtin_roots"] = json.dumps(wrapper.get("builtinRoots", []), sort_keys=True)
    digests: dict[Path, str] = {}
    records = []
    for path in sorted(files):
        resolved = path.resolve(strict=True)
        if resolved not in digests:
            with path.open("rb") as stream:
                digests[resolved] = hashlib.file_digest(stream, "sha256").hexdigest()
        records.append({"path": str(path), "realpath": str(resolved), "sha256": digests[resolved]})
    return {"schema": 1, "context": context, "files": records}


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
