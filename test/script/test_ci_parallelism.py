"""CI convergence, native instrumentation and runner-wide completion parallelism."""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"
CORE_STEP_NAME = "Use every runner core"
# The first step of each native job that runs portal build or cache work; the
# core step must already have exported its environment by then.
FIRST_PORTAL_STEP = {
    "linux": "Provision Bazel tree",
    "macos": "Prepare vcpkg binary cache",
    "windows": "Prepare vcpkg binary cache",
}
CHECK_JOBS_ARGUMENT = {
    "linux": '--jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
    "macos": '--jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
    "windows": "--jobs $env:CMAKE_BUILD_PARALLEL_LEVEL",
}
# The flat gate command each platform's shell actually invokes.
CHECK_COMMANDS = {
    "linux": "./ao check",
    "macos": "./ao check",
    "windows": r".\ao.bat check",
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
        self.assertEqual(workflow.count(f"      - name: {CORE_STEP_NAME}\n"), len(FIRST_PORTAL_STEP))
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
        for job_name, shell in (
            ("linux", "bash"),
            ("macos", "bash"),
            ("windows", "pwsh"),
        ):
            with self.subTest(job=job_name):
                self.assertIn(f"        shell: {shell}\n", _step(_job(job_name), CORE_STEP_NAME))

    def test_each_native_job_uses_one_fresh_complete_gate_with_the_verified_base_and_core_count(self):
        for job_name, argument in CHECK_JOBS_ARGUMENT.items():
            with self.subTest(job=job_name):
                job = _job(job_name)
                self.assertEqual(job.count("      - name: Check Bazel completion\n"), 1)
                gate = _step(job, "Check Bazel completion")
                self.assertIn(argument, gate)
                base = "$env:BASE_SHA" if job_name == "windows" else '"$BASE_SHA"'
                self.assertIn(f"{CHECK_COMMANDS[job_name]} --fresh --commit {base}", gate)
                self.assertIn("BASE_SHA: ${{ needs.changes.outputs.base }}", gate)
                self.assertNotIn("\n        if:", gate)
                if job_name == "linux":
                    # Independent failure collection still feeds a mandatory gate.
                    required = _step(job, "Require successful Bazel checks")
                    self.assertNotIn("continue-on-error:", required)
                    self.assertIn("BAZEL_CHECK_OUTCOME: ${{ steps.bazel-check.outcome }}", required)
                    self.assertIn('"$BAZEL_CHECK_OUTCOME" != success', required)
                else:
                    self.assertNotIn("continue-on-error:", gate)
                self.assertNotIn("      - name: Run hygiene\n", job)
                self.assertNotIn("      - name: Test portal tooling\n", job)
                self.assertNotRegex(job, r"(?:\./ao|\.\\ao\.bat) (?:build|test|run|hygiene)\b")

    def test_completion_artifacts_include_the_default_fastbuild_receipts(self):
        for job_name, step_name, separator in (
            ("linux", "Preserve Bazel completion evidence", "/"),
            ("macos", "Preserve Bazel evidence", "/"),
            ("windows", "Preserve Bazel evidence", "\\"),
        ):
            with self.subTest(job=job_name):
                evidence = _step(_job(job_name), step_name)
                self.assertIn(f"checks{separator}fastbuild{separator}check-*", evidence)
                self.assertIn(f"contracts{separator}fastbuild{separator}verify-*", evidence)
        # Sanitizer contract analysis remains Debug even though normal
        # completion now writes fastbuild receipts.
        linux_evidence = _step(_job("linux"), "Preserve Bazel completion evidence")
        for profile in ("asan", "tsan"):
            self.assertIn(f"aobus-bazel-{profile}-analysis/contracts/debug/verify-*", linux_evidence)

    def test_linux_instrumentation_remains_required_by_aggregation_and_cache_promotion(self):
        job = _job("linux")
        smoke = _step(job, "Check Bazel instrumentation smoke")
        self.assertIn("AOBUS_BAZEL_EXECUTABLE: ${{ steps.bazel-executable.outputs.executable }}", smoke)
        required = _step(job, "Require successful Bazel checks")
        self.assertIn("BAZEL_INSTRUMENTATION_OUTCOME: ${{ steps.bazel-instrumentation.outcome }}", required)
        self.assertIn('"$BAZEL_INSTRUMENTATION_OUTCOME" != success', required)
        self.assertIn("steps.bazel-instrumentation.outcome == 'success'", _step(job, "Save Bazel disk cache"))


@unittest.skipIf(os.name == "nt", "The CI bash core step runs on a POSIX tooling host.")
class CoreStepBashTest(unittest.TestCase):
    """The extracted bash core step against the real host, not controlled inputs."""

    # Exercise every POSIX call site so a future mac-only edit does not lose
    # local execution protection.
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
                self.assertEqual(len(assignments), 1)
                name, value = assignments[0].split("=", 1)
                self.assertEqual(name, "CMAKE_BUILD_PARALLEL_LEVEL")
                cores = int(value)
                self.assertGreater(cores, 0)
                self.assertIn(f"Bazel and tidy jobs={cores}", process.stdout)

    def test_an_unwritable_destination_fails_the_step(self):
        # A directory is not a writable GITHUB_ENV destination, so the export fails.
        for job_name in self.POSIX_JOBS:
            with self.subTest(job=job_name):
                with tempfile.TemporaryDirectory() as temporary:
                    process = self._run_step(job_name, Path(temporary))

                self.assertNotEqual(process.returncode, 0, process.stdout)
                self.assertIn("Error:", process.stderr)


@unittest.skipIf(os.name == "nt", "The Linux instrumentation smoke runs in Bash.")
class LinuxInstrumentationStepTest(unittest.TestCase):
    def _run_smoke(self, root: Path, *, fail_profile: str = "") -> tuple[subprocess.CompletedProcess, list[dict]]:
        # Run the real CI script, replacing only its external command boundary.
        fake = textwrap.dedent("""\
            import json, os, sys
            from pathlib import Path
            tool = Path(sys.argv[0]).name
            args = sys.argv[1:]
            with open(os.environ["CALLS"], "a") as sink:
                sink.write(json.dumps({"tool": tool, "args": args}) + "\\n")
            if tool == "ao":
                path = Path(args[args.index("--path") + 1])
                path.mkdir(parents=True, exist_ok=True)
                if args[0] == "verify":
                    (path / "build.log").write_text("analysis\\n")
                elif args[:2] != ["coverage", "--analysis-only"]:
                    sys.exit(2)
            elif "build" in args and args[-1] == "//test:utility_test":
                profile = next(arg.split("=", 1)[1] for arg in args if arg.startswith("--config="))
                event = next(arg.split("=", 1)[1] for arg in args if arg.startswith("--build_event_json_file="))
                Path(event).write_text("{}\\n")
                print("native canary " + profile)
                if profile == os.environ["FAIL_PROFILE"]:
                    sys.exit(7)
            else:
                sys.exit(2)
            """)
        for name in ("ao", "pinned-bazel"):
            executable = root / name
            executable.write_text(f"#!{sys.executable}\n{fake}", encoding="utf-8")
            executable.chmod(0o755)
        calls_path = root / "calls.jsonl"
        process = subprocess.run(
            ["bash", "-euo", "pipefail", "-c", _run_body(_step(_job("linux"), "Check Bazel instrumentation smoke"))],
            cwd=root,
            env={
                **os.environ,
                "RUNNER_TEMP": str(root),
                "AOBUS_STATE_ROOT": str(root / "state"),
                "AOBUS_BAZEL_EXECUTABLE": str(root / "pinned-bazel"),
                "CMAKE_BUILD_PARALLEL_LEVEL": "3",
                "CALLS": str(calls_path),
                "FAIL_PROFILE": fail_profile,
            },
            capture_output=True,
            text=True,
            check=False,
        )
        return process, [json.loads(line) for line in calls_path.read_text().splitlines()]

    def test_native_canaries_keep_both_profiles_analysis_coverage_and_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            process, calls = self._run_smoke(root)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertEqual([call["tool"] for call in calls], ["ao", "pinned-bazel", "ao", "pinned-bazel", "ao"])
            for profile, verify, build in (("asan", calls[0], calls[1]), ("tsan", calls[2], calls[3])):
                with self.subTest(profile=profile):
                    analysis = root / f"aobus-bazel-{profile}-analysis"
                    self.assertEqual(
                        verify["args"], ["verify", "--config", profile, "--path", str(analysis), "--jobs", "3"]
                    )
                    self.assertIn(f"--output_base={analysis}/state/output", build["args"])
                    self.assertIn(f"--config={profile}", build["args"])
                    self.assertIn("--jobs=3", build["args"])
                    self.assertIn("--lockfile_mode=error", build["args"])
                    self.assertEqual(build["args"][-1], "//test:utility_test")
                    self.assertTrue((analysis / "canary-build.bep.json").is_file())
                    self.assertEqual((analysis / "build.log").read_text(), f"analysis\nnative canary {profile}\n")
            self.assertEqual(
                calls[-1]["args"],
                ["coverage", "--analysis-only", "--path", str(root / "aobus-bazel-coverage-analysis"), "--jobs", "3"],
            )

    def test_native_failure_is_not_hidden_by_log_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, calls = self._run_smoke(Path(temporary), fail_profile="asan")
        self.assertEqual(process.returncode, 7, process.stdout + process.stderr)
        self.assertEqual([call["tool"] for call in calls], ["ao", "pinned-bazel"])


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
        self.assertEqual(len(assignments), 1)
        name, value = assignments[0].split("=", 1)
        self.assertEqual(name, "CMAKE_BUILD_PARALLEL_LEVEL")
        cores = int(value)
        self.assertGreater(cores, 0)
        self.assertIn(f"Bazel and tidy jobs={cores}", process.stdout)

    def test_an_unwritable_destination_fails_the_step(self):
        # A directory is not a writable GITHUB_ENV destination, so the export fails.
        with tempfile.TemporaryDirectory() as temporary:
            process = self._run_step(Path(temporary))

        self.assertNotEqual(process.returncode, 0, process.stdout)
        self.assertIn("Error:", process.stderr)


if __name__ == "__main__":
    unittest.main()
