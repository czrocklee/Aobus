"""Local phase measurements and immutable Bazel completion evidence in CI."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao import __main__ as portal
from ao.core import phases, proc


class PortalPhaseTest(unittest.TestCase):
    def test_root_command_records_returned_failure_without_changing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            arguments = mock.Mock(command="check", func=mock.Mock(return_value=7))
            with (
                mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: temporary}),
                mock.patch.object(portal, "parse_arguments", return_value=arguments),
            ):
                self.assertEqual(portal.main(["check"]), 7)
            rows = [json.loads(line) for file in directory.glob("*.jsonl") for line in file.read_text().splitlines()]
            self.assertEqual([row["kind"] for row in rows], ["BEGIN", "END"])
            self.assertEqual(rows[0]["command"], "check")
            self.assertEqual(rows[1]["status"], 7)
            self.assertEqual(rows[1]["outcome"], "fail")
            arguments.func.assert_called_once_with(arguments)

    def test_command_exception_is_recorded_as_failure_and_reraised_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = mock.Mock(command="build")
            arguments.func.side_effect = RuntimeError("private diagnostic")
            with (
                mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: temporary}),
                mock.patch.object(portal, "parse_arguments", return_value=arguments),
                self.assertRaises(RuntimeError),
            ):
                portal.main(["build"])
            text = "".join(file.read_text() for file in Path(temporary).glob("*.jsonl"))
            self.assertNotIn("private diagnostic", text)
            end = json.loads(text.splitlines()[-1])
            self.assertEqual(end["status"], 1)
            self.assertEqual(end["outcome"], "fail")

    def test_command_exit_error_keeps_its_own_status_in_the_phase_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = mock.Mock(command="build")
            arguments.func.side_effect = proc.die("concise portal error", code=3)
            with (
                mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: temporary}),
                mock.patch.object(portal, "parse_arguments", return_value=arguments),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as caught,
            ):
                portal.main(["build"])
            self.assertEqual(caught.exception.code, 3)
            text = "".join(file.read_text() for file in Path(temporary).glob("*.jsonl"))
            self.assertNotIn("concise portal error", text)
            end = json.loads(text.splitlines()[-1])
            self.assertEqual(end["status"], 3)
            self.assertEqual(end["outcome"], "fail")


if __name__ == "__main__":
    unittest.main()
