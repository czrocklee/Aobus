"""Tests for the shared default worker-baseline policy."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import textwrap
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

    def test_a_failed_affinity_read_propagates(self):
        affinity = SimpleNamespace(sched_getaffinity=mock.Mock(side_effect=OSError("no affinity")))
        with mock.patch.object(jobs, "os", affinity), self.assertRaises(OSError):
            jobs.available_cores()


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


class BuildWorkersTest(unittest.TestCase):
    def test_an_ambient_build_override_does_not_query_the_default(self):
        with (
            mock.patch.dict(jobs.os.environ, {jobs.BUILD_PARALLEL_ENV: "2"}),
            mock.patch.object(jobs, "default_workers") as baseline,
        ):
            self.assertEqual(jobs.build_workers(), 2)
            baseline.assert_not_called()

    def test_without_an_override_the_environment_reaches_the_shared_policy(self):
        environment = {jobs.CI_ENV: "true"}
        with mock.patch.object(jobs, "default_workers", return_value=8) as baseline:
            self.assertEqual(jobs.build_workers(environ=environment), 8)
            baseline.assert_called_once_with(environ=environment)

    def test_an_override_that_is_not_a_positive_integer_is_rejected(self):
        for configured in ("none", "3.5", "", "0", "-2"):
            with (
                self.subTest(configured=configured),
                self.assertRaises(ValueError) as raised,
            ):
                jobs.build_workers(environ={jobs.BUILD_PARALLEL_ENV: configured})
            self.assertEqual(str(raised.exception), "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.")


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

    def test_writes_the_build_and_shard_exports(self):
        # One count below and one above the cap cover the shared SHARD_CAP policy.
        for cores in (4, 32):
            with self.subTest(cores=cores):
                github_env = self.root / f"github-env-{cores}"
                with mock.patch.object(jobs, "available_cores", return_value=cores):
                    status, output, _ = self._run_cli(github_env)
                shards = min(cores, jobs.SHARD_CAP)
                self.assertEqual(status, 0, output)
                self.assertEqual(
                    github_env.read_text(encoding="utf-8").splitlines(),
                    [f"{jobs.BUILD_PARALLEL_ENV}={cores}", f"{jobs.SHARD_ENV}={shards}"],
                )
                self.assertEqual(
                    output,
                    f"Detected {cores} runner cores: build and tidy jobs={cores}, test shards={shards}.\n",
                )

    def test_appends_to_an_existing_environment_file(self):
        github_env = self.root / "github-env"
        github_env.write_text("PRIOR=kept\n", encoding="utf-8")
        with mock.patch.object(jobs, "available_cores", return_value=8):
            status, output, _ = self._run_cli(github_env)
        self.assertEqual(status, 0, output)
        self.assertEqual(
            github_env.read_text(encoding="utf-8").splitlines(),
            ["PRIOR=kept", f"{jobs.BUILD_PARALLEL_ENV}=8", f"{jobs.SHARD_ENV}=8"],
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

    def test_an_unreadable_count_fails_without_writing_the_environment(self):
        github_env = self.root / "github-env"
        with mock.patch.object(jobs, "available_cores", side_effect=OSError("affinity unavailable")):
            status, output, errors = self._run_cli(github_env)
        self.assertNotEqual(status, 0, output)
        self.assertFalse(github_env.exists())
        self.assertIn("Error: affinity unavailable", errors)


@unittest.skipIf(not hasattr(os, "sched_setaffinity"), "The host has no sched_setaffinity to restrict.")
class AffinityAgreementTest(unittest.TestCase):
    """A restricted child proves the export and the worker baseline share one discovery path."""

    def test_a_two_core_child_exports_and_defaults_agree(self):
        if len(os.sched_getaffinity(0)) < 2:
            self.skipTest("the host allows fewer than two CPUs")
        script_dir = Path(jobs.__file__).resolve().parents[2]
        # Only this child process's affinity is restricted; no host policy changes.
        probe = textwrap.dedent(
            f"""
            import os
            os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:2])
            import sys
            sys.path.insert(0, {str(script_dir)!r})
            from ao.core import jobs
            cores = jobs.available_cores()
            assert cores == 2, cores
            assert jobs.default_workers(environ={{jobs.CI_ENV: "true"}}) == cores
            assert jobs.default_workers(environ={{}}) == cores - 1
            from pathlib import Path
            jobs.export_runner_cores(github_env=Path(sys.argv[1]))
            """
        )
        with tempfile.TemporaryDirectory() as temporary:
            github_env = Path(temporary) / "github-env"
            process = subprocess.run(
                [sys.executable, "-c", probe, str(github_env)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(
                github_env.read_text(encoding="utf-8").splitlines(),
                [f"{jobs.BUILD_PARALLEL_ENV}=2", f"{jobs.SHARD_ENV}=2"],
            )


if __name__ == "__main__":
    unittest.main()
