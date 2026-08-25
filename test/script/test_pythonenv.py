"""Tests for the native Python tooling bootstrap."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import pythonenv


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

    def test_base_python_must_match_the_pinned_patch_release(self):
        with self.assertRaisesRegex(RuntimeError, "requires Python 0.0.0"):
            pythonenv.validate_base_python({"python": "0.0.0"})

    def test_native_platform_selection_rejects_linux(self):
        self.assertEqual(pythonenv._native_platform_name("darwin"), "macos")
        self.assertEqual(pythonenv._native_platform_name("win32"), "windows")
        with self.assertRaisesRegex(RuntimeError, "unavailable on platform 'linux'"):
            pythonenv._native_platform_name("linux")

    def test_toolchain_and_hashed_lock_pin_the_validated_windows_wheels(self):
        versions = pythonenv.tool_versions()
        requirements = pythonenv.REQUIREMENTS_FILE.read_text(encoding="utf-8")

        self.assertEqual(versions, {"python": "3.14.6", "ruff": "0.15.22", "mypy": "2.1.0"})
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

    def test_fingerprint_changes_with_requirements_or_base_python(self):
        versions = {"python": "3.14.6", "ruff": "0.15.22", "mypy": "2.1.0"}
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
        versions = {"python": "3.14.6", "ruff": "0.15.22", "mypy": "2.1.0"}
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
        versions = {"python": "3.14.6", "ruff": "0.15.22", "mypy": "2.1.0"}
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

    def test_builder_installs_only_hash_locked_binary_packages(self):
        versions = {"python": "3.14.6", "ruff": "0.15.22", "mypy": "2.1.0"}
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
