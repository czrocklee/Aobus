"""CI workflow tests for exporting every runner core to build, tidy, and shard counts."""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from ao.command import test as test_command
from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"
CORE_STEP_NAME = "Use every runner core"
# The first step of each native job that runs portal build or cache work; the
# core step must already have exported its environment by then.
FIRST_PORTAL_STEP = {
    "linux": "Build and test with private PipeWire",
    "macos": "Prepare vcpkg binary cache",
    "windows": "Prepare vcpkg binary cache",
}
HYGIENE_JOBS_ARGUMENT = {
    "linux": '--jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
    "macos": '--jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
    "windows": "--jobs $env:CMAKE_BUILD_PARALLEL_LEVEL",
}


def _job(name: str) -> str:
    """Return the named job's YAML block, bounded by the following job header."""
    remainder = WORKFLOW.read_text(encoding="utf-8").split(f"  {name}:\n", 1)[1]
    return re.split(r"\n(?=  \w+:\n)", remainder, maxsplit=1)[0]


def _step(job: str, name: str) -> str:
    """Return one step's YAML block from a job block."""
    return job.split(f"      - name: {name}\n", 1)[1].split("      - name:", 1)[0]


def _run_body(step: str) -> str:
    """Return the script of a literal-block (`run: |`) or folded (`run: >-`) step."""
    if "        run: |\n" in step:
        return textwrap.dedent(step.split("        run: |\n", 1)[1])
    if "        run: >-\n" in step:
        # Folded scalars join their lines with single spaces.
        return " ".join(textwrap.dedent(step.split("        run: >-\n", 1)[1]).splitlines())
    raise AssertionError("step has no literal or folded run block")


class CoreStepWorkflowTest(unittest.TestCase):
    def test_every_native_job_runs_one_core_step_right_after_checkout(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(workflow.count(f"      - name: {CORE_STEP_NAME}\n"), 3)
        for job_name in FIRST_PORTAL_STEP:
            with self.subTest(job=job_name):
                job = _job(job_name)
                self.assertEqual(job.count(f"      - name: {CORE_STEP_NAME}\n"), 1)
                after_checkout = job.split("      - name: Check out repository\n", 1)[1]
                following = after_checkout.split("      - name: ", 1)[1].split("\n", 1)[0]
                self.assertEqual(following, CORE_STEP_NAME)
                self.assertLess(
                    job.index(f"      - name: {CORE_STEP_NAME}\n"),
                    job.index(f"      - name: {FIRST_PORTAL_STEP[job_name]}\n"),
                )

    def test_core_steps_are_unconditional_hard_gates(self):
        for job_name in FIRST_PORTAL_STEP:
            with self.subTest(job=job_name):
                step = _step(_job(job_name), CORE_STEP_NAME)
                self.assertNotIn("continue-on-error", step)
                # The macOS matrix must stay covered on both architectures.
                self.assertNotIn("\n        if:", step)

    def test_core_steps_use_each_platforms_ci_shell(self):
        for job_name, shell in (("linux", "bash"), ("macos", "bash"), ("windows", "pwsh")):
            with self.subTest(job=job_name):
                self.assertIn(f"        shell: {shell}\n", _step(_job(job_name), CORE_STEP_NAME))

    def test_hygiene_steps_tidy_with_the_detected_core_count(self):
        for job_name, argument in HYGIENE_JOBS_ARGUMENT.items():
            with self.subTest(job=job_name):
                self.assertIn(argument, _step(_job(job_name), "Run hygiene"))


@unittest.skipIf(os.name == "nt", "The CI bash core step runs on a POSIX tooling host.")
class CoreStepBashTest(unittest.TestCase):
    """The extracted bash core step against the real host, not controlled inputs."""

    # Both POSIX jobs share the bash core step; exercise every call site so a
    # future mac-only edit does not lose local execution protection.
    POSIX_JOBS = ("linux", "macos")

    @staticmethod
    def _run_step(job_name: str, github_env: Path) -> subprocess.CompletedProcess:
        """Run the extracted core step body; keep the ambient environment."""
        return subprocess.run(
            ["bash", "-euo", "pipefail", "-c", _run_body(_step(_job(job_name), CORE_STEP_NAME))],
            cwd=PROJECT_ROOT,
            # The workflow's runner provides Python on PATH; native tooling
            # uses the governed interpreter without requiring an ambient install.
            env={
                **os.environ,
                "PATH": f"{Path(sys.executable).parent}{os.pathsep}{os.environ.get('PATH', '')}",
                "GITHUB_ENV": str(github_env),
            },
            capture_output=True,
            text=True,
            check=False,
        )

    def test_the_extracted_step_exports_the_real_host_cores(self):
        for job_name in self.POSIX_JOBS:
            with self.subTest(job=job_name):
                with tempfile.TemporaryDirectory() as temporary:
                    github_env = Path(temporary) / "github-env"
                    process = self._run_step(job_name, github_env)
                    assignments = github_env.read_text(encoding="utf-8").splitlines() if github_env.exists() else []

                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(len(assignments), 2)
                name, value = assignments[0].split("=", 1)
                self.assertEqual(name, "CMAKE_BUILD_PARALLEL_LEVEL")
                cores = int(value)
                self.assertGreater(cores, 0)
                self.assertEqual(assignments[1], f"AOBUS_TEST_SHARDS={min(cores, test_command.SHARD_CAP)}")
                self.assertIn(f"build and tidy jobs={cores}", process.stdout)
                self.assertIn(f"test shards={min(cores, test_command.SHARD_CAP)}", process.stdout)

    def test_an_unwritable_destination_fails_the_step(self):
        # A directory is not a writable GITHUB_ENV destination, so the export fails.
        for job_name in self.POSIX_JOBS:
            with self.subTest(job=job_name):
                with tempfile.TemporaryDirectory() as temporary:
                    process = self._run_step(job_name, Path(temporary))

                self.assertNotEqual(process.returncode, 0, process.stdout)
                self.assertIn("Error:", process.stderr)


@unittest.skipIf(os.name != "nt", "The CI core step runs on Windows; exercise it on a native Windows host.")
class CoreStepWindowsTest(unittest.TestCase):
    """The extracted pwsh core step against the real host, not controlled inputs."""

    @classmethod
    def setUpClass(cls):
        # CI hosts run pwsh 7; native Windows also accepts the built-in Windows PowerShell.
        cls.shell = shutil.which("pwsh") or shutil.which("powershell.exe")
        if cls.shell is None:
            # This class already runs only on Windows; a missing shell is a host defect.
            raise RuntimeError("Neither pwsh nor powershell.exe is installed on this Windows host")

    def _run_step(self, github_env: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                self.shell,
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                _run_body(_step(_job("windows"), CORE_STEP_NAME)),
            ],
            cwd=PROJECT_ROOT,
            env={
                **os.environ,
                "GITHUB_ENV": str(github_env),
                "PATH": f"{Path(sys.executable).parent}{os.pathsep}{os.environ.get('PATH', '')}",
            },
            capture_output=True,
            text=True,
            check=False,
        )

    def test_the_extracted_step_exports_the_real_host_cores(self):
        with tempfile.TemporaryDirectory() as temporary:
            github_env = Path(temporary) / "github-env"
            process = self._run_step(github_env)
            assignments = github_env.read_text(encoding="utf-8").splitlines() if github_env.exists() else []

        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(len(assignments), 2)
        name, value = assignments[0].split("=", 1)
        self.assertEqual(name, "CMAKE_BUILD_PARALLEL_LEVEL")
        cores = int(value)
        self.assertGreater(cores, 0)
        self.assertEqual(assignments[1], f"AOBUS_TEST_SHARDS={min(cores, test_command.SHARD_CAP)}")
        self.assertIn(f"build and tidy jobs={cores}", process.stdout)
        self.assertIn(f"test shards={min(cores, test_command.SHARD_CAP)}", process.stdout)

    def test_an_unwritable_destination_fails_the_step(self):
        # A directory is not a writable GITHUB_ENV destination, so the export fails.
        with tempfile.TemporaryDirectory() as temporary:
            process = self._run_step(Path(temporary))

        self.assertNotEqual(process.returncode, 0, process.stdout)
        self.assertIn("Error:", process.stderr)


if __name__ == "__main__":
    unittest.main()
