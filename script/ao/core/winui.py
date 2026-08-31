"""Windows App SDK host checks, runtime setup, and WinUI launch guards."""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import subprocess
import tempfile
import urllib.request
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import builddir
from .dependency_policy import CONTRACT_FILE, DependencyPolicyError, load_contract
from .paths import PROJECT_ROOT

WINDOWS_BUILD_TOOLS_CONFIG = PROJECT_ROOT / "config" / "windows-build-tools.vsconfig"
WINDOWS_SDK_VERSION = "10.0.26100.0"
VISUAL_STUDIO_GENERATOR = "Visual Studio 18 2026"

# Visual Studio uses product-specific package IDs for the same installed
# capabilities.  The governed .vsconfig intentionally targets the standalone
# Build Tools product, while hosted CI supplies Visual Studio Enterprise.
# Accept the equivalent Enterprise workload IDs without weakening the concrete
# compiler, CMake, NuGet, and SDK component checks below.
VISUAL_STUDIO_COMPONENT_ALTERNATIVES: dict[str, tuple[str, ...]] = {
    "Microsoft.VisualStudio.Workload.VCTools": ("Microsoft.VisualStudio.Workload.NativeDesktop",),
    "Microsoft.VisualStudio.Workload.UniversalBuildTools": ("Microsoft.VisualStudio.Workload.Universal",),
    "Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools": (
        "Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.Prerequisites",
    ),
}


@dataclass(frozen=True)
class RuntimeContract:
    package_name: str
    version: str
    architecture: str
    installer_url: str
    sha256: str


@dataclass(frozen=True)
class RuntimePackage:
    version: str
    architecture: str
    package_full_name: str


@dataclass(frozen=True)
class HostCheck:
    label: str
    ok: bool
    detail: str
    required: bool = True


def _program_files_x86(environ: Mapping[str, str]) -> Path:
    value = environ.get("ProgramFiles(x86)") or environ.get("PROGRAMFILES(X86)")
    if not value:
        raise RuntimeError("ProgramFiles(x86) is not defined.")
    return Path(value)


def _runtime_contract(contract_path: Path = CONTRACT_FILE) -> RuntimeContract:
    contract = load_contract(contract_path)
    try:
        runtime = contract["dependencies"]["windows-app-sdk"]["runtime"]
        return RuntimeContract(
            package_name=str(runtime["packageName"]),
            version=str(runtime["version"]),
            architecture=str(runtime["architecture"]),
            installer_url=str(runtime["installerUrl"]),
            sha256=str(runtime["sha256"]).lower(),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise DependencyPolicyError("dependency contract lacks a valid Windows App Runtime definition") from exc


def required_components(path: Path = WINDOWS_BUILD_TOOLS_CONFIG) -> tuple[str, ...]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        components = document["components"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise RuntimeError(f"cannot read the Windows Build Tools component manifest {path}: {exc}") from exc
    if not isinstance(components, list) or not components or not all(isinstance(item, str) for item in components):
        raise RuntimeError(f"{path} must contain a non-empty string array named 'components'")
    return tuple(components)


def vswhere_path(environ: Mapping[str, str] | None = None) -> Path:
    environment = os.environ if environ is None else environ
    return _program_files_x86(environment) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"


def _run_text(
    command: list[str],
    *,
    environ: Mapping[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=None if environ is None else dict(environ),
    )


def _windows_powershell_environment(
    environ: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Drop inherited PowerShell 7 paths before launching Windows PowerShell 5.1."""
    source = os.environ if environ is None else environ
    return {name: value for name, value in source.items() if name.casefold() != "psmodulepath"}


def visual_studio_installation(
    *,
    component: str | None = None,
    environ: Mapping[str, str] | None = None,
) -> Path | None:
    locator = vswhere_path(environ)
    if not locator.is_file():
        return None
    command = [str(locator), "-latest", "-products", "*"]
    if component:
        command += ["-requires", component]
    command += ["-property", "installationPath"]
    result = _run_text(command, environ=environ)
    paths = [Path(line.strip()) for line in result.stdout.splitlines() if line.strip()]
    return paths[-1] if result.returncode == 0 and paths else None


def installed_component(
    component: str,
    *,
    environ: Mapping[str, str] | None = None,
) -> str | None:
    """Return the canonical or product-equivalent installed component ID."""
    candidates = (component, *VISUAL_STUDIO_COMPONENT_ALTERNATIVES.get(component, ()))
    for candidate in candidates:
        if visual_studio_installation(component=candidate, environ=environ) is not None:
            return candidate
    return None


def bundled_cmake(installation: Path) -> Path:
    return installation / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"


def _runtime_packages_from_json(text: str, architecture: str) -> tuple[RuntimePackage, ...]:
    if not text.strip():
        return ()
    try:
        decoded: Any = json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"PowerShell returned invalid Windows App Runtime data: {exc}") from exc
    records = decoded if isinstance(decoded, list) else [decoded]
    result: list[RuntimePackage] = []
    marker = f"_{architecture.lower()}__"
    for record in records:
        if not isinstance(record, dict):
            continue
        package_full_name = str(record.get("PackageFullName", ""))
        if marker not in package_full_name.lower():
            continue
        result.append(
            RuntimePackage(
                version=str(record.get("Version", "")),
                architecture=architecture,
                package_full_name=package_full_name,
            )
        )
    return tuple(result)


def installed_runtime_packages(
    runtime: RuntimeContract | None = None,
    *,
    powershell: str = "powershell.exe",
    environ: Mapping[str, str] | None = None,
) -> tuple[RuntimePackage, ...]:
    selected = _runtime_contract() if runtime is None else runtime
    command = (
        f"Get-AppxPackage -Name '{selected.package_name}' -PackageTypeFilter Framework | "
        "Select-Object Name,Version,Architecture,PackageFullName | ConvertTo-Json -Compress"
    )
    result = _run_text(
        [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
        environ=_windows_powershell_environment(environ),
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "Get-AppxPackage failed")
    return _runtime_packages_from_json(result.stdout, selected.architecture)


def matching_runtime(
    runtime: RuntimeContract | None = None,
    *,
    powershell: str = "powershell.exe",
    environ: Mapping[str, str] | None = None,
) -> RuntimePackage | None:
    selected = _runtime_contract() if runtime is None else runtime
    for package in installed_runtime_packages(selected, powershell=powershell, environ=environ):
        if package.version == selected.version:
            return package
    return None


def _registry_dword(path: str, name: str) -> int | None:
    try:
        registry = __import__("winreg")

        with registry.OpenKey(registry.HKEY_LOCAL_MACHINE, path) as key:
            value, _ = registry.QueryValueEx(key, name)
            return int(value)
    except (ImportError, FileNotFoundError, OSError, TypeError, ValueError):
        return None


def inspect_host(
    *,
    include_runtime: bool = True,
    environ: Mapping[str, str] | None = None,
) -> tuple[HostCheck, ...]:
    environment = os.environ if environ is None else environ
    if os.name != "nt":
        return (HostCheck("Windows host", False, "WinUI development requires native Windows."),)

    checks: list[HostCheck] = []
    locator = vswhere_path(environment)
    checks.append(HostCheck("vswhere", locator.is_file(), str(locator)))

    installation = visual_studio_installation(environ=environment)
    checks.append(
        HostCheck(
            "Visual Studio Build Tools",
            installation is not None,
            str(installation) if installation else "No Build Tools installation was found.",
        )
    )

    for component in required_components():
        present = installed_component(component, environ=environment)
        detail = "installed" if present == component else f"installed via {present}"
        checks.append(
            HostCheck(
                component,
                present is not None,
                detail if present else f"missing; install from {WINDOWS_BUILD_TOOLS_CONFIG}",
            )
        )

    if installation:
        cmake = bundled_cmake(installation)
        generator_present = False
        detail = str(cmake)
        if cmake.is_file():
            result = _run_text([str(cmake), "--help"], environ=environment)
            generator_present = result.returncode == 0 and VISUAL_STUDIO_GENERATOR in result.stdout
            if not generator_present:
                detail = f"{cmake} does not advertise {VISUAL_STUDIO_GENERATOR}"
        checks.append(HostCheck("CMake Visual Studio generator", generator_present, detail))

    sdk_root = _program_files_x86(environment) / "Windows Kits" / "10" / "Include" / WINDOWS_SDK_VERSION
    checks.append(HostCheck(f"Windows SDK {WINDOWS_SDK_VERSION}", sdk_root.is_dir(), str(sdk_root)))

    if include_runtime:
        try:
            runtime = _runtime_contract()
            installed = matching_runtime(runtime, environ=environment)
            checks.append(
                HostCheck(
                    f"Windows App Runtime {runtime.version} {runtime.architecture}",
                    installed is not None,
                    installed.package_full_name if installed else "missing; run `ao.bat setup winui-runtime`",
                )
            )
        except (DependencyPolicyError, RuntimeError) as exc:
            checks.append(HostCheck("Windows App Runtime", False, str(exc)))

    long_paths = _registry_dword(
        r"SYSTEM\CurrentControlSet\Control\FileSystem",
        "LongPathsEnabled",
    )
    checks.append(
        HostCheck(
            "Long paths",
            long_paths == 1,
            "enabled" if long_paths == 1 else "not enabled; Aobus keeps generated build paths on local short paths",
            required=False,
        )
    )
    developer_mode = _registry_dword(
        r"SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock",
        "AllowDevelopmentWithoutDevLicense",
    )
    checks.append(
        HostCheck(
            "Developer Mode",
            developer_mode == 1,
            "enabled" if developer_mode == 1 else "not enabled; unpackaged development builds do not require it",
            required=False,
        )
    )
    return tuple(checks)


def require_build_host() -> None:
    failures = [check for check in inspect_host(include_runtime=False) if check.required and not check.ok]
    if failures:
        details = "\n".join(f"  {check.label}: {check.detail}" for check in failures)
        raise RuntimeError(f"WinUI build prerequisites are incomplete:\n{details}\nRun `ao.bat doctor winui`.")


def require_runtime() -> RuntimePackage:
    runtime = _runtime_contract()
    installed = matching_runtime(runtime)
    if installed is None:
        raise RuntimeError(
            f"Windows App Runtime {runtime.version} {runtime.architecture} is not installed; "
            "run `ao.bat setup winui-runtime`."
        )
    return installed


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _download_verified_installer(runtime: RuntimeContract, state_root: Path) -> Path:
    cache = state_root / "cache" / "windows-app-sdk" / runtime.version
    cache.mkdir(parents=True, exist_ok=True)
    installer = cache / f"WindowsAppRuntimeInstall-{runtime.architecture}.exe"
    if installer.is_file() and file_sha256(installer) == runtime.sha256:
        return installer

    with tempfile.NamedTemporaryFile(prefix="aobus-winapp-runtime-", suffix=".exe", dir=cache, delete=False) as stream:
        staging = Path(stream.name)
        try:
            with urllib.request.urlopen(runtime.installer_url) as response:
                while block := response.read(1024 * 1024):
                    stream.write(block)
        except BaseException:
            staging.unlink(missing_ok=True)
            raise

    actual = file_sha256(staging)
    if actual != runtime.sha256:
        staging.unlink(missing_ok=True)
        raise RuntimeError(
            f"Windows App Runtime installer hash mismatch: expected {runtime.sha256}, downloaded {actual}"
        )
    os.replace(staging, installer)
    return installer


def _verify_authenticode(
    installer: Path,
    *,
    environ: Mapping[str, str] | None = None,
) -> None:
    environment = _windows_powershell_environment(environ)
    environment["AOBUS_AUTHENTICODE_PATH"] = str(installer)
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$path = $env:AOBUS_AUTHENTICODE_PATH; "
        "if ([string]::IsNullOrWhiteSpace($path)) { throw 'Authenticode path is missing.' }; "
        "if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Authenticode file is missing.' }; "
        "$signature = Get-AuthenticodeSignature -LiteralPath $path -ErrorAction Stop; "
        "if ($null -eq $signature) { throw 'Authenticode query returned no result.' }; "
        "$subject = if ($null -eq $signature.SignerCertificate) { '' } "
        "else { [string]$signature.SignerCertificate.Subject }; "
        "[pscustomobject]@{Status=[string]$signature.Status;Subject=$subject} | ConvertTo-Json -Compress"
    )
    result = _run_text(
        ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script],
        environ=environment,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "Authenticode verification failed")
    try:
        signature = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError("Authenticode verification returned invalid data") from exc
    if not isinstance(signature, dict):
        raise RuntimeError("Authenticode verification returned invalid data")
    status = signature.get("Status")
    subject = signature.get("Subject")
    if status != "Valid" or not isinstance(subject, str) or "Microsoft Corporation" not in subject:
        raise RuntimeError(
            f"Windows App Runtime installer has an invalid signer: status={status!r}, subject={subject!r}"
        )


def setup_runtime(*, state_root: Path | None = None) -> RuntimePackage:
    if os.name != "nt":
        raise RuntimeError("Windows App Runtime setup requires native Windows.")
    runtime = _runtime_contract()
    if installed := matching_runtime(runtime):
        return installed

    selected_state_root = builddir.windows_state_root() if state_root is None else state_root
    installer = _download_verified_installer(runtime, selected_state_root)
    _verify_authenticode(installer)
    result = subprocess.run([str(installer), "--quiet"], check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Windows App Runtime installer failed with exit code {result.returncode}.")
    installed = matching_runtime(runtime)
    if installed is None:
        raise RuntimeError(f"Windows App Runtime installer completed but {runtime.version} is still unavailable.")
    return installed


def current_session_id() -> int:
    if os.name != "nt":
        raise RuntimeError("Windows session discovery requires native Windows.")
    kernel32 = vars(ctypes)["windll"].kernel32
    process_id = kernel32.GetCurrentProcessId()
    session_id = ctypes.c_ulong()
    if not kernel32.ProcessIdToSessionId(process_id, ctypes.byref(session_id)):
        raise RuntimeError("Windows could not determine the current process session.")
    return int(session_id.value)


def require_interactive_session() -> int:
    session_id = current_session_id()
    if session_id == 0:
        raise RuntimeError(
            "WinUI cannot display from Windows service session 0. "
            "Run `ao.bat run winui` inside the active RDP desktop session."
        )
    return session_id
