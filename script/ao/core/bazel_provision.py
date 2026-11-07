"""Install a Bazel tree's native vcpkg manifest and the shared Windows SDK caches.

Provisioning runs the pinned vcpkg with the triplets and overlays of the native
CMake presets, so a provisioned tree restores the same package ABIs from the
binary cache without a CMake configure. On Windows it also restores the WinUI
NuGet closure and the pinned LLVM SDK into the managed caches that CMake trees
share, under the same locks and completion markers. Linux uses the pinned Nix
shell instead.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import urllib.request
from collections.abc import Mapping
from pathlib import Path

from . import bazel_native as native
from . import bazel_providers as providers
from . import builddir, buildlock, dependency_policy, proc, windows_llvm_sdk
from .bazel_native import BazelWorkspaceError
from .paths import PROJECT_ROOT

NUGET_LOCK = PROJECT_ROOT / "app/windows-winui/packages.config"
# The CMake WinUI restore writes and reads the same marker and lock files.
_NUGET_MARKER = ".aobus-nuget-complete"
_NUGET_LOCK_FILE = "restore.lock"


def _run_logged(argv: list[str], log: Path) -> None:
    """Stream a long provisioning command while keeping its complete output in the tree."""
    command = f"{Path(argv[0]).name} {argv[1]}"
    print(f"{command}; output: {log}", flush=True)
    if status := proc.run(argv, log=log):
        raise BazelWorkspaceError(f"{command} failed with exit {status}; see {log}")


def _vcpkg() -> Path:
    root = os.environ.get("VCPKG_ROOT")
    if not root:
        raise BazelWorkspaceError("vcpkg provisioning requires VCPKG_ROOT from the native launcher")
    executable = Path(root) / ("vcpkg.exe" if native.native_platform() == "windows" else "vcpkg")
    if not executable.is_file():
        raise BazelWorkspaceError(f"VCPKG_ROOT has no vcpkg executable: {executable}")
    return executable


def install_vcpkg(tree: Path) -> Path:
    """Install the manifest into the tree with vcpkg.cmake's manifest arguments."""
    vcpkg = _vcpkg()
    target, host = providers.vcpkg_triplets()
    argv = [
        str(vcpkg),
        "install",
        "--triplet",
        target,
        "--vcpkg-root",
        str(vcpkg.parent),
        "--x-wait-for-lock",
        f"--x-manifest-root={PROJECT_ROOT}",
        f"--x-install-root={tree / 'vcpkg_installed'}",
        f"--host-triplet={host}",
        f"--overlay-ports={PROJECT_ROOT / 'cmake/vcpkg-ports'}",
    ]
    if native.native_platform() == "macos":
        argv.append(f"--overlay-triplets={PROJECT_ROOT / 'cmake/vcpkg-triplets'}")
    _run_logged(argv, tree / "vcpkg-install.log")
    return providers.vcpkg_prefix(None, tree)


def _nuget_packages(environ: Mapping[str, str]) -> tuple[Path, str]:
    digest = hashlib.sha256(NUGET_LOCK.read_bytes()).hexdigest()
    if selected := environ.get("AOBUS_NUGET_PACKAGES_DIR"):
        return Path(selected), digest
    return builddir.windows_state_root(environ=environ) / "n" / digest[:12] / "p", digest


def _nuget_complete(packages: Path) -> bool:
    return all(
        len(list((packages / f"{name}.{version}").glob("*.nupkg"))) == 1
        for name, version in dependency_policy._packages_config(NUGET_LOCK).items()
    )


def _nuget_restored(packages: Path, digest: str) -> bool:
    marker = packages.parent / _NUGET_MARKER
    if not marker.is_file():
        return False
    try:
        value = marker.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return False
    return value.strip() == digest and _nuget_complete(packages)


def restored_nuget_packages(environ: Mapping[str, str]) -> Path | None:
    """Return the managed NuGet package directory once the lock's closure is completely restored."""
    packages, digest = _nuget_packages(environ)
    return packages if _nuget_restored(packages, digest) else None


def restore_nuget(tree: Path) -> Path:
    """Restore the locked WinUI closure into the managed cache that CMake WinUI trees share."""
    packages, digest = _nuget_packages(os.environ)
    with buildlock.shared_file_lock(packages.parent / _NUGET_LOCK_FILE):
        if _nuget_restored(packages, digest):
            return packages
        vcpkg = _vcpkg()
        fetched = subprocess.run([str(vcpkg), "fetch", "nuget"], capture_output=True, text=True, check=False)
        lines = [line.strip() for line in fetched.stdout.splitlines() if line.strip()]
        nuget = Path(lines[-1]) if fetched.returncode == 0 and lines else None
        if nuget is None or not nuget.is_file():
            raise BazelWorkspaceError(
                f"vcpkg did not report a usable NuGet executable:\n{fetched.stdout}{fetched.stderr}"
            )
        packages.mkdir(parents=True, exist_ok=True)
        _run_logged(
            [
                str(nuget),
                "install",
                str(NUGET_LOCK),
                "-OutputDirectory",
                str(packages),
                "-ConfigFile",
                str(NUGET_LOCK.parent / "NuGet.Config"),
                "-DirectDownload",
                "-NonInteractive",
                "-Verbosity",
                "normal",
            ],
            tree / "nuget-restore.log",
        )
        if not _nuget_complete(packages):
            raise BazelWorkspaceError("NuGet restore completed without the exact Windows App SDK package closure")
        # Text mode matches CMake's file(WRITE) line ending on each host.
        (packages.parent / _NUGET_MARKER).write_text(f"{digest}\n", encoding="utf-8")
    return packages


def _file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _llvm_archive(cache: Path, manifest: windows_llvm_sdk.Manifest) -> Path:
    """Reuse or download the official archive under cmake/LlvmSdk.cmake's download name."""
    name = f"clang%2Bllvm-{manifest.version}-x86_64-pc-windows-msvc.tar.xz"
    archive = cache / "downloads" / f"llvm-{manifest.version}" / name
    if archive.is_file() and _file_sha256(archive) == manifest.archive_sha256:
        return archive
    url = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{manifest.version}/{name}"
    archive.parent.mkdir(parents=True, exist_ok=True)
    partial = archive.with_name(name + ".partial")
    print(f"Downloading LLVM {manifest.version} Windows SDK: {url}", flush=True)
    urllib.request.urlretrieve(url, partial)
    if _file_sha256(partial) != manifest.archive_sha256:
        partial.unlink()
        raise BazelWorkspaceError(f"downloaded LLVM SDK SHA-256 does not match {windows_llvm_sdk.MANIFEST}")
    os.replace(partial, archive)
    return archive


def install_llvm_sdk() -> Path:
    """Verify and extract the pinned Windows LLVM SDK into the cache cmake/LlvmSdk.cmake shares."""
    manifest = windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST)
    if os.environ.get("AOBUS_LLVM_SDK_ROOT"):
        # A pre-provisioned root is only validated, as by the CMake owner.
        return windows_llvm_sdk.ready_root(os.environ, manifest)[0]
    root = windows_llvm_sdk.cache_root(os.environ, manifest)
    cache = root.parent.parent
    with buildlock.shared_file_lock(cache / f"llvm-{manifest.version}.lock"):
        try:
            return windows_llvm_sdk.ready_root(os.environ, manifest)[0]
        except windows_llvm_sdk.WindowsLlvmSdkError as invalid:
            if root.exists():
                print(f"Removing invalid LLVM SDK cache ({invalid}): {root}", flush=True)
                shutil.rmtree(root)
        archive = _llvm_archive(cache, manifest)
        staging = root.with_name(f".{root.name}.extract")
        shutil.rmtree(staging, ignore_errors=True)
        print(f"Extracting {archive.name} into {root}", flush=True)
        with tarfile.open(archive, "r:xz") as source:
            source.extractall(staging, filter="data")
        # The archive has one top-level directory, which FetchContent also strips.
        entries = list(staging.iterdir())
        if len(entries) != 1 or not entries[0].is_dir():
            shutil.rmtree(staging)
            raise BazelWorkspaceError(f"LLVM SDK archive does not hold one top-level directory: {archive}")
        entries[0].rename(root)
        staging.rmdir()
        if missing := [relative for relative in manifest.required_files if not (root / relative).is_file()]:
            shutil.rmtree(root)
            raise BazelWorkspaceError(f"verified LLVM SDK extraction is incomplete: missing {', '.join(missing)}")
        (root / windows_llvm_sdk.MARKER).write_text(windows_llvm_sdk.marker_text(manifest), encoding="utf-8")
    return root


def provision(tree: Path) -> None:
    host = native.native_platform()
    if host == "linux":
        print("Nothing to provision: Linux dependencies come from the pinned Nix shell")
        return
    with buildlock.build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        print(f"Provisioned vcpkg: {install_vcpkg(tree)}")
        if host == "windows":
            print(f"Restored WinUI NuGet packages: {restore_nuget(tree)}")
            print(f"Verified LLVM SDK: {install_llvm_sdk()}")
