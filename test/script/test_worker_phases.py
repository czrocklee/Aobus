"""Worker-thread phase attribution boundaries.

Pool workers do not inherit the submitting thread's ``ContextVar`` state.
These tests pin the two boundaries that propagation must not cross: a reused
worker thread never leaks one task's context into the next, and a bare pool
worker started outside any phase stays unparented. Phase attribution through
the native tidy runner and the lint fixture stage is owned by the workflow
owner's linttest suite and the Bazel lint pipeline's own tests.
"""

import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

from ao.core import phases

ROOT = Path(__file__).resolve().parents[2]


class PhaseLogFixture(unittest.TestCase):
    """Per-test ``AOBUS_PHASE_LOG_DIR`` handling and record readers."""

    def set_phase_log_dir(self, value: str | None) -> None:
        original = os.environ.get(phases.LOG_DIR_ENV)
        if value is None:
            os.environ.pop(phases.LOG_DIR_ENV, None)
        else:
            os.environ[phases.LOG_DIR_ENV] = value

        def restore() -> None:
            if original is None:
                os.environ.pop(phases.LOG_DIR_ENV, None)
            else:
                os.environ[phases.LOG_DIR_ENV] = original

        self.addCleanup(restore)

    def enable_phase_logging(self, directory: Path) -> None:
        self.set_phase_log_dir(str(directory))

    def disable_phase_logging(self) -> None:
        self.set_phase_log_dir(None)

    def phase_rows(self, directory: Path) -> list[dict]:
        rows = []
        for path in sorted(Path(directory).glob("*.jsonl")):
            for line in path.read_text(encoding="utf-8").splitlines():
                rows.append(json.loads(line))
        return rows

    def strict_consumer(self, directory: Path) -> subprocess.CompletedProcess:
        """Run the real strict record consumer in a fresh process."""
        env = dict(os.environ)
        env.pop(phases.LOG_DIR_ENV, None)
        env["PYTHONPATH"] = str(ROOT / "script")
        return subprocess.run(
            [sys.executable, "-m", "ao.core.phases", str(directory)],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )


class UnparentedWorkerTest(PhaseLogFixture):
    def test_a_bare_pool_worker_outside_any_phase_stays_unparented(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_dir = Path(temporary)
            self.enable_phase_logging(log_dir)
            started = threading.Event()
            done = threading.Event()

            def work() -> None:
                started.set()
                with phases.measure("analysis-item", tree="tidy", count=1):
                    done.wait(10)

            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                worker = pool.submit(work)
                self.assertTrue(started.wait(10))
                with phases.measure("suite", suite="lint"):
                    pass
                done.set()
            self.assertTrue(worker.done())

            begins = [row for row in self.phase_rows(log_dir) if row["kind"] == "BEGIN"]
            by_phase = {row["phase"]: row for row in begins}
            # Propagation is per submitted item, never thread-wide: a worker
            # started outside any phase invents no parent and no command,
            # even while the submitting thread measures its own phase.
            self.assertNotIn("parent_id", by_phase["analysis-item"])
            self.assertNotIn("command", by_phase["analysis-item"])
            self.assertNotIn("parent_id", by_phase["suite"])
            self.assertNotIn("command", by_phase["suite"])

            consumer = self.strict_consumer(log_dir)
            self.assertEqual(consumer.returncode, 0, consumer.stderr)


if __name__ == "__main__":
    unittest.main()
