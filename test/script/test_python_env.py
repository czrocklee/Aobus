"""Tests for the native Python tooling bootstrap."""

import concurrent.futures
import contextlib
import io
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core.paths import PROJECT_ROOT
from ao.portal import python_env

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
            first = python_env.environment_fingerprint(versions, requirements, base_prefix=r"C:\Python")
            requirements.write_text("ruff==0.15.23\n", encoding="utf-8")
            changed_lock = python_env.environment_fingerprint(versions, requirements, base_prefix=r"C:\Python")
            changed_python = python_env.environment_fingerprint(versions, requirements, base_prefix=r"D:\Python")

        self.assertNotEqual(first, changed_lock)
        self.assertNotEqual(changed_lock, changed_python)

    def test_fingerprint_canonicalizes_symlinked_base_prefix_spellings(self):
        # One installation is reachable through several spellings: Homebrew's
        # `opt` symlink beside the Cellar real path, or CPython's macOS
        # framework prefix, which differs when site is not imported. The
        # bootstrap, the managed interpreter and the Bazel repository helper
        # must still select the one prepared environment.
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            requirements = root / "requirements.txt"
            requirements.write_text("ruff==0.15.22\n", encoding="utf-8")
            cellar = root / "Cellar/python@3.14/3.14.6/Frameworks/Python.framework/Versions/3.14"
            cellar.mkdir(parents=True)
            opt = root / "opt/python@3.14/Frameworks/Python.framework/Versions/3.14"
            opt.parent.mkdir(parents=True)
            try:
                opt.symlink_to(cellar, target_is_directory=True)
            except OSError, NotImplementedError:
                self.skipTest("this host cannot create the base-prefix alias symlink")

            canonical = python_env.environment_fingerprint(versions, requirements, base_prefix=str(cellar))
            self.assertEqual(
                canonical,
                python_env.environment_fingerprint(versions, requirements, base_prefix=str(opt)),
            )
            self.assertNotEqual(
                canonical,
                python_env.environment_fingerprint(versions, requirements, base_prefix=str(root / "other/python")),
            )

    def test_managed_caller_keeps_the_base_runtime_environment_identity(self):
        # Separate imports reproduce the bootstrap-to-managed-interpreter handoff.
        program = (
            "import json, sys; from pathlib import Path; "
            "sys.path.insert(0, sys.argv[1]); sys.executable = sys.argv[2]; "
            "from ao.portal import python_env; "
            "print(python_env.environment_fingerprint(json.loads(sys.argv[4]), Path(sys.argv[3])))"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            requirements = root / "requirements.txt"
            requirements.write_text("ruff==0.15.22\n", encoding="utf-8")

            def fingerprint(caller: Path) -> str:
                return subprocess.check_output(
                    [
                        sys.executable,
                        "-I",
                        "-c",
                        program,
                        str(PROJECT_ROOT / "script"),
                        str(caller),
                        str(requirements),
                        json.dumps(LOCKED_TOOL_VERSIONS),
                    ],
                    cwd=PROJECT_ROOT,
                    text=True,
                ).strip()

            self.assertEqual(
                fingerprint(root / "base" / "python.exe"),
                fingerprint(root / "managed" / "Scripts" / "python.exe"),
            )

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
            first = python_env.environment_fingerprint(versions, requirements, base_prefix=r"C:\Python")
            docs_lock.write_text(
                "markdown-it-py==4.3.0 \\\n"
                "    --hash=sha256:9f7ebbcd14fe59494226453aed97c1070d83f8d24b6fc3a3bcf9a38092641c4a\n",
                encoding="utf-8",
            )
            changed_docs_lock = python_env.environment_fingerprint(versions, requirements, base_prefix=r"C:\Python")
            requirements.write_text("ruff==0.15.22\n", encoding="utf-8")
            without_docs_lock = python_env.environment_fingerprint(versions, requirements, base_prefix=r"C:\Python")

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
            fingerprint = python_env.environment_fingerprint(versions, requirements, base_prefix="/opt/aobus/python")

            original = deep.read_text(encoding="utf-8")
            deep.write_text(original.replace("mdurl==0.1.2", "mdurl==0.1.9"), encoding="utf-8")
            changed_deep = python_env.environment_fingerprint(versions, requirements, base_prefix="/opt/aobus/python")
            deep.write_text(original, encoding="utf-8")
            docs.write_text(
                docs.read_text(encoding="utf-8").replace("markdown-it-py==4.2.0", "markdown-it-py==4.3.0"),
                encoding="utf-8",
            )
            changed_docs = python_env.environment_fingerprint(versions, requirements, base_prefix="/opt/aobus/python")

        self.assertNotEqual(fingerprint, changed_deep)
        self.assertNotEqual(fingerprint, changed_docs)
        self.assertNotEqual(changed_deep, changed_docs)

    def test_supported_lock_graph_fingerprint_is_preserved(self):
        # Tightening the include policy must not move the fingerprint of locks
        # that use only the bounded supported forms; the pinned digest covers
        # the schema, tool versions, base-runtime path and full lock bytes.
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
            with mock.patch.object(python_env, "os", SimpleNamespace(path=posixpath)):
                self.assertEqual(
                    python_env.environment_fingerprint(versions, requirements, base_prefix="/opt/aobus/python"),
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
            "script/ao/" + python_env.REQUIREMENTS_FILE.name,
            *("script/ao/" + path.name for path in python_env._included_requirements(python_env.REQUIREMENTS_FILE)),
        }
        self.assertTrue(lock_closure <= key_inputs, lock_closure - key_inputs)

    def test_environment_path_uses_the_build_directory_checkout_key(self):
        with mock.patch.object(python_env.build_dir, "windows_checkout_key", return_value="aobus-deadbeef") as key:
            path = python_env.environment_path(
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
        with mock.patch.object(python_env.build_dir, "windows_checkout_key", return_value="aobus-deadbeef") as key:
            path = python_env.environment_path(
                Path("/src/Aobus"),
                Path("/state/Aobus"),
                "fingerprint",
                environ={"AOBUS_CHECKOUT_ID": "id"},
                os_name="posix",
            )

        self.assertEqual(path, Path("/state/Aobus/tools/venvs/aobus-deadbeef/fingerprint"))
        self.assertEqual(python_env._python_path(path, platform_name="macos"), path / "bin" / "python")
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

            with mock.patch.object(python_env, "validate_base_python"):
                with mock.patch.object(python_env, "environment_path", return_value=destination):
                    with mock.patch.object(python_env, "_tools_match", return_value=True):
                        with mock.patch.object(python_env, "_build_environment") as build:
                            actual = python_env.ensure_environment(
                                root, root / "state", config_file=config, requirements_file=requirements
                            )

        self.assertEqual(actual, expected)
        build.assert_not_called()

    def test_windows_native_analysis_result_file_selection_never_probes_or_installs_python_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = root / "python.txt"
            with (
                mock.patch.object(python_env.subprocess, "run", side_effect=AssertionError("unexpected tool probe")),
                mock.patch.object(python_env, "ensure_environment", side_effect=AssertionError("unexpected venv")),
                mock.patch.object(
                    python_env.build_env,
                    "windows_bootstrap_requires_python_tools",
                    wraps=python_env.build_env.windows_bootstrap_requires_python_tools,
                ) as capability,
            ):
                status = python_env.main(
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
                        "check",
                        "contracts",
                        "--config",
                        "debug",
                    ]
                )
            self.assertEqual(status, 0)
            self.assertEqual(result.read_text(encoding="utf-8").strip(), sys.executable)
            self.assertFalse((root / "state").exists())
            capability.assert_called_once_with(["check", "contracts", "--config", "debug"], scope_file=mock.ANY)
            self.assertFalse(capability.call_args.kwargs["scope_file"].parent.exists())

    def test_windows_result_file_selection_uses_the_parsed_tooling_capability(self):
        commands = (
            ([], True),  # no command tail retains the explicit legacy tooling query
            (["--"], False),
            (["--", "help"], False),
            (["--", "check", "docs"], True),
            (["--", "check", "format", "script/ao/core/proc.py"], True),
            (["--", "check", "format", "README.md"], False),
            (["--", "check", "lint", "script/ao/core/proc.py"], True),
            (["--", "check", "lint", "--tidy-check", "aobus-no-c-assert", "script/ao/core/proc.py"], False),
            (["--", "check"], False),
            (["--", "check", "--ci"], False),
            (["--", "setup", "llvm-sdk"], False),
        )
        for command, needs_tools in commands:
            with self.subTest(command=command), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                result = root / "python.txt"
                managed = root / "environment" / "Scripts" / "python.exe"
                with mock.patch.object(python_env, "ensure_environment", return_value=managed) as ensure:
                    status = python_env.main(
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
                    mock.patch.object(python_env, "ensure_environment") as ensure,
                    mock.patch.object(python_env.subprocess, "run") as launch,
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit) as failure,
                ):
                    python_env.main(
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
                mock.patch.object(python_env, "ensure_environment", side_effect=AssertionError("unexpected venv")),
                mock.patch.object(
                    python_env.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 23),
                ) as launch,
            ):
                status = python_env.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                        "check",
                        "contracts",
                        "--config",
                        "debug",
                    ]
                )

            self.assertEqual(status, 23)
            launch.assert_called_once_with(
                [
                    sys.executable,
                    "-m",
                    "ao.portal.windows_portal",
                    "check",
                    "contracts",
                    "--config",
                    "debug",
                ],
                env=mock.ANY,
                check=False,
            )
            self.assertNotIn("_AOBUS_WINDOWS_SCOPE_FILE", launch.call_args.kwargs["env"])
            self.assertNotIn("AOBUS_PREFLIGHT_SCOPE", launch.call_args.kwargs["env"])
            self.assertFalse((root / "state").exists())

    def test_windows_portal_launch_uses_the_selected_tool_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            managed = root / "environment" / "Scripts" / "python.exe"
            # The provisioner is external I/O; command selection stays real.
            with (
                mock.patch.object(python_env, "ensure_environment", return_value=managed) as ensure,
                mock.patch.object(
                    python_env.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as launch,
            ):
                status = python_env.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                        "check",
                        "docs",
                    ]
                )

            self.assertEqual(status, 0)
            ensure.assert_called_once_with(root, root / "state", platform_name="windows")
            launch.assert_called_once_with(
                [str(managed), "-m", "ao.portal.windows_portal", "check", "docs"],
                env=mock.ANY,
                check=False,
            )

    def test_windows_portal_launch_without_arguments_starts_the_portal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(python_env, "ensure_environment", side_effect=AssertionError("unexpected venv")),
                mock.patch.object(
                    python_env.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as launch,
            ):
                status = python_env.main(
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
                [sys.executable, "-m", "ao.portal.windows_portal"],
                env=mock.ANY,
                check=False,
            )

    def test_windows_portal_launch_reports_launch_failures_as_bootstrap_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stderr = io.StringIO()
            with (
                mock.patch.object(python_env.subprocess, "run", side_effect=OSError("cannot start portal")),
                contextlib.redirect_stderr(stderr),
            ):
                status = python_env.main(
                    [
                        "--project-root",
                        str(root),
                        "--state-root",
                        str(root / "state"),
                        "--launch-windows-portal",
                        "--platform",
                        "windows",
                        "--",
                        "check",
                        "contracts",
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
            with mock.patch.object(python_env, "ensure_environment", return_value=managed) as ensure:
                status = python_env.main(
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
                        "check",
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
                (staging / python_env.COMPLETE_MARKER).write_text(f"{fingerprint}\n", encoding="utf-8")

            def ready(environment, _versions, _state, _fingerprint, **_kwargs):
                return environment == destination and environment.is_dir()

            with mock.patch.object(python_env, "validate_base_python"):
                with mock.patch.object(python_env, "environment_path", return_value=destination):
                    with mock.patch.object(python_env, "_tools_match", side_effect=ready):
                        with mock.patch.object(python_env, "_build_environment", side_effect=build) as create:
                            actual = python_env.ensure_environment(
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
            (environment / python_env.COMPLETE_MARKER).write_text("previous-stamp\n", encoding="utf-8")

            with mock.patch.object(python_env.subprocess, "run") as run:
                ready = python_env._tools_match(environment, versions, root / "state", "stamp", platform_name="macos")

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
            with mock.patch.object(python_env.venv, "EnvBuilder", return_value=builder):
                with mock.patch.object(python_env.subprocess, "run") as run:
                    with mock.patch.object(python_env, "_tools_match", return_value=True):
                        python_env._build_environment(
                            destination, versions, requirements, root / "state", "fingerprint"
                        )

        command = run.call_args.args[0]
        self.assertEqual(command[1:4], ["-I", "-m", "pip"])
        self.assertIn("--only-binary=:all:", command)
        self.assertIn("--require-hashes", command)
        self.assertIn("--no-deps", command)
        self.assertEqual(command[-2:], ["-r", str(requirements)])

    def test_builder_passes_offline_wheelhouse_settings_to_pip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            offline = {"PIP_NO_INDEX": "1", "PIP_FIND_LINKS": str(root / "prefetched wheels")}
            with (
                mock.patch.dict(os.environ, offline),
                mock.patch.object(python_env.venv, "EnvBuilder"),
                mock.patch.object(python_env.subprocess, "run") as run,
                mock.patch.object(python_env, "_tools_match", return_value=True),
            ):
                python_env._build_environment(
                    root, LOCKED_TOOL_VERSIONS, root / "requirements.txt", root / "state", "fingerprint"
                )

        run.assert_called_once()
        command = run.call_args.args[0]
        environment = run.call_args.kwargs["env"]
        self.assertEqual({key: environment[key] for key in offline}, offline)
        self.assertNotIn("--isolated", command)
        self.assertTrue(run.call_args.kwargs["check"])

    def test_tool_subprocesses_drop_ambient_python_package_paths(self):
        with mock.patch.dict(
            python_env.os.environ,
            {"PYTHONHOME": r"C:\ambient", "PYTHONPATH": r"C:\ambient\packages"},
        ):
            environment = python_env._process_environment(Path(r"C:\Aobus"))

        self.assertNotIn("PYTHONHOME", environment)
        self.assertNotIn("PYTHONPATH", environment)


class WindowsScopeTransferTest(unittest.TestCase):
    def arguments(self, root, source):
        return [
            "--project-root",
            str(root),
            "--state-root",
            str(root / "state"),
            "--launch-windows-portal",
            "--platform",
            "windows",
            "--",
            "check",
            "lint",
            str(source),
        ]

    def test_private_scope_reaches_a_real_child_and_is_removed_after_failure(self):
        real_run = subprocess.run
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "selected.py"
            source.touch()
            stale = root / "stale.json"
            stale.write_text("do not adopt or overwrite", encoding="utf-8")
            markers = []
            child = (
                "import json,os,pathlib,sys; "
                "p=pathlib.Path(os.environ['_AOBUS_WINDOWS_SCOPE_FILE']); "
                "record=json.loads(p.read_text(encoding='utf-8')); "
                "assert 'AOBUS_PREFLIGHT_SCOPE' not in os.environ; "
                "print(json.dumps(record)); sys.exit(7)"
            )

            def launch(argv, *, env, check):
                self.assertFalse(check)
                self.assertEqual(argv, [sys.executable, "-m", "ao.portal.windows_portal", "check", "lint", str(source)])
                marker = Path(env["_AOBUS_WINDOWS_SCOPE_FILE"])
                self.assertNotEqual(marker, stale)
                self.assertTrue(marker.is_file())
                markers.append(marker)
                result = real_run(
                    [sys.executable, "-B", "-c", child],
                    env=env,
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=15,
                )
                self.assertEqual(result.returncode, 7, result.stderr)
                record = json.loads(result.stdout)
                self.assertEqual([Path(name) for name in record["files"]], [source])
                self.assertEqual(record["explicit_files"], record["files"])
                self.assertEqual(record["scope"]["check_action"], "lint")
                self.assertTrue(marker.exists())
                return result

            with (
                mock.patch.dict(
                    os.environ, {"AOBUS_PREFLIGHT_SCOPE": str(stale), "_AOBUS_WINDOWS_SCOPE_FILE": str(stale)}
                ),
                mock.patch.object(python_env, "ensure_environment", return_value=Path(sys.executable)),
                mock.patch.object(python_env.subprocess, "run", side_effect=launch),
            ):
                self.assertEqual(python_env.main(self.arguments(root, source)), 7)
                self.assertNotIn("AOBUS_PREFLIGHT_SCOPE", os.environ)
                self.assertNotIn("_AOBUS_WINDOWS_SCOPE_FILE", os.environ)
            self.assertEqual(len(markers), 1)
            self.assertFalse(markers[0].parent.exists())
            self.assertEqual(stale.read_text(encoding="utf-8"), "do not adopt or overwrite")

    def test_actual_portal_child_consumes_the_private_scope_without_native_tools(self):
        real_run = subprocess.run
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "unformatted-family.txt"
            source.touch()
            markers = []
            child = (
                "import runpy, sys; from unittest.mock import patch\n"
                "sys.argv = sys.argv[1:]\n"
                "with patch('ao.portal.source_scope.resolve', side_effect=AssertionError('child rescanned')):\n"
                "    runpy.run_module(sys.argv[0], run_name='__main__')\n"
            )

            def launch(argv, *, env, check):
                marker = Path(env["_AOBUS_WINDOWS_SCOPE_FILE"])
                self.assertTrue(marker.is_file())
                markers.append(marker)
                self.assertEqual(argv[:3], [sys.executable, "-m", "ao.portal.windows_portal"])
                child_environment = {**env, "PYTHONPATH": str(PROJECT_ROOT / "script")}
                result = real_run(
                    [argv[0], "-B", "-c", child, *argv[2:]],
                    env=child_environment,
                    capture_output=True,
                    text=True,
                    check=check,
                    timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "No files to format.")
                self.assertTrue(marker.is_file())
                return result

            arguments = self.arguments(root, source)
            arguments[-2] = "format"
            with (
                mock.patch.object(
                    python_env, "ensure_environment", side_effect=AssertionError("no wheels for text-only formatting")
                ),
                mock.patch.object(python_env.subprocess, "run", side_effect=launch),
            ):
                self.assertEqual(python_env.main(arguments), 0)
            self.assertEqual(len(markers), 1)
            self.assertFalse(markers[0].parent.exists())

    def test_simultaneous_launches_own_distinct_live_scope_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = [root / "first.py", root / "second.py"]
            for source in sources:
                source.touch()
            markers = []
            rendezvous = threading.Barrier(2)

            def launch(argv, *, env, check):
                marker = Path(env["_AOBUS_WINDOWS_SCOPE_FILE"])
                record = json.loads(marker.read_text(encoding="utf-8"))
                self.assertEqual([Path(name) for name in record["explicit_files"]], [Path(argv[-1])])
                markers.append(marker)
                rendezvous.wait(timeout=10)
                self.assertEqual(len({path.parent for path in markers}), 2)
                self.assertTrue(all(path.is_file() for path in markers))
                rendezvous.wait(timeout=10)
                return subprocess.CompletedProcess(argv, 0)

            with (
                mock.patch.dict(
                    os.environ, {"AOBUS_PREFLIGHT_SCOPE": "stale-unix", "_AOBUS_WINDOWS_SCOPE_FILE": "stale-windows"}
                ),
                mock.patch.object(python_env, "ensure_environment", return_value=Path(sys.executable)),
                mock.patch.object(python_env.subprocess, "run", side_effect=launch),
                concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor,
            ):
                futures = [executor.submit(python_env.main, self.arguments(root, source)) for source in sources]
                self.assertEqual([future.result(timeout=15) for future in futures], [0, 0])
            self.assertEqual(len(markers), 2)
            self.assertTrue(all(not path.parent.exists() for path in markers))


if __name__ == "__main__":
    unittest.main()
