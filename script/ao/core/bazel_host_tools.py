"""Validate installed host-tool SDK facts; Starlark owns repository projection."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import sysconfig
from pathlib import Path
from typing import Any

from . import bazel_native as native
from . import bazel_providers as providers
from . import dependency_policy


def _linux_gperf() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise native.BazelWorkspaceError("gperf requires the pinned Nix dependency report; re-enter through ./ao")
    report, pin = native.read_json(report_path), native.read_json(native.PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise native.BazelWorkspaceError("Nix dependency report does not match nixpkgs.json; re-enter through ./ao")
    root, version, derivation, outputs = providers.nix_dependency(report, "gperf")
    executable = root / "bin/gperf"
    if not executable.is_file() or providers.nix_root(executable) not in outputs:
        raise native.BazelWorkspaceError(f"gperf executable does not match the Nix report output: {executable}")
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
        [report_path, native.PROJECT_ROOT / "nixpkgs.json"],
    )


def _native_gperf(prefix: Path, host: str) -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    manifest = native.PROJECT_ROOT / "vcpkg.json"
    requests = [
        request
        for request in native.read_json(manifest)["dependencies"]
        if isinstance(request, dict) and request["name"] == "gperf"
    ]
    if len(requests) != 1 or requests[0].get("host") is not True:
        raise native.BazelWorkspaceError("gperf must be a host:true port in vcpkg.json")
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
        raise native.BazelWorkspaceError(f"ambiguous installed gperf host identity: {status}: {exc}") from exc
    if "gperf" not in packages:
        raise native.BazelWorkspaceError(f"gperf host port is not installed: {status}")
    package = packages["gperf"]
    evidence = [status, manifest, native.PROJECT_ROOT / "vcpkg-configuration.json"]
    if host == "windows" and (prefix.name != "x64-windows-static-md" or package.triplet != "x64-windows"):
        raise native.BazelWorkspaceError("Windows gperf requires the installed x64-windows host tool")
    if host not in {"windows", "macos"} or not re.fullmatch(r"[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)+", package.triplet):
        raise native.BazelWorkspaceError(f"unsupported gperf host layout: {host}: {package.triplet}")
    host_prefix = prefix.resolve().parent / package.triplet
    native.native_path(host_prefix, host_prefix)
    directory = host_prefix / "tools/gperf"
    native.native_path(directory, directory)
    executable = directory / ("gperf.exe" if host == "windows" else "gperf")
    native.native_path(executable, directory)
    if not executable.is_file():
        raise native.BazelWorkspaceError(f"installed gperf host executable is missing: {executable}")
    runtime = sorted(path for path in directory.iterdir() if path.suffix.lower() == ".dll")
    if host == "windows":
        info = prefix.resolve().parent / "vcpkg/info"
        ownership = native.native_path(info / f"gperf_{package.upstream_version}_{package.triplet}.list", info)
        if not ownership.is_file():
            raise native.BazelWorkspaceError(f"gperf host ownership manifest is missing: {ownership}")
        owned = ownership.read_text(encoding="utf-8").splitlines()
        if any(f"x64-windows/tools/gperf/{path.name}" not in owned for path in [executable, *runtime]):
            raise native.BazelWorkspaceError("unowned Windows gperf host-tool input")
        evidence.append(ownership)
    for path in runtime:
        native.native_path(path, directory)
        if host != "windows" or not path.is_file():
            raise native.BazelWorkspaceError(f"unsupported adjacent gperf runtime: {path}")
    return (
        {
            "provider": "vcpkg",
            "root": str(host_prefix),
            "triplet": package.triplet,
            "host_machine": native.host_machine(),
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
    report, pin = native.read_json(report_path), native.read_json(native.PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise native.BazelWorkspaceError("D-Bus test tool requires the selected pinned Nix report")
    _, version, derivation, outputs = providers.nix_dependency(report, "dbus")
    candidates = {path.resolve(): path for root in outputs if native.probe(path := root / "bin/dbus-daemon").is_file()}
    if len(candidates) != 1:
        raise native.BazelWorkspaceError("missing/ambiguous dbus-daemon in selected Nix outputs")
    resolved, executable = next(iter(candidates.items()))
    if providers.nix_root(resolved) not in outputs:
        raise native.BazelWorkspaceError("dbus-daemon escapes selected Nix outputs")
    runtime = sorted({path.resolve() for root in outputs for path in (root / "lib").glob("libdbus-1.so*")})
    if any(not path.is_file() or providers.nix_root(path) not in outputs for path in runtime):
        raise native.BazelWorkspaceError("D-Bus runtime escapes selected Nix outputs")
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
        [report_path, native.PROJECT_ROOT / "nixpkgs.json"],
    )


def _linux_gresource() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    """Select the resource compiler and its GLib runtime from report-owned outputs only."""
    if native.native_platform() != "linux":
        raise native.BazelWorkspaceError("GLib resource compiler requires the Linux Nix SDK")
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise native.BazelWorkspaceError("GLib resource compiler requires the pinned Nix dependency report")
    report, pin = native.read_json(report_path), native.read_json(native.PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise native.BazelWorkspaceError("GLib resource compiler requires the selected pinned Nix report")
    root, version, derivation, outputs = providers.nix_dependency(report, "glib")
    candidates = {
        path.resolve(): path
        for output in outputs
        if native.probe(path := output / "bin/glib-compile-resources").is_file()
    }
    if len(candidates) != 1:
        raise native.BazelWorkspaceError("missing/ambiguous glib-compile-resources in selected Nix outputs")
    resolved, executable = next(iter(candidates.items()))
    if providers.nix_root(resolved) not in outputs or not os.access(executable, os.X_OK):
        raise native.BazelWorkspaceError("glib-compile-resources escapes/is not executable in selected Nix outputs")
    runtime = []
    for name in ("gio", "glib", "gobject"):
        matches = {
            path.resolve()
            for output in outputs
            for path in (output / "lib").glob(f"lib{name}-2.0.so*")
            if re.fullmatch(r"lib" + name + r"-2\.0\.so(?:\.\d+)*", path.name)
        }
        if len(matches) != 1:
            raise native.BazelWorkspaceError(f"missing/ambiguous resource compiler GLib runtime: {name}")
        path = matches.pop()
        if not path.is_file() or providers.nix_root(path) not in outputs:
            raise native.BazelWorkspaceError("resource compiler runtime escapes selected GLib outputs")
        runtime.append(path)
    result = subprocess.run(
        [str(executable), "--version"],
        env=native.child_environment(),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode or result.stdout.strip() != version:
        raise native.BazelWorkspaceError(f"glib-compile-resources does not report GLib version {version}")
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
        [report_path, native.PROJECT_ROOT / "nixpkgs.json"],
    )


def _catalog_sdk(
    tree: Path, prefix: Path | None, host: str, *, watch_directories: set[Path] | None = None
) -> tuple[dict[str, str], list[Path], list[Path]]:
    """Installed tool locations plus explicit SDK bytes, not a hermetic runtime claim."""
    evidence = [native.PROJECT_ROOT / "dependency-contract.json"]
    if prefix is None:
        report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
        report, pin = native.read_json(report_path), native.read_json(native.PROJECT_ROOT / "nixpkgs.json")
        if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
            raise native.BazelWorkspaceError("catalog tools require the selected pinned Nix report")
        _, version, _, outputs = providers.nix_dependency(report, "icu")
        evidence.extend([report_path, native.PROJECT_ROOT / "nixpkgs.json"])
    else:
        if host == "windows" and prefix.resolve().name != "x64-windows-static-md":
            raise native.BazelWorkspaceError(
                "Windows catalog tools require the x64-windows-static-md ICU tools provider"
            )
        outputs = [prefix.resolve()]
        status = prefix.parent / "vcpkg/status"
        paragraphs = status.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n\n")
        selected = "\n\n".join(p for p in paragraphs if f"Architecture: {prefix.name}" in p.splitlines())
        package = dependency_policy.parse_vcpkg_status(selected).get("icu")
        if package is None or set(package.features) != {"core", "tools"}:
            raise native.BazelWorkspaceError("catalog tools require installed ICU core/tools features")
        version = package.upstream_version
        evidence.extend([status, native.PROJECT_ROOT / "vcpkg.json", native.PROJECT_ROOT / "vcpkg-configuration.json"])
        if host == "windows":
            info = prefix.resolve().parent / "vcpkg/info"
            ownership = native.native_path(info / f"icu_{package.upstream_version}_{package.triplet}.list", info)
            if not ownership.is_file():
                raise native.BazelWorkspaceError(f"ICU tool ownership manifest is missing: {ownership}")
            owned = ownership.read_text(encoding="utf-8").splitlines()
            tool_directory = "x64-windows-static-md/tools/icu/bin/"
            if any(tool_directory + name + ".exe" not in owned for name in ("genrb", "pkgdata")) or any(
                value.startswith(tool_directory) and value.lower().endswith(".dll") for value in owned
            ):
                raise native.BazelWorkspaceError("unowned/unsupported Windows static ICU tools manifest")
            evidence.append(ownership)
    contract = dependency_policy.load_contract()
    if not dependency_policy.effective_policy(contract, "icu", host).accepts(version):
        raise native.BazelWorkspaceError(f"ICU {version} does not satisfy dependency-contract.json")
    components = version.split(".")
    if len(components) not in {2, 3}:
        raise native.BazelWorkspaceError(f"unsupported ICU provider version format: {version}")
    include = native.header_root(outputs, "unicode/uvernum.h")
    header = include / "unicode/uvernum.h"
    if prefix is None:
        if providers.nix_root(header.resolve()) not in outputs:
            raise native.BazelWorkspaceError(f"ICU header escapes selected Nix outputs: {header}")
    else:
        native.native_path(header, prefix.resolve() / "include")
    macros = ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]
    if len(components) == 3:
        macros.append("U_ICU_VERSION_PATCHLEVEL_NUM")
    native.verify_header_version(header, version, macros)
    evidence.append(header)
    tools = {"python": str(Path(sys.executable).absolute())}
    artifacts = [Path(sys.executable)]
    watched_directories = watch_directories if watch_directories is not None else set()
    for name in ("genrb", "pkgdata"):
        candidates = (
            [root / "bin" / name for root in outputs]
            if prefix is None
            else [prefix / "tools/icu/bin" / (name + (".exe" if host == "windows" else ""))]
        )
        watched_directories.update(path.parent for path in candidates)
        matches = {path.resolve(): path for path in candidates if native.probe(path).is_file()}
        if len(matches) != 1:
            raise native.BazelWorkspaceError(f"missing/ambiguous selected ICU tool: {name}")
        resolved, executable = next(iter(matches.items()))
        if prefix is None:
            if providers.nix_root(resolved) not in outputs:
                raise native.BazelWorkspaceError(f"ICU tool escapes selected Nix outputs: {executable}")
        else:
            native.native_path(executable, prefix.resolve() / "tools/icu/bin")
        tools[name] = str(executable.absolute())
        artifacts.append(executable)
    # Original installed paths retain native loader adjacency/RPATH. Declare
    # the selected ICU runtime bytes as inputs without copying a whole prefix.
    directories = [root / "lib" for root in outputs] if prefix is None else [prefix / "tools/icu/bin"]
    watched_directories.update(directories)
    for directory in directories:
        if not native.probe(directory).is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if (prefix is None and (".so" in path.name or path.suffix == ".dylib")) or (
                prefix is not None and path.suffix.lower() == ".dll" and path.name.lower().startswith("icu")
            ):
                if prefix is None:
                    if providers.nix_root(path) not in outputs:
                        raise native.BazelWorkspaceError(f"ICU runtime escapes selected Nix outputs: {path}")
                else:
                    native.native_path(path, directory)
                if not path.is_file():
                    raise native.BazelWorkspaceError(f"missing ICU tool runtime: {path}")
                artifacts.append(path)
    if host == "windows":
        assert prefix is not None
        directory = prefix.resolve() / "tools/icu/bin"
        native.native_path(directory, directory)
        # This reviewed triplet installs static ICU tools, separately from its
        # target archives. System/CRT runtime remains trusted SDK state.
        if any(path.suffix.lower() == ".dll" for path in directory.iterdir()):
            raise native.BazelWorkspaceError(f"unsupported DLL-bearing Windows static ICU tool layout: {directory}")
    # ICU can print its valid version with usage and exit1 for -V. Match the
    # selected provider's version, as the existing CMake tool admission does.
    version_query = subprocess.run(
        [tools["genrb"], "-V"],
        env=native.child_environment(),
        capture_output=True,
        text=True,
        check=False,
    )
    reported = re.findall(r"\bICU version ([^\s)]+)", version_query.stdout + version_query.stderr)
    if reported != [version]:
        raise native.BazelWorkspaceError(f"genrb executable does not report ICU version {version}")
    stdlib = Path(sysconfig.get_path("stdlib"))
    extension_root = sysconfig.get_config_var("DESTSHARED")
    if extension_root is not None and not isinstance(extension_root, str):
        raise native.BazelWorkspaceError("invalid selected Python native-extension root")
    extensions = Path(sys.base_prefix) / "DLLs" if host == "windows" else Path(extension_root or stdlib / "lib-dynload")
    if not stdlib.is_dir():
        raise native.BazelWorkspaceError(f"selected Python standard library is missing: {stdlib}")
    pending = [stdlib]
    seen = set()
    while pending:
        directory = pending.pop()
        if directory.resolve() in seen:
            raise native.BazelWorkspaceError(f"Python standard library directory alias/cycle: {directory}")
        seen.add(directory.resolve())
        watched_directories.add(directory)
        for path in sorted(directory.iterdir()):
            if path.name in {"site-packages", "dist-packages", "__pycache__"}:
                continue
            if path.is_dir():
                pending.append(path)
            elif path.suffix == ".py":
                artifacts.append(path)
    watched_directories.add(extensions)
    if extensions.is_dir():
        artifacts.extend(
            path for path in sorted(extensions.iterdir()) if path.is_file() and path.suffix in {".so", ".pyd", ".dll"}
        )
    if host == "windows":
        watched_directories.add(Path(sys.base_prefix))
        artifacts.extend(path for path in sorted(Path(sys.base_prefix).glob("*.dll")) if path.is_file())
        system_root = os.environ.get("SYSTEMROOT") or os.environ.get("SystemRoot")
        if not system_root or not Path(system_root).is_absolute():
            raise native.BazelWorkspaceError("catalog actions require the selected Windows SystemRoot")
        tools["SystemRoot"] = system_root
        if user_profile := os.environ.get("USERPROFILE"):
            if not Path(user_profile).is_absolute():
                raise native.BazelWorkspaceError("native Python USERPROFILE must be an absolute path")
            tools["UserProfile"] = user_profile
    else:
        libdir, library = sysconfig.get_config_var("LIBDIR"), sysconfig.get_config_var("LDLIBRARY")
        if (
            isinstance(libdir, str)
            and isinstance(library, str)
            and libdir
            and library
            and (path := native.probe(Path(libdir) / library)).is_file()
        ):
            artifacts.append(path)
        framework_name = sysconfig.get_config_var("PYTHONFRAMEWORK")
        if host == "macos" and framework_name:
            if not isinstance(framework_name, str):
                raise native.BazelWorkspaceError("invalid selected Python framework runtime name")
            framework = native.probe(Path(sys.base_prefix) / framework_name)
            if not framework.is_file():
                raise native.BazelWorkspaceError(f"selected Python framework runtime is missing: {framework}")
            artifacts.append(framework)
    identity_path = tree / "catalog-sdk.json"
    native.write_json(
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


def _app_version(root: Path) -> str:
    # Text projection of the sole project declaration, not CMake evaluation.
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    text = re.sub(r"#\[\[.*?\]\]", "", text, flags=re.DOTALL)
    text = re.sub(r"#[^\n]*", "", text)
    declarations = re.findall(r"\bproject\s*\(([^)]*)\)", text, flags=re.IGNORECASE)
    if len(declarations) != 1:
        raise ValueError("expected one project(aobus VERSION x.y.z) declaration")
    match = re.fullmatch(r"\s*aobus\s+VERSION\s+(\d+\.\d+\.\d+)\s*", declarations[0])
    if match is None:
        raise ValueError("expected project(aobus VERSION x.y.z)")
    return match[1]


def _paths(paths: list[Path]) -> list[str]:
    # Keep lexical basenames and selected native loader paths. Starlark resolves
    # each symlink source independently when materializing the selected bytes.
    return [str(path.absolute()) for path in paths]


def resolve_host_tools(tree: Path, prefix: Path | None, host: str, *, source: object = None) -> dict[str, Any]:
    """Return native SDK facts and read inventory, never BUILD or .bzl programs.

    Catalog Debug/Release library imports are resolved separately by the caller.
    The only generated SDK file is catalog-sdk.json, a diagnostic action input.
    """
    identity, executable, runtime, evidence = _linux_gperf() if prefix is None else _native_gperf(prefix, host)
    version = subprocess.run(
        [str(executable), "--version"], env=native.child_environment(), capture_output=True, text=True, check=True
    )
    if version.stdout.splitlines()[:1] != [f"GNU gperf {identity['version']}"]:
        raise ValueError("gperf executable version differs from its provider")
    if len({path.name.casefold() for path in [executable, *runtime]}) != 1 + len(runtime):
        raise ValueError("ambiguous gperf runtime basenames")
    files = {path.absolute() for path in [*evidence, executable, *runtime]}
    files.update(path.resolve() for path in [executable, *runtime])
    directories = {executable.parent}
    result: dict[str, Any] = {
        "gperf": {
            "identity": identity,
            "executable": str(executable.absolute()),
            "runtime": _paths(runtime),
            "evidence": _paths(evidence),
        },
        "manifest_tools": {},
        "test_tools": {},
    }
    if host == "windows":
        from . import bazel_compiler_probe

        system = Path(os.environ.get("SYSTEMROOT", ""))
        powershell = native.probe(system / "System32/WindowsPowerShell/v1.0/powershell.exe")
        if not system.is_absolute() or not powershell.is_file():
            raise ValueError("selected Windows PowerShell runtime is missing")
        for name, tool in {"mt": bazel_compiler_probe.windows_mt(), "powershell": powershell}.items():
            result["manifest_tools"][name] = tool.resolve().as_posix()
            files.update({tool.absolute(), tool.resolve()})
    tools, artifacts, sdk_evidence = _catalog_sdk(tree, prefix, host, watch_directories=directories)
    result["catalog"] = {"tools": tools, "artifacts": _paths(artifacts), "evidence": _paths(sdk_evidence)}
    files.update(path.absolute() for path in [*artifacts, *sdk_evidence])
    files.update(path.resolve() for path in artifacts)
    directories.update(Path(tools[name]).parent for name in ("python", "genrb", "pkgdata"))
    # The WinUI quality probe embeds the original checkout, not an execution root.
    if source is not None:
        if not isinstance(source, str) or not source or not Path(source).is_absolute() or not Path(source).is_dir():
            raise ValueError("prepared request source must be an existing absolute checkout root")
        result["test_tools"]["source_root"] = source
    if host == "linux":
        resource_identity, compiler, resource_runtime, resource_evidence = _linux_gresource()
        result["resource"] = {
            "identity": resource_identity,
            "executable": str(compiler.absolute()),
            "runtime": _paths(resource_runtime),
            "evidence": _paths(resource_evidence),
        }
        dbus_identity, daemon, dbus_runtime, dbus_evidence = _linux_dbus()
        result["test_tools"].update({"dbus_daemon": str(daemon), "dbus_daemon_directory": str(daemon.parent)})
        result["dbus"] = {"identity": dbus_identity, "artifacts": _paths([daemon, *dbus_runtime, *dbus_evidence])}
        for path in [compiler, *resource_runtime, *resource_evidence, daemon, *dbus_runtime, *dbus_evidence]:
            files.update({path.absolute(), path.resolve()})
        directories.update({compiler.parent, daemon.parent})
        for output in [*resource_identity["outputs"], *dbus_identity["outputs"]]:
            directories.update({Path(output) / "bin", Path(output) / "lib"})
    app_version_path = native.PROJECT_ROOT / "CMakeLists.txt"
    result["app_version"] = _app_version(native.PROJECT_ROOT)
    files.add(app_version_path.absolute())
    result["watch_files"] = sorted(str(path.absolute()) for path in files)
    result["watch_directories"] = sorted({str(path.absolute()) for path in directories})
    return result
