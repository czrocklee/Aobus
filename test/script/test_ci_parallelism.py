"""CI wiring that could go green without running or enforcing native checks."""

import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"


def _job(name: str) -> str:
    remainder = WORKFLOW.read_text(encoding="utf-8").split(f"  {name}:\n", 1)[1]
    return re.split(r"\n(?=  \w+:\n)", remainder, maxsplit=1)[0]


def _step(job: str, name: str) -> str:
    remainder = job.split(f"      - name: {name}\n", 1)[1]
    return re.split(r"(?m)^      - ", remainder, maxsplit=1)[0]


def _field(block: str, indent: int, key: str) -> str:
    prefix = " " * indent
    match = re.search(rf"(?m)^{prefix}{key}:[ \t]*(.*(?:\n{prefix}  .+)*)", block)
    return match[1].strip() if match else ""


def _run_bash(
    step: str, env: dict[str, str], *, cwd: Path | None = None, prelude: str = ""
) -> subprocess.CompletedProcess[str]:
    # GitHub enables pipefail for explicit bash, but not for the Linux default shell.
    options = ["--noprofile", "--norc", "-eo", "pipefail"] if _field(step, 8, "shell") == "bash" else ["-e"]
    body = step.split("        run: |\n", 1)[1]
    return subprocess.run(
        ["bash", *options, "-c", prelude + textwrap.dedent(body)],
        cwd=cwd,
        env={"PATH": os.environ["PATH"], **env},
        capture_output=True,
        text=True,
        timeout=10,
    )


class NativeGateRoutingTest(unittest.TestCase):
    def test_only_successful_docs_only_validation_can_skip_native_jobs(self):
        # actionlint checks syntax, not whether a valid expression skips a required job.
        for name in ("linux", "macos", "windows"):
            with self.subTest(job=name):
                job = _job(name)
                header, _, steps = job.partition("\n    steps:\n")
                self.assertRegex(header, r"(?m)^    needs: changes$")
                condition = re.search(r"(?m)^    if:\s*(?:>-?\s*)?\$\{\{(.*?)\}\}", header, re.DOTALL)
                self.assertIsNotNone(condition)
                self.assertEqual(
                    " ".join(condition[1].split()),
                    "always() && !cancelled() && "
                    "(needs.changes.result != 'success' || needs.changes.outputs.docs-only != 'true')",
                )
                prerequisite = _step(job, "Require successful scope and documentation validation")
                self.assertTrue(
                    steps.startswith("      - name: Require successful scope and documentation validation\n")
                )
                self.assertIn("if: ${{ needs.changes.result != 'success' }}", prerequisite)
                self.assertIn("run: exit 1", prerequisite)
                self.assertNotIn("continue-on-error:", prerequisite)

    def test_complete_graph_cannot_be_conditionally_skipped_or_advisory(self):
        for name in ("linux", "macos", "windows"):
            with self.subTest(job=name):
                job = _job(name)
                gate = _step(job, "Check Bazel completion")
                self.assertRegex(gate, r"(?m)^ +(?:run: )?(?:\./ao|\.\\ao\.bat) check --ci\b")
                self.assertNotRegex(gate, r"(?m)^        if:")
                required = _step(job, "Require successful Bazel checks") if name == "linux" else gate
                self.assertNotIn("continue-on-error:", required)
                if name == "linux":
                    self.assertIn("if: ${{ !cancelled() }}", required)
                    self.assertIn("BAZEL_CHECK_OUTCOME: ${{ steps.bazel-check.outcome }}", required)


@unittest.skipUnless(os.name == "posix", "Linux CI steps require bash")
class LinuxGateFailureTest(unittest.TestCase):
    def test_failed_completion_is_rejected_after_evidence_collection(self):
        step = _step(_job("linux"), "Require successful Bazel checks")
        for outcome, code in (("success", 0), ("failure", 1), ("skipped", 1)):
            with self.subTest(outcome=outcome):
                result = _run_bash(step, {"BAZEL_CHECK_OUTCOME": outcome})
                self.assertEqual(result.returncode, code, result.stderr)

    def test_direct_bazel_runs_without_portal_environment_and_failure_survives_tee(self):
        step = _step(_job("linux"), "Test direct Bazel entry")
        self.assertIn("if: ${{ steps.bazel-check.outcome == 'success' }}", step)
        self.assertNotIn("continue-on-error:", step)
        # GitHub's explicit bash shell uses pipefail; the default shell does not.
        self.assertIn("shell: bash", step)
        with tempfile.TemporaryDirectory(prefix="bare bazel ") as temporary:
            root = Path(temporary)
            bazel = root / "bazel"
            bazel.write_text(
                "#!/usr/bin/env bash\n"
                "[[ -z ${AOBUS_BAZEL_CONFIGURATION+x} && -z ${AOBUS_BAZEL_REMOTE_CACHE+x} ]] || exit 3\n"
                '[[ " $* " == *" test "* ]] || exit 4\n'
                "exit 37\n",
                encoding="utf-8",
            )
            bazel.chmod(0o755)
            # Stand in for the pinned-tool lookup, not the direct Bazel call.
            nix = 'nix-shell() { printf "%s\\t%s\\n" "$PWD/bazel" /fixture/jdk; }\n'
            result = _run_bash(
                step,
                {
                    "RUNNER_TEMP": temporary,
                    "AOBUS_STATE_ROOT": str(root / "state"),
                    "CMAKE_BUILD_PARALLEL_LEVEL": "4",
                    "AOBUS_BAZEL_CONFIGURATION": "must-not-inherit",
                    "AOBUS_BAZEL_REMOTE_CACHE": "must-not-inherit",
                },
                cwd=root,
                prelude=nix,
            )
            self.assertEqual(result.returncode, 37, result.stderr)


@unittest.skipUnless(sys.platform == "linux", "Cache trim uses GNU find")
class LinuxCacheTrimTest(unittest.TestCase):
    def test_trim_requires_a_marker_and_only_removes_restored_ac_and_cas_entries(self):
        step = _step(_job("linux"), "Trim restored Bazel disk cache")
        root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="bazel cache ")))
        cache, marker = root / "cache", root / "marker"
        old, cutoff, fresh = 100, 200, 300
        untouched = {"ac/old": old, "cas/nested/at-cutoff": cutoff}
        retained = {
            "ac/new": fresh,
            "cas/nested/new": fresh,
            "gc/lock": old,
            "tmp/in-progress": old,
            "unknown/old": old,
        }
        for name, mtime in (untouched | retained).items():
            path = cache / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
            os.utime(path, (mtime, mtime))

        def entries():
            return {path.relative_to(cache).as_posix() for path in cache.rglob("*") if path.is_file()}

        env = {"AOBUS_BAZEL_DISK_CACHE": str(cache), "AOBUS_BAZEL_CACHE_MARKER": str(marker)}
        result = _run_bash(step, env, cwd=root)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(entries(), set(untouched) | set(retained))

        marker.touch()
        os.utime(marker, (cutoff, cutoff))
        result = _run_bash(step, env, cwd=root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(entries(), set(retained))


if __name__ == "__main__":
    unittest.main()
