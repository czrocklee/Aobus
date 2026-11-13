"""Buildifier admission and JSON/process-boundary contracts, without native downloads.

Subprocess fakes prove argv, isolation, and failure handling, not acceptance by a
native Buildifier binary. Native control and binary smoke probes own that evidence.
"""

import contextlib
import hashlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import buildifier


class BuildifierRunTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="aobus-buildifier-test-")))
        self.tool = self.root / "managed tools/buildifier"
        self.first = self.root / "BUILD.bazel"
        self.first.write_bytes(b"x=1\n")
        self.payload = None
        self.returncode = 0
        self.stderr = ""
        stack.enter_context(mock.patch.object(buildifier, "PROJECT_ROOT", self.root))
        self.selection = stack.enter_context(mock.patch.object(buildifier, "executable", return_value=self.tool))
        self.process = stack.enter_context(mock.patch.object(buildifier.subprocess, "run", side_effect=self._process))

    def _record(self, filename=None, **changes):
        record = {
            "filename": str(self.first) if filename is None else filename,
            "valid": True,
            "formatted": True,
            "warnings": [],
        }
        record.update(changes)
        return record

    def _warning(self):
        return {
            "start": {"line": 4, "column": 5},
            "end": {"line": 4, "column": 11},
            "category": "unused-variable",
            "actionable": True,
            "autoFixable": False,
            "message": 'Variable "unused" is unused. Please remove it.',
            "url": "https://github.com/bazel-contrib/buildtools/blob/main/WARNINGS.md#unused-variable",
        }

    def _process(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.tool))
        self.assertEqual(kwargs["cwd"], self.root)
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(kwargs["encoding"], "utf-8")
        self.assertEqual(kwargs["errors"], "strict")
        self.assertNotIn("BUILDIFIER_CONFIG", {key.upper() for key in kwargs["env"]})
        if argv[1] == "-mode=check":
            self.assertIn(argv[2], ("-lint=off", "-lint=warn"))
            self.assertEqual(argv[3:5], ["-format=json", "-config=off"])
            if argv[2] == "-lint=warn":
                self.assertEqual(argv[5], "-warnings=-unnamed-macro")
                files = argv[6:]
            else:
                files = argv[5:]
            payload = self.payload
            if payload is None:
                payload = json.dumps({"success": True, "files": [self._record(name) for name in files]})
        else:
            self.assertEqual(argv[1:4], ["-mode=fix", "-lint=off", "-config=off"])
            files = argv[4:]
            for name in files:
                Path(name).write_bytes(b"x = 1\n")
            payload = ""
        self.assertTrue(files)
        self.assertTrue(all(Path(filename).is_absolute() for filename in files))
        return subprocess.CompletedProcess(argv, self.returncode, payload, self.stderr)

    def _run(self, files=None, *, check=True, lint=False, sink=None):
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            code = buildifier.run(["BUILD.bazel"] if files is None else files, check=check, lint=lint, sink=sink)
        return code, output.getvalue()

    def test_empty_selection_does_not_select_or_execute_any_tool(self):
        for check in (False, True):
            self.assertEqual(buildifier.run([], check=check, lint=check), 0)
        self.selection.assert_not_called()
        self.process.assert_not_called()

    def test_clean_complete_json_succeeds_without_modifying_control_bytes(self):
        self.assertEqual(self._run(lint=True), (0, ""))
        self.assertEqual(self.first.read_bytes(), b"x=1\n")
        self.assertEqual(
            self.process.call_args.args[0][1:6],
            ["-mode=check", "-lint=warn", "-format=json", "-config=off", "-warnings=-unnamed-macro"],
        )

    def test_format_only_check_disables_lint_and_requires_structured_output(self):
        self.assertEqual(self._run(), (0, ""))
        self.assertEqual(self.process.call_args.args[0][2], "-lint=off")
        self.payload = ""
        with self.assertRaisesRegex(buildifier.BuildifierError, "JSON"):
            self._run()

    def test_format_fix_uses_no_lint_fixes_and_preserves_the_process_file_argv(self):
        path = self.root / "-spaced café 文件.bzl"
        path.write_bytes(b"x=1\n")
        self.assertEqual(self._run([path.name], check=False), (0, ""))
        self.assertEqual(path.read_bytes(), b"x = 1\n")
        self.assertEqual(
            self.process.call_args.args[0], [str(self.tool), "-mode=fix", "-lint=off", "-config=off", str(path)]
        )

    def test_lint_fix_requests_fail_before_tool_selection(self):
        with self.assertRaisesRegex(buildifier.BuildifierError, "lint requires check"):
            self._run(check=False, lint=True)
        self.selection.assert_not_called()
        self.process.assert_not_called()

    def test_ambient_and_workspace_configs_cannot_change_the_process_contract(self):
        config = self.root / ".buildifier.json"
        config.write_text('{"warnings": ["-unused-variable"], "lint": "fix"}', encoding="utf-8")
        with mock.patch.dict(os.environ, {"BUILDIFIER_CONFIG": str(self.root / "other-config.json")}):
            self.assertEqual(self._run(lint=True), (0, ""))
            self.assertEqual(os.environ["BUILDIFIER_CONFIG"], str(self.root / "other-config.json"))
        self.assertEqual(self.first.read_bytes(), b"x=1\n")
        self.assertIn("-config=off", self.process.call_args.args[0])
        self.assertTrue(config.is_file())

    def test_membership_uses_native_absolute_paths_and_keeps_logical_diagnostic_names(self):
        path = self.root / "directory with spaces" / "café 文件.bzl"
        path.parent.mkdir()
        path.write_bytes(b"x=1\n")
        logical = str(Path("directory with spaces") / path.name)
        self.payload = json.dumps({"success": False, "files": [self._record(str(path), formatted=False)]})
        code, diagnostics = self._run([logical])
        self.assertEqual(code, 1)
        self.assertIn(logical + ": needs Buildifier formatting", diagnostics)
        self.assertEqual(self.process.call_args.args[0][-1], str(path))
        self.assertEqual(path.read_bytes(), b"x=1\n")

    @unittest.skipIf(os.name == "nt", "POSIX backslash filenames are not Windows paths")
    def test_posix_backslash_filename_is_not_rewritten_as_a_directory(self):
        filename = r"literal\name.bzl"
        self.payload = json.dumps({"success": True, "files": [self._record(filename)]})
        self.assertEqual(self._run([filename]), (0, ""))
        self.assertEqual(self.process.call_args.args[0][-1], str(self.root / filename))

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_symlink_argument_is_not_resolved_before_buildifier_enforces_write_safety(self):
        target = self.root / "outside-workspace.bzl"
        target.write_bytes(b"x=1\n")
        link = self.root / "linked.bzl"
        link.symlink_to(target)
        self.assertEqual(self._run([link.name]), (0, ""))
        self.assertEqual(self.process.call_args.args[0][-1], str(link))
        self.assertNotIn("-disable_symlink_safety", self.process.call_args.args[0])

    def test_duplicate_or_invalid_requested_paths_fail_before_selection(self):
        for files in (["BUILD.bazel", str(self.first)], [""], ["bad\0.bzl"]):
            with self.subTest(files=files), self.assertRaises(buildifier.BuildifierError):
                self._run(files)
        self.selection.assert_not_called()
        self.process.assert_not_called()

    def test_warnings_fail_with_exit_zero_even_when_not_actionable_or_auto_fixable(self):
        warning = self._warning()
        warning["actionable"] = False
        self.payload = json.dumps({"success": False, "files": [self._record(warnings=[warning])]})
        code, diagnostics = self._run(lint=True)
        self.assertEqual(code, 1)
        self.assertIn('BUILD.bazel:4:5: unused-variable: Variable "unused" is unused.', diagnostics)
        self.assertIn(warning["url"], diagnostics)
        self.assertEqual(self.first.read_bytes(), b"x=1\n")

    def test_warning_with_exit_zero_is_reported_only_to_the_selected_sink(self):
        self.payload = json.dumps({"success": False, "files": [self._record(warnings=[self._warning()])]})
        sink, stdout = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code, stderr = self._run(lint=True, sink=sink)
        self.assertEqual(code, 1)
        self.assertIn('BUILD.bazel:4:5: unused-variable: Variable "unused" is unused.', sink.getvalue())
        self.assertEqual(stderr, "")
        self.assertEqual(stdout.getvalue(), "")

    def test_formatting_check_findings_are_reported_only_to_the_selected_sink(self):
        self.payload = json.dumps({"success": False, "files": [self._record(formatted=False)]})
        sink, stdout = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code, stderr = self._run(sink=sink)
        self.assertEqual(code, 1)
        self.assertEqual(sink.getvalue(), "BUILD.bazel: needs Buildifier formatting\n")
        self.assertEqual(stderr, "")
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(self.first.read_bytes(), b"x=1\n")

    def test_child_stderr_and_exit_failure_are_reported_only_to_the_selected_sink(self):
        self.stderr, self.returncode = "tool failure\n", 4
        sink, stdout = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code, stderr = self._run(sink=sink)
        self.assertEqual(code, 1)
        self.assertEqual(sink.getvalue(), "tool failure\nBuildifier exited with status 4\n")
        self.assertEqual(stderr, "")
        self.assertEqual(stdout.getvalue(), "")

    def test_fix_mode_stdout_is_not_redirected_to_the_diagnostic_sink(self):
        def format_output(argv, **kwargs):
            result = self._process(argv, **kwargs)
            result.stdout = "formatter stdout\n"
            return result

        self.process.side_effect = format_output
        sink, stdout = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code, stderr = self._run(check=False, sink=sink)
        self.assertEqual(code, 0)
        self.assertEqual(stdout.getvalue(), "formatter stdout\n")
        self.assertEqual(sink.getvalue(), "")
        self.assertEqual(stderr, "")
        self.assertEqual(self.first.read_bytes(), b"x = 1\n")

    def test_explicit_warning_records_are_not_filtered_even_for_the_disabled_category(self):
        warning = self._warning()
        warning["category"] = "unnamed-macro"
        self.payload = json.dumps({"success": False, "files": [self._record(warnings=[warning])]})
        code, diagnostics = self._run(lint=True)
        self.assertEqual(code, 1)
        self.assertIn("unnamed-macro", diagnostics)

    def test_syntax_errors_and_nonzero_exit_status_cannot_pass_even_with_clean_json(self):
        self.payload = json.dumps({"success": False, "files": [self._record(valid=False, formatted=False)]})
        self.returncode = 1
        self.stderr = "BUILD.bazel:1: syntax error near unexpected token\n"
        code, diagnostics = self._run()
        self.assertEqual(code, 1)
        self.assertIn("BUILD.bazel: Buildifier syntax error", diagnostics)
        self.assertIn("unexpected token", diagnostics)
        self.payload = None
        self.stderr = ""
        for code in (1, 2, 3, 4):
            with self.subTest(code=code):
                self.returncode = code
                result, diagnostics = self._run()
                self.assertEqual(result, 1)
                self.assertIn(f"exited with status {code}", diagnostics)

    def test_nonzero_or_unstructured_text_never_bypasses_json_validation(self):
        self.returncode = 1
        self.payload = "BUILD.bazel: syntax error\n"
        with self.assertRaisesRegex(buildifier.BuildifierError, "JSON"):
            self._run()

    def test_process_launch_and_decoding_errors_have_a_local_buildifier_exception(self):
        for failure in (FileNotFoundError("missing"), UnicodeDecodeError("utf-8", b"\xff", 0, 1, "invalid")):
            with self.subTest(failure=failure), mock.patch.object(buildifier.subprocess, "run", side_effect=failure):
                with self.assertRaisesRegex(buildifier.BuildifierError, "cannot execute Buildifier"):
                    self._run()

    def test_large_selections_are_bounded_and_a_later_batch_failure_is_retained(self):
        files = [f"directory with spaces/file {index}.bzl" for index in range(205)]
        self.assertEqual(self._run(files), (0, ""))
        commands = [call.args[0] for call in self.process.call_args_list]
        self.assertEqual([len(command) - 5 for command in commands], [100, 100, 5])
        self.assertEqual(
            [filename for command in commands for filename in command[5:]],
            [str(self.root / filename) for filename in files],
        )
        self.process.reset_mock()

        def fail_second_batch(argv, **kwargs):
            result = self._process(argv, **kwargs)
            if argv[-1] == str(self.root / "directory with spaces/file 199.bzl"):
                result.returncode = 4
            return result

        self.process.side_effect = fail_second_batch
        self.assertEqual(self._run(files)[0], 1)
        self.assertEqual(self.process.call_count, 3)

    def test_command_length_bounds_split_long_unicode_paths(self):
        files = [f"{'文件' * 120}/file{index}.bzl" for index in range(100)]
        self.assertEqual(self._run(files), (0, ""))
        self.assertGreater(self.process.call_count, 1)
        for call in self.process.call_args_list:
            length = len(subprocess.list2cmdline(call.args[0]).encode("utf-16-le")) // 2
            self.assertLessEqual(length, 24000)


class BuildifierProvisionTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="aobus-buildifier-cache-test-")))
        self.state = self.root / "state with spaces café"
        self.manifest = self.root / "buildifier.json"
        self.payload = b"pinned Buildifier executable fixture"
        self.digest = hashlib.sha256(self.payload).hexdigest()
        self.document = {
            "schemaVersion": 1,
            "version": "10.1.0",
            "artifacts": {
                "darwin-arm64": {
                    "url": (
                        "https://github.com/bazel-contrib/buildtools/releases/download/v10.1.0/buildifier-darwin-arm64"
                    ),
                    "sha256": self.digest,
                },
                "darwin-x86_64": {
                    "url": (
                        "https://github.com/bazel-contrib/buildtools/releases/download/v10.1.0/buildifier-darwin-amd64"
                    ),
                    "sha256": self.digest,
                },
                "windows-x86_64": {
                    "url": (
                        "https://github.com/bazel-contrib/buildtools/releases/download/v10.1.0/"
                        "buildifier-windows-amd64.exe"
                    ),
                    "sha256": self.digest,
                },
            },
        }
        self._save_manifest()
        self.host = SimpleNamespace(platform="darwin")
        self.machine = SimpleNamespace(machine=lambda: "arm64")
        self.version_output = "buildifier version: 10.1.0\nbuildifier scm revision: 0123456789abcdef\n"
        self.version_status = 0
        self.locked = False
        stack.enter_context(mock.patch.object(buildifier, "MANIFEST_PATH", self.manifest))
        stack.enter_context(mock.patch.object(buildifier, "PROJECT_ROOT", self.root))
        stack.enter_context(mock.patch.object(buildifier, "sys", self.host))
        stack.enter_context(mock.patch.object(buildifier, "platform", self.machine))
        stack.enter_context(mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": str(self.state)}))
        self.windows_state = stack.enter_context(
            mock.patch.object(buildifier.builddir, "windows_state_root", return_value=self.state)
        )
        self.download = stack.enter_context(
            mock.patch.object(buildifier.urllib.request, "urlretrieve", side_effect=self._download)
        )
        self.process = stack.enter_context(mock.patch.object(buildifier.subprocess, "run", side_effect=self._process))
        real_lock = buildifier.shared_file_lock

        @contextlib.contextmanager
        def locked(path):
            self.assertTrue(path.is_relative_to(self.state))
            with real_lock(path):
                self.locked = True
                try:
                    yield
                finally:
                    self.locked = False

        stack.enter_context(mock.patch.object(buildifier, "shared_file_lock", side_effect=locked))

    def _save_manifest(self):
        self.manifest.write_text(json.dumps(self.document), encoding="utf-8")

    def _tool(self, key="darwin-arm64"):
        filename = "buildifier.exe" if key.startswith("windows-") else "buildifier"
        return self.state / "tools/buildifier/10.1.0" / key / filename

    def _download(self, url, path):
        self.assertTrue(self.locked)
        key = (
            "windows-x86_64"
            if self.host.platform == "win32"
            else ("darwin-x86_64" if self.machine.machine().lower() in ("amd64", "x86_64") else "darwin-arm64")
        )
        self.assertEqual(url, self.document["artifacts"][key]["url"])
        self.assertEqual(Path(path).parent, self._tool(key).parent)
        self.assertFalse(self._tool(key).exists())
        Path(path).write_bytes(self.payload)

    def _process(self, argv, **kwargs):
        self.assertEqual(argv[1:], ["-version", "-config=off"])
        self.assertEqual(kwargs["cwd"], self.root)
        self.assertNotIn("BUILDIFIER_CONFIG", {key.upper() for key in kwargs["env"]})
        self.assertTrue(self.locked)
        path = Path(argv[0])
        self.assertTrue(path.is_relative_to(self.state))
        if self.host.platform == "win32":
            self.assertEqual(path.suffix, ".exe")
        self.assertEqual(path.read_bytes(), self.payload)
        return subprocess.CompletedProcess(argv, self.version_status, self.version_output, "")

    def test_missing_native_tool_is_verified_under_lock_before_atomic_publication(self):
        real_replace = os.replace
        published = []

        def publish(staged, target):
            self.assertTrue(self.locked)
            self.assertEqual(target, self._tool())
            self.assertEqual(staged.parent, target.parent)
            self.assertNotEqual(staged, target)
            self.assertEqual(staged.read_bytes(), self.payload)
            self.assertEqual(self.process.call_args.args[0][0], str(staged))
            published.append(target)
            real_replace(staged, target)

        with mock.patch.object(buildifier.os, "replace", side_effect=publish):
            self.assertEqual(buildifier.executable(), self._tool())
        self.assertEqual(published, [self._tool()])
        self.assertEqual(self._tool().read_bytes(), self.payload)
        self.assertEqual(list(self._tool().parent.glob(".buildifier-stage-*")), [])
        self.download.assert_called_once()
        self.windows_state.assert_not_called()

    def test_native_selection_ignores_ambient_executable_overrides(self):
        with mock.patch.dict(
            os.environ,
            {
                "AOBUS_BUILDIFIER": str(self.root / "ambient-buildifier"),
                "BUILDIFIER": str(self.root / "another-buildifier"),
                "AO_NIX_BUILDIFIER": "/nix/store/not-a-native-selection/bin/buildifier",
            },
        ):
            self.assertEqual(buildifier.executable(), self._tool())
        self.assertEqual(self._tool().read_bytes(), self.payload)

    def test_malformed_version_output_or_exit_one_cannot_publish_a_download(self):
        # One representative per distinct probe failure: nonzero exit, the
        # required version line missing, the pinned version token mismatched,
        # and a disallowed suffix after the version.
        for output, code in (
            ("buildifier version: 10.1.0\n", 1),
            ("10.1.0\n", 0),
            ("buildifier version: 10.1.00 \n", 0),
            ("buildifier version: 10.1.0-custom\n", 0),
        ):
            with self.subTest(output=output, code=code):
                self.version_output, self.version_status = output, code
                with self.assertRaisesRegex(buildifier.BuildifierError, "10.1.0"):
                    buildifier.executable()
                self.assertFalse(self._tool().exists())
                self.assertEqual(list(self._tool().parent.glob(".buildifier-stage-*")), [])

    def test_version_without_scm_line_is_accepted(self):
        self.version_output = "buildifier version: 10.1.0\n"
        self.assertEqual(buildifier.executable(), self._tool())

    def test_bad_download_hash_leaves_no_published_or_staged_executable(self):
        self.document["artifacts"]["darwin-arm64"]["sha256"] = "0" * 64
        self._save_manifest()
        with self.assertRaisesRegex(buildifier.BuildifierError, "downloaded Buildifier SHA-256"):
            buildifier.executable()
        self.assertFalse(self._tool().exists())
        self.assertEqual(list(self._tool().parent.glob(".buildifier-stage-*")), [])
        self.process.assert_not_called()

    def test_download_error_remains_a_local_failure_without_a_partial_cache(self):
        self.download.side_effect = OSError("download interrupted")
        with self.assertRaisesRegex(buildifier.BuildifierError, "download interrupted"):
            buildifier.executable()
        self.assertFalse(self._tool().exists())
        self.assertEqual(list(self._tool().parent.glob(".buildifier-stage-*")), [])

    def test_failed_atomic_publication_leaves_no_partial_executable(self):
        with mock.patch.object(buildifier.os, "replace", side_effect=OSError("publication failed")):
            with self.assertRaisesRegex(buildifier.BuildifierError, "publication failed"):
                buildifier.executable()
        self.assertFalse(self._tool().exists())
        self.assertEqual(list(self._tool().parent.glob(".buildifier-stage-*")), [])

    def test_architecture_aliases_select_separate_native_caches(self):
        for machine, key in (
            ("AMD64", "darwin-x86_64"),
            ("x86_64", "darwin-x86_64"),
            ("aarch64", "darwin-arm64"),
            ("arm64", "darwin-arm64"),
        ):
            with self.subTest(machine=machine):
                self.machine.machine = lambda machine=machine: machine
                self.assertEqual(buildifier.executable(), self._tool(key))
        self.assertNotEqual(self._tool("darwin-arm64"), self._tool("darwin-x86_64"))
        self.assertEqual(self.download.call_count, 2)

    def test_windows_selection_uses_its_state_owner_and_executable_suffix(self):
        self.host.platform = "win32"
        self.machine.machine = lambda: "AMD64"
        self.assertEqual(buildifier.executable(), self._tool("windows-x86_64"))
        self.windows_state.assert_called_once_with()

    def test_macos_default_state_is_the_user_cache_not_a_build_tree(self):
        with (
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": ""}),
            mock.patch.object(buildifier.Path, "home", return_value=self.root),
        ):
            self.state = self.root / "Library/Caches/Aobus"
            self.assertEqual(buildifier.executable(), self._tool())

    def test_missing_artifact_and_unsupported_hosts_fail_before_download_or_cache_creation(self):
        for host, machine in (("darwin", "riscv64"), ("win32", "ARM64"), ("freebsd14", "x86_64")):
            with self.subTest(host=host, machine=machine):
                self.host.platform = host
                self.machine.machine = lambda machine=machine: machine
                with self.assertRaisesRegex(buildifier.BuildifierError, "artifact|platform"):
                    buildifier.executable()
        self.host.platform = "darwin"
        self.machine.machine = lambda: "arm64"
        del self.document["artifacts"]["darwin-arm64"]
        self._save_manifest()
        with self.assertRaisesRegex(buildifier.BuildifierError, "darwin-arm64"):
            buildifier.executable()
        self.assertFalse(self.state.exists())
        self.download.assert_not_called()
        self.process.assert_not_called()


@unittest.skipIf(os.name == "nt", "Nix provider admission uses POSIX store paths")
class BuildifierNixTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="aobus-buildifier-nix-test-")))
        manifest = self.root / "buildifier.json"
        manifest.write_text('{"schemaVersion": 1, "version": "10.1.0", "artifacts": {}}', encoding="utf-8")
        self.tool = Path("/nix/store/fixture-buildifier/bin/buildifier")
        self.version_output = "buildifier version: 10.1.0\n"
        self.status = 0
        stack.enter_context(mock.patch.object(buildifier, "MANIFEST_PATH", manifest))
        stack.enter_context(mock.patch.object(buildifier, "sys", SimpleNamespace(platform="linux")))
        stack.enter_context(
            mock.patch.dict(
                os.environ,
                {
                    "AO_NIX_BUILDIFIER": str(self.tool),
                    "AOBUS_BUILDIFIER": str(self.root / "ambient-buildifier"),
                    "BUILDIFIER": str(self.root / "another-ambient-buildifier"),
                },
            )
        )
        real_is_file = Path.is_file
        stack.enter_context(
            mock.patch.object(
                Path, "is_file", autospec=True, side_effect=lambda path: path == self.tool or real_is_file(path)
            )
        )
        self.access = stack.enter_context(mock.patch.object(buildifier.os, "access", return_value=True))
        self.process = stack.enter_context(mock.patch.object(buildifier.subprocess, "run", side_effect=self._process))
        self.download = stack.enter_context(mock.patch.object(buildifier.urllib.request, "urlretrieve"))

    def _process(self, argv, **kwargs):
        self.assertEqual(argv, [str(self.tool), "-version", "-config=off"])
        self.assertNotIn("BUILDIFIER_CONFIG", {key.upper() for key in kwargs["env"]})
        return subprocess.CompletedProcess(argv, self.status, self.version_output, "")

    def test_recorded_nix_version_report_with_trailing_spaces_is_accepted(self):
        # Exact stdout from version-probe-v1.json; this remains an I/O fake,
        # not a new native process claim or a dependency on external evidence.
        self.version_output = "buildifier version: 10.1.0 \nbuildifier scm revision: v10.1.0 \n"
        self.assertEqual(buildifier.executable(), self.tool)
        self.download.assert_not_called()

    def test_missing_relative_or_outside_store_selection_never_uses_ambient_tools(self):
        for selected in (
            "",
            "relative/buildifier",
            str(self.root / "ambient-buildifier"),
            "/nix/store/missing-buildifier/bin/buildifier",
        ):
            with self.subTest(selected=selected), mock.patch.dict(os.environ, {"AO_NIX_BUILDIFIER": selected}):
                with self.assertRaisesRegex(buildifier.BuildifierError, "AO_NIX_BUILDIFIER"):
                    buildifier.executable()
        self.process.assert_not_called()
        self.download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
