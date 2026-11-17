"""Native test fixture selection, cleanup, and launch boundaries."""

import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from unittest import mock

from ao.graph import bazel_test_runner as runner
from ao.portal import bazel_native_smoke as smoke
from ao.portal import winui


def _unexpected_fixture(*args, **kwargs):
    raise AssertionError("unused fixtures must not start")


class BazelTestRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-test-runner-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.environment = {"PATH": "/bin", "TEST_TMPDIR": str(self.root / "test-tmp")}
        # Exercise Linux selection without installing handlers or starting services.
        self.enterContext(mock.patch.object(runner, "_IPC_TMPDIR", str(self.root)))
        self.enterContext(mock.patch.object(runner.sys, "platform", "linux"))
        self.enterContext(mock.patch.object(runner.signal, "signal", return_value=signal.SIG_DFL))
        self.child = mock.Mock(stdout=None)
        self.child.poll.return_value = 0
        self.child.wait.side_effect = self.child.poll
        self.popen = self.enterContext(mock.patch.object(runner.subprocess, "Popen", return_value=self.child))

    @contextlib.contextmanager
    def _bus(self, **kwargs):
        yield {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/owned/bus"}

    def test_empty_context_forwards_command_and_environment_without_allocating_ipc(self):
        with mock.patch.object(runner.tempfile, "mkdtemp", _unexpected_fixture):
            self.assertEqual(runner.run(["probe", "--flag"], self.environment), 0)
        self.popen.assert_called_once_with(["probe", "--flag"], env=self.environment)

    def test_audio_and_dbus_contexts_use_the_private_bus_without_a_display(self):
        @contextlib.contextmanager
        def audio(log_dir, session, **kwargs):
            self.assertEqual(session["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/owned/bus")
            yield {"AOBUS_REQUIRE_PIPEWIRE": "1"}

        for context in ("audio", "dbus"):
            with (
                self.subTest(context=context),
                mock.patch.object(runner.services, "private_session_bus", self._bus),
                mock.patch.object(
                    runner.services, "private_pipewire", audio if context == "audio" else _unexpected_fixture
                ),
                mock.patch.object(runner.services, "virtual_gtk_displays", _unexpected_fixture),
            ):
                environment = self.environment | {"AOBUS_TEST_CONTEXT": context}
                self.assertEqual(runner.run(["probe"], environment), 0)
                child = self.popen.call_args.kwargs["env"]
                self.assertEqual(child["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/owned/bus")
                self.assertEqual(child.get("AOBUS_REQUIRE_PIPEWIRE"), "1" if context == "audio" else None)
                self.assertNotIn("DISPLAY", child)

    def test_overlapping_fixtures_clean_only_their_own_root_even_on_exception(self):
        @contextlib.contextmanager
        def bus(ipc_root, **kwargs):
            ipc_root.mkdir()
            marker = ipc_root / "owned"
            marker.touch()
            try:
                yield {"marker": str(marker)}
            finally:
                # Services must exit while their runtime directory still exists.
                self.assertTrue(marker.is_file())

        selected = frozenset({"dbus"})
        with mock.patch.object(runner.services, "private_session_bus", bus):
            with runner._fixture_environment(selected, {}, lambda: None) as first:
                first_root = Path(first["marker"]).parents[1]
                with self.assertRaises(OSError):
                    with runner._fixture_environment(selected, {}, lambda: None) as second:
                        second_root = Path(second["marker"]).parents[1]
                        self.assertNotEqual(first_root, second_root)
                        raise OSError("fixture failure")
                self.assertFalse(second_root.exists())
                self.assertTrue(Path(first["marker"]).is_file())
            self.assertFalse(first_root.exists())

    def test_linux_context_off_linux_forwards_status_without_starting_fixtures(self):
        environment = self.environment | {"AOBUS_TEST_CONTEXT": "gtk"}
        with (
            mock.patch.object(runner.sys, "platform", "win32"),
            mock.patch.object(runner, "_run_linux", _unexpected_fixture),
            mock.patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)),
        ):
            self.assertEqual(runner.run(["probe"], environment), 7)

    def test_uncertain_pipewire_runtime_survives_wrapper_teardown(self):
        self.environment["AOBUS_TEST_CONTEXT"] = "audio"
        runtimes = []

        @contextlib.contextmanager
        def audio(log_dir, session, *, runtime_root, check_cancelled):
            runtimes.append(runtime_root)
            runtime_root.mkdir()
            yield {}
            raise RuntimeError("private PipeWire cleanup uncertain")

        with (
            mock.patch.object(runner.services, "private_session_bus", self._bus),
            mock.patch.object(runner.services, "private_pipewire", audio),
            self.assertRaisesRegex(RuntimeError, "cleanup uncertain"),
        ):
            runner.run(["probe"], self.environment)
        self.assertTrue(runtimes[0].is_dir())

    def test_unknown_context_fails_instead_of_running_without_fixtures(self):
        self.environment["AOBUS_TEST_CONTEXT"] = "audoi"
        self.popen.side_effect = _unexpected_fixture
        with self.assertRaisesRegex(RuntimeError, "unknown AOBUS_TEST_CONTEXT"):
            runner.run(["probe"], self.environment)

    def test_child_exit_status_is_not_replaced_by_fixture_cleanup_failure(self):
        self.environment["AOBUS_TEST_CONTEXT"] = "dbus"
        self.child.poll.return_value = 7

        def fail_cleanup():
            raise OSError("bus cleanup failed")

        @contextlib.contextmanager
        def bus(**kwargs):
            with runner.services.cleanup_after(fail_cleanup):
                yield {}

        with mock.patch.object(runner.services, "private_session_bus", bus), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.run(["probe"], self.environment), 7)

    def test_missing_executable_returns_127(self):
        self.popen.side_effect = FileNotFoundError("probe")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.run(["probe"], self.environment), 127)


@unittest.skipUnless(sys.platform == "linux", "Linux wrapper owns these POSIX fixtures")
class BazelTestRunnerProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-test-runner-process-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        script = f"#!{sys.executable}\n" + textwrap.dedent(
            f"""\
            import json
            import os
            import signal
            import sys
            import xml.etree.ElementTree as ET
            from pathlib import Path

            root = Path({str(self.root)!r})
            name = Path(sys.argv[0]).name
            stalled = name == "declared-bus" and (root / "stall-bus").exists()
            if name == "pw-cli":
                if sys.argv[1:] == ["ls", "Metadata"]:
                    print('metadata.name = "default"')
                elif sys.argv[1:] != ["info", "0"]:
                    sys.exit(91)
                sys.exit(0)

            def stop(signum, frame):
                (root / (name + ".stopped")).touch()
                if not stalled:
                    sys.exit(0)

            signal.signal(signal.SIGTERM, stop)
            signal.alarm(15)  # Even an orphaned fake cannot outlive a broken test indefinitely.
            ipc_root = None
            if name == "Xvfb":
                print("87", flush=True)
            elif name == "declared-bus":
                config = Path(sys.argv[2].removeprefix("--config-file="))
                address = ET.parse(config).getroot().findtext("listen")
                ipc_root = str(config.parents[1])
                # Partial stdout must not block cancellation or the readiness deadline.
                print(address[:5] if stalled else address, end="" if stalled else "\\n", flush=True)
            elif name == "pipewire":
                print("fixture daemon started", flush=True)
            elif name == "child":
                assert os.environ["DISPLAY"] == ":87"
                assert os.environ["AOBUS_REQUIRE_PIPEWIRE"] == "1"
                bus = Path(os.environ["DBUS_SESSION_BUS_ADDRESS"].removeprefix("unix:path="))
                assert bus.parent.is_dir() and len(os.fsencode(bus)) < 108
                ipc_root = str(Path(os.environ["XDG_RUNTIME_DIR"]).parent)
                assert bus.parent.parent == Path(ipc_root)
                assert Path(ipc_root).stat().st_mode & 0o777 == 0o700
            marker = root / (name + ".starting")
            marker.write_text(json.dumps(dict(pid=os.getpid(), ipc_root=ipc_root)))
            marker.replace(root / (name + ".ready"))
            while True:
                signal.pause()
            """
        )
        for name in ("Xvfb", "declared-bus", "pipewire", "wireplumber", "pw-cli", "child"):
            tool = self.root / name
            tool.write_text(script, encoding="utf-8")
            tool.chmod(0o700)

    def _launch(self):
        environment = os.environ | {
            "AOBUS_TEST_CONTEXT": "gtk",
            "AOBUS_TEST_DBUS_DAEMON": str(self.root / "declared-bus"),
            "TEST_TMPDIR": str(self.root / ("long-base-" + "x" * 100) / "test-tmp"),
            "TEST_UNDECLARED_OUTPUTS_DIR": str(self.root / "outputs"),
            "PATH": str(self.root) + os.pathsep + os.defpath,
            "PYTHONPATH": str(Path(runner.__file__).absolute().parents[2]),
        }
        command = textwrap.dedent(
            """\
            import os
            import sys
            from pathlib import Path
            from ao.graph import bazel_test_runner as runner

            root = Path(sys.argv[1])
            runner.services.native.executable = lambda name, *, nix: str(root / name)
            raise SystemExit(runner.run([str(root / "child")], os.environ))
            """
        )
        wrapper = subprocess.Popen(
            [sys.executable, "-c", command, str(self.root)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )

        def cleanup():
            # This group contains only this test's wrapper and fake processes.
            with contextlib.suppress(ProcessLookupError):
                os.killpg(wrapper.pid, signal.SIGKILL)
            wrapper.communicate(timeout=5)

        self.addCleanup(cleanup)
        return wrapper

    def _await_marker(self, wrapper, marker):
        path = self.root / marker
        deadline = time.monotonic() + 5
        while not path.exists():
            if wrapper.poll() is not None:
                output, _ = wrapper.communicate(timeout=1)
                self.fail(f"wrapper exited {wrapper.returncode} before {marker}: {output}")
            self.assertLess(time.monotonic(), deadline, f"wrapper never reached {marker}")
            time.sleep(0.02)

    def _assert_terminated(self, wrapper, names):
        output, _ = wrapper.communicate(timeout=5)
        self.assertEqual(wrapper.returncode, 128 + signal.SIGTERM, output)
        for name in names:
            self.assertTrue((self.root / f"{name}.stopped").exists(), f"{name} never received SIGTERM")
            state = json.loads((self.root / f"{name}.ready").read_text(encoding="utf-8"))
            with self.assertRaises(ProcessLookupError, msg=f"{name} was not reaped"):
                os.kill(state["pid"], 0)
            if state["ipc_root"] is not None:
                root = Path(state["ipc_root"])
                self.assertEqual(root.parent, Path("/tmp"))
                self.assertFalse(root.exists())

    def test_sigterm_reaps_the_wrapped_child_and_all_running_fixtures(self):
        wrapper = self._launch()
        names = ("child", "wireplumber", "pipewire", "declared-bus", "Xvfb")
        for name in names:
            self._await_marker(wrapper, f"{name}.ready")
        wrapper.terminate()  # Signal only the wrapper; it owns forwarding.
        self._assert_terminated(wrapper, names)
        log = self.root / "outputs/aobus-test-services/pipewire.stdout.log"
        self.assertIn("fixture daemon started", log.read_text(encoding="utf-8"))

    def test_sigterm_during_partial_startup_escalates_and_survives_a_second_signal(self):
        (self.root / "stall-bus").touch()
        wrapper = self._launch()
        self._await_marker(wrapper, "declared-bus.ready")
        wrapper.terminate()
        self._await_marker(wrapper, "declared-bus.stopped")
        wrapper.terminate()  # Cleanup is waiting for the daemon that ignored SIGTERM.
        self._assert_terminated(wrapper, ("declared-bus", "Xvfb"))
        self.assertFalse((self.root / "pipewire.ready").exists())
        self.assertFalse((self.root / "child.ready").exists())


class BazelNativeSmokeTests(unittest.TestCase):
    def test_batch_command_passes_a_quoted_runfile_and_argument_to_cmd(self):
        executable = Path(r"C:\runfiles tree\probe.bat")
        comspec = r"C:\Windows\System32\cmd.exe"
        with mock.patch.object(smoke.sys, "platform", "win32"):
            command = smoke._command(executable, ["two words"], {"COMSPEC": comspec})
        self.assertEqual(command, rf'"{comspec}" /d /s /c ""C:\runfiles tree\probe.bat" "two words""')

    def test_uia_cannot_launch_without_runtime_or_interactive_session(self):
        for gate in ("require_runtime", "require_interactive_session"):
            with (
                self.subTest(gate=gate),
                mock.patch.object(winui, "require_runtime"),
                mock.patch.object(winui, "require_interactive_session"),
                mock.patch.object(winui, gate, side_effect=RuntimeError("UIA precondition failed")),
                mock.patch.object(smoke, "_create_runfiles", _unexpected_fixture),
                mock.patch.dict(smoke.os.environ, {"AOBUS_TEST_CONTEXT": "uia"}),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
            ):
                self.assertEqual(smoke.main(["pkg/probe"]), 1)
                self.assertIn("UIA precondition failed", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
