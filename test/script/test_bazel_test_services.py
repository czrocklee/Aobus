"""Private PipeWire mock lifecycles and Linux-owned subprocess regressions."""

import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_test_services as services
from ao.core.bazel_native import BazelWorkspaceError


class _Child:
    def __init__(self, pid, status=None, name=None, role=None):
        self.pid = pid
        self.status = status
        self.name = name
        self.role = role
        self.terminated = False
        self.waits = []
        self.wait_error = None
        self.poll_error = None

    def poll(self):
        if self.poll_error is not None:
            error, self.poll_error = self.poll_error, None
            raise error
        return self.status

    def terminate(self):
        self.terminated = True
        if self.wait_error is None:
            self.status = -15

    def wait(self, timeout):
        self.waits.append(timeout)
        if self.wait_error is not None:
            raise self.wait_error
        return self.status


class PrivatePipeWireTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-pw-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.logs = self.root / "logs"
        self.session = {
            "DISPLAY": ":42",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/owned-bus/socket",
            "AOBUS_OWNED_GTK_BUS": "unix:path=/tmp/owned-bus/socket",
            "AOBUS_OWNED_GTK_DISPLAY": "1",
        }
        self.clock = 0.0
        self.calls = []
        self.children = []
        # Connect probes (`pw-cli info 0`) succeed on exit status alone; the
        # default empty stdout also pins that no core-info schema is validated.
        self.connect_results = [(0, "")]
        self.metadata_results = [(0, 'metadata.name = "default"')]
        self.spawn_failure = None
        self.child_setup = lambda name, child: None
        mkdtemp = tempfile.mkdtemp
        self.mkdtemp = self.enterContext(
            mock.patch.object(
                services.tempfile, "mkdtemp", side_effect=lambda **kw: mkdtemp(prefix=kw["prefix"], dir=self.root)
            )
        )
        self.enterContext(mock.patch.object(services.native, "native_platform", return_value="linux"))
        self.executable = self.enterContext(
            mock.patch.object(
                services.native, "executable", side_effect=lambda name, **kw: f"/nix/store/fixture/bin/{name}"
            )
        )
        self.popen = self.enterContext(mock.patch.object(services.subprocess, "Popen", side_effect=self._spawn))
        self.enterContext(mock.patch.object(services.time, "monotonic", side_effect=lambda: self.clock))
        self.enterContext(mock.patch.object(services.time, "sleep", side_effect=self._advance))

    def _advance(self, delay):
        self.clock += delay

    def _spawn(self, argv, **kwargs):
        name = Path(argv[0]).name
        role = None
        if name == "pw-cli":
            # The fake discriminates the supervised daemon-connection probe
            # from the default-metadata policy probe by its own argv.
            self.assertIn(tuple(argv[1:]), (("info", "0"), ("ls", "Metadata")))
            role = "connect" if tuple(argv[1:]) == ("info", "0") else "policy"
            expected = ["/nix/store/fixture/bin/pw-cli", *argv[1:]]
        else:
            expected = {
                "pipewire": ["/nix/store/fixture/bin/pipewire"],
                "wireplumber": ["/nix/store/fixture/bin/wireplumber", "--profile=policy"],
            }
            self.assertIn(name, expected)
            expected = expected[name]
        self.assertEqual(argv, expected)
        self.assertEqual(kwargs["env"]["DBUS_SESSION_BUS_ADDRESS"], self.session["AOBUS_OWNED_GTK_BUS"])
        self.assertNotIn("start_new_session", kwargs)
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.calls.append((argv, kwargs))
        if name == self.spawn_failure:
            raise OSError(f"cannot start {name}")
        if role is not None:
            results = self.connect_results if role == "connect" else self.metadata_results
            status, output = results[0]
            if len(results) > 1:
                results.pop(0)
        else:
            status, output = None, f"{name} startup diagnostic"
        child = _Child(1000 + len(self.children), status, name=name, role=role)
        self.children.append(child)
        kwargs["stdout"].write(output.encode())
        kwargs["stdout"].flush()
        kwargs["stderr"].write(f"{name} stderr diagnostic".encode())
        kwargs["stderr"].flush()
        self.child_setup(name, child)
        return child

    def _service_child(self, name):
        matches = [child for child in self.children if child.name == name]
        self.assertEqual(len(matches), 1, name)
        return matches[0]

    def _probe_children(self, role):
        return [child for child in self.children if child.role == role]

    def _process_record(self, receipt, name):
        matches = [record for record in receipt["processes"] if record["name"] == name]
        self.assertEqual(len(matches), 1, name)
        return matches[0]

    def _receipt(self):
        return json.loads((self.logs / "pipewire-lifecycle.json").read_text())

    def _assert_retired(self):
        self.assertTrue(all(child.status is not None for child in self.children))
        self.assertFalse(Path(self._receipt()["runtime"]).exists())
        self.assertEqual(self._receipt()["cleanup"], "complete")

    def test_ready_policy_freezes_private_environment_and_retains_diagnostics(self):
        personal = {
            "HOME": "/personal/home",
            "TMPDIR": "/personal/tmp",
            "XDG_RUNTIME_DIR": "/personal/runtime",
            "XDG_CONFIG_HOME": "/personal/config",
            "XDG_CONFIG_DIRS": "/personal/config-dirs",
            "PIPEWIRE_RUNTIME_DIR": "/personal/runtime",
            "PIPEWIRE_REMOTE": "/personal/socket",
            "PIPEWIRE_CONFIG_DIR": "/personal/pipewire",
            "PIPEWIRE_MODULE_DIR": "/personal/modules",
            "SPA_PLUGIN_DIR": "/personal/spa",
            "WIREPLUMBER_CONFIG_DIR": "/personal/wireplumber",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/personal/bus",
            "PRIVATE_UNRELATED_VALUE": "not-for-children",
        }
        original_session = dict(self.session)
        with mock.patch.dict(os.environ, personal):
            with services.private_pipewire(self.logs, self.session) as environment:
                runtime = Path(environment["XDG_RUNTIME_DIR"])
                self.assertEqual(environment["PIPEWIRE_RUNTIME_DIR"], str(runtime))
                self.assertEqual(environment["PIPEWIRE_REMOTE"], "pipewire-0")
                self.assertEqual(environment["AOBUS_REQUIRE_PIPEWIRE"], "1")
                self.assertEqual(environment["DISPLAY"], ":42")
                self.assertEqual(environment["TMPDIR"], str(runtime / "tmp"))
                if os.name != "nt":
                    self.assertEqual(runtime.stat().st_mode & 0o777, 0o700)
                    self.assertEqual((runtime / "tmp").stat().st_mode & 0o777, 0o700)
                self.assertEqual(self._receipt()["status"], "ready")
                self.assertEqual(self.session, original_session)
                self.session["DISPLAY"] = ":99"
                environment["PIPEWIRE_REMOTE"] = "changed-after-yield"
                for _, kwargs in self.calls:
                    child_env = kwargs["env"]
                    self.assertEqual(child_env["PIPEWIRE_REMOTE"], "pipewire-0")
                    self.assertEqual(child_env["XDG_CONFIG_HOME"], str(runtime / "config"))
                    self.assertEqual(child_env["XDG_STATE_HOME"], str(runtime / "state"))
                    self.assertFalse(any(value.startswith("/personal/") for value in child_env.values()))
                    self.assertNotIn("PRIVATE_UNRELATED_VALUE", child_env)
                    self.assertNotIn("PIPEWIRE_CONFIG_DIR", child_env)
                    self.assertNotIn("PIPEWIRE_MODULE_DIR", child_env)
                    self.assertNotIn("SPA_PLUGIN_DIR", child_env)
                    self.assertNotIn("WIREPLUMBER_CONFIG_DIR", child_env)
                self.assertEqual(os.environ["PIPEWIRE_REMOTE"], "/personal/socket")
        self.assertEqual(self._receipt()["status"], "passed")
        self._assert_retired()
        self.assertEqual(
            self.executable.call_args_list,
            [mock.call(name, nix=True) for name in ("pipewire", "wireplumber", "pw-cli")],
        )
        self.mkdtemp.assert_called_once_with(prefix="aobus-pipewire-", dir="/tmp")
        for name in ("pipewire", "wireplumber"):
            self.assertEqual((self.logs / f"{name}.stdout.log").read_text(), f"{name} startup diagnostic")
            self.assertEqual((self.logs / f"{name}.stderr.log").read_text(), f"{name} stderr diagnostic")
        self.assertEqual((self.logs / "pipewire-connect-probe-1.stdout.log").read_text(), "")
        self.assertEqual((self.logs / "pipewire-connect-probe-1.stderr.log").read_text(), "pw-cli stderr diagnostic")
        self.assertEqual((self.logs / "pipewire-probe-1.stdout.log").read_text(), 'metadata.name = "default"')
        self.assertEqual((self.logs / "pipewire-probe-1.stderr.log").read_text(), "pw-cli stderr diagnostic")
        self.assertEqual(self._service_child("pipewire").waits, [services._STOP_TIMEOUT])
        self.assertEqual(self._service_child("wireplumber").waits, [services._STOP_TIMEOUT])

    @unittest.skipIf(os.name == "nt", "stable service roots are POSIX-only")
    def test_stable_runtime_root_replaces_the_previous_attempt_without_mkdtemp(self):
        root = self.root / "stable-pipewire"
        root.mkdir()
        (root / "pipewire-0").write_text("previous attempt", encoding="utf-8")
        with services.private_pipewire(self.logs, self.session, runtime_root=root) as environment:
            self.assertEqual(environment["XDG_RUNTIME_DIR"], str(root))
            self.assertFalse((root / "pipewire-0").exists())
            self.assertEqual(root.stat().st_mode & 0o777, 0o700)
        self.mkdtemp.assert_not_called()
        self._assert_retired()

    def test_failed_spawn_conservatively_preserves_runtime_and_retires_admitted_daemon(self):
        self.spawn_failure = "wireplumber"
        with self.assertRaisesRegex(OSError, "cannot start wireplumber"):
            with services.private_pipewire(self.logs, self.session):
                self.fail("partial startup admitted")
        self.assertEqual(len(self.children), 2)
        self.assertTrue(self._service_child("pipewire").terminated)
        self.assertEqual(self._service_child("pipewire").waits, [services._STOP_TIMEOUT])
        receipt = self._receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["cleanup"], "uncertain")
        self.assertTrue(Path(receipt["runtime"]).exists())
        self.assertTrue(receipt["runtime_preserved"])
        wireplumber = self._process_record(receipt, "wireplumber")
        self.assertEqual(wireplumber["admission"], "uncertain")
        self.assertNotIn("pid", wireplumber)
        self.assertIsNone(wireplumber["exit"])

    def test_popen_keyboard_interrupt_preserves_130_and_unadmitted_runtime(self):
        # A KeyboardInterrupt raised inside the fake Popen leaves the record at
        # admission "pending"; production sweeps it to "uncertain" in the
        # finally block without signalling the unregistered handle.
        original = KeyboardInterrupt()

        def interrupt(name, child):
            if name == "wireplumber":
                raise original

        self.child_setup = interrupt
        with self.assertRaises(KeyboardInterrupt) as raised:
            with services.private_pipewire(self.logs, self.session):
                self.fail("unadmitted policy process accepted")
        self.assertIs(raised.exception, original)
        self.assertEqual(len(self.children), 3)
        self.assertTrue(self._service_child("pipewire").terminated)
        self.assertEqual(self._service_child("pipewire").waits, [services._STOP_TIMEOUT])
        # The fake spawned this child, but Popen/admission never published its
        # handle to the supervisor. It must not be signalled or guessed/reaped.
        unadmitted = self.children[-1]
        self.assertEqual(unadmitted.name, "wireplumber")
        self.assertFalse(unadmitted.terminated)
        self.assertEqual(unadmitted.waits, [])
        self.assertIsNone(unadmitted.status)
        receipt = self._receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["exit"], 130)
        self.assertEqual(receipt["cleanup"], "uncertain")
        self.assertTrue(receipt["runtime_preserved"])
        self.assertTrue(Path(receipt["runtime"]).exists())
        daemon = self._process_record(receipt, "pipewire")
        self.assertEqual(daemon["admission"], "admitted")
        self.assertEqual(daemon["status"], "exited")
        policy = self._process_record(receipt, "wireplumber")
        self.assertEqual(policy["status"], "admission-uncertain")
        self.assertEqual(policy["admission"], "uncertain")
        self.assertTrue(policy["cleanup_uncertain"])
        self.assertNotIn("pid", policy)
        self.assertIsNone(policy["exit"])
        self.assertTrue(any("cleanup uncertain" in note for note in original.__notes__))

    def test_readiness_requires_successful_default_metadata_not_just_a_socket(self):
        # Native observation: right after the daemon admits connections, the
        # only metadata object is "settings"; "default" appears with the
        # policy service, so the gate must keep validating the metadata name.
        self.metadata_results = [
            (1, 'metadata.name = "default"'),
            (0, 'metadata.name = "settings"'),
            (0, 'metadata.name = "default"'),
        ]
        with services.private_pipewire(self.logs, self.session):
            self.assertEqual(self._receipt()["readiness_attempts"], 3)
        self._assert_retired()
        self.assertEqual(len(self._probe_children("connect")), 1)
        self.assertEqual(len(self._probe_children("policy")), 3)
        self.assertEqual(len(self.calls), 6)

    def test_wireplumber_starts_only_after_a_successful_connect_probe(self):
        # Native observation: `pw-cli info 0` exits 255 while the daemon is not
        # accepting connections yet. WirePlumber must wait for a probe that
        # succeeds, not for the daemon process merely to exist.
        self.connect_results = [(255, ""), (0, "fixture core connected")]
        with services.private_pipewire(self.logs, self.session):
            pass
        self._assert_retired()
        order = [(Path(argv[0]).name, tuple(argv[1:])) for argv, _ in self.calls]
        self.assertEqual(
            order,
            [
                ("pipewire", ()),
                ("pw-cli", ("info", "0")),
                ("pw-cli", ("info", "0")),
                ("wireplumber", ("--profile=policy",)),
                ("pw-cli", ("ls", "Metadata")),
            ],
        )

    def test_early_daemon_exit_fails_before_wireplumber_or_any_probe_starts(self):
        self.child_setup = lambda name, child: setattr(child, "status", 17) if name == "pipewire" else None
        with self.assertRaisesRegex(BazelWorkspaceError, "pipewire exited 17"):
            with services.private_pipewire(self.logs, self.session):
                self.fail("dead daemon admitted")
        self.assertEqual(self._receipt()["processes"][0]["exit"], 17)
        self.assertEqual([Path(argv[0]).name for argv, _ in self.calls], ["pipewire"])
        self.assertNotIn("wireplumber", [record["name"] for record in self._receipt()["processes"]])
        self._assert_retired()

    def test_readiness_deadline_spans_connection_and_metadata_gates(self):
        self.connect_results = [(255, ""), (255, ""), (0, "")]
        self.metadata_results = [(0, "no default metadata")]
        with mock.patch.object(services, "_READY_TIMEOUT", 0.12):
            with self.assertRaisesRegex(BazelWorkspaceError, "not ready within"):
                with services.private_pipewire(self.logs, self.session):
                    self.fail("unready policy admitted")
        self.assertGreaterEqual(self.clock, 0.12)
        # One deadline created before the first service start covers both
        # phases: the daemon-connection retries above already spent 0.10s of
        # the 0.12s budget, so a fresh per-phase deadline would run to about
        # 0.22s and past this bound.
        self.assertLess(self.clock, 0.2)
        self.assertEqual(len(self._probe_children("connect")), 3)
        self._service_child("wireplumber")
        self.assertTrue(self._probe_children("policy"))
        self.assertEqual((self.logs / "pipewire-connect-probe-1.stdout.log").read_text(), "")
        self.assertTrue((self.logs / "pipewire-probe-1.stdout.log").is_file())
        self._assert_retired()

    def test_hung_connect_probe_is_bounded_and_readiness_counts_only_policy_probes(self):
        self.connect_results = [(None, "waiting"), (0, "")]
        with mock.patch.object(services, "_PROBE_TIMEOUT", 0.1):
            with services.private_pipewire(self.logs, self.session):
                self.assertTrue(self._probe_children("connect")[0].terminated)
                receipt = self._receipt()
                self.assertEqual(receipt["readiness_attempts"], 1)
                self.assertTrue(self._process_record(receipt, "pipewire-connect-probe-1")["timed_out"])
        self._assert_retired()

    def test_policy_exit_during_consumer_work_fails_completion(self):
        with self.assertRaisesRegex(BazelWorkspaceError, "wireplumber exited 23"):
            with services.private_pipewire(self.logs, self.session):
                self._service_child("wireplumber").status = 23
        self._assert_retired()

    def test_interrupted_connect_probe_retires_daemon_and_probe_without_wireplumber(self):
        def interrupt(name, child):
            if name == "pw-cli":
                child.status = None
                child.poll_error = KeyboardInterrupt()

        self.child_setup = interrupt
        with self.assertRaises(KeyboardInterrupt):
            with services.private_pipewire(self.logs, self.session):
                self.fail("interrupted probe admitted")
        self.assertTrue(all(child.terminated for child in self.children))
        self.assertEqual([Path(argv[0]).name for argv, _ in self.calls], ["pipewire", "pw-cli"])
        self._assert_retired()

    def test_uncertain_cleanup_preserves_runtime_and_fails_successful_body(self):
        with self.assertRaisesRegex(BazelWorkspaceError, "cleanup uncertain"):
            with services.private_pipewire(self.logs, self.session):
                self._service_child("wireplumber").wait_error = subprocess.TimeoutExpired("wireplumber", 5)
        receipt = self._receipt()
        self.assertEqual(receipt["cleanup"], "uncertain")
        self.assertEqual(receipt["status"], "failed")
        self.assertTrue(receipt["runtime_preserved"])
        self.assertTrue(Path(receipt["runtime"]).exists())
        self.assertTrue(self._service_child("pipewire").terminated)
        self.assertEqual(self._service_child("wireplumber").waits, [services._STOP_TIMEOUT])
        self.assertTrue(self._process_record(receipt, "wireplumber")["cleanup_uncertain"])

    def test_interrupted_cleanup_preserves_interrupt_and_runtime_but_retires_other_child(self):
        interruption = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt) as raised:
            with services.private_pipewire(self.logs, self.session):
                self._service_child("wireplumber").wait_error = interruption
        self.assertIs(raised.exception, interruption)
        self.assertTrue(self._service_child("pipewire").terminated)
        self.assertTrue(self._receipt()["runtime_preserved"])
        self.assertEqual(self._receipt()["cleanup"], "uncertain")

    def test_unjoined_policy_probe_preserves_its_runtime_and_stops_services(self):
        def hang_probe(name, child):
            if child.role == "policy":
                child.wait_error = subprocess.TimeoutExpired("pw-cli", 5)

        self.child_setup = hang_probe
        self.metadata_results = [(None, "waiting")]
        with mock.patch.object(services, "_PROBE_TIMEOUT", 0.1):
            with self.assertRaisesRegex(BazelWorkspaceError, "readiness probe could not exit"):
                with services.private_pipewire(self.logs, self.session):
                    self.fail("unjoined probe admitted")
        self.assertTrue(self._receipt()["runtime_preserved"])
        self.assertTrue(self._service_child("pipewire").terminated)
        self.assertTrue(self._service_child("wireplumber").terminated)
        self.assertEqual(self._probe_children("policy")[0].waits, [services._STOP_TIMEOUT])

    def test_uncertain_cleanup_does_not_replace_original_consumer_exit(self):
        original = SystemExit(3)
        with self.assertRaises(SystemExit) as raised:
            with services.private_pipewire(self.logs, self.session):
                self._service_child("wireplumber").wait_error = subprocess.TimeoutExpired("wireplumber", 5)
                raise original
        self.assertIs(raised.exception, original)
        self.assertEqual(raised.exception.code, 3)
        self.assertTrue(any("cleanup uncertain" in note for note in original.__notes__))
        receipt = self._receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["exit"], 3)
        self.assertTrue(receipt["runtime_preserved"])
        self.assertTrue(self._service_child("pipewire").terminated)


@unittest.skipUnless(sys.platform == "linux", "Private PipeWire process supervision is Linux-only")
class PrivatePipeWireProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-pw-process-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.logs = self.root / "logs"
        self.session = {
            "DISPLAY": ":42",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/owned-fixture-bus/socket",
            "AOBUS_OWNED_GTK_BUS": "unix:path=/tmp/owned-fixture-bus/socket",
        }
        self.tools = {}
        for name in ("pipewire", "wireplumber", "pw-cli"):
            self.tools[name] = self.root / name
            self._write_tool(name)
        # SDK selection alone is replaced. Popen, clocks, signals, waits, logs,
        # runtime ownership, and the production readiness loop remain real.
        self.lookup = self.enterContext(
            mock.patch.object(services.native, "executable", side_effect=lambda name, *, nix: str(self.tools[name]))
        )
        self.addCleanup(self._retire_remaining_fixture_children)

    def _write_tool(self, name, exit_status=None):
        self.tools[name].write_text(
            f"#!{sys.executable}\nEXIT_STATUS = {exit_status!r}\n"
            + textwrap.dedent(
                """\
                import json
                import os
                import signal
                import sys
                import time
                from pathlib import Path

                name = Path(sys.argv[0]).name
                runtime = Path(os.environ["XDG_RUNTIME_DIR"])
                if name == "pw-cli":
                    if sys.argv[1:] == ["info", "0"]:
                        # The supervised connection probe announces its request,
                        # then succeeds only once the daemon admitted connections.
                        (runtime / "connect.request").write_text("fixture core info requested")
                        if not (runtime / "pipewire.ready").is_file():
                            print("fixture core not accepting connections yet", file=sys.stderr, flush=True)
                            sys.exit(255)
                        print("fixture core connected", flush=True)
                        print("fixture core observed", file=sys.stderr, flush=True)
                        sys.exit(0)
                    if sys.argv[1:] != ["ls", "Metadata"]:
                        sys.exit(91)
                    if not all((runtime / (service + ".ready")).is_file()
                               for service in ("pipewire", "wireplumber")):
                        print("fixture services not ready", file=sys.stderr, flush=True)
                        sys.exit(1)
                    print('metadata.name = "default"', flush=True)
                    print("fixture metadata observed", file=sys.stderr, flush=True)
                    sys.exit(0)
                expected_args = [] if name == "pipewire" else ["--profile=policy"]
                if sys.argv[1:] != expected_args:
                    sys.exit(92)
                def stop(signum, frame):
                    print(name + " stopped", flush=True)
                    sys.exit(0)
                signal.signal(signal.SIGTERM, stop)
                # A broken supervisor must not leave even this harmless fixture
                # running indefinitely. Normal retirement uses SIGTERM, not alarm.
                signal.alarm(20)
                print(name + " started", flush=True)
                print(name + " diagnostic", file=sys.stderr, flush=True)
                if EXIT_STATUS is not None:
                    print("startup refused " + str(EXIT_STATUS), file=sys.stderr, flush=True)
                    sys.exit(EXIT_STATUS)
                if name == "pipewire":
                    # Deterministic startup race: the daemon admits connections
                    # only after a supervised `pw-cli info 0` probe asked for them.
                    while not (runtime / "connect.request").is_file():
                        time.sleep(0.02)
                elif not (runtime / "pipewire.ready").is_file():
                    # Starting WirePlumber before the daemon accepts connections
                    # must fail deterministically instead of racing a socket.
                    print("wireplumber started before pipewire accepted connections",
                          file=sys.stderr, flush=True)
                    sys.exit(5)
                marker = runtime / (name + ".starting")
                marker.write_text(json.dumps({
                    "pid": os.getpid(), "parent": os.getppid(),
                    "runtime": str(runtime),
                    "config": os.environ["XDG_CONFIG_HOME"],
                    "state": os.environ["XDG_STATE_HOME"],
                    "bus": os.environ["DBUS_SESSION_BUS_ADDRESS"],
                }))
                marker.replace(runtime / (name + ".ready"))
                signal.pause()
                """
            ),
            encoding="utf-8",
        )
        self.tools[name].chmod(0o700)

    def _receipt(self):
        return json.loads((self.logs / "pipewire-lifecycle.json").read_text())

    def _retire_remaining_fixture_children(self):
        receipt_path = self.logs / "pipewire-lifecycle.json"
        if not receipt_path.is_file():
            return
        for record in self._receipt()["processes"]:
            if "pid" not in record:
                continue
            pid = record["pid"]
            try:
                joined, _ = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                continue
            if joined == 0:
                # waitpid established this is still our own direct child. This
                # fallback runs only if the production supervisor regresses.
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, 0)

    def _assert_all_joined_and_runtime_removed(self):
        receipt = self._receipt()
        self.assertEqual(receipt["cleanup"], "complete")
        self.assertFalse(receipt["runtime_preserved"])
        self.assertFalse(Path(receipt["runtime"]).exists())
        for record in receipt["processes"]:
            self.assertEqual(record["status"], "exited")
            self.assertEqual(record["admission"], "admitted")
            self.assertIsInstance(record["exit"], int)
            # A live or zombie child would return a PID/status here. ECHILD
            # proves the supervisor already reaped every recorded direct child.
            with self.assertRaises(ChildProcessError, msg=record["name"]):
                os.waitpid(record["pid"], os.WNOHANG)
        self.assertEqual(
            self.lookup.call_args_list, [mock.call(name, nix=True) for name in ("pipewire", "wireplumber", "pw-cli")]
        )

    def test_real_foreground_children_are_ready_logged_and_joined_on_success(self):
        with services.private_pipewire(self.logs, self.session) as environment:
            runtime = Path(environment["XDG_RUNTIME_DIR"])
            receipt = self._receipt()
            self.assertEqual(receipt["status"], "ready")
            names = [record["name"] for record in receipt["processes"]]
            connect_at = [index for index, name in enumerate(names) if name.startswith("pipewire-connect-probe-")]
            policy_at = [index for index, name in enumerate(names) if name.startswith("pipewire-probe-")]
            self.assertTrue(connect_at)
            self.assertTrue(policy_at)
            wireplumber_at = names.index("wireplumber")
            self.assertEqual(names[0], "pipewire")
            # WirePlumber starts only after a supervised connect probe
            # succeeded, and the default-metadata gate runs only after it.
            self.assertTrue(all(index < wireplumber_at for index in connect_at))
            self.assertTrue(all(index > wireplumber_at for index in policy_at))
            self.assertEqual(receipt["processes"][connect_at[-1]]["exit"], 0)
            for refused in connect_at[:-1]:
                self.assertEqual(receipt["processes"][refused]["exit"], 255)
            self.assertEqual(receipt["readiness_attempts"], len(policy_at))
            for name in ("pipewire", "wireplumber"):
                record = next(record for record in receipt["processes"] if record["name"] == name)
                self.assertEqual(os.waitpid(record["pid"], os.WNOHANG), (0, 0))
                state = json.loads((runtime / (name + ".ready")).read_text())
                self.assertEqual(state["pid"], record["pid"])
                self.assertEqual(state["parent"], os.getpid())
                self.assertEqual(state["runtime"], str(runtime))
                self.assertEqual(state["config"], str(runtime / "config"))
                self.assertEqual(state["state"], str(runtime / "state"))
                self.assertEqual(state["bus"], self.session["AOBUS_OWNED_GTK_BUS"])
        self._assert_all_joined_and_runtime_removed()
        self.assertEqual(self._receipt()["status"], "passed")
        for name in ("pipewire", "wireplumber"):
            self.assertEqual((self.logs / f"{name}.stdout.log").read_text(), f"{name} started\n{name} stopped\n")
            self.assertEqual((self.logs / f"{name}.stderr.log").read_text(), f"{name} diagnostic\n")
            record = next(record for record in self._receipt()["processes"] if record["name"] == name)
            self.assertEqual(record["exit"], 0)
        receipt = self._receipt()
        connect = next(
            record for record in reversed(receipt["processes"]) if record["name"].startswith("pipewire-connect-probe-")
        )
        self.assertEqual(connect["exit"], 0)
        self.assertEqual(Path(connect["stdout"]).read_text(), "fixture core connected\n")
        self.assertEqual(Path(connect["stderr"]).read_text(), "fixture core observed\n")
        policy = [record for record in receipt["processes"] if record["name"].startswith("pipewire-probe-")][-1]
        self.assertEqual(policy["exit"], 0)
        self.assertEqual(Path(policy["stdout"]).read_text(), 'metadata.name = "default"\n')
        self.assertEqual(Path(policy["stderr"]).read_text(), "fixture metadata observed\n")

    def test_real_early_daemon_exit_fails_before_wireplumber_starts(self):
        self._write_tool("pipewire", exit_status=27)
        with self.assertRaisesRegex(BazelWorkspaceError, "pipewire exited 27"):
            with services.private_pipewire(self.logs, self.session):
                self.fail("exited fixture service admitted")
        self._assert_all_joined_and_runtime_removed()
        receipt = self._receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertIn("pipewire exited 27", receipt["error"])
        self.assertEqual(receipt["processes"][0]["name"], "pipewire")
        self.assertEqual(receipt["processes"][0]["exit"], 27)
        self.assertNotIn("wireplumber", [record["name"] for record in receipt["processes"]])
        self.assertEqual((self.logs / "pipewire.stdout.log").read_text(), "pipewire started\n")
        self.assertEqual((self.logs / "pipewire.stderr.log").read_text(), "pipewire diagnostic\nstartup refused 27\n")
        self.assertFalse((self.logs / "wireplumber.stdout.log").exists())
        self.assertFalse((self.logs / "wireplumber.stderr.log").exists())


@unittest.skipIf(os.name == "nt", "stable service roots are POSIX-only")
class StableServiceRootTests(unittest.TestCase):
    def test_root_follows_the_tree_and_refuses_an_entry_this_user_does_not_own(self):
        root = services.stable_service_root(Path("/tmp/build/one"), "pipewire")
        self.assertEqual(root, services.stable_service_root(Path("/tmp/build/one"), "pipewire"))
        self.assertNotEqual(root, services.stable_service_root(Path("/tmp/build/two"), "pipewire"))
        self.assertNotEqual(root, services.stable_service_root(Path("/tmp/build/one"), "gtk-bus"))
        with tempfile.TemporaryDirectory(prefix="ao-service-root-") as directory:
            target = Path(directory) / "elsewhere"
            target.mkdir()
            link = Path(directory) / "root"
            link.symlink_to(target)
            with self.assertRaisesRegex(BazelWorkspaceError, "not this user's directory"):
                services.recreate_private_root(link)
            self.assertTrue(target.is_dir())


if __name__ == "__main__":
    unittest.main()
