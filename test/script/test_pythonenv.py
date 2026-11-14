"""Tests for the native Python tooling bootstrap."""

import contextlib
import io
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import pythonenv
from ao.core.paths import PROJECT_ROOT

# Fixture pins deliberately differ from the checked-in contract so
# config-driven tests cannot pass by reading the real toolchain.
LOCKED_TOOL_VERSIONS = {
    "python": "3.14.6",
    "ruff": "0.15.22",
    "mypy": "2.1.0",
    "markdown-it-py": "4.2.0",
    "mdurl": "0.1.2",
}


class NativePythonEnvironmentTest(unittest.TestCase):
    def test_fingerprint_changes_with_requirements_or_base_python(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            requirements = Path(temp_dir) / "requirements.txt"
            requirements.write_text("ruff==0.15.22\n", encoding="utf-8")
            first = pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")
            requirements.write_text("ruff==0.15.23\n", encoding="utf-8")
            changed_lock = pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")
            changed_python = pythonenv.environment_fingerprint(
                versions, requirements, executable=r"D:\Python\python.exe"
            )

        self.assertNotEqual(first, changed_lock)
        self.assertNotEqual(changed_lock, changed_python)

    def test_fingerprint_changes_with_an_included_lock(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            docs_lock = root / "docs-requirements.txt"
            docs_lock.write_text(
                "markdown-it-py==4.2.0 \\\n"
                "    --hash=sha256:9f7ebbcd14fe59494226453aed97c1070d83f8d24b6fc3a3bcf9a38092641c4a\n",
                encoding="utf-8",
            )
            requirements = root / "requirements.txt"
            requirements.write_text("-r docs-requirements.txt\nruff==0.15.22\n", encoding="utf-8")
            first = pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")
            docs_lock.write_text(
                "markdown-it-py==4.3.0 \\\n"
                "    --hash=sha256:9f7ebbcd14fe59494226453aed97c1070d83f8d24b6fc3a3bcf9a38092641c4a\n",
                encoding="utf-8",
            )
            changed_docs_lock = pythonenv.environment_fingerprint(
                versions, requirements, executable=r"C:\Python\python.exe"
            )
            requirements.write_text("ruff==0.15.22\n", encoding="utf-8")
            without_docs_lock = pythonenv.environment_fingerprint(
                versions, requirements, executable=r"C:\Python\python.exe"
            )

        self.assertNotEqual(first, changed_docs_lock)
        self.assertNotEqual(first, without_docs_lock)
        self.assertNotEqual(changed_docs_lock, without_docs_lock)

    def test_supported_lock_graph_hash_preserves_the_include_closure(self):
        # The bounded parser must keep honoring every supported form: comments,
        # empty lines, package and hash lines, and read-only nested includes
        # whose selective mutation moves the fingerprint of the whole graph.
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            deep = root / "deep-requirements.txt"
            deep.write_text(
                "# shared leaf lock\n\nmdurl==0.1.2 \\\n"
                "    --hash=sha256:84008a41e51615a49fc9966191ff91509e3c40b939176e643fd50a5c2196b8f8\n",
                encoding="utf-8",
            )
            docs = root / "docs-requirements.txt"
            docs.write_text(
                "-r deep-requirements.txt\nmarkdown-it-py==4.2.0 \\\n"
                "    --hash=sha256:9f7ebbcd14fe59494226453aed97c1070d83f8d24b6fc3a3bcf9a38092641c4a\n",
                encoding="utf-8",
            )
            requirements = root / "requirements.txt"
            requirements.write_text(
                "# native tooling lock\n-r docs-requirements.txt\n\nruff==0.15.22 \\\n"
                "    --hash=sha256:97f2015c92aa97105b0eab19eb5d224884399281cfc5da86a92db4ab5e7fb2ca\n",
                encoding="utf-8",
            )
            fingerprint = pythonenv.environment_fingerprint(versions, requirements, executable="/opt/aobus/python")

            original = deep.read_text(encoding="utf-8")
            deep.write_text(original.replace("mdurl==0.1.2", "mdurl==0.1.9"), encoding="utf-8")
            changed_deep = pythonenv.environment_fingerprint(versions, requirements, executable="/opt/aobus/python")
            deep.write_text(original, encoding="utf-8")
            docs.write_text(
                docs.read_text(encoding="utf-8").replace("markdown-it-py==4.2.0", "markdown-it-py==4.3.0"),
                encoding="utf-8",
            )
            changed_docs = pythonenv.environment_fingerprint(versions, requirements, executable="/opt/aobus/python")

        self.assertNotEqual(fingerprint, changed_deep)
        self.assertNotEqual(fingerprint, changed_docs)
        self.assertNotEqual(changed_deep, changed_docs)

    def test_supported_lock_graph_fingerprint_is_preserved(self):
        # Tightening the include policy must not move the fingerprint of locks
        # that use only the bounded supported forms; the pinned digest covers
        # the schema, tool versions, interpreter path and full lock bytes.
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs-requirements.txt").write_text(
                "markdown-it-py==4.2.0 \\\n"
                "    --hash=sha256:9f7ebbcd14fe59494226453aed97c1070d83f8d24b6fc3a3bcf9a38092641c4a\n",
                encoding="utf-8",
            )
            requirements = root / "requirements.txt"
            requirements.write_text(
                "-r docs-requirements.txt\nruff==0.15.22 \\\n"
                "    --hash=sha256:97f2015c92aa97105b0eab19eb5d224884399281cfc5da86a92db4ab5e7fb2ca\n",
                encoding="utf-8",
            )
            # Pin bytes and path rules, not this test host's newline/drive defaults.
            for lock in (root / "docs-requirements.txt", requirements):
                lock.write_bytes(lock.read_bytes().replace(b"\r\n", b"\n"))
            with mock.patch.object(pythonenv, "os", SimpleNamespace(path=posixpath)):
                self.assertEqual(
                    pythonenv.environment_fingerprint(versions, requirements, executable="/opt/aobus/python"),
                    "d2664c616a675807",
                )

    def test_ci_windows_managed_tools_key_covers_the_environment_lock_closure(self):
        workflow = (PROJECT_ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        restore_step = workflow.index("- name: Restore managed Windows tools")
        following_step = workflow.index("- name:", restore_step + 1)
        key_inputs = set(re.findall(r"'([^']+)'", workflow[restore_step:following_step].partition("hashFiles(")[2]))
        # The cache key must cover every fingerprint input of the shared lock
        # closure, so a docs-only parser change invalidates the cached managed
        # tools instead of restoring an environment the fingerprint rejects.
        lock_closure = {
            "script/ao/toolchain.json",
            "script/ao/" + pythonenv.REQUIREMENTS_FILE.name,
            *("script/ao/" + path.name for path in pythonenv._included_requirements(pythonenv.REQUIREMENTS_FILE)),
        }
        self.assertTrue(lock_closure <= key_inputs, lock_closure - key_inputs)

    def test_environment_path_uses_the_build_directory_checkout_key(self):
        with mock.patch.object(pythonenv.builddir, "windows_checkout_key", return_value="aobus-deadbeef") as key:
            path = pythonenv.environment_path(
                Path("Y:\\"), Path(r"C:\Aobus"), "fingerprint", environ={"AOBUS_CHECKOUT_ID": "id"}
            )

        self.assertEqual(path, Path(r"C:\Aobus") / "tools" / "venvs" / "aobus-deadbeef" / "fingerprint")
        key.assert_called_once_with(
            Path("Y:\\"),
            environ={"AOBUS_CHECKOUT_ID": "id"},
            create_id=True,
            os_name="nt",
        )

    def test_macos_environment_uses_a_posix_checkout_key_and_python_path(self):
        with mock.patch.object(pythonenv.builddir, "windows_checkout_key", return_value="aobus-deadbeef") as key:
            path = pythonenv.environment_path(
                Path("/src/Aobus"),
                Path("/state/Aobus"),
                "fingerprint",
                environ={"AOBUS_CHECKOUT_ID": "id"},
                os_name="posix",
            )

        self.assertEqual(path, Path("/state/Aobus/tools/venvs/aobus-deadbeef/fingerprint"))
        self.assertEqual(pythonenv._python_path(path, platform_name="macos"), path / "bin" / "python")
        key.assert_called_once_with(
            Path("/src/Aobus"),
            environ={"AOBUS_CHECKOUT_ID": "id"},
            create_id=True,
            os_name="posix",
        )

    def test_ready_environment_is_reused_without_rebuilding(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            config = root / "toolchain.json"
            requirements = root / "requirements.txt"
            config.write_text(json.dumps({"schemaVersion": 1, **versions}), encoding="utf-8")
            requirements.write_text("locked", encoding="utf-8")
            destination = root / "environment"
            expected = destination / "Scripts" / "python.exe"

            with mock.patch.object(pythonenv, "validate_base_python"):
                with mock.patch.object(pythonenv, "environment_path", return_value=destination):
                    with mock.patch.object(pythonenv, "_tools_match", return_value=True):
                        with mock.patch.object(pythonenv, "_build_environment") as build:
                            actual = pythonenv.ensure_environment(
                                root, root / "state", config_file=config, requirements_file=requirements
                            )

        self.assertEqual(actual, expected)
        build.assert_not_called()

    def test_windows_native_build_result_file_selection_never_probes_or_installs_python_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = root / "python.txt"
            with (
                mock.patch.object(pythonenv.subprocess, "run", side_effect=AssertionError("unexpected tool probe")),
                mock.patch.object(pythonenv, "ensure_environment", side_effect=AssertionError("unexpected venv")),
                mock.patch.object(
                    pythonenv.buildenv,
                    "windows_bootstrap_requires_python_tools",
                    wraps=pythonenv.buildenv.windows_bootstrap_requires_python_tools,
                ) as capability,
            ):
                status = pythonenv.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--result-file",
                        str(result),
                        "--platform",
                        "windows",
                        "--",
                        "build",
                        "--target",
                        "//lib/utility:utility",
                    ]
                )
            self.assertEqual(status, 0)
            self.assertEqual(result.read_text(encoding="utf-8").strip(), sys.executable)
            self.assertFalse((root / "state").exists())
            capability.assert_called_once_with(["build", "--target", "//lib/utility:utility"])

    def test_windows_result_file_selection_uses_the_parsed_tooling_capability(self):
        commands = (
            ([], True),  # no command tail retains the explicit legacy tooling query
            (["--"], False),
            (["--", "help"], False),
            (["--", "build"], False),
            (["--", "run", "cli"], False),
            (["--", "docs", "check"], True),
            (["--", "format", "--check"], True),
            (["--", "hygiene"], True),
            (["--", "tidy"], True),
            (["--", "test", "--tooling"], True),
            (["--", "test", "--suite=tooling"], True),
            (["--", "test", "--core"], False),
            (["--", "check"], True),
            (["--", "setup", "llvm-sdk"], False),
            (["--", "doctor", "winui"], False),
        )
        for command, needs_tools in commands:
            with self.subTest(command=command), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                result = root / "python.txt"
                managed = root / "environment" / "Scripts" / "python.exe"
                with mock.patch.object(pythonenv, "ensure_environment", return_value=managed) as ensure:
                    status = pythonenv.main(
                        [
                            "--project-root",
                            str(root),
                            "--state-root",
                            str(root / "state"),
                            "--result-file",
                            str(result),
                            "--platform",
                            "windows",
                            *command,
                        ]
                    )
                self.assertEqual(status, 0)
                self.assertEqual(
                    result.read_text(encoding="utf-8").strip(), str(managed) if needs_tools else sys.executable
                )
                if needs_tools:
                    ensure.assert_called_once_with(root, root / "state", platform_name="windows")
                else:
                    ensure.assert_not_called()
                self.assertFalse((root / "state").exists())

    def test_windows_invalid_commands_fail_before_environment_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = root / "python.txt"
            for command in (["no-such-command"], ["test", "--bogus"], ["run"]):
                with (
                    self.subTest(command=command),
                    mock.patch.object(pythonenv, "ensure_environment") as ensure,
                    mock.patch.object(pythonenv.subprocess, "run") as launch,
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit) as failure,
                ):
                    pythonenv.main(
                        [
                            "--project-root",
                            str(root),
                            "--state-root",
                            str(root / "state"),
                            "--result-file",
                            str(result),
                            "--platform",
                            "windows",
                            "--",
                            *command,
                        ]
                    )
                self.assertEqual(failure.exception.code, 2)
                ensure.assert_not_called()
                launch.assert_not_called()
                self.assertFalse(result.exists())
                self.assertFalse((root / "state").exists())

    def test_windows_portal_launch_returns_the_child_status_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # Only the external launch is faked; Bazel selection, the toolchain
            # lock and base-Python validation stay real.
            with (
                mock.patch.object(pythonenv, "ensure_environment", side_effect=AssertionError("unexpected venv")),
                mock.patch.object(
                    pythonenv.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 23),
                ) as launch,
            ):
                status = pythonenv.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                        "build",
                        "--target",
                        "//lib/utility:utility",
                    ]
                )

            self.assertEqual(status, 23)
            launch.assert_called_once_with(
                [
                    sys.executable,
                    "-m",
                    "ao.core.windows_portal",
                    "build",
                    "--target",
                    "//lib/utility:utility",
                ],
                check=False,
            )
            self.assertFalse((root / "state").exists())

    def test_windows_portal_launch_uses_the_selected_tool_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            managed = root / "environment" / "Scripts" / "python.exe"
            # The provisioner is external I/O; command selection stays real.
            with (
                mock.patch.object(pythonenv, "ensure_environment", return_value=managed) as ensure,
                mock.patch.object(
                    pythonenv.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as launch,
            ):
                status = pythonenv.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                        "docs",
                        "check",
                    ]
                )

            self.assertEqual(status, 0)
            ensure.assert_called_once_with(root, root / "state", platform_name="windows")
            launch.assert_called_once_with(
                [str(managed), "-m", "ao.core.windows_portal", "docs", "check"],
                check=False,
            )

    def test_windows_portal_launch_without_arguments_starts_the_portal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(pythonenv, "ensure_environment", side_effect=AssertionError("unexpected venv")),
                mock.patch.object(
                    pythonenv.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as launch,
            ):
                status = pythonenv.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                    ]
                )

            self.assertEqual(status, 0)
            launch.assert_called_once_with(
                [sys.executable, "-m", "ao.core.windows_portal"],
                check=False,
            )

    def test_windows_portal_launch_reports_launch_failures_as_bootstrap_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stderr = io.StringIO()
            with (
                mock.patch.object(pythonenv.subprocess, "run", side_effect=OSError("cannot start portal")),
                contextlib.redirect_stderr(stderr),
            ):
                status = pythonenv.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                        "test",
                    ]
                )

            self.assertEqual(status, 1)
            self.assertIn("cannot start portal", stderr.getvalue())
            self.assertFalse((root / "state").exists())

    def test_macos_result_file_keeps_the_legacy_single_line_interpreter_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = root / "python.txt"
            managed = root / "environment" / "bin" / "python"
            with mock.patch.object(pythonenv, "ensure_environment", return_value=managed) as ensure:
                status = pythonenv.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--result-file",
                        str(result),
                        "--platform",
                        "macos",
                        "--",
                        "build",
                    ]
                )

            self.assertEqual(status, 0)
            ensure.assert_called_once_with(root, root / "state", platform_name="macos")
            # Path.write_text applies the host newline convention; the macOS
            # launcher consumes exactly one interpreter line.
            self.assertEqual(result.read_bytes(), f"{managed}{os.linesep}".encode())

    def test_new_environment_is_staged_then_published(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            config = root / "toolchain.json"
            requirements = root / "requirements.txt"
            config.write_text(json.dumps({"schemaVersion": 1, **versions}), encoding="utf-8")
            requirements.write_text("locked", encoding="utf-8")
            destination = root / "environment"

            def build(staging, _versions, _requirements, _state, fingerprint, **_kwargs):
                (staging / "Scripts").mkdir(parents=True)
                (staging / "Scripts" / "python.exe").touch()
                (staging / pythonenv.COMPLETE_MARKER).write_text(f"{fingerprint}\n", encoding="utf-8")

            def ready(environment, _versions, _state, _fingerprint, **_kwargs):
                return environment == destination and environment.is_dir()

            with mock.patch.object(pythonenv, "validate_base_python"):
                with mock.patch.object(pythonenv, "environment_path", return_value=destination):
                    with mock.patch.object(pythonenv, "_tools_match", side_effect=ready):
                        with mock.patch.object(pythonenv, "_build_environment", side_effect=build) as create:
                            actual = pythonenv.ensure_environment(
                                root, root / "state", config_file=config, requirements_file=requirements
                            )

            self.assertEqual(actual, destination / "Scripts" / "python.exe")
            self.assertTrue(destination.is_dir())
            self.assertFalse(any(path.name.startswith(".") and ".tmp-" in path.name for path in root.iterdir()))
            create.assert_called_once()

    def test_environment_with_a_stale_stamp_is_not_reused(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            environment = root / "environment"
            (environment / "bin").mkdir(parents=True)
            (environment / "bin" / "python").touch()
            (environment / pythonenv.COMPLETE_MARKER).write_text("previous-stamp\n", encoding="utf-8")

            with mock.patch.object(pythonenv.subprocess, "run") as run:
                ready = pythonenv._tools_match(environment, versions, root / "state", "stamp", platform_name="macos")

        self.assertFalse(ready)
        run.assert_not_called()

    def test_builder_installs_only_hash_locked_binary_packages(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            destination = root / "environment"
            requirements = root / "requirements.txt"
            requirements.write_text("locked", encoding="utf-8")

            def create_environment(path):
                (path / "Scripts").mkdir(parents=True)
                (path / "Scripts" / "python.exe").touch()

            builder = mock.Mock()
            builder.create.side_effect = create_environment
            with mock.patch.object(pythonenv.venv, "EnvBuilder", return_value=builder):
                with mock.patch.object(pythonenv.subprocess, "run") as run:
                    with mock.patch.object(pythonenv, "_tools_match", return_value=True):
                        pythonenv._build_environment(destination, versions, requirements, root / "state", "fingerprint")

        command = run.call_args.args[0]
        self.assertEqual(command[1:4], ["-I", "-m", "pip"])
        self.assertIn("--only-binary=:all:", command)
        self.assertIn("--require-hashes", command)
        self.assertIn("--no-deps", command)
        self.assertEqual(command[-2:], ["-r", str(requirements)])

    def test_tool_subprocesses_drop_ambient_python_package_paths(self):
        with mock.patch.dict(
            pythonenv.os.environ,
            {"PYTHONHOME": r"C:\ambient", "PYTHONPATH": r"C:\ambient\packages"},
        ):
            environment = pythonenv._process_environment(Path(r"C:\Aobus"))

        self.assertNotIn("PYTHONHOME", environment)
        self.assertNotIn("PYTHONPATH", environment)


if __name__ == "__main__":
    unittest.main()
