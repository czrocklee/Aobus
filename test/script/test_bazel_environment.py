"""Ready-only host discovery, independent of target compilation mode."""

import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_environment as bootstrap
from ao.core import bazel_native as native
from ao.core import builddir

ROOT = Path(__file__).resolve().parents[2]


class EnvironmentDiscoveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-host-environment-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "source"
        self.output = self.root / "repository/environment.json"
        self.store = self.root / "store"
        self.python = self.file("store/python/bin/python3")
        self.report = self.file(
            "store/report/dependencies.json",
            json.dumps(
                {
                    "schemaVersion": 1,
                    "nixpkgsRevision": "accepted-revision",
                }
            ),
        )
        self.policy = json.loads((ROOT / "script/ao/toolchain.json").read_text(encoding="utf-8"))
        self.mac_policy = json.loads((ROOT / "script/ao/macos-toolchain.json").read_text(encoding="utf-8"))
        self.source_file("script/ao/toolchain.json", json.dumps(self.policy))
        self.source_file("script/ao/macos-toolchain.json", json.dumps(self.mac_policy))
        self.source_file("nixpkgs.json", json.dumps({"rev": "accepted-revision"}))
        for script in ("windows-build-env.bat", "windows-vsenv.bat", "macos-vcpkg-bootstrap.sh"):
            self.source_file("script/ao/" + script)
        self.environment = {"PATH": "/no/ambient/tools", "AOBUS_NIX_DEPENDENCY_REPORT": str(self.report)}
        self.host = "linux"
        self.addCleanup(patch.stopall)
        patch.object(bootstrap, "PROJECT_ROOT", self.source).start()
        # Admit the same local fixture even when tooling runs from a checkout
        # that production Windows Bazel cannot build (for example, SMB).
        patch.object(native, "PROJECT_ROOT", self.source).start()
        patch.object(bootstrap, "_NIX_STORE", self.store).start()
        patch.object(bootstrap.sys, "executable", str(self.python)).start()
        patch.object(bootstrap.sys, "version_info", tuple(map(int, self.policy["python"].split(".")))).start()

    def file(self, relative, content="native fixture\n"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def source_file(self, relative, content="fixture\n"):
        return self.file("source/" + relative, content)

    def request(self, **fields):
        request = {"schema": 1, "host": self.host, "source": str(self.source), **fields}
        return self.file("request.json", json.dumps(request))

    def discover(self, *, request=None, environment=None):
        platform = {"linux": "linux", "macos": "darwin", "windows": "win32"}[self.host]
        with patch.object(bootstrap.sys, "platform", platform):
            return bootstrap.discover_environment(
                output=self.output,
                request=request,
                environment=self.environment if environment is None else environment,
            )

    def native_prefix(self, triplet):
        prefix = self.root / "installed" / triplet
        (prefix / "include").mkdir(parents=True, exist_ok=True)
        self.file("installed/vcpkg/status", "installed status is validated by the projection\n")
        self.environment["AOBUS_BAZEL_VCPKG_PREFIX"] = str(prefix)
        return prefix

    def macos(self):
        self.host = "macos"
        self.prefix = self.native_prefix("arm64-aobus-osx")
        self.llvm = self.root / "llvm"
        self.file("llvm/bin/clang++")
        self.file("llvm/include/c++/v1/__expected/expected.h")
        self.file("shim/__expected/expected.h")
        self.sdk = self.root / "Developer/SDKs/MacOSX.sdk"
        self.file("Developer/SDKs/MacOSX.sdk/SDKSettings.json", "{}")
        for directory in ("usr/include", "usr/lib"):
            (self.sdk / directory).mkdir(parents=True)
        self.linker = self.file("Developer/usr/bin/ld")
        pkgconf = self.file("tools/pkgconf")
        pkgconf.chmod(0o755)
        self.environment.update(
            {
                "PATH": str(pkgconf.parent),
                "AOBUS_LLVM_ROOT": str(self.llvm),
                "AOBUS_LIBCXX_EXPECTED_SHIM": str(self.root / "shim"),
                "AOBUS_VCPKG_TRIPLET": self.prefix.name,
                "SDKROOT": str(self.sdk),
                "DEVELOPER_DIR": str(self.root / "Developer"),
            }
        )
        self.queries = []

    def mac_query(self, command, **kwargs):
        self.queries.append(command)
        self.assertNotIn("SECRET_TOKEN", kwargs["env"])
        answers = {
            (str(self.llvm / "bin/clang++"), "-dumpversion"): self.mac_policy["homebrew"]["llvmMajorVersion"] + ".1.2",
            ("/usr/bin/xcrun", "--show-sdk-path"): str(self.sdk),
            ("/usr/bin/xcrun", "--find", "ld"): str(self.linker),
        }
        self.assertIn(tuple(command), answers, "unexpected native query")
        return subprocess.CompletedProcess(command, 0, answers[tuple(command)] + "\n", "")

    def windows(self):
        self.host = "windows"
        self.prefix = self.native_prefix("x64-windows")
        self.vc = self.root / "VS/VC/Tools/MSVC/14.44.35207"
        self.sdk = self.root / "Windows Kits/10"
        self.compiler = {
            "INCLUDE": ";".join(
                str(path)
                for path in (
                    self.vc / "include",
                    self.sdk / "Include/10.0.26100.0/um",
                    self.sdk / "Include/10.0.26100.0/ucrt",
                )
            ),
            "LIB": ";".join(
                str(path)
                for path in (
                    self.vc / "lib/x64",
                    self.sdk / "Lib/10.0.26100.0/um/x64",
                    self.sdk / "Lib/10.0.26100.0/ucrt/x64",
                )
            ),
            "PATH": str(self.vc / "bin/Hostx64/x64"),
            "TMP": str(self.root / "legacy-tmp"),
            "VCToolsInstallDir": str(self.vc),
            "WindowsSdkDir": str(self.sdk),
            "WindowsSDKVersion": "10.0.26100.0\\",
            "VCToolsVersion": "14.44.35207",
            "UCRTVersion": "10.0.26100.0",
        }
        for directory in (*self.compiler["INCLUDE"].split(";"), *self.compiler["LIB"].split(";"), self.compiler["TMP"]):
            Path(directory).mkdir(parents=True, exist_ok=True)
        for role in ("cl", "link", "lib", "ml64", "dumpbin"):
            self.file(f"VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/{role}.exe")
        self.file("VS/VC/Auxiliary/Build/vcvarsall.bat")
        for relative in (
            "bin/10.0.26100.0/x64/mt.exe",
            "Include/10.0.26100.0/um/Windows.h",
            "Include/10.0.26100.0/ucrt/corecrt.h",
        ):
            self.file("Windows Kits/10/" + relative)
        self.command = self.file("Windows/System32/cmd.exe")
        self.environment.update(
            {
                **self.compiler,
                "COMSPEC": str(self.command),
                "SYSTEMROOT": str(self.root / "Windows"),
                "VSINSTALLDIR": str(self.root / "VS"),
                "BAZEL_VC": str(self.root / "VS/VC"),
                "BAZEL_VC_FULL_VERSION": "14.44.35207",
                "BAZEL_WINSDK_FULL_VERSION": "10.0.26100.0",
            }
        )

    def test_linux_is_mode_neutral_and_preserves_lazy_sdk_selectors_without_credentials(self):
        self.environment.update(
            {
                "AOBUS_LINT_CLANGXX": "/selected/clang++",
                "NIXPKGS_CMAKE_PREFIX_PATH": "/selected/metadata",
                "AOBUS_LLVM_SDK_ROOT": "/ready/lazy/sdk",
                "BAZEL_SH": "/selected/bash",
                "SECRET_TOKEN": "private",
                "CXXFLAGS": "-unreviewed",
                "PYTHONPATH": "/ambient",
            }
        )
        first = self.discover(request=self.request(config="debug", prefix="ignored-on-linux"))
        second = self.discover(request=self.request(config="not-a-mode", prefix=None))
        self.assertEqual(first, second)
        self.assertNotIn("config", first)
        self.assertIsNone(first["prefix"])
        self.assertEqual(first["source"], str(self.source))
        for key in ("AOBUS_LINT_CLANGXX", "NIXPKGS_CMAKE_PREFIX_PATH", "AOBUS_LLVM_SDK_ROOT", "BAZEL_SH"):
            self.assertEqual(first["environment"][key], self.environment[key])
        for key in ("SECRET_TOKEN", "CXXFLAGS", "PYTHONPATH"):
            self.assertNotIn(key, first["environment"])
        self.assertIn(str(self.report), first["watch_files"])
        self.assertIn(str(self.source / "script/ao/toolchain.json"), first["watch_files"])
        self.assertFalse(self.output.exists(), "the discovery API does not publish")

    def test_explicit_mapping_never_inherits_ambient_selection(self):
        with patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(self.report), "AOBUS_LINT_CLANG": "/ambient"}):
            with self.assertRaisesRegex(bootstrap.EnvironmentError, "AOBUS_NIX_DEPENDENCY_REPORT"):
                self.discover(environment={})
            result = self.discover()
        self.assertNotIn("AOBUS_LINT_CLANG", result["environment"])

    def test_omitted_mapping_filters_the_prepared_child_environment(self):
        # Pin the fixture host exactly like discover(): the omitted mapping is
        # the subject, and an unpinned native Windows host would run real VS
        # discovery against machine state instead of the fixture environment.
        with (
            patch.dict(os.environ, {**self.environment, "SECRET_TOKEN": "private"}, clear=True),
            patch.object(bootstrap.sys, "platform", "linux"),
        ):
            result = bootstrap.discover_environment(output=self.output)
        self.assertEqual(result["environment"]["AOBUS_NIX_DEPENDENCY_REPORT"], str(self.report))
        self.assertNotIn("SECRET_TOKEN", result["environment"])

    def test_one_pin_rule_admits_only_a_current_schema_report_from_the_pinned_revision(self):
        # The same rule gates this helper and the resolvers' bazel_providers.nix_report.
        report = {"schemaVersion": 1, "nixpkgsRevision": "accepted-revision"}
        self.assertEqual(bootstrap.pinned_nix_revision(report, {"rev": "accepted-revision"}), "accepted-revision")
        for changed, pin in (
            ({"schemaVersion": 2}, {"rev": "accepted-revision"}),
            ({"nixpkgsRevision": "stale"}, {"rev": "accepted-revision"}),
            ({"nixpkgsRevision": None}, {}),
            ({"nixpkgsRevision": ""}, {"rev": ""}),
        ):
            with self.subTest(changed=changed, pin=pin):
                self.assertIsNone(bootstrap.pinned_nix_revision({**report, **changed}, pin))

    def test_python_version_comes_from_the_existing_policy(self):
        self.source_file("script/ao/toolchain.json", json.dumps({**self.policy, "python": "0.0.0"}))
        with self.assertRaisesRegex(bootstrap.EnvironmentError, "selected Python"):
            self.discover()

    def test_macos_uses_prepared_sdk_and_existing_triplet_with_patch_flexible_python(self):
        self.macos()
        components = self.policy["python"].split(".")
        self.source_file(
            "script/ao/toolchain.json", json.dumps({**self.policy, "python": ".".join([*components[:2], "999"])})
        )
        with patch.object(bootstrap.subprocess, "run", side_effect=self.mac_query):
            result = self.discover()
        self.assertEqual(result["prefix"], str(self.prefix))
        self.assertEqual(result["environment"]["SDKROOT"], str(self.sdk))
        self.assertIn(str(self.linker), result["watch_files"])
        self.assertIn(str(self.sdk / "SDKSettings.plist"), result["watch_files"])
        self.assertNotIn("compiler_environment", result)

    def test_macos_rejects_an_xcrun_fallback_instead_of_accepting_another_sdk(self):
        self.macos()
        other = self.root / "Other.sdk"
        other.mkdir()
        original = self.sdk

        def query(command, **kwargs):
            result = self.mac_query(command, **kwargs)
            if command == ["/usr/bin/xcrun", "--show-sdk-path"]:
                result.stdout = str(other)
            return result

        with patch.object(bootstrap.subprocess, "run", side_effect=query):
            with self.assertRaisesRegex(bootstrap.EnvironmentError, "no fallback"):
                self.discover()
        self.assertEqual(self.sdk, original)
        self.assertNotIn(["/usr/bin/xcrun", "--find", "ld"], self.queries)

    def test_macos_missing_explicit_sdk_is_not_installed_or_reselected(self):
        self.macos()
        selected = self.root / "absent.sdk"
        self.environment["SDKROOT"] = str(selected)
        with patch.object(bootstrap.subprocess, "run", side_effect=self.mac_query):
            with self.assertRaisesRegex(bootstrap.EnvironmentError, "missing"):
                self.discover()
        self.assertFalse(selected.exists())
        self.assertEqual(len(self.queries), 1)

    def test_windows_bare_preserves_original_tuple_case_insensitively_without_reselection(self):
        self.windows()
        self.environment["SECRET_TOKEN"] = "private-input"
        self.environment = {key.swapcase(): value for key, value in self.environment.items()}
        previous = dict(os.environ)
        with patch.object(bootstrap.subprocess, "run", side_effect=AssertionError("unexpected VS selection")):
            result = self.discover()
        self.assertEqual(dict(os.environ), previous)
        self.assertEqual(result["compiler_environment"]["TMP"], str(self.output.parent / "_tmp"))
        self.assertEqual(result["compiler_environment"]["VCToolsInstallDir"], str(self.vc))
        self.assertEqual(result["environment"]["BAZEL_VC"], str(self.root / "VS/VC"))
        self.assertEqual(result["environment"]["BAZEL_VC_FULL_VERSION"], "14.44.35207")
        self.assertEqual(result["environment"]["BAZEL_WINSDK_FULL_VERSION"], "10.0.26100.0")
        self.assertNotIn("SECRET_TOKEN", result["environment"])
        self.assertIn(str(self.root / "VS/VC/Auxiliary/Build/vcvarsall.bat"), result["watch_files"])
        self.assertIn(str(self.vc.parent), result["watch_directories"])

    def test_windows_bare_rejects_a_selector_conflict_without_native_reselection(self):
        # One representative selector: all three run through the same generic
        # explicit-selector conflict loop in windows_compiler_selection.
        self.windows()
        with (
            patch.object(bootstrap.subprocess, "run", side_effect=AssertionError("unexpected native command")),
            self.assertRaisesRegex(bootstrap.EnvironmentError, "BAZEL_VC disagrees"),
        ):
            self.discover(environment={**self.environment, "BAZEL_VC": str(self.root / "Other/VC")})

    def test_portal_windows_compiler_environment_needs_no_helper_tuple_or_reselection(self):
        self.windows()
        environment = {"AOBUS_BAZEL_VCPKG_PREFIX": str(self.prefix)}
        with patch.object(bootstrap.subprocess, "run", side_effect=AssertionError("unexpected native command")):
            result = self.discover(
                request=self.request(compiler_environment={**self.compiler, "SECRET_TOKEN": "private"}),
                environment=environment,
            )
        self.assertEqual(result["compiler_environment"], self.compiler)
        self.assertEqual(result["environment"]["WindowsSdkDir"], str(self.sdk))
        self.assertNotEqual(result["environment"]["TMP"], self.compiler["TMP"])

    def test_portal_windows_compiler_environment_retains_matching_tuple_and_rejects_conflicts(self):
        self.windows()
        request = self.request(compiler_environment={key.swapcase(): value for key, value in self.compiler.items()})
        result = self.discover(request=request)
        self.assertEqual(result["compiler_environment"], self.compiler)
        self.assertEqual(result["environment"]["BAZEL_WINSDK_FULL_VERSION"], "10.0.26100.0")
        with self.assertRaisesRegex(bootstrap.EnvironmentError, "BAZEL_WINSDK_FULL_VERSION disagrees"):
            self.discover(
                request=request, environment={**self.environment, "BAZEL_WINSDK_FULL_VERSION": "10.0.22621.0"}
            )

    def test_bad_explicit_prefix_never_falls_back_to_valid_legacy_prefix(self):
        self.windows()
        self.environment["AOBUS_BAZEL_VCPKG_PREFIX"] = "relative-invalid"
        with self.assertRaisesRegex(bootstrap.EnvironmentError, "absolute"):
            self.discover(request=self.request(prefix=str(self.prefix), compiler_environment=self.compiler))

    def test_permission_errors_are_not_reported_as_missing_sdk(self):
        self.macos()
        original_stat = Path.stat
        selected = self.sdk / "SDKSettings.json"

        def stat_file(path, *args, **kwargs):
            if path == selected:
                raise PermissionError("permission-denied-fixture")
            return original_stat(path, *args, **kwargs)

        with (
            patch.object(Path, "stat", stat_file),
            patch.object(bootstrap.subprocess, "run", side_effect=self.mac_query),
        ):
            with self.assertRaises(PermissionError):
                self.discover()

    def test_cli_publishes_only_after_success_and_redacts_conflicting_selector_values(self):
        self.windows()
        self.output.parent.mkdir(parents=True)
        with patch.object(bootstrap.sys, "platform", "win32"):
            self.assertEqual(
                bootstrap.main(["--output", str(self.output), "--environment-json", json.dumps(self.environment)]), 0
            )
        self.assertEqual(json.loads(self.output.read_text(encoding="utf-8"))["host"], "windows")
        self.environment["BAZEL_WINSDK_FULL_VERSION"] = "private-invalid-selector"
        stderr = io.StringIO()
        with patch.object(bootstrap.sys, "platform", "win32"), contextlib.redirect_stderr(stderr):
            status = bootstrap.main(["--output", str(self.output), "--environment-json", json.dumps(self.environment)])
        self.assertEqual(status, 1)
        self.assertFalse(self.output.exists())
        self.assertIn("BAZEL_WINSDK_FULL_VERSION", stderr.getvalue())
        self.assertNotIn("private", stderr.getvalue())

    def test_interruption_removes_stale_success_and_never_publishes(self):
        self.windows()
        self.output.parent.mkdir(parents=True)
        self.output.write_text("old success", encoding="utf-8")
        stderr = io.StringIO()
        with (
            patch.object(bootstrap.sys, "platform", "win32"),
            contextlib.redirect_stderr(stderr),
            patch.object(bootstrap, "_compiler_environment", side_effect=KeyboardInterrupt),
        ):
            status = bootstrap.main(["--output", str(self.output), "--environment-json", json.dumps(self.environment)])
        self.assertEqual(status, 130)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.output.with_suffix(".json.tmp").exists())

    def test_lexical_python_and_prefix_aliases_watch_their_resolved_inputs(self):
        self.windows()
        python_alias = self.root / "python-alias"
        prefix_alias = self.root / "prefix-alias"
        try:
            python_alias.symlink_to(self.python)
            prefix_alias.symlink_to(self.prefix, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"native symlinks unavailable: {type(error).__name__}")
        self.environment["AOBUS_BAZEL_VCPKG_PREFIX"] = str(prefix_alias)
        with patch.object(bootstrap.sys, "executable", str(python_alias)):
            result = self.discover(request=self.request(compiler_environment=self.compiler))
        self.assertEqual(result["prefix"], str(self.prefix))
        self.assertEqual(result["python"], str(python_alias))
        for path in (python_alias, self.python, prefix_alias, self.prefix):
            self.assertIn(str(path), result["watch_files"])

    def test_native_timeout_is_redacted_and_does_not_publish_success(self):
        self.macos()
        stderr = io.StringIO()
        with (
            patch.object(bootstrap.sys, "platform", "darwin"),
            contextlib.redirect_stderr(stderr),
            patch.object(
                bootstrap.subprocess,
                "run",
                side_effect=subprocess.TimeoutExpired("private-command", 120, output=b"private"),
            ),
        ):
            status = bootstrap.main(["--output", str(self.output), "--environment-json", json.dumps(self.environment)])
        self.assertEqual(status, 1)
        self.assertFalse(self.output.exists())
        self.assertIn("TimeoutExpired", stderr.getvalue())
        self.assertNotIn("private", stderr.getvalue())

    def test_invalid_explicit_json_is_redacted(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            status = bootstrap.main(["--output", str(self.output), "--environment-json", "private-secret-invalid-json"])
        self.assertEqual(status, 1)
        self.assertNotIn("private-secret", stderr.getvalue())
        self.assertFalse(self.output.exists())


class WindowsCompilerSelectionTests(unittest.TestCase):
    def setUp(self):
        self.environment = {
            "VSINSTALLDIR": "C:\\Program Files\\Visual Studio\\2022\\BuildTools\\",
            "VCToolsVersion": "14.44.35207",
            "WindowsSDKVersion": "10.0.26100.0\\",
        }

    def test_initialized_environment_derives_the_same_concrete_tuple_for_portal_and_bare_clients(self):
        self.assertEqual(
            bootstrap.windows_compiler_selection(self.environment),
            {
                "BAZEL_VC": "C:/Program Files/Visual Studio/2022/BuildTools/VC",
                "BAZEL_VC_FULL_VERSION": "14.44.35207",
                "BAZEL_WINSDK_FULL_VERSION": "10.0.26100.0",
            },
        )
        self.assertNotIn("BAZEL_VC", self.environment, "the caller must export selectors before starting Bazel")

    def test_windows_names_and_explicit_vc_paths_compare_case_insensitively(self):
        environment = {key.swapcase(): value for key, value in self.environment.items()}
        environment.update(
            {
                "bazel_vc": "c:/PROGRAM FILES/visual studio/2022/buildtools/VC/",
                "bazel_vc_full_version": "14.44.35207",
                "bazel_winsdk_full_version": "10.0.26100.0",
            }
        )
        self.assertEqual(
            bootstrap.windows_compiler_selection(environment), bootstrap.windows_compiler_selection(self.environment)
        )

    def test_explicit_selector_conflicts_fail_instead_of_overwriting_it(self):
        # One generic conflict plus the trailing-backslash and empty-value
        # cells, which exercise the distinct normalization before comparison.
        for name, value in (
            ("BAZEL_VC", "C:/Other/VC"),
            ("BAZEL_VC_FULL_VERSION", "14.44.35207\\"),
            ("BAZEL_WINSDK_FULL_VERSION", ""),
        ):
            with self.subTest(name=name, value=value), self.assertRaisesRegex(bootstrap.EnvironmentError, name):
                bootstrap.windows_compiler_selection({**self.environment, name.lower(): value})


class NativePlatformDiscoveryTests(unittest.TestCase):
    def test_native_platform_does_not_select_portal_build_trees_or_checkout_state(self):
        with patch.object(builddir, "platform_profile", side_effect=AssertionError("portal profile selection")):
            for platform, expected in (("linux", "linux"), ("darwin", "macos"), ("win32", "windows")):
                with self.subTest(platform=platform), patch.object(native.sys, "platform", platform):
                    self.assertEqual(native.native_platform(), expected)


if __name__ == "__main__":
    unittest.main()
