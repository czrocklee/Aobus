"""Tests for the shared ``phases.measure`` call spy in ``phase_support``."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from phase_support import PhaseRecorder

from ao.core import phases


class PhaseRecorderTest(unittest.TestCase):
    def test_records_name_metadata_and_default_status(self):
        recorder = PhaseRecorder()
        with recorder.measure("compile-commands", tree="winui", count=2):
            pass
        self.assertEqual(
            recorder.records,
            [{"name": "compile-commands", "tree": "winui", "count": 2, "status": 0}],
        )

    def test_status_assignment_is_recorded_not_an_outcome(self):
        recorder = PhaseRecorder()
        with recorder.measure("suite", tree="native", suite="core", jobs=1, count=1) as phase:
            phase.status = 7
        self.assertEqual(recorder.records[0]["status"], 7)

    def test_exception_without_assignment_keeps_the_default_status(self):
        # The with-body raising leaves the untouched default status 0; this spy
        # does not model production END/exception-wrapper status semantics.
        recorder = PhaseRecorder()
        with self.assertRaises(RuntimeError):
            with recorder.measure("configure", tree="winui"):
                raise RuntimeError("configure failed")
        self.assertEqual(recorder.records, [{"name": "configure", "tree": "winui", "status": 0}])

    def test_invalid_metadata_is_delegated_to_the_real_validator(self):
        recorder = PhaseRecorder()
        for name, kwargs in (
            ("phase", {"phase": "bogus"}),
            ("tree", {"phase": "build", "tree": "bogus"}),
            ("mode", {"phase": "build", "mode": "bogus"}),
            ("suite", {"phase": "build", "suite": "bogus"}),
            ("command", {"phase": "build", "command": "bogus"}),
            ("zero jobs", {"phase": "build", "jobs": 0}),
            ("negative count", {"phase": "build", "count": -1}),
            ("source_id phase", {"phase": "build", "source_id": "a" * 64}),
        ):
            with self.subTest(name=name), self.assertRaises(ValueError):
                with recorder.measure(**kwargs):
                    pass
        self.assertEqual(recorder.records, [])

    def test_phase_keyword_matches_the_production_signature(self):
        recorder = PhaseRecorder()
        with recorder.measure(phase="build", tree="native"):
            pass
        self.assertEqual(recorder.records, [{"name": "build", "tree": "native", "status": 0}])
        with self.assertRaises(TypeError):
            recorder.measure(name="build")
        self.assertEqual(len(recorder.records), 1)

    def test_unknown_keywords_raise_the_production_type_error(self):
        recorder = PhaseRecorder()
        with self.assertRaises(TypeError):
            recorder.measure("build", tree="native", arbitrary="accepted")
        self.assertEqual(recorder.records, [])

    def test_no_phase_log_records_are_written(self):
        recorder = PhaseRecorder()
        with tempfile.TemporaryDirectory() as temporary:
            log_dir = Path(temporary)
            with mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: temporary}):
                with recorder.measure("build", tree="native"):
                    pass
            self.assertEqual(list(log_dir.glob("*.jsonl")), [])
