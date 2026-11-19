"""Validate installed host-tool SDK facts; Starlark owns repository projection."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import sysconfig
from pathlib import Path
from typing import Any

from ao.core import bazel_native as native
from ao.policy import dependency_policy
from ao.portal import bazel_providers as providers


def _linux_gperf() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    report, report_path, revision = providers.nix_report()
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
            "nixpkgsRevision": revision,
        },
        executable,
        [],
        [report_path, native.PROJECT_ROOT / "nixpkgs.json"],
    )


def _native_gperf(prefix: Path, host: str) -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    manifest = native.PROJECT_ROOT / "vcpkg.json"
    status = prefix.resolve().parent / "vcpkg/status"
    # The host tool can live in a sibling triplet. Windows selects its reviewed
    # host role explicitly; other native layouts must remain unambiguous.
    paragraphs = status.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n\n")
    selected = "\n\n".join(
        p
        for p in paragraphs
        if "Package: gperf" in p.splitlines() and (host != "windows" or "Architecture: x64-windows" in p.splitlines())
    )
    packages = dependency_policy.parse_vcpkg_status(selected)
    if "gperf" not in packages:
        raise native.BazelWorkspaceError(f"gperf host port is not installed: {status}")
    package = packages["gperf"]
    evidence = [status, manifest, native.PROJECT_ROOT / "vcpkg-configuration.json"]
    if host == "windows" and (prefix.name != "x64-windows" or package.triplet != "x64-windows"):
        raise native.BazelWorkspaceError("Windows gperf requires the installed x64-windows host tool")
    if host not in {"windows", "macos"} or not re.fullmatch(r"[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)+", package.triplet):
        raise native.BazelWorkspaceError(f"unsupported gperf host layout: {host}: {package.triplet}")
    host_prefix = prefix.resolve().parent / package.triplet
    native.native_path(host_prefix)
    directory = host_prefix / "tools/gperf"
    native.native_path(directory)
    executable = directory / ("gperf.exe" if host == "windows" else "gperf")
    native.native_path(executable)
    if not executable.is_file():
        raise native.BazelWorkspaceError(f"installed gperf host executable is missing: {executable}")
    runtime = sorted(path for path in directory.iterdir() if path.suffix.lower() == ".dll")
    if host == "windows":
        info = prefix.resolve().parent / "vcpkg/info"
        ownership = native.native_path(info / f"gperf_{package.upstream_version}_{package.triplet}.list")
        if not ownership.is_file():
            raise native.BazelWorkspaceError(f"gperf host ownership manifest is missing: {ownership}")
        owned = ownership.read_text(encoding="utf-8").splitlines()
        if any(f"x64-windows/tools/gperf/{path.name}" not in owned for path in [executable, *runtime]):
            raise native.BazelWorkspaceError("unowned Windows gperf host-tool input")
        evidence.append(ownership)
    for path in runtime:
        native.native_path(path)
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
    report, report_path, revision = providers.nix_report()
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
            "nixpkgsRevision": revision,
            "runtime_contract": "Installed immutable Nix SDK; not full OS/runtime hermeticity",
        },
        executable,
        runtime,
        [report_path, native.PROJECT_ROOT / "nixpkgs.json"],
    )


def _linux_gresource() -> tuple[dict[str, Any], Path, list[Path], list[Path]]:
    """Select the resource compiler and its GLib runtime from report-owned outputs only."""
    report, report_path, revision = providers.nix_report()
    root, version, derivation, outputs = providers.nix_dependency(report, "glib")
    candidates = {
        path.resolve(): path
        for output in outputs
        if native.probe(path := output / "bin/glib-compile-resources").is_file()
    }
    if len(candidates) != 1:
        raise native.BazelWorkspaceError("missing/ambiguous glib-compile-resources in selected Nix outputs")
    executable = next(iter(candidates.values()))
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
        runtime.append(matches.pop())
    return (
        {
            "provider": "nix",
            "root": str(root),
            "version": version,
            "deriver": derivation,
            "outputs": [str(path) for path in outputs],
            "nixpkgsRevision": revision,
            "runtime_contract": "Installed immutable GLib SDK; not full OS/runtime hermeticity",
        },
        executable,
        sorted(runtime),
        [report_path, native.PROJECT_ROOT / "nixpkgs.json"],
    )


def _catalog_sdk(
    tree: Path,
    prefix: Path | None,
    host: str,
    *,
    python: dict[str, Any] | None = None,
    watch_directories: set[Path] | None = None,
) -> tuple[dict[str, str], list[Path], list[Path]]:
    """Installed tool locations plus explicit SDK bytes, not a hermetic runtime claim."""
    if watch_directories is None:
        watch_directories = set()
    evidence = [native.PROJECT_ROOT / "dependency-contract.json"]
    parsed_windows_inputs: dict[str, str] = {}
    if prefix is None:
        report, report_path, _ = providers.nix_report()
        _, version, _, outputs = providers.nix_dependency(report, "icu")
        evidence.extend([report_path, native.PROJECT_ROOT / "nixpkgs.json"])
    else:
        if host == "windows" and prefix.resolve().name != "x64-windows":
            raise native.BazelWorkspaceError("Windows catalog tools require the x64-windows ICU tools provider")
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
            ownership = native.native_path(info / f"icu_{package.upstream_version}_{package.triplet}.list")
            if not ownership.is_file():
                raise native.BazelWorkspaceError(f"ICU tool ownership manifest is missing: {ownership}")
            owned = ownership.read_text(encoding="utf-8").splitlines()
            tool_directory = "x64-windows/tools/icu/bin/"
            if any(tool_directory + name + ".exe" not in owned for name in ("genrb", "pkgdata")):
                raise native.BazelWorkspaceError("unowned Windows ICU tools manifest")
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
        native.native_path(header)
    macros = ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]
    if len(components) == 3:
        macros.append("U_ICU_VERSION_PATCHLEVEL_NUM")
    native.verify_header_version(header, version, macros)
    evidence.append(header)
    if python is None:
        python = _python_action_sdk(host, watch_directories)
    tools = dict(python["tools"])
    artifacts = list(python["artifacts"])
    for name in ("genrb", "pkgdata"):
        candidates = (
            [root / "bin" / name for root in outputs]
            if prefix is None
            else [prefix / "tools/icu/bin" / (name + (".exe" if host == "windows" else ""))]
        )
        watch_directories.update(path.parent for path in candidates)
        matches = {path.resolve(): path for path in candidates if native.probe(path).is_file()}
        if len(matches) != 1:
            raise native.BazelWorkspaceError(f"missing/ambiguous selected ICU tool: {name}")
        resolved, executable = next(iter(matches.items()))
        if prefix is None:
            if providers.nix_root(resolved) not in outputs:
                raise native.BazelWorkspaceError(f"ICU tool escapes selected Nix outputs: {executable}")
        else:
            native.native_path(executable)
        tools[name] = str(executable.absolute())
        artifacts.append(executable)
    # Original installed paths retain native loader adjacency/RPATH. Declare
    # the selected ICU runtime bytes as inputs without copying a whole prefix.
    directories = [root / "lib" for root in outputs] if prefix is None else [prefix / "tools/icu/bin"]
    watch_directories.update(directories)
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
                    native.native_path(path)
                if not path.is_file():
                    raise native.BazelWorkspaceError(f"missing ICU tool runtime: {path}")
                if host != "windows":
                    artifacts.append(path)
    if host == "windows":
        assert prefix is not None
        directory = prefix.resolve() / "tools/icu/bin"
        native.native_path(directory)
        from ao.portal.bazel_dependencies import _windows_binary_inventory, _windows_pe, _windows_runtime_closure

        inventory, manifests = _windows_binary_inventory(prefix.resolve(), {"icu": version})
        runtime = []
        for name in ("genrb", "pkgdata"):
            image = _windows_pe(Path(tools[name]), "release", dll=False)
            parsed_windows_inputs[tools[name]] = image["sha256"]
            for dependency in image["imports"]:
                key = (directory / dependency).relative_to(prefix.resolve()).as_posix().casefold()
                if key in inventory:
                    runtime.append(inventory[key])
                elif dependency not in image["crt"] and dependency not in {"kernel32.dll", "advapi32.dll", "ntdll.dll"}:
                    raise native.BazelWorkspaceError(f"unowned ICU host-tool runtime dependency: {name}: {dependency}")
        extra, observations = _windows_runtime_closure(runtime, directory, inventory, prefix.resolve(), "release")
        parsed_windows_inputs.update({value["path"]: value["sha256"] for value in observations.values()})
        selected_runtime = {path.resolve() for path in [*runtime, *extra]}
        physical = {native.native_path(path) for path in directory.iterdir() if path.suffix.lower() == ".dll"}
        if any(path.relative_to(prefix.resolve()).as_posix().casefold() not in inventory for path in physical):
            raise native.BazelWorkspaceError(f"unowned Windows ICU host-tool runtime layout: {directory}")
        artifacts.extend(sorted(selected_runtime))
        evidence.extend(manifests)
    # ICU can print its valid version with usage and exit1 for -V. Match the
    # selected provider's version, not the exit status.
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
    identity_path = tree / "catalog-sdk.json"
    native.write_json(
        identity_path,
        {
            **python["identity"],
            "icu_version": version,
            "tools": {name: value for name, value in tools.items() if name != "UserProfile"},
            **({"windows_binary_inputs": parsed_windows_inputs} if host == "windows" else {}),
        },
    )
    artifacts.append(identity_path)
    return tools, sorted(set(artifacts)), evidence


def _python_action_sdk(host: str, watch_directories: set[Path]) -> dict[str, Any]:
    """Observe the original base interpreter used by isolated stdlib actions."""
    executable = native.governed_base_executable()
    tools = {"python": str(executable)}
    artifacts = [executable]
    watch_directories.add(executable.parent)
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
        watch_directories.add(directory)
        for path in sorted(directory.iterdir()):
            if path.name in {"site-packages", "dist-packages", "__pycache__"}:
                continue
            if path.is_dir():
                pending.append(path)
            elif path.suffix == ".py":
                artifacts.append(path)
    watch_directories.add(extensions)
    if extensions.is_dir():
        artifacts.extend(
            path for path in sorted(extensions.iterdir()) if path.is_file() and path.suffix in {".so", ".pyd", ".dll"}
        )
    if host == "windows":
        watch_directories.add(Path(sys.base_prefix))
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
    return {
        "tools": tools,
        "artifacts": sorted(set(artifacts)),
        "identity": {
            # Only the full-portal lint fixture consumes the Windows profile;
            # it belongs to that action's environment, not other action keys.
            "tools": {name: value for name, value in tools.items() if name != "UserProfile"},
            "python_version": sys.version,
            "python_base_prefix": sys.base_prefix,
            "stdlib": str(stdlib),
            "extensions": str(extensions),
            "runtime_contract": (
                "Installed native SDK locations; declared payload bytes, not full OS/runtime hermeticity"
            ),
        },
    }


def _lock_graph(requirements: Path) -> list[Path]:
    # The tooling venv's location embeds the fingerprint of the whole included
    # lock graph, so every governed lock file is a repository input.
    from ao.portal import python_env

    files = [requirements]
    for included in python_env._included_requirements(requirements):
        files.extend(_lock_graph(included))
    return files


def _python_runtime(host: str) -> tuple[dict[str, Any], list[Path], list[Path]]:
    """Select the interpreter that actually runs governed Python tests, never install one.

    Linux runs under the Nix portal interpreter, whose withPackages
    environment is itself the base installation. Windows and macOS run under
    the checkout-isolated locked tooling venv when the existing bootstrap
    prepared it; without that venv the base interpreter is an honest recorded
    fallback whose runtime lacks the pinned tool wheels.
    """
    from ao.portal import build_dir, python_env

    versions = python_env.tool_versions()
    python_env.validate_base_python(versions, exact_patch=host != "macos")
    actual_version = ".".join(str(part) for part in sys.version_info[:3])
    base = native.governed_base_executable()
    evidence = [python_env.TOOLCHAIN_FILE]
    executable = base
    tooling = ""
    provider = "nix" if host == "linux" else "native-base"
    if host == "linux":
        tooling = str(base)
    else:
        requirements = python_env.MACOS_REQUIREMENTS_FILE if host == "macos" else python_env.REQUIREMENTS_FILE
        evidence.extend(_lock_graph(requirements))
        fingerprint = python_env.environment_fingerprint(
            versions,
            requirements,
            base_prefix=sys.base_prefix,
            runtime_version=actual_version,
        )
        try:
            destination = python_env.environment_path(
                native.PROJECT_ROOT,
                build_dir.state_root(system="Windows" if host == "windows" else "Darwin"),
                fingerprint,
                environ=dict(os.environ),
                os_name="nt" if host == "windows" else "posix",
            )
        except RuntimeError:
            # Without the portal's stable checkout identity there is no shared
            # tooling venv location to select.
            destination = None
        complete = False
        if destination is not None:
            candidate = python_env._python_path(destination, platform_name=host)
            marker = destination / python_env.COMPLETE_MARKER
            evidence.extend([candidate, marker])
            try:
                complete = candidate.is_file() and marker.read_text(encoding="utf-8").strip() == fingerprint
            except OSError:
                complete = False
            if complete:
                executable = candidate
                tooling = str(candidate)
                provider = "tooling-venv"
    contracts = {
        "nix": (
            "Installed immutable Nix portal environment; declared interpreter location, not full OS/runtime hermeticity"
        ),
        "tooling-venv": (
            "Checkout-isolated locked tooling venv over the installed base; declared interpreter "
            "location, not full OS/runtime hermeticity"
        ),
        "native-base": (
            "Base interpreter without the prepared locked tooling venv; pinned tool wheels are "
            "unavailable in this runtime"
        ),
    }
    if provider != "native-base":
        # The pinned documentation parser packages must import under the
        # runtime that actually executes Python tests.
        probe = subprocess.run(
            [
                str(executable),
                "-I",
                "-c",
                "import sys; from importlib.metadata import version; "
                "print('.'.join(map(str, sys.version_info[:3]))); "
                "print(version('markdown-it-py')); print(version('mdurl'))",
            ],
            env=native.child_environment(),
            capture_output=True,
            text=True,
            check=False,
        )
        components = 3 if host != "macos" else 2
        pinned = ".".join(versions["python"].split(".")[:components])
        reported = probe.stdout.split()
        if (
            probe.returncode
            or len(reported) != 3
            or not reported[0].startswith(pinned)
            or reported[1] != versions["markdown-it-py"]
            or reported[2] != versions["mdurl"]
        ):
            raise native.BazelWorkspaceError(
                f"selected Python tooling runtime does not match the pinned toolchain: {executable}"
            )
    identity = {
        "provider": provider,
        # Publish the interpreter that actually runs governed tests, not the
        # governed pin: macOS admits any patch of the pinned minor version, so
        # only the observed runtime describes the toolchain consumers receive.
        "version": actual_version,
        "implementation": "cpython",
        "executable": str(executable.absolute()),
        "base_prefix": sys.base_prefix,
        "tooling_python": tooling,
        "runtime_contract": contracts[provider],
    }
    return identity, [*evidence, executable], evidence


def _app_version(root: Path) -> str:
    # Shared plaintext root VERSION.txt fact, so no build file restates the
    # version. The name is VERSION.txt, not VERSION: a bare root VERSION is
    # spelled like the C++20 <version> header, which MSVC on case-insensitive
    # filesystems admits from the execroot include path as that header.
    path = root / "VERSION.txt"
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as error:
        raise ValueError(f"missing shared root version file: {path}") from error
    match = re.fullmatch(r"\s*(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\s*", text, flags=re.ASCII)
    if match is None:
        raise ValueError(f"expected {path} to hold exactly one x.y.z version")
    return ".".join(match.groups())


def _paths(paths: list[Path]) -> list[str]:
    # Keep lexical basenames and selected native loader paths. Starlark resolves
    # each symlink source independently when materializing the selected bytes.
    return [str(path.absolute()) for path in paths]


def _windows_mt() -> Path:
    # One-consumer helper: the selected Windows SDK manifest tool, never a compiler view.
    sdk = Path(os.environ.get("WindowsSdkDir", ""))
    version = os.environ.get("WindowsSDKVersion", "").rstrip("\\").rstrip("/")
    tool = sdk / "bin" / version / "x64/mt.exe"
    if not sdk.is_absolute() or not version or not tool.is_file():
        raise native.BazelWorkspaceError(f"selected Windows SDK manifest tool is missing: {tool}")
    return tool


def resolve_host_tools(tree: Path, prefix: Path | None, host: str, *, source: object = None) -> dict[str, Any]:
    """Return native SDK facts and read inventory, never BUILD or .bzl programs.

    Catalog Debug/Release library imports are resolved separately by the caller.
    The generated catalog and Python identities key their respective actions.
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
        system = Path(os.environ.get("SYSTEMROOT", ""))
        powershell = native.probe(system / "System32/WindowsPowerShell/v1.0/powershell.exe")
        if not system.is_absolute() or not powershell.is_file():
            raise ValueError("selected Windows PowerShell runtime is missing")
        for name, tool in {"mt": _windows_mt(), "powershell": powershell}.items():
            result["manifest_tools"][name] = tool.resolve().as_posix()
            files.update({tool.absolute(), tool.resolve()})
    python = _python_action_sdk(host, directories)
    result["python_action"] = {**python, "artifacts": _paths(python["artifacts"])}
    tools, artifacts, sdk_evidence = _catalog_sdk(tree, prefix, host, python=python, watch_directories=directories)
    python_artifacts = set(python["artifacts"])
    result["catalog"] = {
        "tools": tools,
        "artifacts": _paths([path for path in artifacts if path not in python_artifacts]),
        "evidence": _paths(sdk_evidence),
    }
    for path in artifacts:
        files.update({path.absolute(), path.resolve()})
    files.update(path.absolute() for path in sdk_evidence)
    directories.update(Path(tools[name]).parent for name in ("genrb", "pkgdata"))
    python_identity, python_inputs, _ = _python_runtime(host)
    result["python"] = python_identity
    result["test_tools"]["tooling_python"] = python_identity["tooling_python"]
    files.update(path.absolute() for path in python_inputs)
    # The rules_python replay launcher uses the toolchain's lexical executable
    # (a redirector for a prepared native venv), not the direct actions' base
    # path. The venv configuration is loaded beside the interpreter even under
    # -S. Do not key base-only actions on the venv or its pinned parser wheels.
    replay_executable = Path(python_identity["executable"])
    replay_inputs = []
    if python_identity["provider"] == "tooling-venv":
        configuration = replay_executable.parent.parent / "pyvenv.cfg"
        directories.add(configuration.parent)
        if configuration.is_file():
            replay_inputs.append(configuration)
    if replay_executable.resolve() not in {path.resolve() for path in python_artifacts}:
        replay_inputs.append(replay_executable)
    for path in replay_inputs:
        files.update({path.absolute(), path.resolve()})
    result["python_replay"] = {"identity": python_identity, "artifacts": _paths(replay_inputs)}
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
    version_path = native.PROJECT_ROOT / "VERSION.txt"
    result["app_version"] = _app_version(native.PROJECT_ROOT)
    files.add(version_path.absolute())
    result["watch_files"] = sorted(str(path.absolute()) for path in files)
    result["watch_directories"] = sorted({str(path.absolute()) for path in directories})
    return result
