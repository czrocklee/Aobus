"""CI workflow tests for exporting every runner core to build, tidy, and shard counts."""

import os
import re
import shlex
import shutil
import stat
import subprocess
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
    """Return the script of a literal-block (`run: |`) step."""
    return textwrap.dedent(step.split("        run: |\n", 1)[1])


def _install_fake_getconf(directory: Path, output: str, status: int) -> None:
    """Install a getconf stub reporting a chosen core count or command failure."""
    stub = directory / "getconf"
    stub.write_text(
        f"#!/bin/sh\nprintf '%s\\n' {shlex.quote(output)}\nexit {status}\n",
        encoding="utf-8",
    )
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)


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


@unittest.skipIf(os.name == "nt", "The CI core steps run in Bash; exercise them on a POSIX tooling host.")
class CoreStepBashTest(unittest.TestCase):
    def _run_step(self, job_name: str, output: str, status: int = 0) -> tuple[subprocess.CompletedProcess, list[str]]:
        """Run one extracted core step against a fake getconf; keep the ambient environment.

        Returns the process and the GITHUB_ENV assignments it wrote, or an empty
        list when it failed before creating the file.
        """
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _install_fake_getconf(root, output, status)
            github_env = root / "github-env"
            process = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", _run_body(_step(_job(job_name), CORE_STEP_NAME))],
                env={
                    **os.environ,
                    "PATH": f"{root}{os.pathsep}{os.environ.get('PATH', '')}",
                    "GITHUB_ENV": str(github_env),
                },
                capture_output=True,
                text=True,
                check=False,
            )
            assignments = github_env.read_text(encoding="utf-8").splitlines() if github_env.exists() else []
        return process, assignments

    def test_detected_cores_drive_the_build_and_shard_exports(self):
        for job_name in ("linux", "macos"):
            # Counts above the cap pin the workflow's literal to ao.command.test.SHARD_CAP.
            for cores in ("1", "4", "32", "100"):
                with self.subTest(job=job_name, cores=cores):
                    process, assignments = self._run_step(job_name, cores)

                    self.assertEqual(process.returncode, 0, process.stderr)
                    shards = min(int(cores), test_command.SHARD_CAP)
                    self.assertEqual(
                        assignments,
                        [f"CMAKE_BUILD_PARALLEL_LEVEL={cores}", f"AOBUS_TEST_SHARDS={shards}"],
                    )
                    self.assertIn(f"build and tidy jobs={cores}", process.stdout)
                    self.assertIn(f"test shards={shards}", process.stdout)

    def test_invalid_core_counts_fail_loud_without_writing_the_environment(self):
        for job_name in ("linux", "macos"):
            for output, status in (("", 0), ("0", 0), ("-2", 0), ("text", 0), ("4", 1)):
                with self.subTest(job=job_name, output=output, status=status):
                    process, assignments = self._run_step(job_name, output, status)

                    self.assertNotEqual(process.returncode, 0, process.stdout)
                    self.assertEqual(assignments, [])
                    if status == 0:
                        self.assertIn("expected a positive decimal core count", process.stderr)
                    else:
                        self.assertIn("::error::getconf _NPROCESSORS_ONLN failed", process.stderr)


@unittest.skipIf(os.name != "nt", "The CI core step runs in pwsh; exercise it on a native Windows host.")
class CoreStepPwshTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # CI hosts run pwsh 7; native Windows also accepts the built-in Windows PowerShell.
        cls.shell = shutil.which("pwsh") or shutil.which("powershell.exe")
        if cls.shell is None:
            # This class already runs only on Windows; a missing shell is a host defect.
            raise RuntimeError("Neither pwsh nor powershell.exe is installed on this Windows host")

    def _run_step(self, cores: str | None) -> tuple[subprocess.CompletedProcess, list[str]]:
        """Run the Windows core step with its native or test-owned input binding.

        Returns the process and the GITHUB_ENV assignments it wrote, or an empty
        list when it failed before creating the file.
        """
        with tempfile.TemporaryDirectory() as temporary:
            github_env = Path(temporary) / "github-env"
            environment = dict(os.environ)
            environment["GITHUB_ENV"] = str(github_env)
            script = _run_body(_step(_job("windows"), CORE_STEP_NAME))
            if cores is not None:
                # Windows synthesizes reads of NUMBER_OF_PROCESSORS even when
                # its raw environment entry was overwritten. Substitute only
                # that input binding; the validation/cap/export body is intact.
                self.assertIn("$env:NUMBER_OF_PROCESSORS", script)
                environment["AOBUS_TEST_RUNNER_CORES"] = cores
                script = script.replace("$env:NUMBER_OF_PROCESSORS", "$env:AOBUS_TEST_RUNNER_CORES")
            process = subprocess.run(
                [self.shell, "-NoProfile", "-NonInteractive", "-Command", script],
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            assignments = self._assignments(github_env.read_bytes()) if github_env.exists() else []
        return process, assignments

    @staticmethod
    def _assignments(payload: bytes) -> list[str]:
        # pwsh 7 appends UTF-8; Windows PowerShell 5.1 appends UTF-16 with a BOM.
        if payload.startswith(b"\xff\xfe"):
            return payload.decode("utf-16").splitlines()
        return payload.decode("utf-8").splitlines()

    def test_native_input_binding_reports_a_positive_host_count(self):
        process, assignments = self._run_step(None)

        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(len(assignments), 2)
        name, value = assignments[0].split("=", 1)
        self.assertEqual(name, "CMAKE_BUILD_PARALLEL_LEVEL")
        cores = int(value)
        self.assertGreater(cores, 0)
        self.assertEqual(assignments[1], f"AOBUS_TEST_SHARDS={min(cores, test_command.SHARD_CAP)}")

    def test_detected_cores_drive_the_build_and_shard_exports(self):
        # Counts above the cap pin the workflow's literal to ao.command.test.SHARD_CAP.
        for cores in ("1", "4", "32", "100"):
            with self.subTest(cores=cores):
                process, assignments = self._run_step(cores)

                self.assertEqual(process.returncode, 0, process.stderr)
                shards = min(int(cores), test_command.SHARD_CAP)
                self.assertEqual(
                    assignments,
                    [f"CMAKE_BUILD_PARALLEL_LEVEL={cores}", f"AOBUS_TEST_SHARDS={shards}"],
                )
                self.assertIn(f"build and tidy jobs={cores}", process.stdout)
                self.assertIn(f"test shards={shards}", process.stdout)

    def test_invalid_core_counts_fail_loud_without_writing_the_environment(self):
        for cores in ("", "0", "-2", "text"):
            with self.subTest(cores=cores):
                process, assignments = self._run_step(cores)

                self.assertNotEqual(process.returncode, 0, process.stdout)
                self.assertEqual(assignments, [])
                self.assertIn("expected a positive integer", process.stderr)


if __name__ == "__main__":
    unittest.main()
