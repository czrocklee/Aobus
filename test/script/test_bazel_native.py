"""Scoped native input observations shared by independent repository probes."""

import tempfile
import unittest
from pathlib import Path

from ao.core import bazel_native as native


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
