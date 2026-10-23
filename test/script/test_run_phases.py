"""Run phase boundaries around non-returning exec and source-view supervision."""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao import __main__ as portal
from ao.command import run
from ao.core import builddir, phases, workspace_cache

ROOT = Path(__file__).resolve().parents[2]
CANARY = "PRIVATE-run-argument-and-environment"
ENV = {**os.environ, "PYTHONPATH": str(ROOT / "script"), "PYTHONDONTWRITEBYTECODE": "1"}

# Only harmless Python fixture programs are launched. Parser, dispatch, recorder, and exec stay real;
# replacing exec with a returning mock cannot prove the successful handoff case.
CHILD_SOURCE = """
import contextlib
import os
import subprocess
import sys
from pathlib import Path
from unittest import mock
from ao import __main__ as portal
from ao.command import run
from ao.core import builddir, phases, workspace_cache

scenario, directory, requested_status, canary = sys.argv[1:]
direct_exec = scenario.startswith('direct-')
scenario = scenario.removeprefix('direct-')
root = Path(directory)
status = int(requested_status)
source_view = root / 'active-source-view'
application = None
real_popen = run.subprocess.Popen

@contextlib.contextmanager
def owned_views(args):
    source_view.touch()
    os.environ[workspace_cache.WINDOWS_SOURCE_VIEW] = str(source_view)
    try:
        yield
    finally:
        assert application is not None and application.poll() is not None
        source_view.unlink()

def spawn(*args, **kwargs):
    global application
    application = real_popen(*args, **kwargs)
    return application

def build_fixture(*args):
    with phases.measure('build', tree='native'):
        pass

checker = scenario in ('execvpe', 'handoff-env')
profile = builddir.MACOS_PROFILE if checker else builddir.LINUX_PROFILE
app = 'appkit' if checker else 'cli'
executable = Path(sys.executable)
if scenario == 'missing':
    executable = root / 'missing-executable'
elif scenario == 'exec-error':
    executable = root / 'not-an-executable'
    executable.mkdir()

lifetime_check = ''
if scenario == 'source-view':
    lifetime_check = 'assert pathlib.Path(os.environ["AOBUS_WINDOWS_SOURCE_VIEW"]).is_file(); '
elif os.name == 'posix' and scenario not in ('handoff', 'handoff-env'):
    lifetime_check = 'assert os.getpid() == ' + str(os.getpid()) + '; '
program = (
    'import os, pathlib, sys; '
    'assert sys.argv[1:] == [' + repr(canary) + ']; '
    'assert os.environ["AOBUS_RUN_PRIVATE"] == ' + repr(canary) + '; '
    + lifetime_check
    + ('assert os.environ["AOBUS_RUN_CHECKER"] == "forwarded"; ' if checker else '')
    + 'print("application stdout"); print("application stderr", file=sys.stderr); '
    'sys.exit(' + str(status) + ')'
)
app_arguments = ['-c', program, canary]
if scenario in ('handoff', 'handoff-env'):
    script = root / 'handoff application.py'
    script.write_text(program + chr(10), encoding='utf-8')
    # CRT exec does not quote space-bearing arguments. Quote this fixture path only;
    # production argument transport is deliberately unchanged.
    script_argument = subprocess.list2cmdline([str(script)]) if os.name == 'nt' else str(script)
    app_arguments = [script_argument, canary]
arguments = ['run', app, '-p', str(root)]
if scenario != 'build':
    arguments.append('--no-build')
if checker:
    arguments.append('--main-thread-checker')
arguments += ['--', *app_arguments]
checker_env = {**os.environ, 'AOBUS_RUN_CHECKER': 'forwarded'}
if direct_exec:
    arguments = [str(executable), *app_arguments]
    if checker:
        os.execvpe(str(executable), arguments, checker_env)
    else:
        os.execvp(str(executable), arguments)
    raise AssertionError('successful exec must not return')
with contextlib.ExitStack() as stack:
    stack.enter_context(mock.patch.object(builddir, 'platform_profile', return_value=profile))
    stack.enter_context(mock.patch.dict(run.APPS, {app: run.AppSpec('unused', executable)}))
    activate = stack.enter_context(mock.patch.object(portal.compiler_cache, 'activate_local'))
    build = stack.enter_context(mock.patch.object(run.build, 'do_build', side_effect=build_fixture))
    stack.enter_context(mock.patch.object(run.workspace_cache, 'validate_consumer'))
    stack.enter_context(mock.patch.object(run, '_main_thread_checker_environment', return_value=checker_env))
    if scenario == 'source-view':
        stack.enter_context(mock.patch.object(workspace_cache, 'command_context', owned_views))
        stack.enter_context(mock.patch.object(run.subprocess, 'Popen', side_effect=spawn))
    parsed = portal.parse_arguments(portal.make_parser(), arguments)
    expect_activation = (
        portal.buildenv.requires_parsed_build_env(parsed) or workspace_cache.command_uses_context(parsed)
    )
    try:
        result = portal.main(arguments)
    except OSError:
        assert scenario == 'exec-error'
        sys.exit(79)
    if expect_activation:
        activate.assert_called_once_with()
    else:
        activate.assert_not_called()
    if scenario != 'build':
        build.assert_not_called()
    sys.exit(result)
"""


def child_environment(log_dir: Path | None) -> dict[str, str]:
    env = dict(ENV)
    for name in (
        phases.LOG_DIR_ENV,
        workspace_cache.WINDOWS_SOURCE_VIEW,
        portal.compiler_cache.SHARED_WORKSPACES_EFFECTIVE,
        "AOBUS_PREFLIGHT_SCOPE",
    ):
        env.pop(name, None)
    if log_dir is not None:
        env[phases.LOG_DIR_ENV] = str(log_dir)
    if os.name == "nt":
        # Match main's tracking setting in both children, without acquiring drive views.
        env[portal.msbuild_cache.TRACKING_ENV] = "0"
    env["AOBUS_RUN_PRIVATE"] = CANARY
    return env


def launch_child(directory: Path, scenario: str, status: int = 0, *, logging: bool = True):
    return subprocess.run(
        [sys.executable, "-c", CHILD_SOURCE, scenario, str(directory), str(status), CANARY],
        env=child_environment(directory / "phases" if logging else None),
        capture_output=True,
        timeout=60,
    )


class RunPhaseTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="private-run-")))
        self.log_dir = self.root / "phases"
        self.enterContext(mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: str(self.log_dir)}, clear=True))
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.activate = self.enterContext(mock.patch.object(portal.compiler_cache, "activate_local"))
        self.build = self.enterContext(mock.patch.object(run.build, "do_build"))
        self.validate = self.enterContext(mock.patch.object(workspace_cache, "validate_consumer"))
        self.enterContext(mock.patch.dict(run.APPS, {"cli": run.AppSpec("unused", Path(sys.executable))}))

    def report(self, log_dir: Path | None = None) -> list[dict]:
        directory = self.log_dir if log_dir is None else log_dir
        raw = "".join(path.read_text(encoding="utf-8") for path in directory.glob("*.jsonl"))
        self.assertTrue(raw, "the regression must produce records, not an empty passing report")
        result = subprocess.run(
            [sys.executable, "-m", "ao.core.phases", str(directory)],
            env=child_environment(None),
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        for private in (CANARY, str(directory.parent), sys.executable, "application stdout"):
            self.assertNotIn(private, raw + result.stdout)
        records = [
            json.loads(line.removeprefix("AO_PHASE_RECORD "))
            for line in result.stdout.splitlines()
            if line.startswith("AO_PHASE_RECORD ")
        ]
        self.assertTrue(records)
        self.assertTrue(all(record["command"] == "run" for record in records))
        self.assertNotIn("command", [record["phase"] for record in records])
        return records

    def invoke(self, *options: str) -> int:
        return portal.main(["run", "cli", "--no-build", "-p", str(self.root), *options])

    @unittest.skipUnless(os.name == "posix", "successful exec replacement requires POSIX")
    def test_successful_exec_closes_preparation_before_replacing_the_portal(self):
        for status in (0, 23):
            with self.subTest(status=status):
                directory = self.root / str(status)
                directory.mkdir()
                result = launch_child(directory, "exec", status)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(result.stdout, b"application stdout\n")
                self.assertEqual(result.stderr, b"application stderr\n")
                records = self.report(directory / "phases")
                self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 0)])
                self.assertNotIn("parent_id", records[0])

    def _assert_native_exec_handoff(self, scenario: str):
        for status in (0, 23):
            with self.subTest(scenario=scenario, status=status):
                directory = self.root / f"{scenario}-{status}"
                directory.mkdir()
                # Both fresh children construct exactly the same app program/environment;
                # Windows exec need not share POSIX PID or exit-status behavior.
                baseline = launch_child(directory, f"direct-{scenario}", status)
                result = launch_child(directory, scenario, status)
                self.assertEqual(baseline.stdout, f"application stdout{os.linesep}".encode("ascii"))
                self.assertEqual(baseline.stderr, f"application stderr{os.linesep}".encode("ascii"))
                self.assertEqual(
                    (result.returncode, result.stdout, result.stderr),
                    (baseline.returncode, baseline.stdout, baseline.stderr),
                )
                records = self.report(directory / "phases")
                self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 0)])
                self.assertNotIn("parent_id", records[0])

    def test_native_exec_handoff_matches_direct_exec_bytes_and_observed_status(self):
        self._assert_native_exec_handoff("handoff")

    @unittest.skipUnless(os.name == "posix", "Main Thread Checker launch is a macOS/POSIX path")
    def test_native_checker_handoff_matches_direct_exec_bytes_and_observed_status(self):
        # Explicit-env exec is selected only for the macOS Main Thread Checker;
        # synthesizing that unsupported mode on Windows tests an unrelated CRT API.
        self._assert_native_exec_handoff("handoff-env")

    @unittest.skipUnless(os.name == "posix", "successful exec replacement requires POSIX")
    def test_disabled_logging_keeps_exec_output_and_status_byte_identical_without_files(self):
        directory = self.root / "disabled"
        directory.mkdir()
        result = launch_child(directory, "exec", 23, logging=False)
        baseline = subprocess.run(
            [
                sys.executable,
                "-c",
                'import sys; print("application stdout"); print("application stderr", file=sys.stderr); sys.exit(23)',
            ],
            env=child_environment(None),
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(
            (result.returncode, result.stdout, result.stderr),
            (baseline.returncode, baseline.stdout, baseline.stderr),
        )
        self.assertEqual(list(directory.iterdir()), [])

    @unittest.skipUnless(os.name == "posix", "successful exec replacement requires POSIX")
    def test_execvpe_forwards_prepared_environment_without_an_open_span(self):
        result = launch_child(self.root, "execvpe")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"application stdout\n")
        self.assertEqual(result.stderr, b"application stderr\n")
        records = self.report()
        self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 0)])

    @unittest.skipUnless(os.name == "posix", "successful exec replacement requires POSIX")
    def test_preparatory_build_inherits_run_and_links_only_to_open_preparation(self):
        result = launch_child(self.root, "build")
        self.assertEqual(result.returncode, 0, result.stderr)
        records = self.report()
        self.assertEqual([r["phase"] for r in records], ["launch-preparation", "build"])
        self.assertEqual(records[1]["parent_id"], records[0]["phase_id"])
        self.assertNotIn("parent_id", records[0])

    def test_missing_executable_records_failed_preparation_before_consumer_validation(self):
        with (
            mock.patch.dict(run.APPS, {"cli": run.AppSpec("unused", self.root / "missing")}),
            mock.patch.object(run.os, "execvp") as execute,
            contextlib.redirect_stderr(io.StringIO()) as errors,
            self.assertRaises(SystemExit) as caught,
        ):
            self.invoke()
        self.assertEqual(caught.exception.code, 1)
        self.assertIn("Did you build the project", errors.getvalue())
        execute.assert_not_called()
        self.validate.assert_not_called()
        self.build.assert_not_called()
        records = self.report()
        self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 1)])

    def test_invalid_checker_option_fails_preparation_without_build_or_exec(self):
        with (
            mock.patch.object(run.os, "execvp") as execute,
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            self.invoke("--main-thread-checker")
        execute.assert_not_called()
        self.build.assert_not_called()
        records = self.report()
        self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 1)])

    def test_exec_oserror_is_reraised_unchanged_with_a_separate_paired_failure(self):
        failure = OSError(CANARY)
        with mock.patch.object(run.os, "execvp", side_effect=failure), self.assertRaises(OSError) as caught:
            self.invoke("--", CANARY)
        self.assertIs(caught.exception, failure)
        records = self.report()
        self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 0), ("launch-error", 1)])
        self.assertTrue(all("parent_id" not in record for record in records))

    @unittest.skipUnless(os.name == "posix", "kernel exec failure probe requires POSIX")
    def test_real_exec_failure_leaves_no_unpaired_records(self):
        result = launch_child(self.root, "exec-error")
        self.assertEqual(result.returncode, 79, result.stderr)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"")
        records = self.report()
        self.assertEqual([(r["phase"], r["status"]) for r in records], [("launch-preparation", 0), ("launch-error", 1)])

    def test_returning_exec_test_double_still_normalizes_to_zero_in_main(self):
        with mock.patch.object(run.os, "execvp", return_value=None):
            self.assertEqual(self.invoke(), 0)
        self.assertEqual([(r["phase"], r["status"]) for r in self.report()], [("launch-preparation", 0)])

    def test_source_view_wait_observes_application_exit_before_retiring_views(self):
        result = launch_child(self.root, "source-view", 17)
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(result.stdout, f"application stdout{os.linesep}".encode("ascii"))
        self.assertEqual(result.stderr, f"application stderr{os.linesep}".encode("ascii"))
        self.assertFalse((self.root / "active-source-view").exists())
        self.assertEqual(
            [(r["phase"], r["status"]) for r in self.report()], [("launch-preparation", 0), ("application", 17)]
        )

    def test_source_view_wait_preserves_signed_exit_status_in_main_and_records(self):
        child = mock.MagicMock()
        child.__enter__.return_value = child
        child.wait.return_value = -15
        with (
            mock.patch.dict(os.environ, {workspace_cache.WINDOWS_SOURCE_VIEW: "owned"}),
            mock.patch.object(run.subprocess, "Popen", return_value=child),
        ):
            self.assertEqual(self.invoke(), -15)
        self.assertEqual(
            [(r["phase"], r["status"]) for r in self.report()], [("launch-preparation", 0), ("application", -15)]
        )

    def test_source_view_interrupt_reaps_child_before_context_cleanup(self):
        events = []
        child = mock.MagicMock()
        child.__enter__.return_value = child
        child.wait.side_effect = [KeyboardInterrupt, 0]
        child.kill.side_effect = lambda: events.append("kill")
        child.__exit__.side_effect = lambda *args: events.append("child-exit")

        @contextlib.contextmanager
        def views(args):
            with mock.patch.dict(os.environ, {workspace_cache.WINDOWS_SOURCE_VIEW: "owned"}):
                try:
                    yield
                finally:
                    self.assertEqual(child.wait.call_count, 2)
                    events.append("views-exit")

        with (
            mock.patch.object(workspace_cache, "command_context", views),
            mock.patch.object(run.subprocess, "Popen", return_value=child),
            mock.patch.object(run.os, "execvp") as execute,
        ):
            self.assertEqual(self.invoke(), 130)
        execute.assert_not_called()
        self.assertEqual(events, ["kill", "child-exit", "views-exit"])
        self.assertEqual(
            [(r["phase"], r["status"]) for r in self.report()], [("launch-preparation", 0), ("application", 130)]
        )

    def test_failed_application_reap_reraises_interrupt_and_records_failure(self):
        interrupt = KeyboardInterrupt()
        child = mock.MagicMock()
        child.__enter__.return_value = child
        child.wait.side_effect = [interrupt, OSError(CANARY)]
        with (
            mock.patch.dict(os.environ, {workspace_cache.WINDOWS_SOURCE_VIEW: "owned"}),
            mock.patch.object(run.subprocess, "Popen", return_value=child),
            self.assertRaises(KeyboardInterrupt) as caught,
        ):
            self.invoke()
        self.assertIs(caught.exception, interrupt)
        self.assertIn("Could not reap the application", interrupt.__notes__[0])
        child.kill.assert_called_once_with()
        self.assertEqual(child.wait.call_count, 2)
        self.assertEqual(
            [(r["phase"], r["status"]) for r in self.report()], [("launch-preparation", 0), ("application", 130)]
        )

    def test_workspace_entry_and_exit_failures_are_reported_as_launch_errors(self):
        for timing in ("entry", "exit"):
            with self.subTest(timing=timing):
                self.log_dir = self.root / timing

                @contextlib.contextmanager
                def views(args, timing=timing):
                    if timing == "entry":
                        raise workspace_cache.WorkspaceCacheError(CANARY)
                    yield
                    raise workspace_cache.WorkspaceCacheError(CANARY)

                with (
                    mock.patch.object(workspace_cache, "command_context", views),
                    mock.patch.object(run.os, "execvp", return_value=None) as execute,
                    contextlib.redirect_stderr(io.StringIO()) as errors,
                    mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: str(self.log_dir)}),
                ):
                    self.assertEqual(self.invoke(), 1)
                self.assertIn(CANARY, errors.getvalue())
                self.assertEqual(execute.call_count, 0 if timing == "entry" else 1)
                expected = [] if timing == "entry" else [("launch-preparation", 0)]
                self.assertEqual([(r["phase"], r["status"]) for r in self.report()], [*expected, ("launch-error", 1)])


if __name__ == "__main__":
    unittest.main()
