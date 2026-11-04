"""Orchestration contracts of the shared native environment repository rule."""

import ast
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


class _StarlarkFailure(Exception):
    """Sentinel for the rule's fail() calls; carries the Starlark message."""


class _RepoPath:
    def __init__(self, path, directory_reads):
        self.path = Path(path)
        self.directory_reads = directory_reads

    def __str__(self):
        return str(self.path)

    @property
    def dirname(self):
        return _RepoPath(self.path.parent, self.directory_reads)

    @property
    def exists(self):
        return self.path.exists()

    @property
    def is_dir(self):
        return self.path.is_dir()

    def get_child(self, name):
        return _RepoPath(self.path / name, self.directory_reads)

    def readdir(self, *, watch):
        if watch != "yes":
            raise AssertionError("directory membership must be watched")
        self.directory_reads.add(str(self.path))
        return [_RepoPath(path, self.directory_reads) for path in self.path.iterdir()]


class EnvironmentRuleTest(unittest.TestCase):
    """The repository rule's orchestration; helper output is a minimal valid fixture."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-environment-rule-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        (self.root / "CMakeLists.txt").write_text("project(aobus)\n", encoding="utf-8")
        (self.root / "bazel_environment.py").write_text("# helper\n", encoding="utf-8")
        tools = self.root / "script/ao"
        tools.mkdir(parents=True)
        (tools / "toolchain.json").write_text(json.dumps({"python": "3.14.7"}), encoding="utf-8")
        self.watches = set()
        self.directory_reads = set()
        self.files = {}
        self.executions = []
        self.events = []
        self.environ = {}
        self.which = None
        self.metadata = {}
        self.execute_code = 0
        self.expected_command = None

        def attribute(**kwargs):
            return kwargs

        def fail(message):
            raise _StarlarkFailure(message)

        def starlark_type(value):
            # Starlark's type() names values; the rule checks metadata with it.
            if value is None:
                return "NoneType"
            if isinstance(value, bool):
                return "bool"
            if isinstance(value, dict):
                return "dict"
            if isinstance(value, str):
                return "string"
            if isinstance(value, int):
                return "int"
            if isinstance(value, list):
                return "list"
            return type(value).__name__

        self.namespace = {
            "attr": SimpleNamespace(label=attribute),
            "Label": str,
            "repository_rule": attribute,
            "json": SimpleNamespace(encode=json.dumps, decode=json.loads),
            "type": starlark_type,
            "fail": fail,
        }
        source = (Path(__file__).resolve().parents[2] / "bazel/environment.bzl").read_text(encoding="utf-8")
        exec(compile(ast.parse(source), "environment.bzl", "exec"), self.namespace)
        self.quote = self.namespace["_quote"]
        self.impl = self.namespace["_environment_impl"]

    def context(self, os_name):
        return SimpleNamespace(
            os=SimpleNamespace(name=os_name, environ=self.environ),
            attr=SimpleNamespace(helper="bazel_environment.py", root="CMakeLists.txt"),
            getenv=self.environ.get,
            which=lambda name: self.which if name == "nix-shell" else None,
            path=lambda value: _RepoPath(self.root / str(value), self.directory_reads),
            watch=lambda path: self.watches.add(str(path)),
            file=self.file,
            report_progress=lambda message: self.events.append(("progress", message)),
            execute=self.execute,
            read=lambda path: (self.root / str(path)).read_text(encoding="utf-8"),
        )

    def file(self, name, content, *, executable=False):
        self.assertFalse(executable)
        self.files[name] = content
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def execute(self, argv, **kwargs):
        self.events.append(("execute", str(argv[0])))
        if str(argv[0]) != self.expected_command:
            raise AssertionError("unexpected repository command: " + str(argv[0]))
        # The real helper publishes its metadata only through this output.
        (self.root / "environment.json").write_text(json.dumps(self.metadata), encoding="utf-8")
        self.executions.append((list(argv), kwargs))
        return SimpleNamespace(return_code=self.execute_code, stdout="", stderr="")

    def metadata_for(self, host, *, watch_files=(), watch_directories=()):
        return {
            "schema": 1,
            "host": host,
            "python": "/prepared/python",
            "environment": {"PATH": "/prepared/bin"},
            "watch_files": list(watch_files),
            "watch_directories": list(watch_directories),
        }

    def outside_inputs(self):
        outside = self.root.parent / (self.root.name + "-inputs")
        outside.mkdir()
        self.addCleanup(outside.rmdir)
        return outside

    def test_linux_bootstraps_one_pure_pinned_nix_shell_with_closed_client_environment(self):
        state = self.root / "state"
        self.environ = {
            "PATH": "/usr/bin",
            "HOME": "/home/user",
            "AOBUS_STATE_ROOT": str(state),
            "XDG_CACHE_HOME": str(self.root / "cache"),
            "R2_SECRET_ACCESS_KEY": "secret",
            "AWS_SECRET_ACCESS_KEY": "secret",
            "PYTHONPATH": "ambient/modules",
            "NIX_CFLAGS_COMPILE": "ambient-flags",
            "AOBUS_NIX_DEPENDENCY_REPORT": str(self.root / "ambient-report.json"),
            "UNRELATED_CLIENT_VALUE": "closed",
            "PYTHONPYCACHEPREFIX": "/client/source-cache",
            "TEMP": "volatile-client-temp",
        }
        nix = self.root / "bin/nix-shell"
        nix.parent.mkdir()
        nix.write_text("#!/bin/sh\n", encoding="utf-8")
        self.which = str(nix)
        outside = self.outside_inputs()
        self.metadata = self.metadata_for(
            "linux",
            watch_files=[str(self.root / "environment.json"), str(outside / "input.json")],
            watch_directories=[str(self.root / "_tmp"), str(outside)],
        )
        self.expected_command = str(nix)
        self.impl(self.context("linux"))
        # One bootstrap serves the whole shared host repository.
        self.assertEqual(len(self.executions), 1)
        ((argv, options),) = self.executions
        self.assertEqual(argv[0], str(nix))
        self.assertEqual(argv[1], "--pure")
        self.assertEqual(argv[-3], str(self.root / "shell.nix"))
        keeps = {name for index, name in enumerate(argv) if index and argv[index - 1] == "--keep"}
        self.assertEqual(keeps, {"AOBUS_STATE_ROOT", "XDG_CACHE_HOME"})
        command = 'exec "$AO_NIX_PORTAL_PYTHON" -I -S -B -X utf8 -X ' + self.quote(
            "pycache_prefix=" + str(self.root / "_tmp/pycache")
        )
        command += " " + self.quote(str(self.root / "bazel_environment.py"))
        command += " " + self.quote("--output") + " " + self.quote(str(self.root / "environment.json"))
        self.assertEqual(argv[-2:], ["--run", command])
        # Bytecode never comes from a client source cache; -B alone does not
        # prevent stale source-backed pyc reads.
        self.assertNotIn("/client/source-cache", command)
        environment = options["environment"]
        self.assertEqual(environment["AOBUS_STATE_ROOT"], str(state))
        self.assertEqual(environment["XDG_CACHE_HOME"], str(self.root / "cache"))
        self.assertEqual(environment["PATH"], "/usr/bin")
        for name in (
            "R2_SECRET_ACCESS_KEY",
            "AWS_SECRET_ACCESS_KEY",
            "PYTHONPATH",
            "NIX_CFLAGS_COMPILE",
            "AOBUS_NIX_DEPENDENCY_REPORT",
            "UNRELATED_CLIENT_VALUE",
            "PYTHONPYCACHEPREFIX",
        ):
            self.assertIsNone(environment[name])
        for name in ("TEMP", "TMP", "TMPDIR"):
            self.assertEqual(environment[name], str(self.root / "_tmp"))
        self.assertEqual(environment["PYTHONDONTWRITEBYTECODE"], "1")
        # Progress is reported before the one long repository execution.
        progress = [event for event in self.events if event[0] == "progress"]
        executes = [event for event in self.events if event[0] == "execute"]
        self.assertEqual(len(progress), 1)
        self.assertIn("linux", progress[0][1])
        self.assertLess(self.events.index(progress[0]), self.events.index(executes[0]))
        for path in (self.root / "bazel_environment.py", nix, self.root / "shell.nix", self.root / "nixpkgs.json"):
            self.assertIn(str(path), self.watches)
        self.assertIn(str(outside / "input.json"), self.watches)
        self.assertNotIn(str(self.root / "environment.json"), self.watches)
        self.assertIn(str(outside), self.directory_reads)
        self.assertNotIn(str(self.root / "_tmp"), self.directory_reads)
        self.assertIn("BUILD.bazel", self.files)

    def test_missing_nix_fails_before_any_repository_execution(self):
        self.environ = {"PATH": "/usr/bin"}
        self.metadata = self.metadata_for("linux")
        with self.assertRaisesRegex(_StarlarkFailure, "requires Nix installed and nix-shell on PATH"):
            self.impl(self.context("linux"))
        self.assertEqual(self.executions, [])
        self.assertNotIn("BUILD.bazel", self.files)

    def test_failed_or_invalid_helper_output_never_materializes_the_repository(self):
        self.environ = {"PATH": "/usr/bin"}
        nix = self.root / "bin/nix-shell"
        nix.parent.mkdir()
        nix.write_text("#!/bin/sh\n", encoding="utf-8")
        self.which = str(nix)
        self.expected_command = str(nix)
        for name, code, metadata in (
            ("helper exit", 1, self.metadata_for("linux")),
            ("schema", 0, {**self.metadata_for("linux"), "schema": 0}),
            ("host", 0, self.metadata_for("windows")),
        ):
            with self.subTest(name=name):
                self.executions = []
                self.files = {}
                self.execute_code = code
                self.metadata = metadata
                with self.assertRaisesRegex(_StarlarkFailure, "Cannot prepare|Invalid native environment metadata"):
                    self.impl(self.context("linux"))
                self.assertEqual(len(self.executions), 1)
                self.assertNotIn("BUILD.bazel", self.files)

    def test_windows_selects_ready_base_python_and_sdk_state_case_insensitively(self):
        state = self.root / "state"
        python = state / "tools/python/3.14.7/python.exe"
        python.parent.mkdir(parents=True)
        python.write_bytes(b"")
        outside = self.outside_inputs()
        self.environ = {
            "ProgramFiles(x86)": "C:/Program Files (x86)",
            "ProgramW6432": "C:/Program Files",
            "WindowsSdkDir": "C:/windows-kits/sdk",
            "VCToolsInstallDir": "C:/vc/tools/msvc",
            "AOBUS_STATE_ROOT": str(state),
            "UNRELATED_CLIENT_VALUE": "closed",
            "TMP": "volatile-client-temp",
        }
        self.metadata = self.metadata_for(
            "windows", watch_files=[str(self.root / "Environment.json"), str(outside / "Sdk.json")]
        )
        self.expected_command = str(python)
        self.impl(self.context("windows"))
        self.assertEqual(len(self.executions), 1)
        ((argv, options),) = self.executions
        # The ready base Python runs the helper directly; fetch never installs.
        self.assertEqual(
            argv,
            [
                str(python),
                "-I",
                "-S",
                "-B",
                "-X",
                "utf8",
                "-X",
                "pycache_prefix=" + str(self.root / "_tmp/pycache"),
                str(self.root / "bazel_environment.py"),
                "--output",
                str(self.root / "environment.json"),
            ],
        )
        environment = options["environment"]
        self.assertEqual(environment["PROGRAMFILES(X86)"], "C:/Program Files (x86)")
        self.assertEqual(environment["PROGRAMW6432"], "C:/Program Files")
        self.assertEqual(environment["WINDOWSSDKDIR"], "C:/windows-kits/sdk")
        self.assertEqual(environment["VCTOOLSINSTALLDIR"], "C:/vc/tools/msvc")
        self.assertIsNone(environment["UNRELATED_CLIENT_VALUE"])
        self.assertEqual(environment["TMP"], str(self.root / "_tmp"))
        self.assertIn(str(python), self.watches)
        # Own-repository entries are skipped even with different casing.
        self.assertNotIn(str(self.root / "Environment.json"), self.watches)
        self.assertIn(str(outside / "Sdk.json"), self.watches)
        self.assertIn("BUILD.bazel", self.files)

    def test_windows_missing_explicit_python_fails_without_installing(self):
        self.environ = {
            "AOBUS_BAZEL_PYTHON": str(self.root / "missing/python.exe"),
            "LOCALAPPDATA": str(self.root / "local"),
        }
        self.metadata = self.metadata_for("windows")
        with self.assertRaisesRegex(_StarlarkFailure, "Explicit native Python is missing"):
            self.impl(self.context("windows"))
        self.assertEqual(self.executions, [])
        self.assertNotIn("BUILD.bazel", self.files)

    def test_windows_mixed_case_explicit_selectors_do_not_fall_through_to_defaults(self):
        state = self.root / "state"
        base = state / "tools/python/3.14.7/python.exe"
        base.parent.mkdir(parents=True)
        base.write_bytes(b"")
        request = self.root / "portal-request.json"
        request.write_text("{}\n", encoding="utf-8")
        explicit = self.root / "explicit/python.exe"
        explicit.parent.mkdir()
        explicit.write_bytes(b"")
        self.environ = {
            "aobus_bazel_python": str(explicit),
            "aobus_bazel_configuration": str(request),
            "AOBUS_STATE_ROOT": str(state),
        }
        self.metadata = self.metadata_for("windows")
        self.expected_command = str(explicit)
        self.impl(self.context("windows"))
        self.assertEqual(len(self.executions), 1)
        ((argv, _),) = self.executions
        # The valid mixed-case explicit interpreter is used, not the base default.
        self.assertEqual(argv[0], str(explicit))
        self.assertEqual(argv[-2:], ["--request", str(request)])
        self.assertIn(str(request), self.watches)
        self.assertIn(str(explicit), self.watches)
        self.assertIn("BUILD.bazel", self.files)
        # A missing mixed-case explicit interpreter still fails; the ready base
        # Python exists but must never become the silent fallback.
        self.environ = {
            "aobus_bazel_python": str(self.root / "missing/python.exe"),
            "AOBUS_STATE_ROOT": str(state),
        }
        self.executions = []
        self.files = {}
        with self.assertRaisesRegex(_StarlarkFailure, "Explicit native Python is missing"):
            self.impl(self.context("windows"))
        self.assertEqual(self.executions, [])
        self.assertNotIn("BUILD.bazel", self.files)

    def test_macos_wraps_only_existing_ready_bootstrap_functions_with_repository_state(self):
        self.environ = {
            "DEVELOPER_DIR": "/Applications/Xcode.app",
            "AOBUS_STATE_ROOT": str(self.root / "state"),
            "UNRELATED_CLIENT_VALUE": "closed",
        }
        self.metadata = self.metadata_for("macos")
        self.expected_command = "/bin/bash"
        self.impl(self.context("mac os x"))
        self.assertEqual(len(self.executions), 1)
        ((argv, options),) = self.executions
        self.assertEqual(argv[:2], ["/bin/bash", "-c"])
        command = argv[2]
        bootstrap = self.quote(str(self.root / "script/ao/macos-vcpkg-bootstrap.sh"))
        self.assertTrue(command.startswith("set -e; source " + bootstrap + "; "))
        self.assertIn("aobus_macos_prepare_python " + self.quote(str(self.root)) + "; ", command)
        self.assertIn("export AOBUS_STATE_ROOT=" + self.quote(str(self.root / "_state")) + "; ", command)
        self.assertIn("aobus_macos_prepare_bazel_environment " + self.quote(str(self.root)) + "; ", command)
        self.assertIn("export AOBUS_STATE_ROOT=$original_state; ", command)
        self.assertIn(
            'exec "$AOBUS_PYTHON" -I -S -B -X utf8 -X '
            + self.quote("pycache_prefix=" + str(self.root / "_tmp/pycache"))
            + " "
            + self.quote(str(self.root / "bazel_environment.py")),
            command,
        )
        self.assertIn(self.quote("--output") + " " + self.quote(str(self.root / "environment.json")), command)
        self.assertNotIn("vcpkg install", command)
        self.assertNotIn("pip install", command)
        environment = options["environment"]
        self.assertEqual(environment["DEVELOPER_DIR"], "/Applications/Xcode.app")
        self.assertIsNone(environment["UNRELATED_CLIENT_VALUE"])
        self.assertEqual(environment["TMP"], str(self.root / "_tmp"))
        self.assertIn("BUILD.bazel", self.files)


if __name__ == "__main__":
    unittest.main()
