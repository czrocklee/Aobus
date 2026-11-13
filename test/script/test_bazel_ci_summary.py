"""CI evidence summary rendering: exact reported counters and never-green missing evidence."""

import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from ao.core import bazel_ci_summary


def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _line(event: dict) -> str:
    return json.dumps(event) + "\n"


class CiSummaryTest(unittest.TestCase):
    def test_mixed_cache_counts_render_exactly_and_append_preserves_prior_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            _write(
                tree / "checks/fastbuild/check-t1/receipt.json",
                json.dumps(
                    {
                        "schema": 1,
                        "config": "fastbuild",
                        "status": "passed",
                        "exit": 0,
                        "test_cache_policy": "fresh",
                        "test_summary": {"passed": 2, "executed_runs": 3, "cached_runs": 1},
                        "phases": [
                            {"name": "guardrails", "status": "passed", "elapsed_seconds": 1.25},
                            {"name": "native-context", "status": "passed", "elapsed_seconds": 9.0},
                        ],
                    }
                ),
            )
            _write(
                tree / "checks/fastbuild/check-t1/build.bep.json",
                _line(
                    {
                        "id": {"buildMetrics": {}},
                        "buildMetrics": {
                            "timingMetrics": {"wallTimeInMs": "1234"},
                            "actionSummary": {
                                "runnerCount": [
                                    {"name": "total", "count": 5},
                                    {"name": "disk cache hit", "count": 2, "execKind": "Remote"},
                                    {"name": "local", "count": 3, "execKind": "Local"},
                                ],
                                "actionCacheStatistics": {"hits": 4},
                            },
                        },
                    }
                )
                + _line(
                    {"id": {"buildFinished": {}}, "finished": {"overallSuccess": True, "exitCode": {"name": "SUCCESS"}}}
                ),
            )
            summary = _write(Path(temporary) / "step-summary.md", "prior content\n")
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(bazel_ci_summary.main(["--tree", str(tree), "--github-summary", str(summary)]), 0)
            text = output.getvalue()
            self.assertIn("passed (SUCCESS)", text)
            self.assertIn("disk cache hit: 2 (Remote)", text)
            self.assertIn("4 hit(s), 0 miss(es)", text)  # zero-omitted misses stay zero, not unknown
            self.assertIn("| 1.2 |", text)  # wallTimeInMs 1234 renders as seconds
            self.assertIn("2 passed target(s), 3 executed run(s), 1 cached run(s)", text)
            written = summary.read_text(encoding="utf-8")
            self.assertTrue(written.startswith("prior content\n"))
            self.assertIn("disk cache hit: 2 (Remote)", written)

    def test_missing_and_truncated_evidence_is_never_green(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            _write(tree / "checks/debug/check-t1/receipt.json", "{ not json")
            _write(
                tree / "checks/debug/check-t1/build.bep.json",
                _line(
                    {
                        "id": {"buildMetrics": {}},
                        "buildMetrics": {"actionSummary": {"actionCacheStatistics": {"hits": 1}}},
                    }
                )
                + '{"id": {"progress": {"trunc',
            )
            _write(
                tree / "checks/debug/check-t1/tests.bep.json",
                _line(
                    {"id": {"buildFinished": {}}, "finished": {"overallSuccess": True, "exitCode": {"name": "SUCCESS"}}}
                )
                + '{"id": {"progress": {"trunc',
            )
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(
                    bazel_ci_summary.main(
                        ["--tree", str(tree), "--remote-start", "skipped", "--remote-stop-outcome", "failure"]
                    ),
                    0,
                )
            text = output.getvalue()  # evidence gaps render unknown; they never mask the product result
            self.assertIn("unavailable (no readable schema-1 receipt)", text)
            self.assertIn("incomplete (no finished event", text)
            self.assertIn(
                "incomplete (SUCCESS; stream truncated)", text
            )  # a finished event never rescues a truncated tail
            self.assertIn("accounting unavailable", text)
            self.assertNotIn("accounted complete", text)
            self.assertNotIn("passed (SUCCESS)", text)

    def test_remote_activity_counters_require_started_and_a_successful_stop(self):
        with tempfile.TemporaryDirectory() as temporary:
            stopped = _write(
                Path(temporary) / "stop.json",
                json.dumps(
                    {
                        "schemaVersion": 1,
                        "started": True,
                        "upload": True,
                        "activity": {
                            "queued": 5,
                            "uploaded": 5,
                            "pending": 0,
                            "uploadFailures": 0,
                            "downloaded": 9,
                            "downloadMisses": 2,
                        },
                        "lostUploads": 0,
                    }
                ),
            )
            output = io.StringIO()
            with redirect_stdout(output):
                bazel_ci_summary.main(["--remote-report", str(stopped), "--remote-stop-outcome", "success"])
            text = output.getvalue()
            self.assertIn("5 queued, 5 uploaded, 0 pending, 0 failed", text)
            self.assertIn("9 served, 2 miss(es)", text)
            self.assertIn("Drain: accounted complete", text)

            output = io.StringIO()
            with redirect_stdout(output):
                bazel_ci_summary.main(["--remote-report", str(stopped), "--remote-stop-outcome", "failure"])
            text = output.getvalue()  # valid traffic accounting still cannot claim a drain the stop step failed
            self.assertIn("Drain: not accounted complete", text)
            self.assertNotIn("Drain: accounted complete", text)

            not_started = _write(
                Path(temporary) / "skipped.json",
                json.dumps(
                    {
                        "schemaVersion": 1,
                        "started": False,
                        "upload": False,
                        "activity": {"queued": 0, "uploaded": 0, "pending": 0, "uploadFailures": 0},
                        "lostUploads": 0,
                    }
                ),
            )
            output = io.StringIO()
            with redirect_stdout(output):
                bazel_ci_summary.main(["--remote-report", str(not_started), "--remote-stop-outcome", "success"])
            text = output.getvalue()
            self.assertNotIn("queued", text)  # activity counters stay hidden when no cache was started
            self.assertIn("Drain: not applicable (no cache was started)", text)
            self.assertNotIn("Drain: not applicable (uploads disabled)", text)  # started=false is not an upload state


if __name__ == "__main__":
    unittest.main()
