"""The Bazel runtime bridge must publish the interpreter it can actually use."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_native as native
from ao.portal import bazel_host_tools as tools
from ao.portal import python_env


class PythonRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-python-runtime-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.state = self.root / "state"
        self.base = self.root / "python/bin/python"
        self.base.parent.mkdir(parents=True)
        self.base.touch()
        self.versions = python_env.tool_versions()
        self.enterContext(mock.patch.object(native, "PROJECT_ROOT", self.root))
        self.enterContext(mock.patch.object(sys, "_base_executable", str(self.base), create=True))
        self.enterContext(
            mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": str(self.state), "AOBUS_CHECKOUT_ID": "test"})
        )

    def publish_venv(self, host, *, stale=False):
        macos = host == "macos"
        requirements = python_env.MACOS_REQUIREMENTS_FILE if macos else python_env.REQUIREMENTS_FILE
        fingerprint = python_env.environment_fingerprint(
            self.versions, requirements, runtime_version=".".join(map(str, sys.version_info[:3]))
        )
        destination = python_env.environment_path(
            self.root, self.state, fingerprint, environ=dict(os.environ), os_name="posix" if macos else "nt"
        )
        python = destination / ("bin/python" if macos else "Scripts/python.exe")
        python.parent.mkdir(parents=True)
        python.touch()
        (destination / python_env.COMPLETE_MARKER).write_text("stale" if stale else fingerprint, encoding="utf-8")
        return python

    def wheel_probe(self, python, *, mdurl):
        def run(argv, **kwargs):
            self.assertEqual(argv[:3], [str(python), "-I", "-c"])
            self.assertIn("version('markdown-it-py')", argv[-1])
            self.assertIn("version('mdurl')", argv[-1])
            self.assertEqual(len(argv), 4)
            version = ".".join(map(str, sys.version_info[:3]))
            return subprocess.CompletedProcess(argv, 0, f"{version}\n{self.versions['markdown-it-py']}\n{mdurl}\n", "")

        return mock.patch.object(subprocess, "run", side_effect=run)

    def test_a_macos_venv_publishes_the_admitted_patch_and_executable(self):
        major, minor, patch = map(int, self.versions["python"].split("."))
        with mock.patch.object(sys, "version_info", (major, minor, patch + 1)):
            venv = self.publish_venv("macos")
            with self.wheel_probe(venv, mdurl=self.versions["mdurl"]):
                runtime, _, _ = tools._python_runtime("macos")
        self.assertEqual(runtime["version"], f"{major}.{minor}.{patch + 1}")
        self.assertEqual(runtime["executable"], str(venv))

    def test_a_stale_completion_marker_does_not_admit_a_windows_venv(self):
        self.publish_venv("windows", stale=True)
        with mock.patch.object(subprocess, "run", side_effect=AssertionError("must not run the stale interpreter")):
            runtime, _, _ = tools._python_runtime("windows")
        self.assertEqual(runtime["executable"], str(self.base))
        self.assertEqual(runtime["tooling_python"], "")

    def test_the_selected_runtime_cannot_publish_a_mismatched_docs_dependency(self):
        venv = self.publish_venv("windows")
        with self.wheel_probe(venv, mdurl="0.0.0"), self.assertRaises(native.BazelWorkspaceError):
            tools._python_runtime("windows")


if __name__ == "__main__":
    unittest.main()
