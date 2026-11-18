"""Private GTK display/session-bus isolation and bounded daemon readiness."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from unittest import mock

from ao.core.bazel_native import BazelWorkspaceError
from ao.graph import bazel_test_services as services


class GtkTestServicesTests(unittest.TestCase):
    def setUp(self):
        self.enterContext(
            mock.patch.object(
                services, "_daemon_ready", side_effect=lambda server, *_: server.stdout.readline().strip()
            )
        )
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def _daemon(self, output):
        daemon = mock.Mock(stdout=io.StringIO(output + "\n"), status=None)
        daemon.poll.side_effect = lambda: daemon.status
        daemon.terminate.side_effect = lambda: setattr(daemon, "status", -15)
        daemon.wait.side_effect = lambda *, timeout: daemon.status
        return daemon

    def _assert_stopped(self, *servers):
        for server in servers:
            self.assertEqual(server.status, -15)
            self.assertTrue(server.stdout.closed)

    def test_failed_second_xvfb_start_retires_first_display(self):
        first = self._daemon("40")
        with (
            mock.patch.object(services.subprocess, "Popen", side_effect=[first, FileNotFoundError("Xvfb")]),
            self.assertRaises(BazelWorkspaceError),
            services.virtual_gtk_displays(2),
        ):
            self.fail("partial display startup was admitted")
        self._assert_stopped(first)

    def test_sessions_keep_distinct_displays_and_buses_without_host_activation(self):
        def start_bus(config, daemon):
            root = ElementTree.parse(config).getroot()
            # No desktop service activation directories or includes from the host.
            self.assertEqual({child.tag for child in root}, {"type", "listen", "policy"})
            return self._daemon(f"unix:path=/private/{config.stem}")

        host = {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/host/bus", "G_DEBUG": "gc-friendly"}
        with (
            mock.patch.dict(os.environ, host),
            mock.patch.object(services, "_start_xvfb", side_effect=[self._daemon("40"), self._daemon("41")]),
            mock.patch.object(services, "_start_session_bus", side_effect=start_bus),
            services.virtual_gtk_displays(2) as environments,
        ):
            self.assertEqual([env["DISPLAY"] for env in environments], [":40", ":41"])
            buses = [env["DBUS_SESSION_BUS_ADDRESS"] for env in environments]
            self.assertEqual(buses, ["unix:path=/private/session-0", "unix:path=/private/session-1"])
            self.assertEqual([env["AOBUS_OWNED_GTK_BUS"] for env in environments], buses)
            self.assertTrue(all(env["G_DEBUG"] == "gc-friendly,fatal-warnings" for env in environments))
            self.assertEqual({key: os.environ[key] for key in host}, host)

    @unittest.skipUnless(sys.platform == "linux", "GTK Unix socket path limit")
    def test_gtk_bus_socket_directory_does_not_inherit_a_long_tmpdir(self):
        with tempfile.TemporaryDirectory() as parent:
            inherited = Path(parent) / ("nested-nix-shell-" + "x" * 100)
            inherited.mkdir()

            def start_bus(config, daemon):
                listen = ElementTree.parse(config).getroot().findtext("listen")
                self.assertEqual(listen, f"unix:tmpdir={config.parent.as_posix()}")
                self.assertEqual(config.parent.parent, Path("/tmp"))
                return self._daemon("unix:path=/private/bus")

            with (
                mock.patch.object(tempfile, "tempdir", str(inherited)),
                mock.patch.dict(os.environ, {"TMPDIR": str(inherited)}),
                mock.patch.object(services, "_start_xvfb", return_value=self._daemon("40")),
                mock.patch.object(services, "_start_session_bus", side_effect=start_bus),
                services.virtual_gtk_displays(),
            ):
                self.assertEqual(os.environ["TMPDIR"], str(inherited))

    def test_failed_second_bus_handshake_retires_displays_and_both_buses(self):
        daemons = [self._daemon(output) for output in ("40", "41", "unix:path=/private/bus", "failed to bind")]
        with (
            mock.patch.object(services.subprocess, "Popen", side_effect=daemons),
            self.assertRaises(BazelWorkspaceError),
            services.virtual_gtk_displays(2),
        ):
            self.fail("partial bus startup was admitted")
        self._assert_stopped(*daemons)


@unittest.skipIf(os.name == "nt", "GTK daemon supervision is POSIX-only")
class GtkDaemonProcessTests(unittest.TestCase):
    def _daemon(self, output):
        program = f"import signal; signal.alarm(5); print({output!r}, end='', flush=True); signal.pause()"
        child = subprocess.Popen([sys.executable, "-c", program], stdout=subprocess.PIPE)

        def cleanup():
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
            child.stdout.close()

        self.addCleanup(cleanup)
        return child

    def _assert_retired(self, child):
        self.assertTrue(child.stdout.closed)
        with self.assertRaises(ChildProcessError):
            os.waitpid(child.pid, os.WNOHANG)

    def test_real_daemon_handshakes_supply_display_and_address(self):
        display = self._daemon("42\n")
        bus = self._daemon("unix:path=/private/bus\n")
        with (
            mock.patch.object(services, "_start_xvfb", return_value=display),
            mock.patch.object(services, "_start_session_bus", return_value=bus),
            services.virtual_gtk_displays() as environments,
        ):
            self.assertEqual(environments[0]["DISPLAY"], ":42")
            self.assertEqual(environments[0]["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/private/bus")
            self.assertTrue(display.stdout.closed)
            self.assertTrue(bus.stdout.closed)
            self.assertIsNone(display.poll())
            self.assertIsNone(bus.poll())
        self._assert_retired(display)
        self._assert_retired(bus)

    def test_incomplete_handshake_times_out_and_reaps_the_display_daemon(self):
        display = self._daemon("42")  # A valid display number without the terminating newline.
        with (
            mock.patch.object(services, "_start_xvfb", return_value=display),
            mock.patch.object(services, "_start_session_bus") as bus,
            mock.patch.object(services, "_READY_TIMEOUT", 0.1),
            self.assertRaisesRegex(BazelWorkspaceError, "Xvfb did not report readiness"),
        ):
            with services.virtual_gtk_displays():
                self.fail("tests cannot run while the display handshake is incomplete")
        bus.assert_not_called()
        self._assert_retired(display)

    def test_cancellation_during_readiness_keeps_the_original_interrupt_and_reaps_the_daemon(self):
        display = self._daemon("")
        original = KeyboardInterrupt()
        with mock.patch.object(services, "_start_xvfb", return_value=display) as start:

            def check_cancelled():
                if start.called:
                    raise original

            with self.assertRaises(KeyboardInterrupt) as raised:
                with services.virtual_gtk_displays(check_cancelled=check_cancelled):
                    self.fail("tests cannot run after startup has been cancelled")
        self.assertIs(raised.exception, original)
        self._assert_retired(display)


if __name__ == "__main__":
    unittest.main()
