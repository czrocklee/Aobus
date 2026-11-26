"""Scoped native input observations shared by independent repository probes."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_native as native
from ao.portal import bazel_workspace


class IsolatedClientEnvironmentTests(unittest.TestCase):
    def test_isolated_environment_removes_all_rc_key_spellings_without_mutating_the_caller(self):
        environment = dict(
            os.environ,
            BAZELRC="/missing/one.rc,/missing/two.rc",
            bAzElRc="/missing/mixed.rc",
            AOBUS_RC_TEST_SENTINEL="retained",
        )
        original = dict(environment)
        isolated = native.child_environment(environment, isolate_rc=True)
        self.assertFalse(any(key.casefold() == "bazelrc" for key in isolated))
        self.assertEqual(isolated["AOBUS_RC_TEST_SENTINEL"], "retained")
        self.assertEqual(environment, original)
        ordinary = native.child_environment(environment)
        self.assertEqual(ordinary["BAZELRC"], environment["BAZELRC"])
        self.assertEqual(ordinary["bAzElRc"], environment["bAzElRc"])

    def test_completion_launch_does_not_pass_personal_rc_paths_to_a_real_child(self):
        with tempfile.TemporaryDirectory(prefix="aobus-rc-child-") as directory:
            root = Path(directory).resolve()
            valid = root / "personal.rc"
            valid.write_text("test --test_arg=--list-tests\n", encoding="utf-8")
            missing = root / "missing.rc"
            child = (
                "import json,os; print(json.dumps({k:v for k,v in os.environ.items() "
                "if k.casefold() == 'bazelrc' or k == 'AOBUS_RC_TEST_SENTINEL'}))"
            )
            environment = {
                "AOBUS_STATE_ROOT": str(root / "state"),
                "BAZELRC": f"{valid},{missing}",
                "AOBUS_RC_TEST_SENTINEL": "retained",
            }
            output = root / "build.log"
            with (
                mock.patch.dict(os.environ, environment),
                mock.patch.object(
                    bazel_workspace, "invocation", return_value=[sys.executable, "-I", "-S", "-B", "-c", child]
                ) as invocation,
            ):
                status = bazel_workspace.execute(
                    Path(sys.executable),
                    root,
                    root,
                    "build",
                    "debug",
                    ["//test:async_test"],
                    isolate_rc=True,
                    log=output,
                )
                self.assertEqual(status, 0)
            invocation.assert_called_once_with(Path(sys.executable), root, isolate_rc=True)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), {"AOBUS_RC_TEST_SENTINEL": "retained"})


class GovernedPythonIdentityTests(unittest.TestCase):
    def test_base_and_venv_installation_aliases_share_one_literal_identity(self):
        with tempfile.TemporaryDirectory(prefix="aobus-python-identity-") as directory:
            root = Path(directory).resolve()
            executable = root / "python"
            executable.touch()
            alias = root / "opt-python"
            try:
                alias.symlink_to(executable)
            except OSError as error:
                self.skipTest(f"file symlinks are unavailable: {error}")
            with mock.patch.object(sys, "_base_executable", str(alias)):
                base_identity = native.governed_base_executable()
            with mock.patch.object(sys, "_base_executable", str(executable)):
                venv_identity = native.governed_base_executable()
            self.assertEqual(base_identity, executable)
            self.assertEqual(venv_identity, executable)


class InputObservationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-native-observation-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_nested_observer_restores_the_outer_scope_after_failure(self):
        outer, inner = [], []
        with native.capture_probes(outer.append):
            native.probe(self.root / "before")
            with self.assertRaisesRegex(ValueError, "fixture failure"):
                with native.capture_probes(inner.append):
                    native.probe(self.root / "inside")
                    raise ValueError("fixture failure")
            native.probe(self.root / "after")
        native.probe(self.root / "outside")
        self.assertEqual(outer, [self.root / "before", self.root / "after"])
        self.assertEqual(inner, [self.root / "inside"])

    def test_suspended_observation_does_not_leak_or_replace_the_outer_observer(self):
        observed = []
        with native.capture_probes(observed.append):
            with native.capture_probes(None):
                native.probe(self.root / "muted")
            native.probe(self.root / "selected")
        with native.capture_probes(None):
            native.probe(self.root / "later")
        self.assertEqual(observed, [self.root / "selected"])


if __name__ == "__main__":
    unittest.main()
