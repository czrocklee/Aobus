"""Tests for the native Python tooling bootstrap."""

import json
import os
import posixpath
import re
import shutil
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
    def test_toolchain_schema_is_supported(self):
        values = json.loads(pythonenv.TOOLCHAIN_FILE.read_text(encoding="utf-8"))

        self.assertEqual(values["schemaVersion"], 1)

    def test_actual_tool_versions_match_the_cross_platform_contract(self):
        versions = pythonenv.tool_versions()
        actual_python = ".".join(str(part) for part in sys.version_info[:3])

        if sys.platform == "darwin":
            expected_major_minor = tuple(int(part) for part in versions["python"].split(".")[:2])
            self.assertEqual(sys.version_info[:2], expected_major_minor)
        else:
            self.assertEqual(actual_python, versions["python"])
        for module, prefix in (("ruff", "ruff"), ("mypy", "mypy")):
            command = (
                [sys.executable, "-I", "-m", module, "--version"]
                if os.name == "nt" or sys.platform == "darwin"
                else [shutil.which(module) or module, "--version"]
            )
            completed = subprocess.run(
                command,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            self.assertTrue(
                completed.stdout.strip().startswith(f"{prefix} {versions[module]}"),
                completed.stdout,
            )
        for package in ("markdown-it-py", "mdurl"):
            completed = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "-c",
                    f"from importlib.metadata import version; print(version('{package}'))",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            self.assertEqual(completed.stdout.strip(), versions[package])

    def test_base_python_must_match_the_pinned_patch_release(self):
        with self.assertRaisesRegex(RuntimeError, "requires Python 0.0.0"):
            pythonenv.validate_base_python({"python": "0.0.0"})

    def test_native_platform_selection_rejects_linux(self):
        self.assertEqual(pythonenv._native_platform_name("darwin"), "macos")
        self.assertEqual(pythonenv._native_platform_name("win32"), "windows")
        with self.assertRaisesRegex(RuntimeError, "unavailable on platform 'linux'"):
            pythonenv._native_platform_name("linux")

    def test_toolchain_and_hashed_locks_pin_the_validated_wheels(self):
        versions = pythonenv.tool_versions()
        requirements = pythonenv.REQUIREMENTS_FILE.read_text(encoding="utf-8")

        self.assertEqual(
            versions,
            {
                "python": "3.14.7",
                "ruff": "0.16.10",
                "mypy": "2.1.0",
                "markdown-it-py": "4.2.0",
                "mdurl": "0.1.2",
            },
        )
        self.assertIn("-r docs-requirements.txt", requirements)
        for package, version in {
            "ast-serialize": "0.6.0",
            "librt": "0.13.0",
            "mypy": versions["mypy"],
            "mypy_extensions": "1.1.0",
            "pathspec": "1.1.1",
            "ruff": versions["ruff"],
            "typing_extensions": "4.16.0",
        }.items():
            self.assertIn(f"{package}=={version}", requirements)
        self.assertEqual(requirements.count("--hash=sha256:"), 7)

        macos_requirements = pythonenv.MACOS_REQUIREMENTS_FILE.read_text(encoding="utf-8")
        self.assertIn("-r docs-requirements.txt", macos_requirements)
        for package, version in {
            "ast-serialize": "0.6.0",
            "librt": "0.13.0",
            "mypy": versions["mypy"],
            "mypy_extensions": "1.1.0",
            "pathspec": "1.1.1",
            "ruff": versions["ruff"],
            "typing_extensions": "4.16.0",
        }.items():
            self.assertIn(f"{package}=={version}", macos_requirements)
        self.assertEqual(macos_requirements.count("--hash=sha256:"), 11)

        docs_requirements = (pythonenv.PACKAGE_ROOT / "docs-requirements.txt").read_text(encoding="utf-8")
        for package in ("markdown-it-py", "mdurl"):
            self.assertIn(f"{package}=={versions[package]}", docs_requirements)
        self.assertEqual(docs_requirements.count("--hash=sha256:"), 2)

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

    def test_missing_included_lock_fails_the_fingerprint(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            requirements = Path(temp_dir) / "requirements.txt"
            requirements.write_text("-r docs-requirements.txt\n", encoding="utf-8")

            with self.assertRaises(OSError):
                pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

    def test_unsupported_requirements_include_syntax_fails_closed(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs-requirements.txt").write_text("mdurl==0.1.2\n", encoding="utf-8")
            requirements = root / "requirements.txt"
            for unsupported in (
                "--requirement docs-requirements.txt",
                "-r=docs-requirements.txt",
                "--requirement=docs-requirements.txt",
                "-rdocs-requirements.txt",
                "-r docs requirements.txt",
                "-r docs-requirements.txt # shared docs lock",
                "-r",
            ):
                with self.subTest(include=unsupported):
                    requirements.write_text(f"{unsupported}\n", encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "unsupported requirements include"):
                        pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

            requirements.write_text("-r docs-requirements.txt\n", encoding="utf-8")
            pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

    def test_unsupported_pip_constraint_controls_fail_closed(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs-requirements.txt").write_text("mdurl==0.1.2\n", encoding="utf-8")
            requirements = root / "requirements.txt"
            for unsupported in (
                "-c docs-requirements.txt",
                "-cdocs-requirements.txt",
                "-c=docs-requirements.txt",
                "--constraint docs-requirements.txt",
                "--constraint=docs-requirements.txt",
            ):
                with self.subTest(constraint=unsupported):
                    requirements.write_text(f"{unsupported}\n", encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "unsupported requirements constraint"):
                        pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

    def test_a_constraint_changing_lock_can_never_reuse_a_cached_environment(self):
        # A ready environment is reused by fingerprint alone and pip applies
        # constraint files at install time, so a constraint-bearing lock must
        # fail the fingerprint regardless of the constraint content instead of
        # letting a cached environment silently skip a changed constraint.
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs-requirements.txt").write_text("mdurl==0.1.2\n", encoding="utf-8")
            constraint = root / "constraint.txt"
            requirements = root / "requirements.txt"
            requirements.write_text("-r docs-requirements.txt\n-c constraint.txt\n", encoding="utf-8")
            for pinned in ("ruff==0.15.22\n", "ruff==0.16.10\n"):
                with self.subTest(constraint=pinned):
                    constraint.write_text(pinned, encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "unsupported requirements constraint"):
                        pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

    def test_unknown_pip_option_lines_fail_closed(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs-requirements.txt").write_text("mdurl==0.1.2\n", encoding="utf-8")
            requirements = root / "requirements.txt"
            for unsupported in (
                "--index-url https://example.invalid/simple",
                "--extra-index-url=https://example.invalid/simple",
                "-f wheels",
                "--trusted-host example.invalid",
                "-e .",
                "--pre",
            ):
                with self.subTest(option=unsupported):
                    requirements.write_text(f"{unsupported}\n", encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "unsupported pip requirements control"):
                        pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

    def test_variable_quoted_and_continuation_include_forms_fail_closed(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs-requirements.txt").write_text("mdurl==0.1.2\n", encoding="utf-8")
            requirements = root / "requirements.txt"
            for unsupported in (
                "-r ${DOCS_LOCK}",
                "-r $DOCS_LOCK",
                "-r %DOCS_LOCK%",
                '-r "docs-requirements.txt"',
                "-r 'docs-requirements.txt'",
                "-r \\",
                "-r docs-requirements.txt \\",
            ):
                with self.subTest(include=unsupported):
                    requirements.write_text(f"{unsupported}\ndocs-requirements.txt\n", encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "unsupported requirements"):
                        pythonenv.environment_fingerprint(versions, requirements, executable=r"C:\Python\python.exe")

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

    def test_utf8_bom_cannot_hide_included_lock_identity_or_unsupported_controls(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            child = root / "child.txt"
            child.write_text("mdurl==0.1.2\n", encoding="utf-8")
            requirements = root / "requirements.txt"
            requirements.write_text("\ufeff-r child.txt\n", encoding="utf-8")
            first = pythonenv.environment_fingerprint(versions, requirements)
            child.write_text("mdurl==0.1.3\n", encoding="utf-8")
            self.assertNotEqual(first, pythonenv.environment_fingerprint(versions, requirements))
            for control in ("-rchild.txt", "-c child.txt", "--constraint=child.txt"):
                with self.subTest(control=control):
                    requirements.write_text("\ufeff" + control + "\n", encoding="utf-8")
                    with self.assertRaises(RuntimeError):
                        pythonenv.environment_fingerprint(versions, requirements)

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

    def test_requirements_include_cycle_fails_the_fingerprint(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.txt"
            second = root / "second.txt"
            first.write_text("-r second.txt\n", encoding="utf-8")
            second.write_text("-r first.txt\n", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "requirements include cycle"):
                pythonenv.environment_fingerprint(versions, first, executable=r"C:\Python\python.exe")

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

    def test_version_probe_requires_the_exact_documentation_parser_versions(self):
        versions = dict(LOCKED_TOOL_VERSIONS)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            environment = root / "environment"
            (environment / "bin").mkdir(parents=True)
            (environment / "bin" / "python").touch()
            (environment / pythonenv.COMPLETE_MARKER).write_text("stamp\n", encoding="utf-8")

            def probe(installed):
                def run(command, **_kwargs):
                    text = " ".join(command)
                    if "markdown-it-py" in text or "mdurl" in text:
                        package = "markdown-it-py" if "markdown-it-py" in text else "mdurl"
                        found = installed[package]
                        if found is None:
                            return subprocess.CompletedProcess(command, 1, "PackageNotFoundError")
                        return subprocess.CompletedProcess(command, 0, found)
                    if "ruff" in text:
                        return subprocess.CompletedProcess(command, 0, f"ruff {versions['ruff']}")
                    if "mypy" in text:
                        return subprocess.CompletedProcess(command, 0, f"mypy {versions['mypy']}")
                    return subprocess.CompletedProcess(command, 0, "")

                return run

            cases = (
                ({"markdown-it-py": "4.2.00", "mdurl": versions["mdurl"]}, False),
                ({"markdown-it-py": "4.2.0.dev0", "mdurl": versions["mdurl"]}, False),
                ({"markdown-it-py": versions["markdown-it-py"], "mdurl": "0.1.20"}, False),
                ({"markdown-it-py": None, "mdurl": versions["mdurl"]}, False),
                ({"markdown-it-py": versions["markdown-it-py"], "mdurl": versions["mdurl"]}, True),
            )
            for installed, ready in cases:
                with self.subTest(installed=installed):
                    with mock.patch.object(pythonenv.subprocess, "run", side_effect=probe(installed)):
                        self.assertIs(
                            pythonenv._tools_match(
                                environment, versions, root / "state", "stamp", platform_name="macos"
                            ),
                            ready,
                        )

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

    def test_ci_documentation_job_installs_the_shared_docs_lock_before_the_check(self):
        workflow = (PROJECT_ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        check_step = workflow.index("- name: Check documentation")
        install_step = workflow[:check_step].rsplit("- name:", 1)[1]

        self.assertIn("pip install", install_step)
        self.assertIn("script/ao/docs-requirements.txt", install_step)
        self.assertIn("--require-hashes", install_step)
        self.assertIn("--only-binary=:all:", install_step)
        self.assertIn("--no-deps", install_step)


if __name__ == "__main__":
    unittest.main()
