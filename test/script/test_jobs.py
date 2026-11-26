"""Tests for the shared default worker-baseline policy."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import jobs


class AvailableCoresTest(unittest.TestCase):
    def test_discovery_prefers_affinity_and_preserves_raw_unknown_counts(self):
        for count in (0, 5):
            affinity = SimpleNamespace(sched_getaffinity=mock.Mock(return_value=range(count)))
            with self.subTest(affinity=count), mock.patch.object(jobs, "os", affinity):
                self.assertEqual(jobs.available_cores(), count)
        for count in (None, 0, 1, 4, 32):
            logical = SimpleNamespace(cpu_count=mock.Mock(return_value=count))
            with self.subTest(logical=count), mock.patch.object(jobs, "os", logical):
                self.assertEqual(jobs.available_cores(), count)


class DefaultWorkersTest(unittest.TestCase):
    def test_defaults_reserve_one_locally_but_never_return_zero(self):
        for count, ci, expected in (
            (8, False, 7),
            (8, True, 8),
            (1, False, 1),
            (None, False, 1),
            (None, True, 1),
            (0, False, 1),
        ):
            with self.subTest(count=count, ci=ci), mock.patch.object(jobs, "available_cores", return_value=count):
                self.assertEqual(jobs.default_workers(environ={jobs.CI_ENV: "true"} if ci else {}), expected)

    def test_the_ambient_environment_is_read_by_default(self):
        with (
            mock.patch.dict(jobs.os.environ, {jobs.CI_ENV: "true"}),
            mock.patch.object(jobs, "available_cores", return_value=6),
        ):
            self.assertEqual(jobs.default_workers(), 6)


class ExportCiTest(unittest.TestCase):
    """The standalone export-ci CLI the CI workflow step calls."""

    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.root = Path(self._temporary.name)

    def _run_cli(self, github_env: Path) -> tuple[int, str, str]:
        with (
            contextlib.redirect_stdout(io.StringIO()) as output,
            contextlib.redirect_stderr(io.StringIO()) as errors,
        ):
            status = jobs.main(["export-ci", "--github-env", str(github_env)])
        return status, output.getvalue(), errors.getvalue()

    def test_writes_the_runner_worker_export(self):
        for cores in (4, 32):
            with self.subTest(cores=cores):
                github_env = self.root / f"github-env-{cores}"
                with mock.patch.object(jobs, "available_cores", return_value=cores):
                    status, output, _ = self._run_cli(github_env)
                self.assertEqual(status, 0, output)
                self.assertEqual(
                    github_env.read_text(encoding="utf-8").splitlines(),
                    [f"AOBUS_CI_JOBS={cores}"],
                )
                self.assertEqual(
                    output,
                    f"Detected {cores} runner cores: Bazel and tidy jobs={cores}.\n",
                )

    def test_appends_to_an_existing_environment_file(self):
        github_env = self.root / "github-env"
        github_env.write_text("PRIOR=kept\n", encoding="utf-8")
        with mock.patch.object(jobs, "available_cores", return_value=8):
            status, output, _ = self._run_cli(github_env)
        self.assertEqual(status, 0, output)
        self.assertEqual(
            github_env.read_text(encoding="utf-8").splitlines(),
            ["PRIOR=kept", "AOBUS_CI_JOBS=8"],
        )

    def test_an_unknown_count_does_not_create_or_change_the_environment_file(self):
        for prior in (None, "PRIOR=kept\n"):
            for cores in (None, 0):
                with self.subTest(cores=cores, prior=prior):
                    github_env = self.root / f"github-env-{cores}"
                    if prior is not None:
                        github_env.write_text(prior, encoding="utf-8")
                    with mock.patch.object(jobs, "available_cores", return_value=cores):
                        status, output, errors = self._run_cli(github_env)
                    self.assertNotEqual(status, 0, output)
                    if prior is None:
                        self.assertFalse(github_env.exists())
                    else:
                        self.assertEqual(github_env.read_text(encoding="utf-8"), prior)
                    self.assertIn("expected a positive integer", errors)


if __name__ == "__main__":
    unittest.main()
