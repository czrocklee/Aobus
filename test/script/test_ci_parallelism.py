"""CI routing and failure propagation that actionlint cannot verify."""

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
CODE_SCOPE = "needs.changes.outputs.docs-only == 'false'"


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


def _condition(block: str, indent: int) -> str:
    value = _field(block, indent, "if").lstrip(">|- \n")
    return " ".join(value.removeprefix("${{").removesuffix("}}").split())


def _needs(job: str) -> set[str]:
    return set(re.findall(r"[\w-]+", _field(job, 4, "needs"))) - {"-"}


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


def _run_verdict(step: str, context: dict[str, str]) -> subprocess.CompletedProcess[str]:
    # Feed the actual workflow bindings so a swapped result cannot pass the shell test.
    bindings = re.findall(r"(?m)^ *(\w+):[ \t]*\$\{\{\s*([\w.-]+)\s*\}\}", _field(step, 8, "env"))
    return _run_bash(step, {name: context[source] for name, source in bindings if source in context})


class GateRoutingTest(unittest.TestCase):
    def test_code_changes_trigger_macos_and_windows(self):
        for job in ("macos", "windows"):
            with self.subTest(job=job):
                native = _job(job)
                self.assertIn("changes", _needs(native))
                self.assertEqual(_condition(native, 4), CODE_SCOPE)

    def test_repository_checks_are_required_for_code_and_docs_routes(self):
        repository = _job("repository")
        self.assertIn(_field(repository, 4, "continue-on-error"), ("", "false"))
        code = _step(repository, "Check repository")
        self.assertEqual(_condition(code, 8), CODE_SCOPE)
        self.assertRegex(code, r"\bcheck repository\b")
        docs = _step(repository, "Check documentation")
        self.assertEqual(_condition(docs, 8), "needs.changes.outputs.docs-only == 'true'")
        self.assertRegex(docs, r"\bcheck docs\b")
        self.assertRegex(docs, r"\bcheck format --docs\b")
        for step in (code, docs):
            self.assertIn(_field(step, 8, "continue-on-error"), ("", "false"))

    def test_native_completion_is_required(self):
        for job in ("linux", "macos", "windows"):
            with self.subTest(job=job):
                native = _job(job)
                self.assertIn(_field(native, 4, "continue-on-error"), ("", "false"))
                gate = _step(native, "Check Bazel completion")
                self.assertRegex(gate, r"(?m)^ +(?:run: )?(?:\./ao|\.\\ao\.bat) check --ci\b")
                self.assertEqual(_condition(gate, 8), "")
                self.assertIn(_field(gate, 8, "continue-on-error"), ("", "false"))


@unittest.skipUnless(os.name == "posix", "Linux CI steps require bash")
class LinuxVerdictTest(unittest.TestCase):
    def test_required_linux_status_covers_repository_and_native_work(self):
        job = _job("linux_check")
        self.assertEqual(_field(job, 4, "name"), "Linux / check")  # Existing required status.
        self.assertLessEqual({"changes", "repository", "linux"}, _needs(job))
        self.assertEqual(_condition(job, 4), "always()")
        self.assertIn(_field(job, 4, "continue-on-error"), ("", "false"))
        step = _step(job, "Require the results for the classified scope")
        self.assertIn(_condition(step, 8), ("", "always()"))
        self.assertIn(_field(step, 8, "continue-on-error"), ("", "false"))
        for scope, docs_only, repository, linux, accepted in (
            ("success", "true", "success", "skipped", True),
            ("success", "false", "success", "success", True),
            ("failure", "true", "success", "skipped", False),
            ("success", "true", "success", "cancelled", False),
            ("success", "true", "failure", "skipped", False),
            ("success", "false", "failure", "success", False),
            ("success", "false", "success", "failure", False),
            ("success", "false", "success", "skipped", False),
        ):
            with self.subTest(scope=scope, docs_only=docs_only, repository=repository, linux=linux):
                result = _run_verdict(
                    step,
                    {
                        "needs.changes.result": scope,
                        "needs.changes.outputs.docs-only": docs_only,
                        "needs.repository.result": repository,
                        "needs.linux.result": linux,
                    },
                )
                self.assertEqual(result.returncode == 0, accepted, result.stdout + result.stderr)

    def test_bare_bazel_build_failure_survives_logging(self):
        step = _step(_job("linux"), "Test direct Bazel entry")
        self.assertIn(_field(step, 8, "continue-on-error"), ("", "false"))
        root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="bare bazel ")))
        bazel = root / "bazel"
        bazel.write_text(
            "#!/usr/bin/env bash\n"
            '[[ " $* " == *" build "* && " $* " == *" --disk_cache=$AOBUS_BAZEL_DISK_CACHE "* ]] || exit 2\n'
            "[[ -z ${AOBUS_BAZEL_CONFIGURATION+x} && -z ${AOBUS_BAZEL_REMOTE_CACHE+x} ]] || exit 3\n"
            ': > "$0.ran"\n'
            "exit 37\n",
            encoding="utf-8",
        )
        bazel.chmod(0o755)
        result = _run_bash(
            step,
            {
                "RUNNER_TEMP": str(root),
                "FAKE_BAZEL": str(bazel),
                "TEST_PYTHON": sys.executable,
                "PYTHONDONTWRITEBYTECODE": "1",
                "AOBUS_BAZEL_DISK_CACHE": str(root / "cache"),
                "AOBUS_STATE_ROOT": str(root / "state"),
                "CMAKE_BUILD_PARALLEL_LEVEL": "4",
                "AOBUS_BAZEL_CONFIGURATION": "must-not-inherit",
                "AOBUS_BAZEL_REMOTE_CACHE": "must-not-inherit",
            },
            cwd=PROJECT_ROOT,
            prelude='nix-shell() { printf "%s\\t%s\\t%s\\n" "$FAKE_BAZEL" /fixture/jdk "$TEST_PYTHON"; }\n',
        )
        self.assertTrue(bazel.with_suffix(".ran").is_file(), result.stdout + result.stderr)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


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
