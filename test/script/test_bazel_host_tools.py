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

    def publish_venv(self, host):
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
        (destination / python_env.COMPLETE_MARKER).write_text(fingerprint, encoding="utf-8")
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

    def test_native_runtime_is_independent_of_a_ready_portal_venv(self):
        major, minor, patch = map(int, self.versions["python"].split("."))
        for host, admitted_patch in (("macos", patch + 1), ("windows", patch)):
            with (
                self.subTest(host=host),
                mock.patch.object(sys, "version_info", (major, minor, admitted_patch)),
                mock.patch.object(subprocess, "run", side_effect=AssertionError("native base needs no package probe")),
            ):
                runtime, inputs = tools._python_runtime(host)
                venv = self.publish_venv(host)
                with mock.patch.object(sys, "executable", str(venv)):
                    self.assertEqual(tools._python_runtime(host), (runtime, inputs))
                self.assertEqual(
                    runtime,
                    {
                        "version": f"{major}.{minor}.{admitted_patch}",
                        "implementation": "cpython",
                        "executable": str(self.base),
                    },
                )
                self.assertEqual(inputs, [python_env.TOOLCHAIN_FILE, self.base])

    def test_native_runtime_rejects_versions_outside_host_policy(self):
        major, minor, patch = map(int, self.versions["python"].split("."))
        for host, version in (("windows", (major, minor, patch + 1)), ("macos", (major, minor + 1, patch))):
            with (
                self.subTest(host=host),
                mock.patch.object(sys, "version_info", version),
                self.assertRaisesRegex(RuntimeError, "Aobus requires Python"),
            ):
                tools._python_runtime(host)

    def test_linux_runtime_retains_the_nix_package_version_check(self):
        version = tuple(map(int, self.versions["python"].split(".")))
        with mock.patch.object(sys, "version_info", version), self.wheel_probe(self.base, mdurl=self.versions["mdurl"]):
            runtime, inputs = tools._python_runtime("linux")
        self.assertEqual(runtime["executable"], str(self.base))
        self.assertEqual(inputs, [python_env.TOOLCHAIN_FILE, self.base])

        with (
            mock.patch.object(sys, "version_info", version),
            self.wheel_probe(self.base, mdurl="0.0.0"),
            self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the pinned toolchain"),
        ):
            tools._python_runtime("linux")


if __name__ == "__main__":
    unittest.main()
