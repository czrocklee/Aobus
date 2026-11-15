"""Private GTK display/session-bus isolation and bounded daemon readiness."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ElementTree
from concurrent.futures import Future
from pathlib import Path
from unittest import mock
from urllib.parse import quote

from ao.core import bazel_test_services as services
from ao.core.bazel_native import BazelWorkspaceError


class GtkTestServicesTests(unittest.TestCase):
    def test_a_failed_xvfb_start_shuts_down_the_servers_already_running(self):
        started = []

        def spawn(argv, **kwargs):
            if len(started) == 2:
                raise FileNotFoundError("Xvfb")
            server = mock.Mock(stdout=io.StringIO(f"{40 + len(started)}\n"), wait=mock.Mock(return_value=0))
            started.append(server)
            return server

        with mock.patch.object(services.subprocess, "Popen", side_effect=spawn):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(BazelWorkspaceError):
                with services.virtual_gtk_displays(4):
                    self.fail("the context body must not run")

        self.assertEqual(len(started), 2)
        for server in started:
            server.terminate.assert_called_once()

    @unittest.skipIf(os.name == "nt", "stable service roots are POSIX-only")
    def test_stable_ipc_root_gives_a_fixed_socket_and_guid_free_address(self):
        def spawn(argv, **kwargs):
            if argv[0] == "Xvfb":
                output = "40\n"
            else:
                config = Path(
                    next(arg.removeprefix("--config-file=") for arg in argv if arg.startswith("--config-file="))
                )
                listen = ElementTree.fromstring(config.read_text(encoding="utf-8")).findtext("listen")
                output = f"{listen},guid=0123456789abcdef\n"
            return mock.Mock(stdout=io.StringIO(output), wait=mock.Mock(return_value=0))

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "gtk-bus"
            address = f"unix:path={quote((root / 'bus-0').as_posix(), safe='/')}"
            with mock.patch.object(services.subprocess, "Popen", side_effect=spawn):
                with contextlib.redirect_stdout(io.StringIO()):
                    with services.virtual_gtk_displays(1, ipc_root=root) as environments:
                        self.assertEqual(environments[0]["DBUS_SESSION_BUS_ADDRESS"], address)
                        self.assertEqual(environments[0]["AOBUS_OWNED_GTK_BUS"], address)
            self.assertFalse(root.exists())

    def test_gtk_processes_get_distinct_private_buses_without_host_activation(self):
        started = []
        configs = []

        def spawn(argv, **kwargs):
            if argv[0] == "Xvfb":
                output = f"{40 + len(started)}\n"
            else:
                self.assertEqual(argv[0], "dbus-daemon")
                self.assertIsNone(kwargs["stderr"])
                config = Path(
                    next(arg.removeprefix("--config-file=") for arg in argv if arg.startswith("--config-file="))
                )
                configs.append(config)
                root = ElementTree.fromstring(config.read_text(encoding="utf-8"))
                self.assertEqual([child.tag for child in root], ["type", "listen", "policy"])
                self.assertEqual(root.findtext("type"), "session")
                self.assertRegex(root.findtext("listen") or "", r"^unix:tmpdir=")
                output = f"unix:path=/private/bus-{len(started)}\n"
            server = mock.Mock(stdout=io.StringIO(output), wait=mock.Mock(return_value=0))
            started.append(server)
            return server

        with mock.patch.dict(
            os.environ,
            {
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/host/bus",
                "AOBUS_OWNED_GTK_BUS": "unix:path=/inherited-marker",
                "G_DEBUG": "gc-friendly",
            },
        ):
            with mock.patch.object(services.subprocess, "Popen", side_effect=spawn):
                with contextlib.redirect_stdout(io.StringIO()):
                    with services.virtual_gtk_displays(2) as environments:
                        self.assertEqual([env["DISPLAY"] for env in environments], [":40", ":41"])
                        self.assertEqual(
                            [env["DBUS_SESSION_BUS_ADDRESS"] for env in environments],
                            ["unix:path=/private/bus-2", "unix:path=/private/bus-3"],
                        )
                        self.assertEqual(os.environ["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/host/bus")
                        self.assertTrue(
                            all(env["AOBUS_OWNED_GTK_BUS"] == env["DBUS_SESSION_BUS_ADDRESS"] for env in environments)
                        )
                        self.assertEqual(os.environ["AOBUS_OWNED_GTK_BUS"], "unix:path=/inherited-marker")
                        self.assertTrue(all(env["G_DEBUG"] == "gc-friendly,fatal-warnings" for env in environments))
                        self.assertEqual(os.environ["G_DEBUG"], "gc-friendly")
        self.assertEqual(len(started), 4)
        for server in started:
            server.terminate.assert_called_once()
            server.wait.assert_called_once_with(timeout=5)
            self.assertTrue(server.stdout.closed)
        self.assertTrue(configs)
        self.assertTrue(all(not config.exists() for config in configs))

    @unittest.skipUnless(sys.platform == "linux", "GTK Unix socket path limit")
    def test_gtk_bus_socket_directory_does_not_inherit_a_long_tmpdir(self):
        with tempfile.TemporaryDirectory() as parent:
            inherited = Path(parent) / ("nested-nix-shell-" + "x" * 100)
            inherited.mkdir()
            display = mock.Mock(stdout=io.StringIO("40\n"), wait=mock.Mock(return_value=0))
            bus = mock.Mock(stdout=io.StringIO("unix:path=/private/bus\n"), wait=mock.Mock(return_value=0))

            def start_bus(config):
                self.assertEqual(config.parent.parent, Path("/tmp"))
                root = ElementTree.fromstring(config.read_text(encoding="utf-8"))
                self.assertEqual(root.findtext("listen"), f"unix:tmpdir={config.parent.as_posix()}")
                return bus

            with (
                mock.patch.object(tempfile, "tempdir", str(inherited)),
                mock.patch.dict(os.environ, {"TMPDIR": str(inherited)}),
                mock.patch.object(services, "_start_xvfb", return_value=display),
                mock.patch.object(services, "_start_gtk_bus", side_effect=start_bus),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                with services.virtual_gtk_displays():
                    self.assertEqual(os.environ["TMPDIR"], str(inherited))
                    self.assertEqual(tempfile.gettempdir(), str(inherited))
            bus.terminate.assert_called_once()
            bus.wait.assert_called_once_with(timeout=5)

    def test_failed_private_bus_start_retires_displays_and_prior_buses(self):
        for failure in ("missing-executable", "invalid-address"):
            with self.subTest(failure=failure):
                started = []

                def spawn(argv, *, started=started, failure=failure, **kwargs):
                    if len(started) == 3 and failure == "missing-executable":
                        raise FileNotFoundError("dbus-daemon")
                    output = f"{40 + len(started)}\n" if argv[0] == "Xvfb" else "unix:path=/private/bus\n"
                    if len(started) == 3:
                        output = "failed to bind the session socket\n"
                    server = mock.Mock(stdout=io.StringIO(output), wait=mock.Mock(return_value=0))
                    started.append(server)
                    return server

                with mock.patch.object(services.subprocess, "Popen", side_effect=spawn):
                    with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(BazelWorkspaceError):
                        with services.virtual_gtk_displays(2):
                            self.fail("an incomplete session must not run tests")
                self.assertEqual(len(started), 3 if failure == "missing-executable" else 4)
                for server in started:
                    server.terminate.assert_called_once()
                    server.wait.assert_called_once_with(timeout=5)
                    self.assertTrue(server.stdout.closed)

    def test_interrupted_gtk_readiness_retires_the_writer_before_closing_and_joining(self):
        released = threading.Event()
        finished = threading.Event()

        def read_until_killed():
            released.wait()
            finished.set()
            return ""

        def close():
            killed = released.is_set()
            released.set()
            self.assertTrue(killed, "close must not wait on a still-blocked reader")

        daemon = mock.Mock(
            stdout=mock.Mock(readline=mock.Mock(side_effect=read_until_killed), close=mock.Mock(side_effect=close)),
            kill=mock.Mock(side_effect=released.set),
            wait=mock.Mock(return_value=0),
        )
        with mock.patch.object(Future, "result", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            services._gtk_daemon_ready(daemon, "interrupted daemon")
        daemon.kill.assert_called_once()
        daemon.wait.assert_called_once_with()
        daemon.stdout.close.assert_called_once()
        self.assertTrue(finished.is_set(), "the reader must be joined on interruption")

    def test_gtk_session_body_failure_still_forces_a_stalled_bus_to_exit(self):
        display = mock.Mock(stdout=io.StringIO("40\n"), wait=mock.Mock(return_value=0))
        bus = mock.Mock(
            stdout=io.StringIO("unix:path=/private/bus\n"),
            wait=mock.Mock(side_effect=[subprocess.TimeoutExpired("dbus-daemon", 5), 0]),
        )
        with mock.patch.object(services.subprocess, "Popen", side_effect=[display, bus]):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "body failed"):
                with services.virtual_gtk_displays():
                    raise RuntimeError("body failed")
        display.terminate.assert_called_once()
        bus.terminate.assert_called_once()
        bus.kill.assert_called_once()
        self.assertEqual(bus.wait.call_args_list, [mock.call(timeout=5), mock.call()])
        self.assertTrue(display.stdout.closed)
        self.assertTrue(bus.stdout.closed)

    @unittest.skipIf(os.name == "nt", "GTK daemon supervision is POSIX-only")
    def test_real_daemon_handshake_and_silent_startup_are_bounded(self):
        for ready in (True, False):
            with self.subTest(ready=ready):
                program = "import signal; signal.alarm(10); "
                if ready:
                    program += "print('42', flush=True); "
                program += "signal.pause()"
                child = subprocess.Popen([sys.executable, "-c", program], stdout=subprocess.PIPE, text=True)
                try:
                    if ready:
                        self.assertEqual(services._gtk_daemon_ready(child, "fixture", timeout=2), "42")
                        self.assertIsNone(child.poll())
                    else:
                        with self.assertRaises(BazelWorkspaceError):
                            services._gtk_daemon_ready(child, "silent fixture", timeout=0.1)
                        self.assertIsNotNone(child.returncode)
                        with self.assertRaises(ChildProcessError):
                            os.waitpid(child.pid, os.WNOHANG)
                    self.assertTrue(child.stdout.closed)
                finally:
                    if child.poll() is None:
                        child.kill()
                    child.wait()
                    child.stdout.close()


if __name__ == "__main__":
    unittest.main()
