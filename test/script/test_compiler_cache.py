"""Tests for project-managed compiler-cache policy and activation."""

import contextlib
import hashlib
import io
import json
import os
import stat
import subprocess
import tempfile
import unittest
import zipfile
from dataclasses import replace
from pathlib import Path
from unittest import mock

from ao.__main__ import main as portal_main
from ao.command import test as test_command
from ao.core import compiler_cache


class CompilerCacheContractTest(unittest.TestCase):
    def test_repository_contract_governs_local_and_ci_versions(self):
        policy = compiler_cache.load_policy()

        self.assertEqual(policy.default_size, "20G")
        self.assertEqual((policy.local_provider, policy.local_minimum_version), ("ccache", "4.13.6"))
        self.assertEqual((policy.linux_version, policy.windows_version), ("4.13.6", "4.13.6"))
        self.assertEqual((policy.ci_provider, policy.ci_version), ("sccache", "v0.17.0"))
        self.assertRegex(policy.windows_sha256, r"^[0-9a-f]{64}$")
        self.assertIn(policy.windows_version, policy.windows_url)
        self.assertIn(policy.windows_version, policy.windows_member)

        workflow = (compiler_cache.PROJECT_ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertEqual(workflow.count(f"uses: {policy.ci_action}"), 3)
        self.assertEqual(workflow.count("version: ${{ steps.compiler-cache-contract.outputs.version }}"), 3)
        self.assertNotIn("version: v0.17.0", workflow)
        cmake_options = (compiler_cache.PROJECT_ROOT / "cmake" / "CompilerOptions.cmake").read_text(encoding="utf-8")
        self.assertNotIn("USE_CCACHE", cmake_options)
        windows_activation = workflow.split("  windows:\n", 1)[1].split("--platform Windows", 1)[0]
        self.assertIn('--state-root "$env:RUNNER_TEMP\\aobus-state"', windows_activation)
        self.assertNotIn("--state-root $env:AOBUS_STATE_ROOT", windows_activation)

    def test_invalid_manifest_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "policy.json"
            path.write_text('{"schemaVersion": 2}', encoding="utf-8")
            with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "schemaVersion"):
                compiler_cache.load_policy(path)

    def test_size_policy_compares_common_spellings(self):
        self.assertEqual(compiler_cache.size_bytes("20G"), 20 * 1000**3)
        self.assertEqual(compiler_cache.size_bytes("24.5 GiB"), int(24.5 * 1024**3))
        self.assertEqual(compiler_cache.larger_size("20G", "40 GiB", "30GB"), "40 GiB")
        self.assertEqual(compiler_cache.larger_size("20G", "19GiB"), "19GiB")
        self.assertEqual(compiler_cache.larger_size("20G", "21"), "21")
        self.assertEqual(compiler_cache.larger_size("20G", "0"), "0")
        with self.assertRaises(compiler_cache.CompilerCacheError):
            compiler_cache.size_bytes("many")
        with self.assertRaises(compiler_cache.CompilerCacheError):
            compiler_cache.size_bytes("9" * 400 + "G")

    def test_local_version_policy_accepts_compatible_homebrew_updates(self):
        self.assertTrue(compiler_cache.version_at_least("4.13.6", "4.13.6"))
        self.assertTrue(compiler_cache.version_at_least("4.14", "4.13.6"))
        self.assertFalse(compiler_cache.version_at_least("4.13.5", "4.13.6"))
        self.assertFalse(compiler_cache.version_at_least("not-a-version", "4.13.6"))
        for suffix in ("_1", "-1", ".1", "-rc1", "+dirty", "unexpected"):
            with self.subTest(suffix=suffix):
                self.assertFalse(compiler_cache.version_at_least("4.13.6" + suffix, "4.13.6"))


class CompilerCacheInstallTest(unittest.TestCase):
    @staticmethod
    def make_zip(path: Path, member: str, contents: bytes = b"cache executable") -> str:
        with zipfile.ZipFile(path, "w") as bundle:
            bundle.writestr(member, contents)
            bundle.writestr("../../outside.exe", b"must not be extracted")
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def test_windows_archive_extracts_only_exact_member_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "ccache.zip"
            member = "ccache-version/ccache.exe"
            checksum = self.make_zip(archive, member)
            destination = root / "tools" / "ccache.exe"

            installed_hash = compiler_cache.install_windows_archive(
                archive, destination, expected_sha256=checksum, member_name=member
            )
            first_mtime = destination.stat().st_mtime_ns
            compiler_cache.install_windows_archive(archive, destination, expected_sha256=checksum, member_name=member)

            self.assertEqual(destination.read_bytes(), b"cache executable")
            self.assertEqual(installed_hash, hashlib.sha256(b"cache executable").hexdigest())
            self.assertEqual(destination.stat().st_mtime_ns, first_mtime)
            self.assertFalse((root.parent / "outside.exe").exists())

    def test_checksum_mismatch_cannot_install_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "ccache.zip"
            self.make_zip(archive, "ccache-version/ccache.exe")
            destination = root / "tools" / "ccache.exe"
            with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "checksum mismatch"):
                compiler_cache.install_windows_archive(
                    archive, destination, expected_sha256="0" * 64, member_name="ccache-version/ccache.exe"
                )
            self.assertFalse(destination.exists())

    def test_invalid_windows_archive_cannot_replace_an_existing_executable(self):
        for condition, message in (
            ("invalid", "not a valid ZIP"),
            ("symlink", "must not be a symbolic link"),
            ("empty", "is empty"),
            ("missing", "does not contain exactly"),
        ):
            with self.subTest(condition=condition), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / "ccache.zip"
                member = "bin/ccache.exe"
                if condition == "invalid":
                    archive.write_bytes(b"not a zip")
                else:
                    with zipfile.ZipFile(archive, "w") as bundle:
                        entry = zipfile.ZipInfo("other.exe" if condition == "missing" else member)
                        if condition == "symlink":
                            entry.create_system = 3
                            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                        bundle.writestr(entry, b"" if condition == "empty" else b"contents")
                destination = root / "ccache.exe"
                destination.write_bytes(b"previous executable")
                with self.assertRaisesRegex(compiler_cache.CompilerCacheError, message):
                    compiler_cache.install_windows_archive(
                        archive,
                        destination,
                        expected_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                        member_name=member,
                    )
                self.assertEqual(destination.read_bytes(), b"previous executable")

    def test_posix_lookup_uses_the_supplied_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ccache"
            executable.touch()
            with mock.patch.object(compiler_cache.shutil, "which", return_value=str(executable)) as which:
                self.assertEqual(
                    compiler_cache._resolve_posix_ccache(
                        compiler_cache.load_policy(), system="Linux", environ={"PATH": temporary}
                    ),
                    executable.resolve(),
                )
            which.assert_called_once_with("ccache", path=temporary)

    def test_posix_missing_tools_report_setup_errors(self):
        for system, tool, message in (
            ("Linux", "ccache", "pinned Linux shell"),
            ("Darwin", "brew", "Homebrew is unavailable"),
        ):
            with (
                self.subTest(system=system),
                mock.patch.object(compiler_cache.shutil, "which", return_value=None) as which,
            ):
                with self.assertRaisesRegex(compiler_cache.CompilerCacheError, message):
                    compiler_cache._resolve_posix_ccache(
                        compiler_cache.load_policy(), system=system, environ={"PATH": "chosen-path"}
                    )
                which.assert_called_once_with(tool, path="chosen-path")

    def test_homebrew_installs_and_upgrades_only_when_required(self):
        for version in ("4.13.5", "4.14"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temporary:
                prefix = Path(temporary)
                candidate = prefix / "bin" / "ccache"
                candidate.parent.mkdir()
                candidate.touch()
                with (
                    mock.patch.object(
                        compiler_cache.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, str(prefix))
                    ) as run,
                    mock.patch.object(compiler_cache, "_executable_version", return_value=version),
                ):
                    self.assertEqual(
                        compiler_cache._resolve_posix_ccache(
                            compiler_cache.load_policy(), system="Darwin", environ={"AOBUS_HOMEBREW": "chosen-brew"}
                        ),
                        candidate.resolve(),
                    )
                commands = [call.args[0] for call in run.call_args_list]
                expected = [["chosen-brew", "install", "ccache"], ["chosen-brew", "--prefix", "ccache"]]
                if version == "4.13.5":
                    expected.append(["chosen-brew", "upgrade", "ccache"])
                self.assertEqual(commands, expected)

    def test_windows_setup_publishes_verified_executable_wrapper_and_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy = compiler_cache.load_policy()
            archive = root / "source.zip"
            checksum = self.make_zip(archive, policy.windows_member)
            verified = archive.read_bytes()
            policy = replace(policy, windows_sha256=checksum)
            environment = {"AOBUS_STATE_ROOT": str(root), "PATH": "compiler-path"}
            with (
                mock.patch.object(compiler_cache, "load_policy", return_value=policy),
                mock.patch.object(
                    compiler_cache, "_download", side_effect=lambda _url, path: path.write_bytes(verified)
                ),
                mock.patch.object(compiler_cache, "_executable_version", return_value=policy.windows_version),
                mock.patch.object(compiler_cache, "_configured_max_size", return_value="5G"),
            ):
                compiler_cache.setup_local(environ=environment, system="Windows")
            record = json.loads(compiler_cache.config_path(root).read_text(encoding="utf-8"))
            executable = Path(record["executable"])
            wrapper = Path(environment["AOBUS_MSBUILD_CL_TOOL_EXE"])
            self.assertEqual(executable.read_bytes(), b"cache executable")
            self.assertEqual(wrapper.read_bytes(), executable.read_bytes())
            self.assertEqual(record["executableSha256"], hashlib.sha256(executable.read_bytes()).hexdigest())
            self.assertEqual(environment["CCACHE_MAXSIZE"], "20G")
            self.assertEqual(environment["PATH"], "compiler-path")

    def test_windows_download_mismatch_cannot_enable_or_write_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment = {"AOBUS_STATE_ROOT": str(root)}

            def corrupt_download(_url: str, destination: Path) -> None:
                destination.write_bytes(b"not the governed archive")

            with mock.patch.object(compiler_cache, "_download", side_effect=corrupt_download):
                with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "SHA-256"):
                    compiler_cache.setup_local(environ=environment, system="Windows")

            self.assertFalse(compiler_cache.config_path(root).exists())
            self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", environment)

    def test_windows_setup_reuses_only_a_verified_download(self):
        for cached in (True, False):
            with self.subTest(cached=cached), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                policy = compiler_cache.load_policy()
                archive = root / "cache" / "compiler-cache" / "downloads" / "ccache.zip"
                archive.parent.mkdir(parents=True)
                checksum = self.make_zip(archive, policy.windows_member)
                verified = archive.read_bytes()
                policy = replace(policy, windows_url="https://example.invalid/ccache.zip", windows_sha256=checksum)
                if not cached:
                    archive.write_bytes(b"corrupt cached download")

                with mock.patch.object(
                    compiler_cache,
                    "_download",
                    side_effect=lambda _url, target, data=verified: target.write_bytes(data),
                ) as download:
                    executable = compiler_cache._install_windows_ccache(policy, root)

                self.assertEqual(download.call_count, int(not cached))
                self.assertEqual(executable.read_bytes(), b"cache executable")
                self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), checksum)

    def test_locked_windows_executable_preserves_existing_install_and_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy = compiler_cache.load_policy()
            archive = root / "cache" / "compiler-cache" / "downloads" / "ccache.zip"
            archive.parent.mkdir(parents=True)
            checksum = self.make_zip(archive, policy.windows_member)
            policy = replace(policy, windows_url="https://example.invalid/ccache.zip", windows_sha256=checksum)
            executable = root / "tools" / "compiler-cache" / f"ccache-{policy.windows_version}" / "ccache.exe"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"old executable")
            config = compiler_cache.config_path(root)
            config.parent.mkdir(parents=True)
            config.write_text('{"cacheSize": "40G"}', encoding="utf-8")
            original_config = config.read_bytes()
            environment = {"AOBUS_STATE_ROOT": str(root)}
            with (
                mock.patch.object(compiler_cache, "load_policy", return_value=policy),
                mock.patch.object(compiler_cache.os, "replace", side_effect=PermissionError("executable is busy")),
                mock.patch.object(compiler_cache, "_download") as download,
            ):
                with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "setup failed.*executable is busy"):
                    compiler_cache.setup_local(environ=environment, system="Windows")

            download.assert_not_called()
            self.assertEqual(executable.read_bytes(), b"old executable")
            self.assertEqual(config.read_bytes(), original_config)
            self.assertEqual(environment, {"AOBUS_STATE_ROOT": str(root)})
            self.assertEqual(list(executable.parent.glob(".ccache.exe.*")), [])

    def test_setup_writes_config_only_after_version_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache"
            executable.write_bytes(b"known executable")
            environment = {"AOBUS_STATE_ROOT": str(root), "HOME": str(root)}
            with (
                mock.patch.object(compiler_cache, "_resolve_posix_ccache", return_value=executable),
                mock.patch.object(compiler_cache, "_executable_version", return_value="wrong"),
            ):
                with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "requires 4.13.6"):
                    compiler_cache.setup_local(environ=environment, system="Linux")
            self.assertFalse(compiler_cache.config_path(root).exists())

    def test_setup_preserves_larger_existing_capacity_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache"
            executable.write_bytes(b"known executable")
            config = compiler_cache.config_path(root)
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({"cacheSize": "48 GiB"}), encoding="utf-8")
            environment = {"AOBUS_STATE_ROOT": str(root), "HOME": str(root)}
            with (
                mock.patch.object(compiler_cache, "_resolve_posix_ccache", return_value=executable),
                mock.patch.object(compiler_cache, "_executable_version", return_value="4.13.6"),
                mock.patch.object(compiler_cache, "_configured_max_size", return_value="32G"),
            ):
                updates = compiler_cache.setup_local(environ=environment, system="Linux")
                first_mtime = config.stat().st_mtime_ns
                compiler_cache.setup_local(environ=environment, system="Linux")

            record = json.loads(config.read_text(encoding="utf-8"))
            self.assertEqual(record["cacheSize"], "48 GiB")
            self.assertEqual(updates["CCACHE_MAXSIZE"], "48 GiB")
            self.assertEqual(config.stat().st_mtime_ns, first_mtime)

    def test_windows_setup_remains_usable_after_removing_wrapper_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache.exe"
            executable.write_bytes(b"known executable")
            custom = root / "custom" / "cl.exe"
            custom.parent.mkdir()
            custom.write_bytes(b"custom wrapper")
            environment = {"AOBUS_STATE_ROOT": str(root), "AOBUS_MSBUILD_CL_TOOL_EXE": str(custom)}
            with (
                mock.patch.object(compiler_cache, "_install_windows_ccache", return_value=executable),
                mock.patch.object(compiler_cache, "_executable_version", return_value="4.13.6"),
                mock.patch.object(compiler_cache, "_configured_max_size", return_value="20G"),
            ):
                compiler_cache.setup_local(environ=environment, system="Windows")

            self.assertEqual(environment["AOBUS_MSBUILD_CL_TOOL_EXE"], str(custom))
            self.assertEqual(custom.read_bytes(), b"custom wrapper")
            fresh_environment = {"AOBUS_STATE_ROOT": str(root)}
            with mock.patch.object(compiler_cache, "_copy_wrapper", side_effect=AssertionError("unexpected copy")):
                self.assertTrue(compiler_cache.activate_local(environ=fresh_environment, system="Windows"))
            wrapper = Path(fresh_environment["AOBUS_MSBUILD_CL_TOOL_EXE"])
            self.assertNotEqual(wrapper, custom)
            self.assertEqual(wrapper.read_bytes(), executable.read_bytes())
            for language in ("C", "CXX"):
                self.assertEqual(fresh_environment[f"CMAKE_{language}_COMPILER_LAUNCHER"], str(executable.resolve()))

    def test_windows_wrapper_failure_cannot_publish_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache.exe"
            executable.write_bytes(b"known executable")
            environment = {"AOBUS_STATE_ROOT": str(root), "AOBUS_MSBUILD_CL_TOOL_EXE": "custom-wrapper"}
            original_environment = dict(environment)

            def fail_wrapper(path: Path, _data: bytes, *, executable: bool = False) -> None:
                if path.name == "cl.exe":
                    raise PermissionError("read only")

            with (
                mock.patch.object(compiler_cache, "_install_windows_ccache", return_value=executable),
                mock.patch.object(compiler_cache, "_executable_version", return_value="4.13.6"),
                mock.patch.object(compiler_cache, "_configured_max_size", return_value="20G"),
                mock.patch.object(compiler_cache, "_atomic_write", side_effect=fail_wrapper),
            ):
                with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "cannot prepare"):
                    compiler_cache.setup_local(environ=environment, system="Windows")

            self.assertFalse(compiler_cache.config_path(root).exists())
            self.assertEqual(environment, original_environment)


class CompilerCacheActivationTest(unittest.TestCase):
    @staticmethod
    def record(executable: Path, size: str = "20G") -> dict[str, object]:
        return {
            "schemaVersion": 1,
            "provider": "ccache",
            "version": "4.13.6",
            "executable": str(executable),
            "executableSha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "cacheSize": size,
        }

    def test_local_environment_preserves_explicit_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache"
            executable.write_bytes(b"cache")
            explicit = {
                "CCACHE_DIR": str(root / "chosen-cache"),
                "CCACHE_MAXSIZE": "64G",
                "CMAKE_CXX_COMPILER_LAUNCHER": "/opt/custom/cache",
            }
            updates = compiler_cache.local_environment(
                self.record(executable), root=root, system="Linux", environ=explicit
            )

            self.assertNotIn("CCACHE_DIR", updates)
            self.assertNotIn("CCACHE_MAXSIZE", updates)
            self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", updates)
            self.assertEqual(updates["CMAKE_C_COMPILER_LAUNCHER"], str(executable.resolve()))
            self.assertEqual(updates["AOBUS_MANAGED_C_COMPILER_LAUNCHER"], "1")
            self.assertNotIn("AOBUS_MANAGED_CXX_COMPILER_LAUNCHER", updates)

    def test_activation_preserves_empty_launcher_overrides_on_managed_trees(self):
        for system in ("Linux", "Darwin", "Windows"):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                executable = root / "ccache"
                executable.write_bytes(b"cache")
                config = compiler_cache.config_path(root)
                config.parent.mkdir()
                config.write_text(json.dumps(self.record(executable)), encoding="utf-8")
                environment = {
                    "AOBUS_STATE_ROOT": str(root),
                    "AOBUS_MSBUILD_CL_TOOL_EXE": "custom-wrapper",
                    "CMAKE_C_COMPILER_LAUNCHER": "",
                    "CMAKE_CXX_COMPILER_LAUNCHER": "",
                    "CCACHE_DIR": "",
                    "CCACHE_MAXSIZE": "",
                }
                (root / "CMakeCache.txt").write_text(
                    "CMAKE_C_COMPILER_LAUNCHER:STRING=/managed/ccache\n"
                    "AOBUS_MANAGED_C_COMPILER_LAUNCHER:BOOL=ON\n"
                    "CMAKE_CXX_COMPILER_LAUNCHER:STRING=/managed/ccache\n"
                    "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER:BOOL=ON\n",
                    encoding="utf-8",
                )

                self.assertTrue(compiler_cache.activate_local(environ=environment, system=system))
                for language in ("C", "CXX"):
                    self.assertEqual(environment[f"CMAKE_{language}_COMPILER_LAUNCHER"], "")
                    self.assertNotIn(f"AOBUS_MANAGED_{language}_COMPILER_LAUNCHER", environment)
                self.assertTrue(environment["CCACHE_DIR"])
                self.assertEqual(environment["CCACHE_MAXSIZE"], "20G")
                expected = [
                    "-DCMAKE_C_COMPILER_LAUNCHER=",
                    "-U",
                    "AOBUS_MANAGED_C_COMPILER_LAUNCHER",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=",
                    "-U",
                    "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER",
                ]
                self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=root), expected)
                # A shell can retain ownership markers after clearing a launcher.
                environment.update(AOBUS_MANAGED_C_COMPILER_LAUNCHER="1", AOBUS_MANAGED_CXX_COMPILER_LAUNCHER="1")
                self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=root), expected)

    def test_two_workspaces_share_host_cache_and_keep_distinct_base_dirs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "state"
            executable = Path(temporary) / "ccache"
            executable.write_bytes(b"cache")
            record = self.record(executable)
            first = compiler_cache.local_environment(
                record, root=root, system="Linux", environ={}, project_root=Path(temporary) / "Aobus"
            )
            second = compiler_cache.local_environment(
                record, root=root, system="Linux", environ={}, project_root=Path(temporary) / "RS2"
            )

            self.assertEqual(first["CCACHE_DIR"], second["CCACHE_DIR"])
            self.assertNotEqual(first["CCACHE_BASEDIR"], second["CCACHE_BASEDIR"])

    def test_state_roots_are_host_shared_and_honor_override(self):
        self.assertEqual(
            compiler_cache.state_root(environ={"HOME": "/users/me"}, system="Linux"),
            Path("/users/me/.cache/Aobus"),
        )
        self.assertEqual(
            compiler_cache.state_root(environ={"HOME": "/Users/me"}, system="Darwin"),
            Path("/Users/me/Library/Caches/Aobus"),
        )
        self.assertEqual(
            compiler_cache.state_root(environ={"LOCALAPPDATA": "C:/Users/me/AppData/Local"}, system="Windows"),
            Path("C:/Users/me/AppData/Local/Aobus"),
        )
        self.assertEqual(
            compiler_cache.state_root(environ={"AOBUS_STATE_ROOT": "/chosen"}, system="Linux"), Path("/chosen")
        )

    def test_mismatched_config_cannot_activate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache"
            executable.write_bytes(b"cache")
            record = self.record(executable)
            record["version"] = "4.0"
            path = compiler_cache.config_path(root)
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(record), encoding="utf-8")
            environment = {"AOBUS_STATE_ROOT": str(root)}

            with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "run ./ao setup compiler-cache"):
                compiler_cache.activate_local(environ=environment, system="Linux")
            self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", environment)

    def test_oversized_stale_config_cannot_crash_or_activate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache"
            executable.write_bytes(b"cache")
            record = self.record(executable, size="9" * 400 + "G")
            path = compiler_cache.config_path(root)
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(record), encoding="utf-8")
            environment = {"AOBUS_STATE_ROOT": str(root)}

            with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "run ./ao setup compiler-cache"):
                compiler_cache.activate_local(environ=environment, system="Linux")
            self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", environment)

    def test_absent_configuration_stays_silent(self):
        with tempfile.TemporaryDirectory() as temporary:
            environment = {"AOBUS_STATE_ROOT": temporary}
            output = io.StringIO()
            with contextlib.redirect_stderr(output):
                self.assertFalse(compiler_cache.activate_local(environ=environment, system="Linux"))
            self.assertEqual(output.getvalue(), "")
            self.assertEqual(environment, {"AOBUS_STATE_ROOT": temporary})

    def test_invalid_configuration_warns_without_stopping_the_requested_command(self):
        original_read_text = Path.read_text
        for condition in ("moved", "modified", "malformed", "encoding", "unreadable"):
            with self.subTest(condition=condition), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                executable = root / "ccache"
                executable.write_bytes(b"cache")
                path = compiler_cache.config_path(root)
                path.parent.mkdir(parents=True)
                path.write_text(json.dumps(self.record(executable)), encoding="utf-8")
                if condition == "moved":
                    executable.rename(root / "new-ccache")
                elif condition == "modified":
                    executable.write_bytes(b"changed executable")
                elif condition == "malformed":
                    path.write_text("invalid json", encoding="utf-8")
                elif condition == "encoding":
                    path.write_bytes(b"\xff")
                output = io.StringIO()
                environment = {"AOBUS_STATE_ROOT": str(root)}
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.dict(os.environ, environment, clear=True))
                    stack.enter_context(mock.patch.object(compiler_cache.platform, "system", return_value="Linux"))
                    command = stack.enter_context(mock.patch.object(test_command, "run_command", return_value=0))
                    stack.enter_context(contextlib.redirect_stderr(output))
                    if condition == "unreadable":

                        def read_text(requested_path, *args, unreadable_path=path, **kwargs):
                            if requested_path == unreadable_path:
                                raise PermissionError("denied")
                            return original_read_text(requested_path, *args, **kwargs)

                        stack.enter_context(mock.patch.object(Path, "read_text", read_text))
                    self.assertEqual(portal_main(["test", "--core"]), 0)
                    self.assertEqual(dict(os.environ), environment)
                command.assert_called_once()
                self.assertIn("Warning: compiler cache is disabled", output.getvalue())
                self.assertIn("run ./ao setup compiler-cache", output.getvalue())
                self.assertNotIn("Traceback", output.getvalue())

    def test_windows_stale_configuration_names_the_native_setup_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = compiler_cache.config_path(root)
            path.parent.mkdir(parents=True)
            path.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "run ao.bat setup compiler-cache"):
                compiler_cache.activate_local(environ={"AOBUS_STATE_ROOT": str(root)}, system="Windows")

    def test_windows_environment_selects_versioned_wrapper_without_provisioning(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ccache.exe"
            executable.write_bytes(b"cache")
            environment = {"PATH": "C:/VisualStudio/VC/bin"}
            updates = compiler_cache.local_environment(
                self.record(executable), root=root, system="Windows", environ=environment
            )
            wrapper = Path(updates["AOBUS_MSBUILD_CL_TOOL_EXE"])

            self.assertEqual(wrapper.name, "cl.exe")
            self.assertIn("ccache-4.13.6", str(wrapper))
            self.assertFalse(wrapper.exists())
            self.assertNotIn("PATH", updates)
            self.assertEqual(environment["PATH"], "C:/VisualStudio/VC/bin")

    def test_windows_activation_is_read_only_and_rejects_stale_wrappers(self):
        for condition in ("valid", "missing", "modified", "explicit"):
            with self.subTest(condition=condition), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                executable = root / "ccache.exe"
                executable.write_bytes(b"cache")
                record = self.record(executable)
                config = compiler_cache.config_path(root)
                config.parent.mkdir()
                config.write_text(json.dumps(record), encoding="utf-8")
                wrapper = root / "tools" / "compiler-cache" / "ccache-4.13.6" / "msbuild" / "cl.exe"
                if condition in {"valid", "modified"}:
                    wrapper.parent.mkdir(parents=True)
                    wrapper.write_bytes(b"cache")
                    original_stat = wrapper.stat()
                    if condition == "modified":
                        wrapper.write_bytes(b"CACHE")
                        os.utime(wrapper, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
                environment = {"AOBUS_STATE_ROOT": str(root)}
                if condition == "explicit":
                    environment["AOBUS_MSBUILD_CL_TOOL_EXE"] = "custom-wrapper"
                original_environment = dict(environment)
                with (
                    mock.patch.object(compiler_cache, "_copy_wrapper", side_effect=AssertionError("unexpected copy")),
                    mock.patch.object(compiler_cache, "_atomic_write", side_effect=AssertionError("unexpected write")),
                    mock.patch.object(Path, "chmod", side_effect=AssertionError("unexpected chmod")),
                ):
                    if condition in {"valid", "explicit"}:
                        self.assertTrue(compiler_cache.activate_local(environ=environment, system="Windows"))
                    else:
                        with self.assertRaisesRegex(
                            compiler_cache.CompilerCacheError,
                            "wrapper is missing or changed.*ao.bat setup compiler-cache",
                        ):
                            compiler_cache.activate_local(environ=environment, system="Windows")
                        self.assertEqual(environment, original_environment)
                if condition == "valid":
                    self.assertEqual(environment["AOBUS_MSBUILD_CL_TOOL_EXE"], str(wrapper.resolve()))
                    self.assertEqual(wrapper.stat().st_mtime_ns, original_stat.st_mtime_ns)
                elif condition == "explicit":
                    self.assertEqual(environment["AOBUS_MSBUILD_CL_TOOL_EXE"], "custom-wrapper")
                    self.assertFalse(wrapper.exists())
                elif condition == "missing":
                    self.assertFalse(wrapper.exists())
                else:
                    self.assertEqual(wrapper.read_bytes(), b"CACHE")

    def test_activation_follows_exact_platform_pins_and_homebrew_minimum(self):
        for system in ("Linux", "Windows", "Darwin"):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                executable = root / "ccache"
                executable.write_bytes(b"cache")
                record = self.record(executable)
                record["version"] = "4.14"
                config = compiler_cache.config_path(root)
                config.parent.mkdir()
                config.write_text(json.dumps(record), encoding="utf-8")
                environment = {"AOBUS_STATE_ROOT": str(root)}
                if system == "Darwin":
                    self.assertTrue(compiler_cache.activate_local(environ=environment, system=system))
                else:
                    with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "requires exactly 4.13.6"):
                        compiler_cache.activate_local(environ=environment, system=system)
                    self.assertEqual(environment, {"AOBUS_STATE_ROOT": str(root)})

    def test_platform_pin_update_invalidates_a_still_present_saved_executable(self):
        for system in ("Linux", "Windows"):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                executable = root / "ccache"
                executable.write_bytes(b"cache")
                config = compiler_cache.config_path(root)
                config.parent.mkdir()
                config.write_text(json.dumps(self.record(executable)), encoding="utf-8")
                policy = replace(compiler_cache.load_policy(), linux_version="4.14", windows_version="4.14")
                environment = {"AOBUS_STATE_ROOT": str(root)}
                with mock.patch.object(compiler_cache, "load_policy", return_value=policy):
                    with self.assertRaisesRegex(compiler_cache.CompilerCacheError, "requires exactly 4.14"):
                        compiler_cache.activate_local(environ=environment, system=system)
                self.assertEqual(environment, {"AOBUS_STATE_ROOT": str(root)})

    def test_ci_windows_adapter_provisions_exe_fallback_only_in_supplied_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "sccache.exe"
            executable.write_bytes(b"ci cache")
            state = root / "runner-temp"
            long_lived = root / "managed-tools"
            environment = {"SCCACHE_PATH": str(root / "sccache"), "AOBUS_STATE_ROOT": str(long_lived)}
            updates = compiler_cache.ci_environment(environ=environment, system="Windows", state=state)
            wrapper = Path(updates["AOBUS_MSBUILD_CL_TOOL_EXE"])
            self.assertTrue(wrapper.is_relative_to(state.resolve()))
            self.assertEqual(wrapper.read_bytes(), executable.read_bytes())
            self.assertEqual(updates["SCCACHE_PATH"], str(executable.resolve()))
            self.assertEqual(updates["CMAKE_CXX_COMPILER_LAUNCHER"], str(executable.resolve()))
            self.assertFalse(long_lived.exists())

    def test_ci_adapter_preserves_backend_tokens_capacity_and_launcher(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "sccache"
            executable.write_bytes(b"cache")
            environment = {
                "SCCACHE_PATH": str(executable),
                "SCCACHE_GHA_ENABLED": "true",
                "ACTIONS_CACHE_URL": "secret endpoint",
                "ACTIONS_RUNTIME_TOKEN": "secret token",
                "CMAKE_C_COMPILER_LAUNCHER": "/external/launcher",
                "SCCACHE_CACHE_SIZE": "80G",
            }
            updates = compiler_cache.ci_environment(environ=environment, system="Linux", state=root)

            for preserved in (
                "SCCACHE_GHA_ENABLED",
                "ACTIONS_CACHE_URL",
                "ACTIONS_RUNTIME_TOKEN",
                "CMAKE_C_COMPILER_LAUNCHER",
                "SCCACHE_CACHE_SIZE",
            ):
                self.assertNotIn(preserved, updates)
            self.assertEqual(updates["CMAKE_CXX_COMPILER_LAUNCHER"], str(executable.resolve()))
            self.assertEqual(environment["ACTIONS_RUNTIME_TOKEN"], "secret token")

    def test_ci_adapter_preserves_empty_launchers(self):
        for system in ("Linux", "Darwin", "Windows"):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                executable = root / "sccache.exe"
                executable.write_bytes(b"cache")
                environment = {
                    "SCCACHE_PATH": str(executable),
                    "CMAKE_C_COMPILER_LAUNCHER": "",
                    "CMAKE_CXX_COMPILER_LAUNCHER": "",
                }
                updates = compiler_cache.ci_environment(environ=environment, system=system, state=root)
                for language in ("C", "CXX"):
                    self.assertNotIn(f"CMAKE_{language}_COMPILER_LAUNCHER", updates)
                    self.assertNotIn(f"AOBUS_MANAGED_{language}_COMPILER_LAUNCHER", updates)
                self.assertEqual(
                    compiler_cache.cmake_launcher_arguments({**environment, **updates}),
                    ["-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER="],
                )

    def test_cmake_arguments_rebind_existing_tree(self):
        self.assertEqual(compiler_cache.cmake_launcher_arguments({}), [])
        self.assertEqual(
            compiler_cache.cmake_launcher_arguments(
                {"CMAKE_C_COMPILER_LAUNCHER": "/cache/c", "CMAKE_CXX_COMPILER_LAUNCHER": "/cache/cxx"}
            ),
            ["-DCMAKE_C_COMPILER_LAUNCHER=/cache/c", "-DCMAKE_CXX_COMPILER_LAUNCHER=/cache/cxx"],
        )

    def test_cmake_arguments_clear_only_stale_managed_launchers(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_C_COMPILER_LAUNCHER:FILEPATH=/managed/ccache\n"
                "AOBUS_MANAGED_C_COMPILER_LAUNCHER:BOOL=ON\n"
                "CMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/explicit/cache\n",
                encoding="utf-8",
            )

            self.assertEqual(
                compiler_cache.cmake_launcher_arguments({}, build_dir=build),
                ["-U", "CMAKE_C_COMPILER_LAUNCHER", "-U", "AOBUS_MANAGED_C_COMPILER_LAUNCHER"],
            )

    def test_cmake_arguments_adopt_tree_with_legacy_automatic_discovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            # Legacy discovery cached CCACHE_PROGRAM but set launchers only as
            # normal CMake variables, so they are absent from CMakeCache.txt.
            (build / "CMakeCache.txt").write_text(
                "USE_CCACHE:BOOL=ON\nCCACHE_PROGRAM:FILEPATH=/retired/ccache\n",
                encoding="utf-8",
            )
            environment = {
                "CMAKE_C_COMPILER_LAUNCHER": "/managed/ccache",
                "CMAKE_CXX_COMPILER_LAUNCHER": "/managed/ccache",
                "AOBUS_MANAGED_C_COMPILER_LAUNCHER": "1",
                "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER": "1",
            }

            self.assertEqual(
                compiler_cache.cmake_launcher_arguments(environment, build_dir=build),
                [
                    "-DCMAKE_C_COMPILER_LAUNCHER=/managed/ccache",
                    "-DAOBUS_MANAGED_C_COMPILER_LAUNCHER:BOOL=ON",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=/managed/ccache",
                    "-DAOBUS_MANAGED_CXX_COMPILER_LAUNCHER:BOOL=ON",
                ],
            )

    def test_cmake_arguments_do_not_reconfigure_matching_managed_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_C_COMPILER_LAUNCHER:FILEPATH=/managed/ccache\nAOBUS_MANAGED_C_COMPILER_LAUNCHER:BOOL=ON\n",
                encoding="utf-8",
            )
            environment = {
                "CMAKE_C_COMPILER_LAUNCHER": "/managed/ccache",
                "AOBUS_MANAGED_C_COMPILER_LAUNCHER": "1",
            }

            self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=build), [])

    def test_cmake_arguments_preserve_explicit_cached_launcher(self):
        for name in ("launcher", "ccache", "sccache", "ccache.exe"):
            for present in (False, True):
                with self.subTest(name=name, present=present), tempfile.TemporaryDirectory() as temporary:
                    build = Path(temporary)
                    launcher = build / name
                    if present:
                        launcher.touch()
                    (build / "CMakeCache.txt").write_text(
                        "USE_CCACHE:BOOL=ON\n"
                        f"CCACHE_PROGRAM:FILEPATH={launcher}\n"
                        f"CMAKE_C_COMPILER_LAUNCHER:FILEPATH={launcher}\n",
                        encoding="utf-8",
                    )
                    managed_environment = {
                        "CMAKE_C_COMPILER_LAUNCHER": "/managed/ccache",
                        "AOBUS_MANAGED_C_COMPILER_LAUNCHER": "1",
                    }

                    self.assertEqual(compiler_cache.cmake_launcher_arguments(managed_environment, build_dir=build), [])

    def test_cmake_arguments_preserve_empty_unmanaged_cached_launchers(self):
        for marker_value in (None, "OFF"):
            with self.subTest(marker_value=marker_value), tempfile.TemporaryDirectory() as temporary:
                build = Path(temporary)
                entries = []
                environment = {}
                for language in ("C", "CXX"):
                    key = f"CMAKE_{language}_COMPILER_LAUNCHER"
                    marker = f"AOBUS_MANAGED_{language}_COMPILER_LAUNCHER"
                    entries.append(f"{key}:STRING=\n")
                    if marker_value is not None:
                        entries.append(f"{marker}:BOOL={marker_value}\n")
                    environment[key] = "/managed/ccache"
                    environment[marker] = "1"
                (build / "CMakeCache.txt").write_text("".join(entries), encoding="utf-8")

                self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=build), [])

    def test_cmake_arguments_normalize_windows_launcher_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=C:/Cache/ccache.exe\n"
                "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER:BOOL=ON\n",
                encoding="utf-8",
            )
            environment = {
                "CMAKE_CXX_COMPILER_LAUNCHER": r"c:\Cache\ccache.exe",
                "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER": "1",
            }

            self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=build), [])


class CompilerCacheCliTest(unittest.TestCase):
    def test_setup_reports_only_preexisting_environment_overrides(self):
        for explicit in (False, True):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory() as temporary:
                output = io.StringIO()
                environment = {"CCACHE_DIR": "chosen-store", "CCACHE_MAXSIZE": "10G"} if explicit else {}
                environment["AOBUS_STATE_ROOT"] = temporary
                updates = {"CMAKE_CXX_COMPILER_LAUNCHER": "managed-ccache"}
                if not explicit:
                    updates.update(CCACHE_DIR="managed-store", CCACHE_MAXSIZE="20G")
                with (
                    mock.patch.dict(os.environ, environment, clear=True),
                    mock.patch.object(compiler_cache, "setup_local", return_value=updates),
                    contextlib.redirect_stdout(output),
                ):
                    self.assertEqual(portal_main(["setup", "compiler-cache"]), 0)
                if explicit:
                    self.assertIn("Cache directory: chosen-store", output.getvalue())
                    self.assertIn("Cache capacity: 10G", output.getvalue())
                    self.assertIn("Notice: using CCACHE_DIR override from environment", output.getvalue())
                    self.assertIn("Notice: using CCACHE_MAXSIZE override from environment", output.getvalue())
                else:
                    self.assertIn("Cache capacity: 20G", output.getvalue())
                    self.assertNotIn("Notice:", output.getvalue())

    def test_setup_filesystem_failure_is_a_concise_command_error(self):
        output = io.StringIO()
        with (
            mock.patch.object(compiler_cache.platform, "system", return_value="Windows"),
            mock.patch.object(
                compiler_cache, "_install_windows_ccache", side_effect=PermissionError("state is read-only")
            ),
            contextlib.redirect_stderr(output),
        ):
            with self.assertRaises(SystemExit) as failure:
                portal_main(["setup", "compiler-cache"])
        self.assertEqual(failure.exception.code, 1)
        self.assertIn("compiler-cache setup failed: state is read-only", output.getvalue())
        self.assertNotIn("Traceback", output.getvalue())

    def test_ci_environment_write_failure_is_a_concise_command_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "sccache"
            executable.write_bytes(b"cache")
            output = io.StringIO()
            policy = compiler_cache.load_policy()
            with (
                mock.patch.dict(os.environ, {"SCCACHE_PATH": str(executable)}, clear=True),
                mock.patch.object(compiler_cache, "load_policy", return_value=policy),
                mock.patch.object(Path, "open", side_effect=PermissionError("environment file is read-only")),
                contextlib.redirect_stderr(output),
            ):
                status = compiler_cache.main(
                    ["activate-ci", "--github-env", str(root / "env"), "--state-root", str(root), "--platform", "Linux"]
                )
            self.assertEqual(status, 1)
            self.assertIn("Error: environment file is read-only", output.getvalue())
            self.assertNotIn("Traceback", output.getvalue())

    def test_ci_contract_emits_action_version(self):
        output = io.StringIO()
        with mock.patch("sys.stdout", output):
            status = compiler_cache.main(["ci-contract"])
        self.assertEqual(status, 0)
        self.assertEqual(output.getvalue(), "version=v0.17.0\n")

    def test_github_environment_writer_rejects_multiline_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "github-env"
            destination.touch()
            with self.assertRaises(compiler_cache.CompilerCacheError):
                compiler_cache._write_github_environment(destination, {"VALUE": "one\ntwo"})


if __name__ == "__main__":
    unittest.main()
