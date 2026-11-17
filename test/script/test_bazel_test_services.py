"""Private PipeWire and session-bus startup and teardown contracts."""

import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from unittest import mock
from urllib.parse import quote

from ao.core.bazel_native import BazelWorkspaceError
from ao.graph import bazel_test_services as services


class PrivatePipeWireTests(unittest.TestCase):
    """Inject exceptional process states that the real success path cannot force."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-pw-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.runtime = self.root / "pipewire"
        self.logs = self.root / "logs"
        self.session = {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/owned-bus/socket"}
        self.clock = 0.0
        self.children = {}
        self.connect = [(0, "")]
        self.policy = [(0, 'metadata.name = "default"')]
        self.on_spawn = lambda name, child: None
        if os.name == "nt":
            # The real stable-root API is POSIX-only; the fake still uses a real directory.
            def make_runtime(**kwargs):
                self.runtime.mkdir()
                return str(self.runtime)

            self.enterContext(mock.patch.object(services.tempfile, "mkdtemp", side_effect=make_runtime))
        self.enterContext(mock.patch.object(services.native, "executable", side_effect=lambda name, **_: name))
        self.enterContext(mock.patch.object(services.subprocess, "Popen", side_effect=self._spawn))
        self.enterContext(mock.patch.object(services.time, "monotonic", side_effect=lambda: self.clock))
        self.enterContext(mock.patch.object(services.time, "sleep", side_effect=self._advance))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))

    def _advance(self, delay):
        self.clock += delay
        self.assertLess(self.clock, 0.5, "fixture readiness never settled")

    def _spawn(self, argv, *, stdout, **kwargs):
        roles = {
            ("pipewire",): "pipewire",
            ("wireplumber", "--profile=policy"): "wireplumber",
            ("pw-cli", "info", "0"): "connect",
            ("pw-cli", "ls", "Metadata"): "policy",
        }
        role = roles[tuple(argv)]
        results = {"connect": self.connect, "policy": self.policy}.get(role)
        status, output = (None, "")
        if results:
            status, output = results.pop(0) if len(results) > 1 else results[0]
        child = mock.Mock(pid=1000 + sum(map(len, self.children.values())), status=status)
        child.poll.side_effect = lambda: child.status
        child.wait.side_effect = lambda timeout: child.status
        child.terminate.side_effect = lambda: setattr(child, "status", -signal.SIGTERM)
        self.children.setdefault(role, []).append(child)
        stdout.write(output.encode())
        stdout.flush()
        self.on_spawn(role, child)
        return child

    def _open(self):
        return services.private_pipewire(
            self.logs, self.session, runtime_root=self.runtime if os.name != "nt" else None
        )

    def _assert_stopped(self, name):
        self.assertEqual(self.children[name][0].status, -signal.SIGTERM)

    @staticmethod
    def _cannot_join(child):
        child.terminate.side_effect = None
        child.wait.side_effect = subprocess.TimeoutExpired("fixture", 1)

    def test_interrupted_spawn_retires_admitted_children_but_preserves_unowned_runtime(self):
        original = KeyboardInterrupt()

        def interrupt(role, child):
            if role == "wireplumber":
                raise original  # Spawn may have happened; no child handle was returned.

        self.on_spawn = interrupt
        with self.assertRaises(KeyboardInterrupt) as raised, self._open():
            self.fail("partial startup was admitted")
        self.assertIs(raised.exception, original)
        self._assert_stopped("pipewire")
        unadmitted = self.children["wireplumber"][0]
        unadmitted.terminate.assert_not_called()
        unadmitted.wait.assert_not_called()
        self.assertTrue(self.runtime.is_dir())

    def test_readiness_requires_both_success_and_default_metadata(self):
        self.policy = [
            (1, 'metadata.name = "default"'),
            (0, 'metadata.name = "settings"'),
            (0, 'metadata.name = "default"'),
        ]
        with self._open():
            self.assertEqual(len(self.children["policy"]), 3)

    def test_readiness_deadline_spans_connection_and_metadata_gates(self):
        self.connect = [(255, ""), (255, ""), (0, "")]
        self.policy = [(0, 'metadata.name = "settings"')]
        with (
            mock.patch.object(services, "_READY_TIMEOUT", 0.12),
            self.assertRaises(BazelWorkspaceError),
            self._open(),
        ):
            self.fail("unready policy was admitted")
        # Connection retries already spent 0.10s. A fresh timeout for metadata
        # would let this invocation run well beyond the single startup budget.
        self.assertIn("policy", self.children)
        self.assertGreaterEqual(self.clock, 0.12)
        self.assertLess(self.clock, 0.2)

    def test_hung_connect_probe_is_retired_before_retrying(self):
        self.connect = [(None, ""), (0, "")]
        with mock.patch.object(services, "_PROBE_TIMEOUT", 0.1), self._open():
            first, retry = self.children["connect"]
            self.assertEqual(first.status, -signal.SIGTERM)
            self.assertEqual(retry.status, 0)

    def test_policy_exit_during_consumer_work_fails_completion(self):
        with self.assertRaisesRegex(BazelWorkspaceError, "wireplumber exited 23"), self._open():
            self.children["wireplumber"][0].status = 23
        self._assert_stopped("pipewire")

    def test_interrupted_connect_probe_retires_probe_and_daemon_without_starting_policy(self):
        def interrupt(role, child):
            if role == "connect":
                child.status = None
                original_poll = child.poll.side_effect

                def interrupt_once():
                    child.poll.side_effect = original_poll
                    raise KeyboardInterrupt

                child.poll.side_effect = interrupt_once

        self.on_spawn = interrupt
        with self.assertRaises(KeyboardInterrupt), self._open():
            self.fail("interrupted startup was admitted")
        self._assert_stopped("connect")
        self._assert_stopped("pipewire")
        self.assertNotIn("wireplumber", self.children)

    def test_uncertain_cleanup_fails_successful_body_but_preserves_consumer_exit(self):
        # The first context has a successful body; the second already failed.
        # Use separate logs so a prior run cannot manufacture a setup error.
        for original in (None, SystemExit(3)):
            with self.subTest(original=original):
                self.logs = self.root / str(original)
                self.runtime = self.root / f"runtime-{original}"
                self.children.clear()
                with self.assertRaises(BazelWorkspaceError if original is None else SystemExit) as raised, self._open():
                    self._cannot_join(self.children["wireplumber"][0])
                    if original is not None:
                        raise original
                if original is None:
                    self.assertIn("cleanup uncertain", str(raised.exception))
                else:
                    self.assertIs(raised.exception, original)
                self._assert_stopped("pipewire")
                self.assertTrue(self.runtime.is_dir())

    def test_interrupted_cleanup_preserves_interrupt_and_runtime_but_retires_other_child(self):
        interruption = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt) as raised, self._open():
            self.children["wireplumber"][0].wait.side_effect = interruption
        self.assertIs(raised.exception, interruption)
        self._assert_stopped("pipewire")
        self.assertTrue(self.runtime.is_dir())

    def test_unjoined_policy_probe_preserves_its_runtime_and_stops_services(self):
        self.policy = [(None, "")]
        self.on_spawn = lambda role, child: self._cannot_join(child) if role == "policy" else None
        with (
            mock.patch.object(services, "_PROBE_TIMEOUT", 0.1),
            self.assertRaisesRegex(BazelWorkspaceError, "readiness probe could not exit"),
            self._open(),
        ):
            self.fail("unjoined probe was admitted")
        self._assert_stopped("pipewire")
        self._assert_stopped("wireplumber")
        self.assertTrue(self.runtime.is_dir())


@unittest.skipUnless(sys.platform == "linux", "Private PipeWire process supervision is Linux-only")
class PrivatePipeWireProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-pw-process-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.logs = self.root / "logs"
        self.session = {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/owned-fixture-bus/socket"}
        for name in ("pipewire", "wireplumber", "pw-cli"):
            self._write_tool(name)
        # Only tool lookup is fake. The actual supervisor owns signals, waits and readiness.
        self.enterContext(
            mock.patch.object(services.native, "executable", side_effect=lambda name, *, nix: str(self.root / name))
        )
        self.addCleanup(self._retire_remaining_fixture_children)

    def _write_tool(self, name, exit_status=None):
        tool = self.root / name
        tool.write_text(
            f"#!{sys.executable}\nFIXTURE = {str(self.root)!r}\nEXIT_STATUS = {exit_status!r}\n"
            + textwrap.dedent(
                """\
                import json
                import os
                import signal
                import sys
                import time
                from pathlib import Path

                name = Path(sys.argv[0]).name
                fixture = Path(FIXTURE)
                runtime = Path(os.environ["XDG_RUNTIME_DIR"])
                (fixture / f"{name}-{os.getpid()}.pid").touch()
                if name == "pw-cli":
                    if sys.argv[1:] == ["info", "0"]:
                        (runtime / "connect.request").touch()
                        sys.exit(0 if (runtime / "pipewire.ready").is_file() else 255)
                    if sys.argv[1:] != ["ls", "Metadata"]:
                        sys.exit(91)
                    if not (runtime / "wireplumber.ready").is_file():
                        sys.exit(1)
                    print('metadata.name = "default"', flush=True)
                    sys.exit(0)
                if sys.argv[1:] != ([] if name == "pipewire" else ["--profile=policy"]):
                    sys.exit(92)
                signal.alarm(20)  # Bound any orphan on test failure.
                if EXIT_STATUS is not None:
                    sys.exit(EXIT_STATUS)
                if name == "pipewire":
                    # A deterministic startup race: the daemon won't listen until
                    # `pw-cli info 0` requested a connection at least once.
                    while not (runtime / "connect.request").is_file():
                        time.sleep(0.02)
                elif not (runtime / "pipewire.ready").is_file():
                    sys.exit(5)
                marker = runtime / (name + ".starting")
                marker.write_text(json.dumps(dict(os.environ)))
                marker.replace(runtime / (name + ".ready"))
                signal.pause()
                """
            ),
            encoding="utf-8",
        )
        tool.chmod(0o700)

    def _pids(self, name):
        return [int(path.stem.rsplit("-", 1)[1]) for path in self.root.glob(f"{name}-*.pid")]

    def _retire_remaining_fixture_children(self):
        for name in ("pipewire", "wireplumber", "pw-cli"):
            for pid in self._pids(name):
                try:
                    joined, _ = os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    continue
                if joined == 0:
                    # waitpid established that this is our own direct child.
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)

    def _assert_joined(self, name):
        pids = self._pids(name)
        self.assertTrue(pids, f"{name} did not run")
        for pid in pids:
            with self.assertRaises(ChildProcessError, msg=f"{name} was not reaped"):
                os.waitpid(pid, os.WNOHANG)

    def test_real_children_use_private_configuration_and_are_reaped_on_success(self):
        personal = {
            "HOME": "/personal/home",
            "XDG_CONFIG_HOME": "/personal/config",
            "PIPEWIRE_REMOTE": "/personal/socket",
            "PIPEWIRE_MODULE_DIR": "/personal/modules",
            "SPA_PLUGIN_DIR": "/personal/spa",
            "WIREPLUMBER_CONFIG_DIR": "/personal/wireplumber",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/personal/bus",
        }
        with mock.patch.dict(os.environ, personal), services.private_pipewire(self.logs, self.session) as environment:
            runtime = Path(environment["XDG_RUNTIME_DIR"])
            self.assertEqual(runtime.parent, Path("/tmp"))
            self.assertEqual(runtime.stat().st_mode & 0o777, 0o700)
            self.assertEqual(environment["PIPEWIRE_RUNTIME_DIR"], str(runtime))
            self.assertEqual(environment["AOBUS_REQUIRE_PIPEWIRE"], "1")
            for name in ("pipewire", "wireplumber"):
                (pid,) = self._pids(name)
                self.assertEqual(os.waitpid(pid, os.WNOHANG), (0, 0))
                received = json.loads((runtime / (name + ".ready")).read_text())
                self.assertEqual(received["PIPEWIRE_REMOTE"], "pipewire-0")
                self.assertEqual(received["XDG_CONFIG_HOME"], str(runtime / "config"))
                self.assertEqual(received["XDG_STATE_HOME"], str(runtime / "state"))
                self.assertEqual(received["DBUS_SESSION_BUS_ADDRESS"], self.session["DBUS_SESSION_BUS_ADDRESS"])
                self.assertFalse(set(received.values()) & set(personal.values()))
        self.assertFalse(runtime.exists())
        for name in ("pipewire", "wireplumber", "pw-cli"):
            self._assert_joined(name)

    def test_real_early_daemon_exit_fails_before_wireplumber_starts(self):
        self._write_tool("pipewire", exit_status=27)
        runtime = self.root / "disposable-runtime"
        with (
            self.assertRaisesRegex(BazelWorkspaceError, "pipewire exited 27"),
            services.private_pipewire(self.logs, self.session, runtime_root=runtime),
        ):
            self.fail("dead daemon was admitted")
        self._assert_joined("pipewire")
        self.assertEqual(self._pids("wireplumber"), [])
        if self._pids("pw-cli"):
            self._assert_joined("pw-cli")
        self.assertFalse(runtime.exists())


class PrivateSessionBusTests(unittest.TestCase):
    def setUp(self):
        self.enterContext(
            mock.patch.object(services, "_daemon_ready", side_effect=lambda child, *_: child.stdout.readline().strip())
        )
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))
        self.config = None
        self.bus = mock.Mock(stdout=io.StringIO())
        self.bus.poll.return_value = None
        self.bus.wait.return_value = 0
        self.enterContext(mock.patch.object(services, "_start_session_bus", side_effect=self._start_bus))

    def _start_bus(self, config, daemon):
        self.config = config
        address = ElementTree.parse(config).getroot().findtext("listen")
        self.bus.stdout.write(f"{address},guid=0123456789abcdef\n")
        self.bus.stdout.seek(0)
        return self.bus

    @unittest.skipIf(os.name == "nt", "stable service roots are POSIX-only")
    def test_fixed_socket_ignores_the_daemon_guid_and_retires_its_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "session bus"
            with services.private_session_bus(ipc_root=root) as environment:
                address = f"unix:path={quote(Path(temporary).as_posix(), safe='/')}/session%20bus/bus-0"
                self.assertEqual(environment, {"DBUS_SESSION_BUS_ADDRESS": address, "AOBUS_OWNED_GTK_BUS": address})
            self.assertFalse(root.exists())

    def test_cleanup_failure_does_not_replace_a_body_failure_or_leave_the_config(self):
        original = RuntimeError("body failed")
        with self.assertRaises(RuntimeError) as raised, services.private_session_bus():
            self.bus.terminate.side_effect = OSError("cannot terminate fixture")
            raise original
        self.assertIs(raised.exception, original)
        self.assertTrue(self.bus.stdout.closed)
        self.assertFalse(self.config.exists())


@unittest.skipIf(os.name == "nt", "private service roots are POSIX-only")
class PrivateServiceRootTests(unittest.TestCase):
    def test_recreate_clears_the_previous_attempt(self):
        with tempfile.TemporaryDirectory(prefix="ao-service-root-") as directory:
            root = Path(directory) / "root"
            root.mkdir()
            (root / "stale-socket").touch()
            services.recreate_private_root(root)
            self.assertEqual(list(root.iterdir()), [])
            self.assertEqual(root.stat().st_mode & 0o777, 0o700)

    def test_recreate_refuses_a_symlink_without_removing_its_target(self):
        with tempfile.TemporaryDirectory(prefix="ao-service-root-") as directory:
            target = Path(directory) / "elsewhere"
            target.mkdir()
            link = Path(directory) / "root"
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaises(BazelWorkspaceError):
                services.recreate_private_root(link)
            self.assertTrue(link.is_symlink())
            self.assertTrue(target.is_dir())


if __name__ == "__main__":
    unittest.main()
