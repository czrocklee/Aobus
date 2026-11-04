"""Behavioral tests for provisioning a Bazel tree without a CMake configure.

vcpkg and NuGet are faked at the process boundary and must discriminate the
executable and subcommand; the fakes create only the installed layout the
production validators inspect. The LLVM SDK download is faked at its URL, while
a tiny real archive exercises verification and extraction. The CMake presets,
WinUI restore and cmake/LlvmSdk.cmake own the contracts these cases compare against.
"""

import hashlib
import json
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_provision as provision
from ao.core import bazel_workspace as workspace
from ao.core import dependency_policy, windows_llvm_sdk

LOCKED = dependency_policy._packages_config(provision.NUGET_LOCK)
DIGEST = hashlib.sha256(provision.NUGET_LOCK.read_bytes()).hexdigest()


def _preset(name):
    presets = json.loads((workspace.PROJECT_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    return next(preset for preset in presets["configurePresets"] if preset["name"] == name)["cacheVariables"]


def _restore_packages(packages, names):
    for name in names:
        package = packages / f"{name}.{LOCKED[name]}"
        package.mkdir(parents=True)
        (package / f"{name}.{LOCKED[name]}.nupkg").write_bytes(b"archive")


class VcpkgProvisionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-provision-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tree = self.root / "bazel-debug"
        self.vcpkg_root = self.root / "vcpkg"
        self.vcpkg_root.mkdir()
        self.calls = []

    def install(self, host, triplets, status=0):
        executable = self.vcpkg_root / ("vcpkg.exe" if host == "windows" else "vcpkg")
        executable.write_bytes(b"")

        def run(argv, *, log):
            self.assertEqual((Path(argv[0]), argv[1]), (executable, "install"))
            self.calls.append((argv, log))
            if status == 0:
                installed = Path(next(arg for arg in argv if arg.startswith("--x-install-root=")).partition("=")[2])
                for triplet in triplets:
                    (installed / triplet / "include").mkdir(parents=True)
                (installed / "vcpkg").mkdir()
                (installed / "vcpkg/status").write_text("", encoding="utf-8")
            return status

        environment = {"VCPKG_ROOT": str(self.vcpkg_root), "AOBUS_VCPKG_TRIPLET": "x64-aobus-osx"}
        with (
            patch.dict(os.environ, environment),
            patch.object(workspace, "native_platform", return_value=host),
            patch.object(provision, "native_platform", return_value=host),
            patch.object(provision.proc, "run", side_effect=run),
            patch.object(provision, "restore_nuget", return_value=self.root / "nuget") as restore,
            patch.object(provision, "install_llvm_sdk", return_value=self.root / "llvm") as llvm,
        ):
            provision.provision(self.tree)
        self.assertEqual(llvm.called, host == "windows")
        return restore

    def overlays(self, preset):
        source = str(workspace.PROJECT_ROOT)
        variables = _preset(preset)
        return [
            f"--overlay-{kind}={os.path.normpath(variables[name].replace('${sourceDir}', source))}"
            for kind, name in (("ports", "VCPKG_OVERLAY_PORTS"), ("triplets", "VCPKG_OVERLAY_TRIPLETS"))
            if name in variables
        ]

    def test_windows_tree_installs_the_static_target_and_dynamic_host_triplets_then_shared_sdks(self):
        restore = self.install("windows", ("x64-windows-static-md", "x64-windows"))
        [(argv, log)] = self.calls
        self.assertEqual(argv[argv.index("--triplet") + 1], "x64-windows-static-md")
        self.assertIn("--host-triplet=x64-windows", argv)
        self.assertIn(f"--x-install-root={self.tree / 'vcpkg_installed'}", argv)
        self.assertIn(f"--x-manifest-root={workspace.PROJECT_ROOT}", argv)
        self.assertEqual([arg for arg in argv if arg.startswith("--overlay-")], self.overlays("windows-base"))
        self.assertEqual(log, self.tree / "vcpkg-install.log")
        restore.assert_called_once_with(self.tree)

    def test_macos_tree_installs_the_launcher_triplet_with_project_overlay_triplets(self):
        restore = self.install("macos", ("x64-aobus-osx",))
        [(argv, _)] = self.calls
        self.assertEqual(argv[argv.index("--triplet") + 1], "x64-aobus-osx")
        self.assertIn("--host-triplet=x64-aobus-osx", argv)
        self.assertEqual([arg for arg in argv if arg.startswith("--overlay-")], self.overlays("macos-base"))
        with (
            patch.object(workspace, "native_platform", return_value="macos"),
            patch.dict(os.environ, {"AOBUS_VCPKG_TRIPLET": "x64-aobus-osx"}),
        ):
            selected = workspace.vcpkg_prefix(None, self.tree)
        self.assertEqual(selected, (self.tree / "vcpkg_installed/x64-aobus-osx").resolve())
        restore.assert_not_called()

    def test_failed_install_names_its_log_and_leaves_no_provider(self):
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "vcpkg-install.log"):
            self.install("macos", ("x64-aobus-osx",), status=1)
        with (
            patch.object(workspace, "native_platform", return_value="macos"),
            patch.dict(os.environ, {"AOBUS_VCPKG_TRIPLET": "x64-aobus-osx"}),
            self.assertRaisesRegex(workspace.BazelWorkspaceError, "run `ao bazel provision`"),
        ):
            workspace.vcpkg_prefix(None, self.tree)

    def test_linux_needs_no_vcpkg_or_tree(self):
        with (
            patch.object(provision, "native_platform", return_value="linux"),
            patch.object(provision.proc, "run", side_effect=AssertionError("no provisioning process")),
        ):
            provision.provision(self.tree)
        self.assertFalse(self.tree.exists())


class NugetRestoreTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-nuget-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tree = self.root / "bazel-debug"
        self.tree.mkdir()
        self.vcpkg = self.root / "vcpkg/vcpkg.exe"
        self.vcpkg.parent.mkdir()
        self.vcpkg.write_bytes(b"")
        self.nuget = self.root / "downloads/nuget.exe"
        self.nuget.parent.mkdir()
        self.nuget.write_bytes(b"")
        self.environment = {"VCPKG_ROOT": str(self.vcpkg.parent), "AOBUS_STATE_ROOT": str(self.root / "state")}
        # The CMake WinUI restore owns this location.
        self.managed = self.root / "state/n" / DIGEST[:12] / "p"
        self.installs = []

    def fetch(self, argv, **_):
        self.assertEqual(argv, [str(self.vcpkg), "fetch", "nuget"])
        return subprocess.CompletedProcess(argv, 0, f"Fetching nuget...\n{self.nuget}\n", "")

    def restore(self, restored=tuple(LOCKED)):
        def install(argv, *, log):
            self.assertEqual((Path(argv[0]), argv[1]), (self.nuget, "install"))
            self.installs.append((argv, log))
            _restore_packages(Path(argv[argv.index("-OutputDirectory") + 1]), restored)
            return 0

        with (
            patch.dict(os.environ, self.environment, clear=True),
            patch.object(provision, "native_platform", return_value="windows"),
            patch.object(provision.subprocess, "run", side_effect=self.fetch),
            patch.object(provision.proc, "run", side_effect=install),
        ):
            return provision.restore_nuget(self.tree)

    def test_missing_closure_is_restored_into_the_managed_cache_and_marked_complete(self):
        self.assertEqual(self.restore(), self.managed)
        [(argv, log)] = self.installs
        self.assertEqual(
            argv[2:],
            [
                str(provision.NUGET_LOCK),
                "-OutputDirectory",
                str(self.managed),
                "-ConfigFile",
                str(provision.NUGET_LOCK.parent / "NuGet.Config"),
                "-DirectDownload",
                "-NonInteractive",
                "-Verbosity",
                "normal",
            ],
        )
        self.assertEqual(log, self.tree / "nuget-restore.log")
        self.assertEqual((self.managed.parent / ".aobus-nuget-complete").read_text(encoding="utf-8"), f"{DIGEST}\n")
        self.assertTrue((self.managed.parent / "restore.lock").is_file())
        self.assertEqual(provision.restored_nuget_packages(self.environment), self.managed)

    def test_marked_complete_closure_is_reused_without_fetching_nuget(self):
        _restore_packages(self.managed, LOCKED)
        # CMake on Windows writes its marker with CRLF; both readers strip it.
        (self.managed.parent / ".aobus-nuget-complete").write_bytes(f"{DIGEST}\r\n".encode())
        with (
            patch.dict(os.environ, self.environment, clear=True),
            patch.object(provision.subprocess, "run", side_effect=AssertionError("no NuGet fetch")),
            patch.object(provision.proc, "run", side_effect=AssertionError("no NuGet restore")),
        ):
            self.assertEqual(provision.restore_nuget(self.tree), self.managed)

    def test_corrupt_marker_is_not_selected_and_is_replaced_after_restore(self):
        marker = self.managed.parent / ".aobus-nuget-complete"
        marker.parent.mkdir(parents=True)
        marker.write_bytes(b"\xff\xfeinvalid marker")
        self.assertIsNone(provision.restored_nuget_packages(self.environment))
        self.assertEqual(self.restore(), self.managed)
        self.assertEqual(len(self.installs), 1)
        self.assertEqual(marker.read_text(encoding="utf-8"), f"{DIGEST}\n")
        self.assertEqual(provision.restored_nuget_packages(self.environment), self.managed)

    def test_marker_access_error_does_not_start_restore_or_overwrite_it(self):
        _restore_packages(self.managed, LOCKED)
        marker = self.managed.parent / ".aobus-nuget-complete"
        marker.write_bytes(f"{DIGEST}\n".encode())
        read_text = Path.read_text

        def read(path, *args, **kwargs):
            if path == marker:
                raise PermissionError("marker access denied")
            return read_text(path, *args, **kwargs)

        with (
            patch.dict(os.environ, self.environment, clear=True),
            patch.object(Path, "read_text", read),
            patch.object(provision, "_vcpkg", side_effect=AssertionError("no restore on access error")),
            self.assertRaisesRegex(PermissionError, "marker access denied"),
        ):
            provision.restore_nuget(self.tree)
        self.assertEqual(marker.read_bytes(), f"{DIGEST}\n".encode())

    def test_partial_restore_fails_unmarked_and_is_never_selected(self):
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "exact Windows App SDK package closure"):
            self.restore(restored=tuple(LOCKED)[1:])
        self.assertFalse((self.managed.parent / ".aobus-nuget-complete").exists())
        self.assertIsNone(provision.restored_nuget_packages(self.environment))

    def test_stale_marker_or_missing_archive_is_not_selected(self):
        _restore_packages(self.managed, LOCKED)
        marker = self.managed.parent / ".aobus-nuget-complete"
        marker.write_text("0" * 64 + "\n", encoding="utf-8")
        self.assertIsNone(provision.restored_nuget_packages(self.environment))
        marker.write_text(f"{DIGEST}\n", encoding="utf-8")
        name = next(iter(LOCKED))
        (self.managed / f"{name}.{LOCKED[name]}" / f"{name}.{LOCKED[name]}.nupkg").unlink()
        self.assertIsNone(provision.restored_nuget_packages(self.environment))

    def test_explicit_package_directory_is_restored_and_marked_beside_itself(self):
        selected = self.root / "explicit/packages"
        self.environment["AOBUS_NUGET_PACKAGES_DIR"] = str(selected)
        self.assertEqual(self.restore(), selected)
        self.assertEqual((selected.parent / ".aobus-nuget-complete").read_text(encoding="utf-8"), f"{DIGEST}\n")
        self.assertFalse(self.managed.exists())


class LlvmSdkProvisionTest(unittest.TestCase):
    VERSION = "31.2.3"
    REQUIRED = ("bin/clang-format.exe", "lib/clangTidyMain.lib", "lib/clang/31/include/stddef.h")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-llvm-provision-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.cache = self.root / "cache"
        self.archive = self.archive_of({f"clang+llvm-{self.VERSION}-x86_64-pc-windows-msvc": self.REQUIRED})
        self.manifest = self.root / "windows-llvm-sdk.json"
        self.write_manifest(hashlib.sha256(self.archive.read_bytes()).hexdigest())
        self.enterContext(patch.object(windows_llvm_sdk, "MANIFEST", self.manifest))
        self.environment = {"AOBUS_LLVM_SDK_CACHE_ROOT": str(self.cache)}
        self.downloads = []
        # cmake/LlvmSdk.cmake owns these cache, download and lock locations.
        self.sdk = self.cache / f"toolchains/llvm-{self.VERSION}-x86_64-windows-msvc"
        name = f"clang%2Bllvm-{self.VERSION}-x86_64-pc-windows-msvc.tar.xz"
        self.url = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{self.VERSION}/{name}"
        self.download = self.cache / f"downloads/llvm-{self.VERSION}" / name

    def archive_of(self, trees, name="archive.tar.xz"):
        source = self.root / "source" / name
        for top, files in trees.items():
            for relative in files:
                (source / top / relative).parent.mkdir(parents=True, exist_ok=True)
                (source / top / relative).write_text(relative, encoding="utf-8")
        archive = self.root / name
        with tarfile.open(archive, "w:xz") as output:
            for top in trees:
                output.add(source / top, arcname=top)
        return archive

    def write_manifest(self, checksum):
        required = [entry for entry in self.REQUIRED if not entry.startswith("lib/clang/")]
        value = {"version": self.VERSION, "archiveSha256": checksum, "requiredFiles": required}
        self.manifest.write_text(json.dumps(value), encoding="utf-8")

    def install(self, environment=None, served=None):
        def retrieve(url, destination):
            self.downloads.append((url, Path(destination)))
            Path(destination).write_bytes((served or self.archive).read_bytes())

        with (
            patch.dict(os.environ, environment or self.environment, clear=True),
            patch.object(provision.urllib.request, "urlretrieve", side_effect=retrieve),
        ):
            return provision.install_llvm_sdk()

    def test_verified_archive_is_extracted_without_its_top_level_directory_and_marked(self):
        self.assertEqual(self.install(), self.sdk)
        self.assertEqual(self.downloads, [(self.url, self.download.with_name(self.download.name + ".partial"))])
        self.assertTrue(self.download.is_file())
        self.assertTrue((self.cache / f"llvm-{self.VERSION}.lock").is_file())
        self.assertEqual(sorted(path.name for path in self.sdk.parent.iterdir()), [self.sdk.name])
        for relative in self.REQUIRED:
            self.assertEqual((self.sdk / relative).read_text(encoding="utf-8"), relative)
        manifest = windows_llvm_sdk.read_manifest(self.manifest)
        self.assertEqual(windows_llvm_sdk.ready_root(self.environment, manifest), (self.sdk, True))
        self.assertEqual(self.install(), self.sdk)
        self.assertEqual(len(self.downloads), 1)

    def test_checksum_mismatch_discards_the_download_and_leaves_no_sdk(self):
        self.write_manifest("0" * 64)
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "SHA-256"):
            self.install()
        self.assertEqual(list(self.download.parent.iterdir()), [])
        self.assertFalse(self.sdk.exists())

    def test_invalid_cached_sdk_is_replaced_from_the_verified_cached_archive(self):
        self.download.parent.mkdir(parents=True)
        self.download.write_bytes(self.archive.read_bytes())
        (self.sdk / "bin").mkdir(parents=True)
        (self.sdk / "bin/stale.exe").write_bytes(b"")
        (self.sdk / windows_llvm_sdk.MARKER).write_text("version=31.2.2\n", encoding="utf-8")
        self.assertEqual(self.install(), self.sdk)
        self.assertEqual(self.downloads, [])
        self.assertFalse((self.sdk / "bin/stale.exe").exists())
        self.assertTrue((self.sdk / "bin/clang-format.exe").is_file())

    def test_corrupt_cached_sdk_marker_is_replaced_from_the_verified_cached_archive(self):
        self.download.parent.mkdir(parents=True)
        self.download.write_bytes(self.archive.read_bytes())
        for relative in self.REQUIRED:
            file = self.sdk / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("old cache", encoding="utf-8")
        marker = self.sdk / windows_llvm_sdk.MARKER
        marker.write_bytes(b"\xff\xfeinvalid marker")
        self.assertEqual(self.install(), self.sdk)
        self.assertEqual(self.downloads, [])
        manifest = windows_llvm_sdk.read_manifest(self.manifest)
        self.assertEqual(marker.read_text(encoding="utf-8"), windows_llvm_sdk.marker_text(manifest))
        for relative in self.REQUIRED:
            self.assertEqual((self.sdk / relative).read_text(encoding="utf-8"), relative)

    def test_marker_access_error_does_not_remove_cached_sdk(self):
        self.assertEqual(self.install(), self.sdk)
        marker = self.sdk / windows_llvm_sdk.MARKER
        original = marker.read_bytes()
        read_text = Path.read_text

        def read(path, *args, **kwargs):
            if path == marker:
                raise PermissionError("marker access denied")
            return read_text(path, *args, **kwargs)

        with (
            patch.object(Path, "read_text", read),
            patch.object(provision, "_llvm_archive", side_effect=AssertionError("no repair on access error")),
            self.assertRaisesRegex(PermissionError, "marker access denied"),
        ):
            self.install()
        self.assertEqual(marker.read_bytes(), original)
        self.assertTrue((self.sdk / "bin/clang-format.exe").is_file())
        self.assertEqual(len(self.downloads), 1)

    def test_archive_without_one_top_level_directory_installs_nothing(self):
        archive = self.archive_of({"first": self.REQUIRED, "second": ("README",)}, name="split.tar.xz")
        self.write_manifest(hashlib.sha256(archive.read_bytes()).hexdigest())
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "one top-level directory"):
            self.install(served=archive)
        self.assertEqual(list(self.sdk.parent.iterdir()), [])

    def test_preprovisioned_sdk_is_validated_without_downloading_or_writing_a_cache(self):
        explicit = self.root / "explicit"
        environment = {"AOBUS_LLVM_SDK_ROOT": str(explicit), **self.environment}
        with self.assertRaisesRegex(windows_llvm_sdk.WindowsLlvmSdkError, "clang-format.exe"):
            self.install(environment)
        for relative in self.REQUIRED:
            (explicit / relative).parent.mkdir(parents=True, exist_ok=True)
            (explicit / relative).write_bytes(b"")
        marker = explicit / windows_llvm_sdk.MARKER
        marker.write_bytes(b"\xff\xfeignored explicit marker")
        self.assertEqual(self.install(environment), explicit)
        self.assertEqual(marker.read_bytes(), b"\xff\xfeignored explicit marker")
        self.assertEqual(self.downloads, [])
        self.assertFalse(self.cache.exists())


class PreparedNugetSelectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-nuget-prepare-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.managed = self.root / "state/n" / DIGEST[:12] / "p"

    def prepared_rc(self, environment):
        with (
            patch.dict(os.environ, {"AOBUS_STATE_ROOT": str(self.root / "state"), **environment}, clear=True),
            patch.object(workspace, "native_platform", return_value="windows"),
            patch.object(workspace, "vcpkg_prefix", return_value=self.root / "provider"),
        ):
            prepared = workspace.prepare_workspace(self.root / "build", "debug")
        return (prepared / "aobus.bazelrc").read_text(encoding="utf-8")

    def test_restored_managed_cache_is_selected_unless_the_caller_names_one(self):
        option = "--repo_env=AOBUS_NUGET_PACKAGES_DIR="
        self.assertNotIn(option, self.prepared_rc({}))
        _restore_packages(self.managed, LOCKED)
        (self.managed.parent / ".aobus-nuget-complete").write_text(f"{DIGEST}\n", encoding="utf-8")
        self.assertIn(json.dumps(option + str(self.managed)), self.prepared_rc({}))
        explicit = str(self.root / "explicit")
        rc = self.prepared_rc({"AOBUS_NUGET_PACKAGES_DIR": explicit})
        self.assertIn(json.dumps(option + explicit), rc)
        self.assertNotIn(str(self.managed), rc)


if __name__ == "__main__":
    unittest.main()
