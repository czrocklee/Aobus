"""CI summaries distinguish execution, cached results, failures and missing evidence."""

import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from ao.portal import bazel_ci_summary


class CiSummaryTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tree = Path(tmp.name)
        self.attempt = self.tree / "checks/fastbuild/check-one"
        self.attempt.mkdir(parents=True)

    def render(self, *args):
        with redirect_stdout(io.StringIO()) as output:
            self.assertEqual(bazel_ci_summary.main(["--tree", str(self.tree), *args]), 0)
        return output.getvalue()

    def events(self, *events):
        (self.attempt / "tests.bep.json").write_text("\n".join(map(json.dumps, events)), encoding="utf-8")

    def test_windows_tests_failed_exit_code_without_overall_success_is_a_failure(self):
        # Shape from PR #158's Windows BEP; protobuf omits the false boolean.
        self.events({"finished": {"exitCode": {"name": "TESTS_FAILED", "code": 3}}}, {"lastMessage": True})
        text = self.render()
        self.assertIn("status **failed (TESTS_FAILED)**", text)
        self.assertNotIn("unknown (TESTS_FAILED)", text)

    def test_no_log_or_truncated_stream_can_establish_success(self):
        (self.attempt / "test.log").write_text("bazel failed to start\n", encoding="utf-8")
        self.assertIn("completion status is unknown", self.render())
        self.events({"finished": {"overallSuccess": True, "exitCode": {"name": "SUCCESS"}}})
        text = self.render()
        self.assertIn("status **incomplete", text)
        self.assertNotIn("passed (SUCCESS)", text)

    def test_summary_appends_distinct_configured_tests_and_separate_cache_counters(self):
        def summary(label, config, runs=1, cached=0):
            return {
                "id": {"testSummary": {"label": label, "configuration": {"id": config}}},
                "testSummary": {"overallStatus": "PASSED", "totalRunCount": runs, "totalNumCached": cached},
            }

        self.events(
            summary("//test:unit", "a"),
            summary("@@//test:unit", "a", cached=1),
            summary("//test:unit", "b", runs=2),
            {"finished": {"overallSuccess": True, "exitCode": {"name": "SUCCESS"}}},
            {
                "lastMessage": True,
                "buildMetrics": {
                    "actionSummary": {
                        "actionCacheStatistics": {"hits": 4},
                        "runnerCount": [{"name": "disk cache hit", "count": 2}],
                    }
                },
            },
        )
        path = self.tree / "summary.md"
        path.write_text("prior content\n", encoding="utf-8")
        self.render("--github-summary", str(path))
        text = path.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("prior content\n"))
        self.assertIn("2 passed target(s), 2 executed run(s), 1 cached run(s)", text)
        self.assertIn("4 hit(s), 0 miss(es)", text)
        self.assertIn("disk cache hit: 2", text)

    def test_an_attempt_with_a_receipt_is_not_counted_again_from_its_bep(self):
        (self.attempt / "receipt.json").write_text(json.dumps({"schema": 1, "status": "passed"}), encoding="utf-8")
        self.events({"finished": {"overallSuccess": True}}, {"lastMessage": True})
        text = self.render()
        self.assertIn("#### `checks/fastbuild/check-one/receipt.json`", text)
        self.assertNotIn("#### `checks/fastbuild/check-one/tests.bep.json`", text)

    def test_complete_remote_accounting_requires_a_successful_stop(self):
        path = self.tree / "stop.json"
        path.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "started": True,
                    "upload": True,
                    "activity": {"uploadFailures": 0},
                    "lostUploads": 0,
                }
            ),
            encoding="utf-8",
        )

        def report(outcome):
            return self.render("--remote-report", str(path), "--remote-stop-outcome", outcome)

        self.assertIn("Drain: accounted complete", report("success"))
        self.assertIn("Drain: not accounted complete", report("failure"))
        path.unlink()
        self.assertIn("Drain: accounting unavailable", report("success"))


if __name__ == "__main__":
    unittest.main()
