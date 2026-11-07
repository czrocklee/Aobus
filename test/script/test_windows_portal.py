"""Windows portal preparation, dispatch, and native launch-boundary regressions."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import format as format_command
from ao.core import buildenv, gitfiles, pythoncheck, windows_portal
from ao.core.paths import PROJECT_ROOT

REPOSITORY = Path(__file__).resolve().parents[2]
WINDOWS_PORTAL_SCOPE = "script/ao/core/windows_portal.py"
_DEFAULT = object()

# The fixture package is only an external next-stage consumer. It records the
# arguments and selected environment it actually received; it does not model
# buildenv, command selection, or the production portal graph.
_FIXTURE_PYTHONENV = """\
import json
import os
import sys

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
if os.environ.get("AOBUS_FIXTURE_WAIT"):
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


@unittest.skipUnless(os.name == "nt", "native Windows short-path comparison")
class PathComparisonTest(unittest.TestCase):
    def test_short_ancestors_compare_equal_for_existing_and_missing_paths(self):
        import ctypes
        from ctypes import wintypes

        get_short_path = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
        get_short_path.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD)
        get_short_path.restype = wintypes.DWORD
        with tempfile.TemporaryDirectory(prefix="aobus long path comparison ") as temporary:
            root = Path(temporary).resolve()
            buffer = ctypes.create_unicode_buffer(32768)
            length = get_short_path(str(root), buffer, len(buffer))
            self.assertGreater(length, 0, ctypes.get_last_error())
            self.assertLess(length, len(buffer))
            short = Path(buffer.value)
            if short == root:
                self.skipTest("fixture volume does not expose an 8.3 alias")
            self.assertTrue(short.samefile(root))
            for relative in (Path(), Path("not-created") / "state"):
                with self.subTest(relative=relative):
                    self.assertEqual(_normalized(short / relative), _normalized(root / relative))
            self.assertFalse((root / "not-created").exists())


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
        expected = dict(captured)
        expected.pop("_AOBUS_ENV_SCRIPT")
        expected.pop("_AOBUS_ENV_PYTHON")
        completed = subprocess.CompletedProcess([], 0, stdout=json.dumps(captured).encode("ascii"), stderr=b"")

        with _isolated_environment(original):
            with (
                mock.patch.object(windows_portal.subprocess, "run", return_value=completed) as run,
                contextlib.redirect_stdout(io.StringIO()) as stdout,
                contextlib.redirect_stderr(io.StringIO()) as stderr,
            ):
                windows_portal.prepare_build_environment()

                self.assertEqual(dict(os.environ), expected)
                self.assertNotIn("AOBUS_ORIGINAL_ONLY", os.environ)

            self.assertEqual(stdout.getvalue(), "")
            self.assertEqual(stderr.getvalue(), "")

        run.assert_called_once()
        command, kwargs = run.call_args
        self.assertEqual(command, ('cmd.exe /d /v:off /s /c ""%_AOBUS_ENV_SCRIPT%""',))
        self.assertEqual(kwargs["executable"], original["COMSPEC"])
        self.assertEqual(kwargs["env"]["_AOBUS_ENV_SCRIPT"], str(PROJECT_ROOT / "script/ao/windows-build-env.bat"))
        self.assertEqual(kwargs["env"]["_AOBUS_ENV_PYTHON"], sys.executable)
        self.assertEqual(kwargs["env"]["AOBUS_ORIGINAL_ONLY"], "keep")
        self.assertTrue(kwargs["capture_output"])
        self.assertFalse(kwargs["check"])

    def test_prepare_build_environment_failures_keep_the_original_environment_and_never_echo_stdout(self):
        secret = "full-environment-secret"
        original = {
            "COMSPEC": "fixture-cmd.exe",
            "PATH": "original path",
            "AOBUS_ORIGINAL_ONLY": "keep",
        }
        cases = (
            ("malformed-json", b'{"' + secret.encode("ascii") + b'":', 0, b"", RuntimeError),
            ("invalid-utf8", b"\xff", 0, b"", RuntimeError),
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
                "null-value",
                json.dumps({"PATH": None, "AOBUS_SECRET_TOKEN": secret}).encode("ascii"),
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
                    self.assertEqual(raised.exception.cmd, "fixture command")
                    self.assertIn("setup failed", stderr.getvalue())
                else:
                    self.assertIn("Visual Studio", str(raised.exception))
                self.assertEqual(dict(os.environ), original)
                self.assertNotIn(secret, stdout.getvalue() + stderr.getvalue())


class WindowsPortalMainTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-portal-state-")
        self.addCleanup(temporary.cleanup)
        self.state_root = str(Path(temporary.name) / "state")

    def test_python_only_format_scope_uses_one_capability_decision_and_prepared_scope(self):
        calls = []

        def formatter(command, **kwargs):
            # platform.system() may ask CMD for its version on Windows. Keep
            # that external OS probe distinct from native build setup and Ruff.
            if command == "ver" and kwargs.get("shell") is True:
                return subprocess.CompletedProcess(
                    command, 0, stdout="Microsoft Windows [Version 10.0.12345.0]", stderr=""
                )
            if isinstance(command, str):
                raise AssertionError(f"unexpected build environment command: {command}")
            calls.append((tuple(command), kwargs))
            return subprocess.CompletedProcess(command, 0)

        with tempfile.TemporaryDirectory() as temporary:
            working_directory = Path(temporary)
            environment = {
                "PATH": "fixture-path",
                "AOBUS_STATE_ROOT": self.state_root,
                "_AOBUS_PORTAL_WORKING_DIRECTORY": str(working_directory),
                "AOBUS_PREFLIGHT_SCOPE": "stale-scope",
                "_AOBUS_UNC_MAPPING": "1",
            }
            with (
                _isolated_environment(environment),
                mock.patch.object(windows_portal.subprocess, "run", side_effect=formatter),
                mock.patch.object(
                    buildenv, "requires_parsed_build_env", wraps=buildenv.requires_parsed_build_env
                ) as required,
                mock.patch.object(format_command, "resolve_files", wraps=format_command.resolve_files) as resolve,
                mock.patch.object(gitfiles, "find_sources", wraps=gitfiles.find_sources) as scan,
                contextlib.redirect_stdout(io.StringIO()) as stdout,
                contextlib.redirect_stderr(io.StringIO()) as stderr,
            ):
                status = windows_portal.main(["format", "--check", "--folder", "script/ao"])
                process_directory = Path(os.getcwd())

                self.assertEqual(os.environ.get("PATH"), str(Path(sys.executable).parent) + os.pathsep + "fixture-path")
                self.assertNotIn("_AOBUS_PORTAL_WORKING_DIRECTORY", os.environ)
                self.assertNotIn("_AOBUS_UNC_MAPPING", os.environ)
                self.assertNotIn("AOBUS_PREFLIGHT_SCOPE", os.environ)

        self.assertEqual(status, 0, stderr.getvalue())
        self.assertEqual(stderr.getvalue(), "")
        self.assertEqual(process_directory.resolve(), working_directory.resolve())
        self.assertEqual(required.call_count, 1)
        self.assertEqual(resolve.call_count, 2)
        args = required.call_args.args[0]
        self.assertIs(resolve.call_args_list[0].args[0], args)
        self.assertIs(resolve.call_args_list[1].args[0], args)
        scan.assert_called_once_with(["script/ao"], suffixes=gitfiles.SOURCE_SUFFIXES)
        prepared = args._resolved_sources
        self.assertIn(WINDOWS_PORTAL_SCOPE, prepared)
        self.assertEqual(len(calls), 1)
        command, kwargs = calls[0]
        self.assertEqual(command, pythoncheck.ruff_command("format", "--check", *prepared))
        self.assertEqual(kwargs["cwd"], PROJECT_ROOT)
        self.assertIn(f"Checking {len(prepared)} file(s) with ruff format...", stdout.getvalue())
        self.assertIn("Done.", stdout.getvalue())

    def test_preparation_failure_returns_the_native_exit_status(self):
        cases = (
            (subprocess.CompletedProcess("cmd.exe", 17, stdout=b"", stderr=b"setup failed"), 17),
            (subprocess.CompletedProcess("cmd.exe", 0, stdout=b"not-json", stderr=b""), 1),
            (OSError("cannot prepare"), 1),
        )

        for result, expected in cases:
            with (
                self.subTest(result=result),
                _isolated_environment({"PATH": "fixture-path", "AOBUS_STATE_ROOT": self.state_root}),
            ):
                with (
                    mock.patch.object(
                        windows_portal.subprocess,
                        "run",
                        side_effect=result if isinstance(result, OSError) else None,
                        return_value=result,
                    ) as run,
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                ):
                    status = windows_portal.main(["build"])

                run.assert_called_once()
                self.assertEqual(status, expected)
                self.assertIn("Aobus Windows preparation failed", stderr.getvalue())

    def test_help_and_invalid_arguments_launch_no_external_command(self):
        cases = (
            (["help"], 0, False),
            (["--help"], 0, True),
            (["format", "--bogus"], 2, True),
        )

        for arguments, status, raises in cases:
            with (
                self.subTest(arguments=arguments),
                _isolated_environment({"PATH": "fixture-path", "AOBUS_STATE_ROOT": self.state_root}),
            ):
                with (
                    mock.patch.object(
                        windows_portal.subprocess, "run", side_effect=AssertionError("external command launched")
                    ) as run,
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    if raises:
                        with self.assertRaises(SystemExit) as raised:
                            windows_portal.main(list(arguments))
                        self.assertEqual(raised.exception.code, status)
                    else:
                        self.assertEqual(windows_portal.main(list(arguments)), status)

                run.assert_not_called()


@unittest.skipUnless(sys.platform == "win32", "native Windows batch and PowerShell launch boundary")
class WindowsLaunchBoundaryTest(unittest.TestCase):
    """Prove native batch/PowerShell transport and ready-base reuse, not the full portal."""

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
            )
            cls.managed_ready = ready.returncode == 0
        if not cls.base_python.is_file():
            raise unittest.SkipTest("sys.base_prefix does not contain python.exe")
        probe = subprocess.run(
            [str(cls.base_python), "-I", "-c", "import ensurepip, venv"],
            capture_output=True,
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
        core = self.package / "core"
        core.mkdir()
        (core / "__init__.py").write_text("", encoding="utf-8")
        (core / "pythonenv.py").write_text(_FIXTURE_PYTHONENV, encoding="utf-8")

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

    def _run_cmd(self, command, *, environment):
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
                "ao.core.pythonenv",
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
        return self._record(self._run_cmd(self._direct_command(tail), environment=environment), exit_code)

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
        return self._record(self._run_cmd(self._batch_command(tail), environment=environment), exit_code)

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
        self.assertTrue(self.base_python.is_file())
        if not explicit:
            self.assertTrue(self.managed_python.is_file())

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
                    record = self._run_batch(tail, explicit=explicit)
                    self.assertEqual(record["portalArgs"], direct["portalArgs"])
                    self._assert_launch_environment(record, explicit=explicit)

    def test_native_launch_boundary_restores_powershell_host_environment(self):
        for explicit in (False, True):
            for present in (True, False):
                with self.subTest(explicit=explicit, present=present):
                    module_path = self.ps_module_path if present else None
                    policy = self.ps_execution_policy if present else None
                    record = self._run_batch(
                        '"powershell environment"',
                        explicit=explicit,
                        ps_module_path=module_path,
                        ps_execution_policy=policy,
                    )
                    self.assertEqual(record["portalArgs"], ["powershell environment"])
                    self._assert_launch_environment(
                        record,
                        explicit=explicit,
                        ps_module_path=module_path,
                        ps_execution_policy=policy,
                    )

    def test_native_launch_boundary_preserves_exit_status(self):
        for explicit in (False, True):
            for status in (0, 7, 2):
                with self.subTest(explicit=explicit, status=status):
                    record = self._run_batch('"status check"', explicit=explicit, exit_code=status)
                    self.assertEqual(record["portalArgs"], ["status check"])
                    self._assert_launch_environment(record, explicit=explicit)

    def test_native_launch_boundary_keeps_simultaneous_calls_isolated(self):
        cases = (
            (False, 0, "parallel managed zero"),
            (False, 7, "parallel managed seven"),
            (True, 2, "parallel explicit two"),
            (True, 0, "parallel explicit zero"),
        )
        processes = []
        try:
            for explicit, status, tail in cases:
                environment = self._environment(explicit=explicit, exit_code=status)
                environment["AOBUS_FIXTURE_WAIT"] = "1"
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
                processes.append((explicit, status, tail, process))

            records = []
            for _explicit, _status, _tail, process in processes:
                self.assertIsNotNone(process.stdout)
                line = process.stdout.readline()
                self.assertTrue(line, "launch exited before the consumer readiness gate")
                records.append(json.loads(line))
            # All foreground launchers must coexist at a controlled checkpoint;
            # elapsed time and merely starting several processes are not proof.
            self.assertTrue(all(process.poll() is None for *_case, process in processes))
            for (explicit, status, tail, process), record in zip(processes, records, strict=True):
                with self.subTest(tail=tail):
                    _stdout, stderr = process.communicate(input="continue\n")
                    self.assertEqual(process.returncode, status, stderr)
                    self.assertEqual(record["portalArgs"], [tail])
                    self._assert_launch_environment(record, explicit=explicit)
        finally:
            for *_case, process in processes:
                # Release even failed assertions normally; never kill a launcher.
                process.communicate(input=None if process.stdin.closed else "continue\n")


if __name__ == "__main__":
    unittest.main()
