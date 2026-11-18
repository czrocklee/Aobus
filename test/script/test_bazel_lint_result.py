"""The graph action can succeed while the Bazel test must report findings."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.graph import bazel_lint_result as result


class LintGraphResultTest(unittest.TestCase):
    def test_completed_finding_fails_the_test_and_replays_its_location(self):
        with tempfile.TemporaryDirectory(prefix="ao-lint-result-") as temporary:
            root = Path(temporary)
            status_file = root / "result.json"
            status_file.write_text('{"schema":1,"platform":"linux","status":1}', encoding="utf-8")
            log_file = root / "log.txt"
            finding = "lib/sample/Sample.cpp:12:1: warning: sample finding [aobus-license-header]\n"
            log_file.write_text(finding, encoding="utf-8")
            runfiles = mock.Mock()
            runfiles.Rlocation.side_effect = {
                "aobus/result.json": str(status_file),
                "aobus/log.txt": str(log_file),
            }.__getitem__
            with (
                mock.patch.object(result, "_create_runfiles", return_value=runfiles),
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                self.assertEqual(result.main(["aobus/result.json", "aobus/log.txt", "linux"]), 1)
            self.assertEqual(output.getvalue(), finding)


if __name__ == "__main__":
    unittest.main()
