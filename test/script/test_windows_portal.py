"""Windows portal preparation, dispatch, and native launch-boundary regressions."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from ao.command import options
from ao.core.paths import PROJECT_ROOT
from ao.portal import build_env, git_files, python_check, windows_portal
from ao.portal.source_scope import SourceScope

REPOSITORY = Path(__file__).resolve().parents[2]
WINDOWS_PORTAL_SCOPE = "script/ao/portal/windows_portal.py"
_DEFAULT = object()

# The fixture package is only an external next-stage consumer. It records the
# arguments and selected environment it actually received; it does not model
# build_env, command selection, or the production portal graph.
_FIXTURE_PYTHON_ENV = """\
import json
import os
import sys
from pathlib import Path

arguments = sys.argv[1:]
separator = arguments.index("--") if "--" in arguments else len(arguments)
record = {
    "argv": arguments,
    "portalArgs": arguments[separator + 1 :],
    "cwd": os.getcwd(),
    "executable": sys.executable,
    "pythonPath": os.environ.get("PYTHONPATH"),
    "stateRoot": os.environ.get("AOBUS_STATE_ROOT"),
    "vcpkgRoot": os.environ.get("VCPKG_ROOT"),
    "psModulePath": os.environ.get("PSModulePath"),
    "psExecutionPolicy": os.environ.get("PSExecutionPolicyPreference"),
    "preflightScope": os.environ.get("AOBUS_PREFLIGHT_SCOPE"),
}
print(json.dumps(record, ensure_ascii=True, sort_keys=True), flush=True)
ready_file = os.environ.get("AOBUS_FIXTURE_READY_FILE")
if ready_file:
    Path(ready_file).touch(exist_ok=False)
    if sys.stdin.readline() != "continue\\n":
        raise SystemExit(117)
raise SystemExit(int(os.environ.get("AOBUS_FIXTURE_EXIT_CODE", "0")))
"""

_ARGUMENT_CORPUS = (
    ("no-arguments", ""),
    ("empty", '""'),
    ("spaces", '"alpha beta"'),
    ("embedded-quotes", '"""quoted"""'),
    ("backslashes", r'"C:\dir with space\\tail"'),
    ("trailing-backslash", "C:\\dir\\"),
    ("unicode", '"café 雪"'),
    # The controlled value is itself a paired %NAME%; a second CMD expansion
    # would produce second-value instead of the literal baseline.
    ("percent", '"%AOBUS_FIXTURE_PERCENT%"'),
    ("bang", '"bang!value"'),
    ("caret", '"caret^value"'),
    ("metacharacters", '"amp&pipe|less<greater>"'),
    ("multiple-double-dash", "-- alpha -- beta"),
)

_EXPECTED_PORTAL_ARGS = {
    "no-arguments": [],
    "empty": [""],
    "spaces": ["alpha beta"],
    "backslashes": ["C:\\dir with space\\\\tail"],
    "trailing-backslash": ["C:\\dir\\"],
    "unicode": ["café 雪"],
    "percent": ["%AOBUS_FIXTURE_SECOND%"],
    "bang": ["bang!value"],
    "caret": ["caret^value"],
    "metacharacters": ["amp&pipe|less<greater>"],
    "multiple-double-dash": ["--", "alpha", "--", "beta"],
}


@contextlib.contextmanager
def _isolated_environment(values):
    cwd = os.getcwd()
    with mock.patch.dict(os.environ, values, clear=True):
        try:
            yield
        finally:
            os.chdir(cwd)


def _normalized(path):
    # Launchers may expand an inherited 8.3 TEMP ancestor, even for paths whose
    # final component does not exist. Compare filesystem spellings, not aliases.
    return os.path.normcase(os.path.realpath(str(path)))


class PrepareBuildEnvironmentTest(unittest.TestCase):
    def test_prepare_build_environment_replaces_the_environment_from_captured_json(self):
        original = {
            "COMSPEC": "fixture-cmd.exe",
            "PATH": "original path",
            "AOBUS_ORIGINAL_ONLY": "keep",
        }
        captured = {
            "PATH": "captured path",
            "AOBUS_UNICODE": "café\nsnow 雪",
            "AOBUS_EQUALS": "left=right=tail",
            "AOBUS_SECRET_TOKEN": "secret-token",
            "_AOBUS_ENV_SCRIPT": "private script",
            "_AOBUS_ENV_PYTHON": "private python",
        }
        expected = {key: value for key, value in captured.items() if not key.startswith("_AOBUS_ENV_")}
        completed = subprocess.CompletedProcess([], 0, stdout=json.dumps(captured).encode("ascii"), stderr=b"")

        with _isolated_environment(original):
            with (
                mock.patch.object(windows_portal.subprocess, "run", return_value=completed) as run,
                contextlib.redirect_stdout(io.StringIO()) as stdout,
            ):
                windows_portal.prepare_build_environment()
                self.assertEqual(dict(os.environ), expected)
                self.assertNotIn("secret-token", stdout.getvalue())

        _, kwargs = run.call_args
        self.assertEqual(kwargs["executable"], original["COMSPEC"])
        self.assertEqual(kwargs["env"]["_AOBUS_ENV_SCRIPT"], str(PROJECT_ROOT / "script/ao/windows-build-env.bat"))
        self.assertEqual(kwargs["env"]["_AOBUS_ENV_PYTHON"], sys.executable)

    def test_prepare_build_environment_failures_keep_the_original_environment_and_never_echo_stdout(self):
        secret = "full-environment-secret"
        original = {
            "COMSPEC": "fixture-cmd.exe",
            "PATH": "original path",
            "AOBUS_ORIGINAL_ONLY": "keep",
        }
        cases = (
            ("malformed-json", b'{"' + secret.encode("ascii") + b'":', 0, b"", RuntimeError),
            ("non-object", json.dumps([secret]).encode("ascii"), 0, b"", RuntimeError),
            ("empty-mapping", b"{}", 0, b"", RuntimeError),
            (
                "non-string-value",
                json.dumps({"PATH": 1, "AOBUS_SECRET_TOKEN": secret}).encode("ascii"),
                0,
                b"",
                RuntimeError,
            ),
            (
                "nonzero",
                json.dumps({"PATH": "captured", "AOBUS_SECRET_TOKEN": secret}).encode("ascii"),
                7,
                b"setup failed\n",
                subprocess.CalledProcessError,
            ),
        )

        for name, stdout_bytes, returncode, stderr_bytes, expected_error in cases:
            with self.subTest(case=name), _isolated_environment(original):
                completed = subprocess.CompletedProcess(
                    "fixture command", returncode, stdout=stdout_bytes, stderr=stderr_bytes
                )
                with (
                    mock.patch.object(windows_portal.subprocess, "run", return_value=completed),
                    contextlib.redirect_stdout(io.StringIO()) as stdout,
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                ):
                    with self.assertRaises(expected_error) as raised:
                        windows_portal.prepare_build_environment()

                if expected_error is subprocess.CalledProcessError:
                    self.assertEqual(raised.exception.returncode, returncode)
                self.assertEqual(dict(os.environ), original)
                self.assertNotIn(secret, stdout.getvalue() + stderr.getvalue() + str(raised.exception))


class WindowsPortalMainTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-portal-state-")
        self.addCleanup(temporary.cleanup)
        self.state_root = str(Path(temporary.name) / "state")

    def test_python_only_format_scope_runs_ruff_without_native_setup(self):
        expected = python_check.ruff_command("format", "--check", WINDOWS_PORTAL_SCOPE)
        formatted = []

        def formatter(command, **kwargs):
            # platform.system() may ask CMD for its version on Windows. Keep
            # that external OS probe distinct from native build setup and Ruff.
            if command == "ver" and kwargs.get("shell") is True:
                return subprocess.CompletedProcess(
                    command, 0, stdout="Microsoft Windows [Version 10.0.12345.0]", stderr=""
                )
            self.assertEqual(command, expected)
            self.assertEqual(kwargs["cwd"], PROJECT_ROOT)
            formatted.append(Path.cwd().resolve())
            return subprocess.CompletedProcess(command, 0)

        with tempfile.TemporaryDirectory() as temporary:
            working_directory = Path(temporary)
            environment = {
                "PATH": "fixture-path",
                "AOBUS_STATE_ROOT": self.state_root,
                "AOBUS_CHECKOUT_ID": "windows-portal-fixture",
                "_AOBUS_PORTAL_WORKING_DIRECTORY": str(working_directory),
                "AOBUS_PREFLIGHT_SCOPE": "stale-scope",
                "_AOBUS_UNC_MAPPING": "1",
            }
            with (
                _isolated_environment(environment),
                mock.patch.object(windows_portal.subprocess, "run", side_effect=formatter),
                # Bound discovery to a Python file; do not admit unrelated formatters.
                mock.patch.object(git_files, "find_sources", return_value=[WINDOWS_PORTAL_SCOPE]),
                mock.patch.object(git_files, "prettier_sources", return_value=[]),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
            ):
                status = windows_portal.main(["check", "format", "script/ao"])
                self.assertEqual(status, 0, stderr.getvalue())
                self.assertEqual(formatted, [working_directory.resolve()])
                self.assertEqual(os.environ["PATH"].split(os.pathsep)[0], str(Path(sys.executable).parent))
                self.assertNotIn("_AOBUS_PORTAL_WORKING_DIRECTORY", os.environ)
                self.assertNotIn("_AOBUS_UNC_MAPPING", os.environ)
                self.assertNotIn("AOBUS_PREFLIGHT_SCOPE", os.environ)

    def test_private_scope_is_loaded_before_native_admission_without_rediscovery(self):
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.object(options.source_scope, "PROJECT_ROOT", Path(temporary)),
        ):
            root = Path(temporary)
            for name in ("lib/Sample.cpp", "app/macos-appkit/Explicit.mm"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            scope_file = root / "sources.json"
            stale = root / "stale.json"
            stale.write_text("not this invocation", encoding="utf-8")
            arguments = ["check", "lint", "lib", "app/macos-appkit/Explicit.mm"]
            prepared = build_env.parse_command_arguments(arguments[0], arguments[1:])
            scope = SourceScope(("lib/Sample.cpp", "app/macos-appkit/Explicit.mm"), ("app/macos-appkit/Explicit.mm",))
            with mock.patch.dict(os.environ, {"AOBUS_PREFLIGHT_SCOPE": str(scope_file)}):
                build_env.record_source_scope(prepared, scope)

            prepared_environment = []

            def prepare():
                prepared_environment.append(dict(os.environ))

            def dispatch(args, *, build_environment_required):
                self.assertTrue(build_environment_required)
                (environment,) = prepared_environment
                self.assertNotIn("_AOBUS_WINDOWS_SCOPE_FILE", environment)
                self.assertNotIn("AOBUS_PREFLIGHT_SCOPE", environment)
                self.assertEqual(args._resolved_sources, list(scope.files))
                self.assertEqual(args._explicit_sources, list(scope.explicit_files))
                self.assertNotIn("_AOBUS_WINDOWS_SCOPE_FILE", os.environ)
                self.assertNotIn("AOBUS_PREFLIGHT_SCOPE", os.environ)
                return 7

            with (
                _isolated_environment(
                    {
                        "PATH": "fixture-path",
                        "AOBUS_CHECKOUT_ID": "windows-scope-fixture",
                        "AOBUS_STATE_ROOT": self.state_root,
                        "AOBUS_PREFLIGHT_SCOPE": str(stale),
                        "_AOBUS_WINDOWS_SCOPE_FILE": str(scope_file),
                    }
                ),
                mock.patch.object(
                    options.source_scope, "resolve", side_effect=AssertionError("must not rediscover frozen sources")
                ),
                mock.patch.object(windows_portal, "prepare_build_environment", side_effect=prepare),
                mock.patch.object(windows_portal, "dispatch", side_effect=dispatch),
            ):
                self.assertEqual(windows_portal.main(arguments), 7)
            self.assertTrue(scope_file.exists())  # The waiting bootstrap, not its child, owns cleanup.
            self.assertEqual(stale.read_text(encoding="utf-8"), "not this invocation")

    def test_missing_or_empty_private_scope_fails_before_native_admission(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            empty = root / "empty.json"
            empty.touch()
            for marker in (empty, root / "missing.json"):
                with (
                    self.subTest(marker=marker),
                    _isolated_environment({"_AOBUS_WINDOWS_SCOPE_FILE": str(marker)}),
                    mock.patch.object(
                        windows_portal, "prepare_build_environment", side_effect=AssertionError("admitted native tools")
                    ),
                    mock.patch.object(
                        windows_portal, "dispatch", side_effect=AssertionError("dispatched without scope")
                    ),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(windows_portal.main(["check", "docs"]), 1)
                    self.assertNotIn("_AOBUS_WINDOWS_SCOPE_FILE", os.environ)

    def test_preparation_failure_returns_the_native_exit_status_without_dispatching(self):
        result = subprocess.CompletedProcess("cmd.exe", 17, stdout=b"", stderr=b"setup failed")
        with (
            _isolated_environment({"PATH": "fixture-path", "AOBUS_STATE_ROOT": self.state_root}),
            mock.patch.object(windows_portal.subprocess, "run", return_value=result),
            mock.patch.object(windows_portal, "dispatch", side_effect=AssertionError("dispatched after failure")),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(windows_portal.main(["check", "contracts"]), 17)

    def test_help_and_invalid_arguments_launch_no_external_command(self):
        cases = (
            (["help"], 0, False),
            (["--help"], 0, True),
            (["format", "--bogus"], 2, True),
        )

        for arguments, status, raises in cases:
            with (
                self.subTest(arguments=arguments),
                _isolated_environment(
                    {
                        "PATH": "fixture-path",
                        "AOBUS_STATE_ROOT": self.state_root,
                        "AOBUS_CHECKOUT_ID": "windows-portal-fixture",
                    }
                ),
            ):
                with (
                    mock.patch.object(
                        windows_portal.subprocess, "run", side_effect=AssertionError("external command launched")
                    ),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    if raises:
                        with self.assertRaises(SystemExit) as raised:
                            windows_portal.main(list(arguments))
                        self.assertEqual(raised.exception.code, status)
                    else:
                        self.assertEqual(windows_portal.main(list(arguments)), status)


@unittest.skipUnless(sys.platform == "win32", "native Windows batch and PowerShell launch boundary")
class WindowsLaunchBoundaryTest(unittest.TestCase):
    """Prove native batch/PowerShell transport and ready-base reuse, not the full portal."""

    _STEP_TIMEOUT = 30
    _CLEANUP_TIMEOUT = 5

    @classmethod
    def setUpClass(cls):
        cls.comspec = os.environ.get("COMSPEC", "cmd.exe")
        if shutil.which("powershell.exe") is None:
            raise unittest.SkipTest("powershell.exe is unavailable")
        cls.base_python = Path(sys.base_prefix) / "python.exe"
        cls.managed_state = Path(os.environ.get("AOBUS_STATE_ROOT") or str(Path(os.environ["LOCALAPPDATA"]) / "Aobus"))
        version = ".".join(str(part) for part in sys.version_info[:3])
        cls.managed_python = cls.managed_state / "tools" / "python" / version / "python.exe"
        cls.managed_ready = False
        if cls.managed_python.is_file():
            ready = subprocess.run(
                [
                    str(cls.managed_python),
                    "-I",
                    "-c",
                    f"import ensurepip, sys, venv; raise SystemExit(sys.version_info[:3] != {sys.version_info[:3]!r})",
                ],
                capture_output=True,
                timeout=cls._STEP_TIMEOUT,
            )
            cls.managed_ready = ready.returncode == 0
        if not cls.base_python.is_file():
            raise unittest.SkipTest("sys.base_prefix does not contain python.exe")
        probe = subprocess.run(
            [str(cls.base_python), "-I", "-c", "import ensurepip, venv"],
            capture_output=True,
            timeout=cls._STEP_TIMEOUT,
        )
        if probe.returncode != 0:
            raise unittest.SkipTest("sys.base_prefix cannot provide ensurepip and venv")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus launch Åsa 音乐 ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.checkout = self.root / "checkout"
        self.package = self.checkout / "script" / "ao"
        self.state = self.root / "state-Åsa 音乐"
        self.caller = self.root / "caller"
        self.caller.mkdir(parents=True)
        self.package.mkdir(parents=True)

        shutil.copy2(REPOSITORY / "ao.bat", self.checkout / "ao.bat")
        shutil.copy2(REPOSITORY / "script/ao/bootstrap-python.ps1", self.package / "bootstrap-python.ps1")
        version = ".".join(str(part) for part in sys.version_info[:3])
        (self.package / "toolchain.json").write_text(
            json.dumps({"schemaVersion": 1, "python": version}), encoding="utf-8"
        )
        (self.package / "__init__.py").write_text("", encoding="utf-8")
        portal = self.package / "portal"
        portal.mkdir()
        (portal / "__init__.py").write_text("", encoding="utf-8")
        (portal / "python_env.py").write_text(_FIXTURE_PYTHON_ENV, encoding="utf-8")

        # Managed cases reuse only an already validated base installation. There
        # are no symlinks/junctions, runtime copies or provisioning in this fixture.
        # A missing vcpkg sentinel stops an unexpected reuse failure before download.
        self.missing_vcpkg = self.root / "missing-vcpkg"
        self.percent_value = "%AOBUS_FIXTURE_SECOND%"
        self.second_value = "second-value"
        self.ps_module_path = str(self.root / "fixture !bang! %token% ps modules")
        self.ps_execution_policy = "RemoteSigned"

    def _environment(
        self,
        *,
        explicit,
        exit_code=0,
        ps_module_path=_DEFAULT,
        ps_execution_policy=_DEFAULT,
    ):
        if not explicit and not self.managed_ready:
            self.skipTest("no matching existing managed base Python; provisioning is forbidden")
        if ps_module_path is _DEFAULT:
            ps_module_path = self.ps_module_path
        if ps_execution_policy is _DEFAULT:
            ps_execution_policy = self.ps_execution_policy
        # os.environ is case-insensitive on Windows; this ordinary dict is not.
        # Remove host names case-insensitively before adding controlled values.
        inherited = {
            key: value
            for key, value in os.environ.items()
            if key.upper() not in {"PSMODULEPATH", "PSEXECUTIONPOLICYPREFERENCE"}
        }
        environment = {
            **inherited,
            "COMSPEC": self.comspec,
            "AOBUS_STATE_ROOT": str(self.state if explicit else self.managed_state),
            "VCPKG_ROOT": str(self.missing_vcpkg),
            "AOBUS_FIXTURE_EXIT_CODE": str(exit_code),
            "AOBUS_FIXTURE_PERCENT": self.percent_value,
            "AOBUS_FIXTURE_SECOND": self.second_value,
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONIOENCODING": "utf-8",
            "PYTHONUTF8": "1",
        }
        if ps_module_path is None:
            environment.pop("PSModulePath", None)
        else:
            environment["PSModulePath"] = ps_module_path
        if ps_execution_policy is None:
            environment.pop("PSExecutionPolicyPreference", None)
        else:
            environment["PSExecutionPolicyPreference"] = ps_execution_policy
        environment.pop("AOBUS_PYTHON", None)
        environment.pop("AOBUS_FIXTURE_READY_FILE", None)
        environment.pop("AOBUS_PREFLIGHT_SCOPE", None)
        environment.pop("PYTHONHOME", None)
        if explicit:
            environment["AOBUS_PYTHON"] = sys.executable
        return environment

    def _cmd_line(self, command):
        # Build the cmd.exe command line as one raw string. Passing the same
        # text through subprocess's list2cmdline would apply CRT quoting before
        # cmd.exe sees it, which is not the native batch boundary under test.
        return f'"{self.comspec}" /d /v:off /s /c "{command}"'

    def _run_cmd(self, command, *, environment, label):
        print(f"{self.id()}: launching {label}", flush=True)
        # CMD can have PowerShell and Python descendants holding the capture
        # pipes. Use Bazel's process-tree deadline for complete one-shot runs.
        return subprocess.run(
            self._cmd_line(command),
            cwd=self.caller,
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    def _direct_command(self, tail):
        command = " ".join(
            (
                f'"{sys.executable}"',
                "-m",
                "ao.portal.python_env",
                "--project-root",
                f'"{self.checkout}"',
                "--state-root",
                f'"{self.state}"',
                "--launch-windows-portal",
                "--",
            )
        )
        return f"{command} {tail}" if tail else command

    def _batch_command(self, tail):
        command = f'"{self.checkout / "ao.bat"}"'
        return f"{command} {tail}" if tail else command

    def _record(self, completed, expected_status):
        self.assertEqual(completed.returncode, expected_status, completed.stderr)
        lines = [line for line in completed.stdout.splitlines() if line.strip()]
        self.assertTrue(lines, completed.stdout)
        return json.loads(lines[-1])

    def _run_direct(self, tail, *, exit_code=0):
        environment = self._environment(explicit=True, exit_code=exit_code)
        environment["PYTHONPATH"] = str(self.checkout / "script")
        completed = self._run_cmd(self._direct_command(tail), environment=environment, label=f"direct {tail!a}")
        return self._record(completed, exit_code)

    def _run_batch(
        self,
        tail,
        *,
        explicit,
        exit_code=0,
        ps_module_path=_DEFAULT,
        ps_execution_policy=_DEFAULT,
    ):
        environment = self._environment(
            explicit=explicit,
            exit_code=exit_code,
            ps_module_path=ps_module_path,
            ps_execution_policy=ps_execution_policy,
        )
        mode = "explicit" if explicit else "managed"
        label = f"{mode} batch {tail!a} (expected exit {exit_code}, PSModulePath present={ps_module_path is not None})"
        completed = self._run_cmd(self._batch_command(tail), environment=environment, label=label)
        return self._record(completed, exit_code)

    def _assert_launch_environment(
        self,
        record,
        *,
        explicit,
        ps_module_path=_DEFAULT,
        ps_execution_policy=_DEFAULT,
    ):
        if ps_module_path is _DEFAULT:
            ps_module_path = self.ps_module_path
        if ps_execution_policy is _DEFAULT:
            ps_execution_policy = self.ps_execution_policy
        argv = record["argv"]
        self.assertEqual(argv[0], "--project-root")
        self.assertEqual(_normalized(argv[1]), _normalized(self.checkout))
        self.assertEqual(argv[2], "--state-root")
        selected_state = self.state if explicit else self.managed_state
        self.assertEqual(_normalized(argv[3]), _normalized(selected_state))
        self.assertEqual(argv[4:6], ["--launch-windows-portal", "--"])
        self.assertEqual(_normalized(record["cwd"]), _normalized(self.caller))
        self.assertEqual(_normalized(record["pythonPath"]), _normalized(self.checkout / "script"))
        self.assertEqual(_normalized(record["stateRoot"]), _normalized(selected_state))
        self.assertEqual(_normalized(record["vcpkgRoot"]), _normalized(self.missing_vcpkg))
        self.assertIsNone(record["preflightScope"])
        if ps_module_path is None:
            self.assertFalse(record["psModulePath"])
        else:
            self.assertEqual(record["psModulePath"], ps_module_path)
        if ps_execution_policy is None:
            self.assertFalse(record["psExecutionPolicy"])
        else:
            self.assertEqual(record["psExecutionPolicy"], ps_execution_policy)
        expected_python = Path(sys.executable) if explicit else self.managed_python
        self.assertEqual(Path(record["executable"]).resolve(), expected_python.resolve())
        self.assertFalse(self.missing_vcpkg.exists())
        self.assertFalse((self.state / "tools").exists())

    def test_native_launch_boundary_preserves_argument_tails(self):
        for name, tail in _ARGUMENT_CORPUS:
            with self.subTest(case=name):
                direct = self._run_direct(tail)
                expected = _EXPECTED_PORTAL_ARGS.get(name)
                if expected is None:
                    self.assertEqual(len(direct["portalArgs"]), 1)
                    self.assertIn("quoted", direct["portalArgs"][0])
                else:
                    self.assertEqual(direct["portalArgs"], expected)

                for explicit in (True, False):
                    with self.subTest(explicit=explicit):
                        record = self._run_batch(tail, explicit=explicit)
                        self.assertEqual(record["portalArgs"], direct["portalArgs"])
                        self._assert_launch_environment(record, explicit=explicit)

    def test_native_launch_boundary_does_not_leak_powershell_defaults_into_an_unset_environment(self):
        record = self._run_batch("", explicit=False, ps_module_path=None, ps_execution_policy=None)
        self._assert_launch_environment(record, explicit=False, ps_module_path=None, ps_execution_policy=None)

    def test_native_launch_boundary_preserves_exit_status(self):
        for explicit in (True, False):
            with self.subTest(explicit=explicit):
                self._run_batch('"status check"', explicit=explicit, exit_code=17)

    def _await_parallel_readiness(self, pending):
        deadline = time.monotonic() + self._STEP_TIMEOUT
        while True:
            unready = [tail for tail, (_, ready) in pending.items() if not ready.is_file()]
            for tail, (process, _) in pending.items():
                self.assertIsNone(process.poll(), f"{tail} exited before the common readiness gate")
            if not unready:
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.fail(f"launchers not ready after {self._STEP_TIMEOUT}s: {', '.join(unready)}")
            time.sleep(min(remaining, 0.05))

    def _drain_parallel_launches(self, pending):
        # On early failure, EOF releases any consumers still waiting at the
        # fixture gate. Never kill just CMD and leave descendants on its pipes.
        for process, _ in pending.values():
            process.stdin.close()
            process.stdin = None
        deadline = time.monotonic() + self._CLEANUP_TIMEOUT
        for tail, (process, _) in pending.items():
            try:
                stdout, stderr = process.communicate(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                print(f"{tail}: did not drain within {self._CLEANUP_TIMEOUT}s cleanup budget", flush=True)
            else:
                print(f"{tail}: exit {process.returncode}, stdout={stdout!a}, stderr={stderr!a}", flush=True)

    def test_native_launch_boundary_keeps_simultaneous_calls_isolated(self):
        cases = (
            (False, 0, "parallel managed zero"),
            (False, 7, "parallel managed seven"),
            (True, 2, "parallel explicit two"),
            (True, 0, "parallel explicit zero"),
        )
        pending = {}
        try:
            for explicit, status, tail in cases:
                environment = self._environment(explicit=explicit, exit_code=status)
                ready = self.root / f"{tail}.ready"
                environment["AOBUS_FIXTURE_READY_FILE"] = str(ready)
                print(f"{self.id()}: launching {tail}", flush=True)
                process = subprocess.Popen(
                    self._cmd_line(self._batch_command(f'"{tail}"')),
                    cwd=self.caller,
                    env=environment,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                )
                pending[tail] = (process, ready)

            # Both a ready marker from each consumer and live foreground
            # launchers are required before any consumer is released.
            self._await_parallel_readiness(pending)
            deadline = time.monotonic() + self._STEP_TIMEOUT
            for explicit, status, tail in cases:
                process, _ = pending[tail]
                try:
                    stdout, stderr = process.communicate(
                        input="continue\n", timeout=max(0, deadline - time.monotonic())
                    )
                except subprocess.TimeoutExpired:
                    self.fail(f"{tail} did not exit within the {self._STEP_TIMEOUT}s shared release budget")
                del pending[tail]
                with self.subTest(tail=tail):
                    self.assertEqual(process.returncode, status, stderr)
                    record = json.loads(stdout)
                    self.assertEqual(record["portalArgs"], [tail])
                    self._assert_launch_environment(record, explicit=explicit)
        finally:
            self._drain_parallel_launches(pending)


@unittest.skipUnless(sys.platform == "win32", "native Windows batch environment bootstrap")
class WindowsBuildEnvReadinessTest(unittest.TestCase):
    """Prove the bootstrap reuses only a fully initialized x64 environment.

    The real windows-build-env.bat runs through the native cmd.exe boundary.
    A fixture VsDevCmd records initialization, so every case proves which path
    the readiness gate selected instead of reimplementing its checks.
    """

    # Readiness-relevant names never inherit from the host shell: a developer
    # prompt initialized for another architecture must not decide the cases.
    _CONTROLLED = (
        "VCPKG_ROOT",
        "VSDEVCMD",
        "VSROOT",
        "VSWHERE",
        "VSINSTALLDIR",
        "VCToolsInstallDir",
        "WindowsSdkDir",
        "WindowsSDKVersion",
        "VCToolsVersion",
        "UCRTVersion",
        "NETFXSDKDir",
        "EXTERNAL_INCLUDE",
        "INCLUDE",
        "LIB",
        "VSCMD_ARG_HOST_ARCH",
        "VSCMD_ARG_TGT_ARCH",
        "_AOBUS_ENV_SCRIPT",
        "_AOBUS_ENV_PYTHON",
    )

    @classmethod
    def setUpClass(cls):
        cls.comspec = os.environ.get("COMSPEC", "cmd.exe")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus build-env Åsa 音乐 ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scripts = self.root / "script-ao"
        self.scripts.mkdir()
        shutil.copy2(REPOSITORY / "script/ao/windows-build-env.bat", self.scripts / "windows-build-env.bat")
        shutil.copy2(REPOSITORY / "script/ao/windows-vsenv.bat", self.scripts / "windows-vsenv.bat")
        # The fixture installation records initializations instead of running
        # a real VsDevCmd, and carries the bundled vcpkg the selection defaults to.
        self.vsroot = self.root / "fixture-vs"
        tools = self.vsroot / "Common7" / "Tools"
        tools.mkdir(parents=True)
        (tools / "VsDevCmd.bat").write_bytes(b'@echo off\r\nset "AOBUS_FIXTURE_VSDEVCMD=invoked"\r\nexit /b 0\r\n')
        self.vsdevcmd = tools / "VsDevCmd.bat"
        self.vcpkg = self.vsroot / "VC" / "vcpkg"
        toolchain = self.vcpkg / "scripts" / "buildsystems"
        toolchain.mkdir(parents=True)
        (toolchain / "vcpkg.cmake").write_text("fixture toolchain\n", encoding="utf-8")
        self.compiler = self.root / "fixture-cl"
        self.compiler.mkdir()
        (self.compiler / "cl.exe").write_bytes(b"fixture compiler\r\n")
        self.caller = self.root / "caller"
        self.caller.mkdir()

    def _ready(self, *drop):
        values = {
            "VSCMD_ARG_HOST_ARCH": "x64",
            "VSCMD_ARG_TGT_ARCH": "x64",
            "VSINSTALLDIR": str(self.vsroot) + "\\",
            "VCToolsInstallDir": str(self.vsroot / "VC" / "Tools" / "MSVC" / "14.42.34433") + "\\",
            "WindowsSdkDir": "C:\\Windows Kits\\10\\",
            "WindowsSDKVersion": "10.0.26100.0\\",
            "VCToolsVersion": "14.42.34433",
            "INCLUDE": str(self.vsroot / "VC" / "Tools" / "MSVC" / "14.42.34433" / "include"),
            "LIB": str(self.vsroot / "VC" / "Tools" / "MSVC" / "14.42.34433" / "lib" / "x64"),
        }
        for name in drop:
            values.pop(name)
        return values

    def _environment(self, ready, *, cl=True, vcpkg_root=_DEFAULT):
        inherited = {
            key: value
            for key, value in os.environ.items()
            if key.upper() not in {name.upper() for name in self._CONTROLLED}
        }
        system32 = Path(os.environ["SystemRoot"]) / "System32"
        environment = {
            **inherited,
            "COMSPEC": self.comspec,
            "_AOBUS_ENV_PYTHON": sys.executable,
            "VSDEVCMD": str(self.vsdevcmd),
            "PATH": (str(self.compiler) + ";" if cl else "") + str(system32),
        }
        if vcpkg_root is _DEFAULT:
            environment["VCPKG_ROOT"] = str(self.vcpkg)
        elif vcpkg_root is not None:
            environment["VCPKG_ROOT"] = str(vcpkg_root)
        environment.update(ready)
        return environment

    def _run(self, ready, *, cl=True, vcpkg_root=_DEFAULT):
        environment = self._environment(ready, cl=cl, vcpkg_root=vcpkg_root)
        command = f'"{self.scripts / "windows-build-env.bat"}"'
        missing = sorted(self._ready().keys() - ready.keys())
        print(f"{self.id()}: launching build environment (missing={missing}, cl on PATH={cl})", flush=True)
        completed = subprocess.run(
            f'"{self.comspec}" /d /v:off /s /c "{command}"',
            cwd=self.caller,
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return json.loads(completed.stdout)

    def test_a_fully_initialized_x64_host_target_environment_is_reused(self):
        capture = self._run(self._ready())
        self.assertNotIn("AOBUS_FIXTURE_VSDEVCMD", capture)

    def test_anything_less_initializes_vsdevcmd_instead_of_reusing(self):
        cases = [(f"missing-{name}", self._ready(name), True) for name in self._ready()]
        for label, name, value in (
            ("x86-host", "VSCMD_ARG_HOST_ARCH", "x86"),
            ("x86-target", "VSCMD_ARG_TGT_ARCH", "x86"),
        ):
            wrong = self._ready()
            wrong[name] = value
            cases.append((label, wrong, True))
        cases.append(("no-cl-on-path", self._ready(), False))
        for label, ready, cl in cases:
            with self.subTest(case=label):
                capture = self._run(ready, cl=cl)
                self.assertEqual(capture.get("AOBUS_FIXTURE_VSDEVCMD"), "invoked")

    def test_an_explicit_selection_defaults_vcpkg_beside_its_installation(self):
        # VCPKG_ROOT stays undefined, so the bootstrap must derive it from the
        # preserved explicit selection instead of discovering another one.
        for ready in (self._ready(), self._ready("VSCMD_ARG_TGT_ARCH")):
            with self.subTest(initialized="reused" if "VSCMD_ARG_TGT_ARCH" in ready else "vsdevcmd"):
                capture = self._run(ready, vcpkg_root=None)
                self.assertEqual(_normalized(capture["VCPKG_ROOT"]), _normalized(self.vcpkg))


if __name__ == "__main__":
    unittest.main()
