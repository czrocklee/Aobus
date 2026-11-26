"""CI routing and failure propagation that actionlint cannot verify."""

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from fnmatch import fnmatchcase
from pathlib import Path

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"
RELEASE_WORKFLOW = WORKFLOW.with_name("release.yml")
CODE_SCOPE = "needs.changes.outputs.docs-only == 'false'"
COMPLETION_DIAGNOSTIC = "completion fixture stderr diagnostic"
LINT_BASE_SHA = "a1" * 20
COMPLETION_CASES = (("", ["--extend"], 0), (LINT_BASE_SHA, ["--lint-base", LINT_BASE_SHA], 37))


def _job(name: str, workflow: Path = WORKFLOW) -> str:
    remainder = workflow.read_text(encoding="utf-8").split(f"  {name}:\n", 1)[1]
    return re.split(r"\n(?=  \w+:\n)", remainder, maxsplit=1)[0]


def _step(job: str, name: str) -> str:
    remainder = job.split(f"      - name: {name}\n", 1)[1]
    return re.split(r"(?m)^      - ", remainder, maxsplit=1)[0]


def _field(block: str, indent: int, key: str) -> str:
    prefix = " " * indent
    match = re.search(rf"(?m)^{prefix}{key}:[ \t]*(.*(?:\n{prefix}  .+)*)", block)
    return match[1].strip() if match else ""


def _windows_completion_step() -> str:
    (step,) = (block for block in re.split(r"(?m)^      - ", _job("windows")) if _field(block, 8, "id") == "build")
    return step


def _condition(block: str, indent: int) -> str:
    value = _field(block, indent, "if").lstrip(">|- \n")
    return " ".join(value.removeprefix("${{").removesuffix("}}").split())


def _needs(job: str) -> set[str]:
    return set(re.findall(r"[\w-]+", _field(job, 4, "needs"))) - {"-"}


def _direct_nix_shell_lines(workflow: str) -> list[int]:
    """Heuristically scan inline and literal-block workflow `run:` scalars only.

    Match common shell command positions, simple assignments, and backslash
    continuations. This is not shell parsing: quoting and here-docs are not
    interpreted, and folded YAML scalars are not scanned.
    """
    assignment = r"[a-zA-Z_]\w*=(?:'[^']*'|\"[^\"]*\"|[^\s'\";|&()]*)\s+"
    direct_call = re.compile(
        r"(?:^|\$\(|[;|&])\s*['\"]?(?:(?:command|exec)\s+)?(?:env\s+)?"
        rf"(?:{assignment})*(?:[\w./-]+/)?nix-shell(?=$|[\s'\");|&])"
    )
    calls = []
    run_indent = None
    folded = False
    pending = ""
    start = 0
    for number, line in enumerate(workflow.splitlines(), 1):
        indent = len(line) - len(line.lstrip())
        if run_indent is not None and (not line.strip() or indent > run_indent):
            if folded:
                continue
            command = line
        else:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            pending = ""
            run_indent = None
            if not (run := re.match(r"^ *(?:- )?(?P<key>run): *(?P<body>.*)$", line)):
                continue
            command = run["body"]
            if re.match(r"^[|>][1-9+-]{0,2}(?:\s|$)", command):
                run_indent = run.start("key")
                folded = command.startswith(">")
                continue
        if not pending:
            start = number
        command = re.split(r"(?:^|\s)#", pending + command, maxsplit=1)[0]
        if command.endswith("\\"):
            pending = command[:-1]
            continue
        pending = ""
        if direct_call.search(command):
            calls.append(start)
    return calls


def _run_bash(step: str, env: dict[str, str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    # GitHub enables pipefail for explicit bash, but not for the Linux default shell.
    options = ["--noprofile", "--norc", "-eo", "pipefail"] if _field(step, 8, "shell") == "bash" else ["-e"]
    body = step.split("        run: |\n", 1)[1]
    return subprocess.run(
        ["bash", *options, "-c", textwrap.dedent(body)],
        cwd=cwd,
        env={"PATH": os.environ["PATH"], **env},
        capture_output=True,
        text=True,
        timeout=10,
    )


def _step_env(step: str, context: dict[str, str]) -> dict[str, str]:
    # Feed the actual workflow bindings so a swapped result cannot pass the shell test.
    bindings = re.findall(r"(?m)^ *(\w+):[ \t]*\$\{\{\s*([\w.-]+)\s*\}\}", _field(step, 8, "env"))
    return {name: context[source] for name, source in bindings if source in context}


def _run_verdict(step: str, context: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return _run_bash(step, _step_env(step, context))


class WorkflowNixShellTest(unittest.TestCase):
    def test_detects_common_direct_calls_without_matching_command_arguments(self):
        for workflow, expected in (
            ("run: nix-shell --run true", [1]),
            ("run: NIX_PATH= nix-shell --run true", [1]),
            ('run: env NIX_PATH="" LABEL="two words" /usr/bin/nix-shell --run true', [1]),
            ('run: tools="$(NIX_PATH= nix-shell --pure --run true)"', [1]),
            ("run: |\n  env NIX_PATH= \\\n    nix-shell --run true", [2]),
            ("run: |\n  echo \\\n    nix-shell", []),
            ("run: |\n  # nix-shell --run true\n  echo nix-shell # ; nix-shell", []),
            ("run: env NIX_PATH= script/ao/pinned-nix-shell.sh ./shell.nix", []),
            ("run: >-\n  echo\n  nix-shell", []),
            ("name: nix-shell --run true\nenv:\n  COMMAND: nix-shell", []),
        ):
            with self.subTest(workflow=workflow):
                self.assertEqual(_direct_nix_shell_lines(workflow), expected)

    def test_workflow_run_commands_use_the_pinned_nix_shell_launcher(self):
        calls = [
            f"{path.relative_to(PROJECT_ROOT)}:{line}"
            for path in sorted(WORKFLOW.parent.iterdir())
            if path.suffix in (".yml", ".yaml") and path.is_file()
            for line in _direct_nix_shell_lines(path.read_text(encoding="utf-8"))
        ]
        self.assertFalse(calls, "Use script/ao/pinned-nix-shell.sh instead of nix-shell at:\n" + "\n".join(calls))


class GateRoutingTest(unittest.TestCase):
    def test_code_changes_trigger_all_native_jobs(self):
        for job in ("linux", "macos", "windows"):
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
                gate = _windows_completion_step() if job == "windows" else _step(native, "Check Bazel completion")
                self.assertEqual(_condition(gate, 8), "")
                self.assertIn(_field(gate, 8, "continue-on-error"), ("", "false"))

    def test_native_lint_base_comes_only_from_verified_pr_scope(self):
        self.assertEqual(
            _field(_job("changes"), 6, "lint-base"),
            "${{ github.event_name == 'pull_request' && steps.scope.outputs.sha || '' }}",
        )

    def test_checker_report_probe_is_required_only_for_affected_linux_and_windows_runs(self):
        self.assertEqual(_field(_job("changes"), 6, "checker-reports"), "${{ steps.scope.outputs.checker-reports }}")
        name = "Test native checker reports"
        self.assertNotIn(f"- name: {name}", _job("macos"))
        for job in ("linux", "windows"):
            with self.subTest(job=job):
                native = _job(job)
                step = _step(native, name)
                self.assertEqual(_condition(step, 8), "needs.changes.outputs.checker-reports == 'true'")
                self.assertIn(_field(step, 8, "continue-on-error"), ("", "false"))
                self.assertLess(native.index("- name: Check Bazel completion"), native.index(f"- name: {name}"))
                self.assertLess(native.index(f"- name: {name}"), native.index("- name: Stop Bazel remote cache"))
                self.assertIn("test/lint/check_checker_reports.py", _field(step, 8, "run"))

    def test_windows_native_completion_binds_verified_lint_base(self):
        step = _windows_completion_step()
        self.assertEqual(_field(step, 8, "shell"), "pwsh")
        self.assertEqual(
            _step_env(step, {"needs.changes.outputs.lint-base": LINT_BASE_SHA}), {"LINT_BASE_SHA": LINT_BASE_SHA}
        )

    def _assert_completion_output(
        self, result: subprocess.CompletedProcess[str], transcript: Path, argv: list[str], status: int
    ) -> None:
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertEqual(
            [line.removeprefix("arg=") for line in result.stdout.splitlines() if line.startswith("arg=")], argv
        )
        self.assertIn(COMPLETION_DIAGNOSTIC, result.stdout)
        self.assertIn(COMPLETION_DIAGNOSTIC, transcript.read_text(encoding="utf-8-sig"))

    @unittest.skipUnless(os.name == "nt", "Windows CI completion requires native batch execution")
    def test_windows_native_completion_invocation(self):
        pwsh = shutil.which("pwsh.exe")
        if pwsh is None:
            if os.environ.get("GITHUB_ACTIONS") == "true":
                self.fail("Windows CI completion requires pwsh.exe on PATH")
            self.skipTest("PowerShell 7 is not installed on this Windows host")

        step = _windows_completion_step()
        run = re.search(r"(?m)^        run:[ \t]*\|[+-]?[ \t]*\n((?: {10}.*\n|[ \t]*\n)+)", step)
        if run is None:
            self.fail("No literal run block in Windows completion step")
        root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="native check ")))
        script = root / "check.ps1"
        # Actions prepends Stop. Do not let an implicit exit footer mask lost propagation in the body.
        script.write_text("$ErrorActionPreference = 'stop'\n" + textwrap.dedent(run[1]), encoding="utf-8")
        (root / "ao.bat").write_text(
            "@echo off\n:arg\nif [%1]==[] goto done\necho(arg=%~1\nshift\ngoto arg\n"
            f":done\necho({COMPLETION_DIAGNOSTIC} 1>&2\nexit /b %FAKE_AO_EXIT%\n",
            encoding="ascii",
            newline="\r\n",
        )
        for base, lint_args, status in COMPLETION_CASES:
            with self.subTest(base=base, status=status):
                tree = root / f"check tree {status}"
                env = _step_env(step, {"needs.changes.outputs.lint-base": base})
                result = subprocess.run(
                    [pwsh, "-NoProfile", "-NonInteractive", "-File", str(script)],
                    cwd=root,
                    env={
                        **os.environ,
                        **env,
                        "CHECK_TREE": str(tree),
                        "CMAKE_BUILD_PARALLEL_LEVEL": "4",
                        "FAKE_AO_EXIT": str(status),
                    },
                    capture_output=True,
                    text=True,
                    timeout=15,
                    check=False,
                )
                argv = ["check", "--native-only", "--desktop-tests", "--path", str(tree), "--jobs", "4", *lint_args]
                self._assert_completion_output(result, tree / "completion.log", argv, status)

    def test_release_completion_requests_full_release_lint_without_desktop_tests(self):
        for job in ("linux", "macos", "windows"):
            with self.subTest(job=job):
                step = _step(_job(job, RELEASE_WORKFLOW), "Check Release")
                argv = shlex.split(_field(step, 8, "run"), comments=True)
                args = argv[argv.index("check") + 1 :]
                self.assertIn("--extend", args)
                self.assertEqual(args[args.index("--config") + 1], "release")
                self.assertTrue({"--native-only", "--desktop-tests"}.isdisjoint(args))

    def test_native_artifacts_include_transcript_and_scoped_lint_evidence(self):
        for job, step in (
            ("linux", "Preserve Bazel completion evidence"),
            ("macos", "Preserve Bazel evidence"),
            ("windows", "Preserve Bazel evidence"),
        ):
            with self.subTest(job=job):
                upload = _step(_job(job), step)
                self.assertEqual(_condition(upload, 8), "always()")
                globs = [path.strip() for path in _field(upload, 10, "path").replace("\\", "/").splitlines()[1:]]
                if job == "windows":
                    tree = _field(_windows_completion_step(), 10, "CHECK_TREE").replace("\\", "/")
                    self.assertRegex(tree, r"^[A-Za-z]:/")
                    transcript = f"{tree}/completion.log"
                else:
                    tree = "${{ runner.temp }}/aobus-bazel-fastbuild"
                    transcript = "${{ runner.temp }}/aobus-completion.log"
                lint_run = f"{tree}/lint/run-example"
                for path in (
                    transcript,
                    f"{lint_run}/run.receipt.json",
                    f"{lint_run}/build.bep.json",
                    f"{lint_run}/build.log",
                    f"{lint_run}/consumers.log",
                ):
                    self.assertTrue(any(fnmatchcase(path, glob) for glob in globs), path)

    @unittest.skipUnless(os.name == "posix", "Unix CI completion steps require bash")
    def test_unix_native_completion_invocation(self):
        root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="native check ")))
        portal = root / "ao"
        portal.write_text(
            f'#!/usr/bin/env bash\nprintf "arg=%s\\n" "$@"\nprintf "%s\\n" "{COMPLETION_DIAGNOSTIC}" >&2\n'
            'exit "$FAKE_AO_EXIT"\n',
            encoding="utf-8",
        )
        portal.chmod(0o755)
        for job in ("linux", "macos"):
            step = _step(_job(job), "Check Bazel completion")
            for base, lint_args, status in COMPLETION_CASES:
                with self.subTest(job=job, base=base, status=status):
                    runner_temp = root / f"{job} temp {status}"
                    runner_temp.mkdir()
                    env = _step_env(step, {"needs.changes.outputs.lint-base": base})
                    result = _run_bash(
                        step,
                        {
                            **env,
                            "RUNNER_TEMP": str(runner_temp),
                            "CMAKE_BUILD_PARALLEL_LEVEL": "4",
                            "FAKE_AO_EXIT": str(status),
                        },
                        cwd=root,
                    )
                    tree = runner_temp / "aobus-bazel-fastbuild"
                    argv = ["check", "--native-only", "--path", str(tree), "--jobs", "4", *lint_args]
                    self._assert_completion_output(result, runner_temp / "aobus-completion.log", argv, status)


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

        def stub(name: str, body: str) -> Path:
            executable = root / name
            executable.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8")
            executable.chmod(0o755)
            return executable

        stub("nix-build", '[[ "$*" == "./shell.nix -A portalShell --no-out-link" ]] && echo /fixture/pinned-bash\n')
        stub(
            "nix-shell",
            '[[ "$1" == ./shell.nix && "$2" == --pure && "$3" == --run ]] || exit 2\n'
            '[[ "$NIX_BUILD_SHELL" == /fixture/pinned-bash/bin/bash ]] || exit 3\n'
            'printf "%s\\t%s\\t%s\\n" "$FAKE_BAZEL" /fixture/jdk "$TEST_PYTHON"\n',
        )
        bazel = stub(
            "bazel",
            '[[ " $* " == *" build "* && " $* " == *" --disk_cache=$AOBUS_BAZEL_DISK_CACHE "* ]] || exit 2\n'
            "[[ -z ${AOBUS_BAZEL_CONFIGURATION+x} && -z ${AOBUS_BAZEL_REMOTE_CACHE+x} ]] || exit 3\n"
            "[[ -z ${NIX_BUILD_SHELL+x} ]] || exit 4\n"
            ': > "$0.ran"\n'
            "exit 37\n",
        )
        result = _run_bash(
            step,
            {
                "RUNNER_TEMP": str(root),
                "PATH": f"{root}{os.pathsep}{os.environ['PATH']}",
                "NIX_PATH": "",
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
