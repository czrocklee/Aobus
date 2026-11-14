"""Tests for CI's Bazel remote-cache proxy over the shared R2 bucket."""

import contextlib
import errno
import hashlib
import io
import json
import socket
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_remote_cache as remote
from ao.core import r2_credentials

CREDENTIALS = {
    "R2_ENDPOINT": "https://account.r2.cloudflarestorage.com",
    "R2_ACCESS_KEY_ID": "key-id",
    "R2_SECRET_ACCESS_KEY": "secret-key",
}
HASH = "41e02d18975b01df58e5565898a2481aba0e54fedf20d15f94c10683cbb9164e"
# Access-log lines in bazel-remote 2.6's own spelling.
WRITE = f"2026/11/04 06:10:19 GRPC BYTESTREAM WRITE COMPLETED: uploads/1b4a/blobs/{HASH}/15962"
AC_PUT = f"2026/11/04 06:10:19 GRPC AC PUT {HASH} OK"
CAS_PUT = f"2026/11/04 06:10:19 GRPC CAS PUT {HASH} OK"
SKIPPED = f"2026/11/04 06:10:19 GRPC BYTESTREAM SKIPPED WRITE: uploads/f5ed/blobs/{HASH}/146"
UPLOADED = f"2026/11/04 06:22:57 S3 UPLOAD aobus-sccache bazel-v1/cas.v2/41/{HASH} OK"
UPLOAD_DENIED = f"2026/11/04 06:22:57 S3 UPLOAD aobus-sccache bazel-v1/ac/41/{HASH} Access Denied."
AC_UPLOADED = f"2026/11/04 06:22:57 S3 UPLOAD aobus-sccache bazel-v1/ac/41/{HASH} OK"
OTHER_HASH = "ff" * 32
CANCELLED = f"2026/11/04 06:10:19 GRPC BYTESTREAM WRITE FAILED: uploads/ab12/blobs/{OTHER_HASH}/10 context canceled"
OTHER_UPLOADED = f"2026/11/04 06:22:57 S3 UPLOAD aobus-sccache bazel-v1/cas.v2/ff/{OTHER_HASH} OK"
DOWNLOADED = f"2026/11/04 06:25:56 S3 DOWNLOAD aobus-sccache bazel-v1/ac/41/{HASH} OK"
MISSED = f"2026/11/04 06:22:57 S3 DOWNLOAD aobus-sccache bazel-v1/ac/41/{HASH} NOT FOUND"
CONTAINS = f"2026/11/04 06:10:19 S3 CONTAINS aobus-sccache bazel-v1/cas.v2/41/{HASH} OK"


class PinnedArtifactTest(unittest.TestCase):
    def test_windows_runs_the_pinned_linux_binary_from_a_pinned_rootfs(self):
        # bazel-remote refuses Windows builds upstream; WSL hosts the Linux one.
        self.assertEqual(remote.artifact("Windows", "X64"), remote.artifact("Linux", "X64"))
        rootfs = remote.wsl_rootfs()
        self.assertTrue(rootfs.url.startswith("https://"))
        self.assertRegex(rootfs.sha256, r"\A[0-9a-f]{64}\Z")
        with self.assertRaisesRegex(remote.RemoteCacheError, "no pinned bazel-remote for Windows ARM64"):
            remote.artifact("Windows", "ARM64")


class EndpointTest(unittest.TestCase):
    def test_the_r2_secret_spellings_select_the_host(self):
        for endpoint in (
            "https://account.r2.cloudflarestorage.com",
            "https://account.r2.cloudflarestorage.com/",
            "account.r2.cloudflarestorage.com",
        ):
            with self.subTest(endpoint=endpoint):
                self.assertEqual(remote.s3_endpoint(endpoint), "account.r2.cloudflarestorage.com")

    def test_other_origins_are_rejected_without_echoing_the_secret(self):
        for endpoint in (
            "http://f00dcafe.r2.cloudflarestorage.com",
            "https://f00dcafe.r2.cloudflarestorage.com/aobus-sccache",
            "https://user@f00dcafe.r2.cloudflarestorage.com",
            "https://f00dcafe.r2.cloudflarestorage.com?x=1",
        ):
            with self.subTest(endpoint=endpoint), self.assertRaises(remote.RemoteCacheError) as raised:
                remote.s3_endpoint(endpoint)
            self.assertNotIn("f00dcafe", str(raised.exception))


class InstallTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.payload = b"bazel-remote"
        self.selected = remote.Artifact("https://example.com/bazel-remote", hashlib.sha256(self.payload).hexdigest())

    def fetch(self, payload: bytes):
        def write(url: str, destination: Path) -> None:
            self.assertEqual(url, self.selected.url)
            destination.write_bytes(payload)

        return mock.Mock(side_effect=write)

    def test_a_verified_download_is_executable_and_reused(self):
        fetch = self.fetch(self.payload)
        binary = remote.install(self.selected, self.directory, fetch=fetch)
        self.assertEqual(binary.read_bytes(), self.payload)
        if sys.platform != "win32":
            # The proxy runs on Unix; Windows stat has no Unix execute-bit contract.
            self.assertTrue(binary.stat().st_mode & 0o100)
        self.assertEqual(remote.install(self.selected, self.directory, fetch=fetch), binary)
        fetch.assert_called_once()

    def test_a_mismatched_download_leaves_nothing_to_run(self):
        with self.assertRaisesRegex(remote.RemoteCacheError, "does not match"):
            remote.install(self.selected, self.directory, fetch=self.fetch(b"tampered"))
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_named_artifacts_keep_separate_files_and_diagnostics(self):
        rootfs = remote.install(self.selected, self.directory, name="wsl-rootfs", fetch=self.fetch(self.payload))
        self.assertEqual(rootfs.name, f"wsl-rootfs-{self.selected.sha256[:16]}")
        rootfs.unlink()
        with self.assertRaisesRegex(remote.RemoteCacheError, "downloaded wsl-rootfs does not match"):
            remote.install(self.selected, self.directory, name="wsl-rootfs", fetch=self.fetch(b"tampered"))

    def test_a_corrupted_cached_binary_is_downloaded_again(self):
        binary = remote.install(self.selected, self.directory, fetch=self.fetch(self.payload))
        binary.write_bytes(b"corrupted")
        fetch = self.fetch(self.payload)
        self.assertEqual(remote.install(self.selected, self.directory, fetch=fetch).read_bytes(), self.payload)
        fetch.assert_called_once()


@unittest.skipUnless(sys.platform in {"linux", "darwin"}, "the pinned proxy runs on native Linux/macOS")
class AddressAvailabilityTest(unittest.TestCase):
    def test_a_closed_server_address_is_reusable_before_time_wait_expires(self):
        with socket.socket() as listener, socket.socket() as client:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.settimeout(2)
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            address = listener.getsockname()
            client.settimeout(2)
            client.connect(address)
            with listener.accept()[0] as connection:
                connection.settimeout(2)
                # Observe both FINs after a server-initiated close, without sleeps.
                connection.shutdown(socket.SHUT_WR)
                self.assertEqual(client.recv(1), b"")
                client.shutdown(socket.SHUT_WR)
                self.assertEqual(connection.recv(1), b"")
        endpoint = f"{address[0]}:{address[1]}"
        with mock.patch.multiple(remote, HTTP_ADDRESS=endpoint, GRPC_ADDRESS=endpoint):
            remote._require_free_addresses()

    def test_an_active_listener_still_blocks_either_server_address(self):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            address = listener.getsockname()
            endpoint = f"{address[0]}:{address[1]}"
            for name in ("HTTP_ADDRESS", "GRPC_ADDRESS"):
                with (
                    self.subTest(address=name),
                    mock.patch.multiple(remote, HTTP_ADDRESS="127.0.0.1:0", GRPC_ADDRESS="127.0.0.1:0"),
                    mock.patch.object(remote, name, endpoint),
                    self.assertRaises(remote.RemoteCacheError) as raised,
                ):
                    remote._require_free_addresses()
                self.assertEqual(raised.exception.__cause__.errno, errno.EADDRINUSE)


class WindowsAddressAvailabilityTest(unittest.TestCase):
    def test_windows_probes_request_exclusive_use_instead_of_shared_reuse(self):
        with (
            mock.patch.object(remote.sys, "platform", "win32"),
            mock.patch.object(remote.socket, "SO_EXCLUSIVEADDRUSE", -5, create=True),
            mock.patch.object(remote.socket, "socket") as factory,
        ):
            remote._require_free_addresses()
        options = [call.args[1] for call in factory.return_value.__enter__.return_value.setsockopt.call_args_list]
        self.assertEqual(options, [-5, -5])


class StartTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.state = Path(temporary.name)
        self.output = self.state / "github-output"
        self.output.touch()
        self.binary = self.state / "bin" / "bazel-remote"
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch("sys.stdout", new_callable=io.StringIO))
        self.install = stack.enter_context(mock.patch.object(remote, "install", return_value=self.binary))
        self.popen = stack.enter_context(mock.patch.object(remote.subprocess, "Popen"))
        self.popen.return_value.pid = 4242
        self.popen.return_value.poll.return_value = None
        self.status = stack.enter_context(mock.patch.object(remote, "_status", return_value={"NumFiles": 0}))
        self.socket = stack.enter_context(mock.patch.object(remote.socket, "socket"))
        stack.enter_context(mock.patch.object(remote.time, "sleep"))

    def start(self, *, upload: bool, environ: dict[str, str], key_prefix: str = remote.KEY_PREFIX) -> None:
        remote.start(self.state, self.output, "Linux", "X64", upload=upload, environ=environ, key_prefix=key_prefix)

    def test_secretless_runs_start_nothing_and_publish_no_cache(self):
        for environ in (
            {},
            {key: value for key, value in CREDENTIALS.items() if key != "R2_ACCESS_KEY_ID"},
            {"AWS_ACCESS_KEY_ID": "legacy-writer", "AWS_SECRET_ACCESS_KEY": "legacy-writer-secret"},
        ):
            with self.subTest(environ=sorted(environ)):
                self.start(upload=True, environ=environ)
        self.popen.assert_not_called()
        self.install.assert_not_called()
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")

    def test_the_server_reads_secrets_only_from_its_environment(self):
        self.start(
            upload=False,
            environ={
                **CREDENTIALS,
                "PATH": "/usr/bin",
                "RUNNER_TRACKING_ID": "job-123",
                "AWS_ACCESS_KEY_ID": "legacy-writer",
                "AWS_SECRET_ACCESS_KEY": "legacy-writer-secret",
                "BAZEL_REMOTE_S3_DISABLE_SSL": "true",
                "BAZEL_REMOTE_S3_UPDATE_TIMESTAMPS": "true",
            },
        )
        argv = self.popen.call_args.args[0]
        environment = self.popen.call_args.kwargs["env"]
        self.assertEqual(argv[0], str(self.binary))
        for secret in CREDENTIALS.values():
            self.assertFalse(any(secret in argument for argument in argv), secret)
        self.assertFalse([key for key in environment if key.startswith("R2_")])
        self.assertEqual(
            {key: value for key, value in environment.items() if key.startswith("BAZEL_REMOTE_S3_")},
            {
                "BAZEL_REMOTE_S3_ENDPOINT": "account.r2.cloudflarestorage.com",
                "BAZEL_REMOTE_S3_ACCESS_KEY_ID": "key-id",
                "BAZEL_REMOTE_S3_SECRET_ACCESS_KEY": "secret-key",
            },
        )
        self.assertEqual(environment["PATH"], "/usr/bin")
        self.assertEqual(environment["RUNNER_TRACKING_ID"], "job-123")
        self.assertEqual(
            set(environment),
            {
                "PATH",
                "RUNNER_TRACKING_ID",
                "BAZEL_REMOTE_S3_ENDPOINT",
                "BAZEL_REMOTE_S3_ACCESS_KEY_ID",
                "BAZEL_REMOTE_S3_SECRET_ACCESS_KEY",
            },
        )
        # The shared R2 cache bucket, under the Bazel-only prefix.
        self.assertEqual(argv[argv.index("--s3.bucket") + 1], r2_credentials.R2_BUCKET)
        self.assertEqual(argv[argv.index("--s3.prefix") + 1], "bazel-v1")

    def test_only_trusted_writers_start_uploaders(self):
        for upload in (True, False):
            with self.subTest(upload=upload):
                self.start(upload=upload, environ=CREDENTIALS)
                argv = self.popen.call_args.args[0]
                self.assertEqual(argv[argv.index("--num_uploaders") + 1] != "0", upload)
                server = json.loads((self.state / remote.SERVER_NAME).read_text(encoding="utf-8"))
                self.assertEqual(server, {"pid": 4242, "upload": upload, "key_prefix": remote.KEY_PREFIX})

    def test_the_cli_passes_an_isolated_prefix_to_the_server_and_persists_it(self):
        prefix = "bazel-validation.ci/run-42"
        with mock.patch.dict(remote.os.environ, CREDENTIALS, clear=True):
            self.assertEqual(
                remote.main(
                    [
                        "start",
                        "--state",
                        str(self.state),
                        "--github-output",
                        str(self.output),
                        "--runner-os",
                        "Linux",
                        "--runner-arch",
                        "X64",
                        "--trusted-writer",
                        "true",
                        "--key-prefix",
                        prefix,
                    ]
                ),
                0,
            )
        argv = self.popen.call_args.args[0]
        self.assertEqual(argv[argv.index("--s3.prefix") + 1], prefix)
        self.assertEqual(
            json.loads((self.state / remote.SERVER_NAME).read_text(encoding="utf-8")),
            {"pid": 4242, "upload": True, "key_prefix": prefix},
        )

    def test_unsafe_prefixes_fail_before_any_install_or_server_io(self):
        # Preserve path grammar, whole-string matching and ASCII boundaries.
        for prefix in (
            "",
            "/absolute",
            "a/../b",
            "a//b",
            "a/",
            "a\\b",
            "a\n",
            "non-ascii-é",
        ):
            with self.subTest(prefix=prefix), self.assertRaises(remote.RemoteCacheError):
                self.start(upload=True, environ=CREDENTIALS, key_prefix=prefix)
        self.install.assert_not_called()
        self.socket.assert_not_called()
        self.popen.assert_not_called()
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")

    def test_the_cache_url_is_published_only_once_the_server_answers(self):
        self.status.side_effect = [None, None, {"NumFiles": 0}]
        self.start(upload=False, environ=CREDENTIALS)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "url=grpc://127.0.0.1:9092\n")
        self.assertEqual(self.status.call_count, 3)

    def test_pre_existing_http_or_grpc_listeners_are_rejected_without_starting_a_child(self):
        for occupied in (remote.HTTP_ADDRESS, remote.GRPC_ADDRESS):
            host, port = occupied.rsplit(":", 1)

            def bind(address: tuple[str, int], expected: tuple[str, int] = (host, int(port))) -> None:
                if address == expected:
                    raise OSError("address already in use")

            self.socket.return_value.__enter__.return_value.bind.side_effect = bind
            with self.subTest(address=occupied), self.assertRaisesRegex(remote.RemoteCacheError, "unavailable"):
                self.start(upload=True, environ=CREDENTIALS)
        self.popen.assert_not_called()
        self.status.assert_not_called()
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_an_exited_child_cannot_publish_even_when_an_http_server_answers(self):
        self.popen.return_value.poll.return_value = 1
        with self.assertRaisesRegex(remote.RemoteCacheError, "did not become ready"):
            self.start(upload=True, environ=CREDENTIALS)
        self.popen.return_value.kill.assert_not_called()
        self.popen.return_value.wait.assert_called_once()
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_a_server_that_exits_before_answering_fails_without_a_cache_url(self):
        self.status.return_value = None
        self.popen.return_value.poll.return_value = 1
        with self.assertRaisesRegex(remote.RemoteCacheError, "did not become ready"):
            self.start(upload=True, environ=CREDENTIALS)
        self.popen.return_value.wait.assert_called_once()
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_a_readiness_timeout_reaps_the_owned_child_without_logging_credentials(self):
        child = self.popen.return_value

        def launch(*args, **kwargs):
            kwargs["stdout"].write(" ".join(CREDENTIALS.values()).encode())
            kwargs["stdout"].flush()
            return child

        self.popen.side_effect = launch
        with self.assertRaises(remote.RemoteCacheError) as raised:
            remote.start(self.state, self.output, "Linux", "X64", upload=True, environ=CREDENTIALS, ready_seconds=0)
        child.kill.assert_called_once()
        child.wait.assert_called_once()
        for secret in (*CREDENTIALS.values(), "account.r2.cloudflarestorage.com"):
            self.assertNotIn(secret, str(raised.exception))
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")
        self.assertFalse((self.state / remote.SERVER_NAME).exists())


_TRANSITIONAL = {"Installing", "Uninstalling", "Converting", "Exporting", "Compacting"}
_LIST_ARGUMENTS = ["--list", "--quiet", "--all"]


class FakeWsl:
    """Answer discriminated wsl.exe commands and record what crossed that boundary."""

    def __init__(self):
        self.calls: list[tuple[str, list[str], dict[str, str], bool]] = []
        self.alive = True
        self.log = ""
        self.fail: set[str] = set()
        self.errors: dict[str, BaseException] = {}
        self.timeouts: set[str] = set()
        # Name to registration state. Transitional states stay hidden unless --all is requested.
        self.registered: dict[str, str] = {}
        self.sticky = False
        self.unregister_code: int | None = None
        self.partial_import = False
        self.list_text: str | None = None
        self.marker_at_import: str | None = None

    def __call__(self, action, arguments, environ, *, check=True, echo=True):
        self.calls.append((action, list(arguments), dict(environ), echo))
        if action in self.timeouts:
            raise remote.RemoteCacheError(f"WSL {action} did not finish within {remote._WSL_SECONDS} seconds")
        if arguments[:1] == ["--unregister"]:
            return self._unregister(arguments, check)
        if arguments[:1] == ["--list"]:
            return self._list(arguments, action, check)
        if arguments[:1] == ["--import"]:
            return self._import(arguments, action, check)
        if action in self.errors:
            raise self.errors[action]
        if action in self.fail:
            if check:
                raise remote.RemoteCacheError(f"WSL {action} failed with exit code 1")
            return 1, ""
        if action == "path translation":
            return 0, "/mnt/d/state/" + arguments[-1].rsplit("/", 1)[-1] + "\n"
        if action == "proxy pid read":
            return 0, "77\n"
        if action == "proxy liveness check":
            return (0 if self.alive else 1), ""
        if action == "proxy log read":
            return 0, self.log
        return 0, ""

    def _unregister(self, arguments, check):
        if "unregister" in self.fail:
            if check:
                raise remote.RemoteCacheError("WSL unregister failed with exit code 1")
            return 1, ""
        name = arguments[1]
        removed = False
        if not self.sticky:
            folded = name.casefold()
            kept = {key: value for key, value in self.registered.items() if key.casefold() != folded}
            removed = len(kept) != len(self.registered)
            self.registered = kept
        if self.unregister_code is not None:
            return self.unregister_code, ""
        # A missing distribution is a nonzero unregister, not proof it is gone.
        return (0 if removed else 1), ""

    def _list(self, arguments, action, check):
        if action in self.fail:
            if check:
                raise remote.RemoteCacheError(f"WSL {action} failed with exit code 1")
            return 1, ""
        if self.list_text is not None:
            return 0, self.list_text
        # Quiet list with no visible distros is empty and successful. --all is what
        # reveals Installing/Uninstalling/Converting/Exporting/Compacting names.
        include_transitional = arguments == _LIST_ARGUMENTS
        names = [name for name, state in self.registered.items() if include_transitional or state not in _TRANSITIONAL]
        if not names:
            return 0, ""
        return 0, "".join(f"{name}\r\n" for name in names)

    def _import(self, arguments, action, check):
        disk = Path(arguments[2])
        marker = disk.parent / remote.SERVER_NAME
        self.marker_at_import = marker.read_text(encoding="utf-8") if marker.is_file() else None
        if self.partial_import:
            disk.mkdir(parents=True, exist_ok=True)
            (disk / "ext4.vhdx").write_bytes(b"partial-import")
            self.registered[arguments[1]] = "Installing"
        if action in self.errors:
            raise self.errors[action]
        if action in self.fail or self.partial_import:
            if check:
                raise remote.RemoteCacheError(f"WSL {action} failed with exit code 1")
            return 1, ""
        self.registered[arguments[1]] = "Running"
        return 0, ""

    def actions(self) -> list[str]:
        return [action for action, *_ in self.calls]


class WslCommandTest(unittest.TestCase):
    def run_wsl(self, completed=None, **options):
        with mock.patch.object(remote.subprocess, "run", return_value=completed) as run:
            if completed is None:
                run.side_effect = remote.subprocess.TimeoutExpired("wsl.exe", 1)
            result = remote._wsl("import", ["--import", "name"], {"PATH": "C:/Windows"}, **options)
        return result, run

    def test_management_output_is_requested_as_utf8_and_stripped_of_utf16_padding(self):
        completed = mock.Mock(returncode=0, stdout="ok\n".encode("utf-16-le"))
        (code, output), run = self.run_wsl(completed)
        self.assertEqual((code, output), (0, "ok\n"))
        self.assertEqual(run.call_args.args[0], ["wsl.exe", "--import", "name"])
        self.assertEqual(run.call_args.kwargs["env"], {"PATH": "C:/Windows", "WSL_UTF8": "1"})

    def test_management_output_decodes_utf16_bom_and_utf8_bom(self):
        samples = {
            "utf-16-le-bom": "Aobus-Bazel-Remote\r\n".encode("utf-16"),
            "utf-8-bom": "\ufeffok\n".encode("utf-8"),
        }
        expected = {"utf-16-le-bom": "Aobus-Bazel-Remote\r\n", "utf-8-bom": "ok\n"}
        for name, stdout in samples.items():
            with self.subTest(name=name):
                completed = mock.Mock(returncode=0, stdout=stdout)
                code, output = self.run_wsl(completed)[0]
                self.assertEqual((code, output), (0, expected[name]))

    def test_an_undecodable_distribution_list_is_not_absence(self):
        def run(command, **kwargs):
            if command[1:] == _LIST_ARGUMENTS:
                return mock.Mock(returncode=0, stdout=b"\xff\n")
            return mock.Mock(returncode=1, stdout=b"")

        with mock.patch.object(remote.subprocess, "run", side_effect=run) as invoked:
            with self.assertRaisesRegex(remote.RemoteCacheError, "WSL distribution list contains undecodable output"):
                remote._remove_wsl({"PATH": "C:/Windows"})
        self.assertEqual(invoked.call_args_list[0].args[0], ["wsl.exe", "--unregister", remote.WSL_DISTRO])
        self.assertEqual(invoked.call_args_list[1].args[0], ["wsl.exe", *_LIST_ARGUMENTS])

    def test_failures_report_output_unless_the_call_carried_credentials(self):
        completed = mock.Mock(returncode=3, stdout=b"disk full")
        with self.assertRaisesRegex(remote.RemoteCacheError, "WSL import failed with exit code 3:\ndisk full"):
            self.run_wsl(completed)
        with self.assertRaises(remote.RemoteCacheError) as raised:
            self.run_wsl(completed, echo=False)
        self.assertEqual(str(raised.exception), "WSL import failed with exit code 3")
        self.assertEqual(self.run_wsl(completed, check=False)[0], (3, "disk full"))

    def test_a_hung_call_becomes_a_cache_error(self):
        with self.assertRaisesRegex(remote.RemoteCacheError, "WSL import did not finish"):
            self.run_wsl(None)

    def test_only_windows_variables_reach_wsl_exe(self):
        environ = {
            **CREDENTIALS,
            "Path": "C:/Windows",
            "WslEnv": "R2_SECRET_ACCESS_KEY",
            "aws_secret_access_key": "legacy",
            "BAZEL_REMOTE_S3_DISABLE_SSL": "true",
        }
        self.assertEqual(remote._wsl_environment(environ), {"Path": "C:/Windows"})


class WslStartTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.state = Path(temporary.name).resolve()
        self.output = self.state / "github-output"
        self.output.touch()
        bundle = self.state / "git-ca-bundle.crt"
        bundle.write_text("trust", encoding="utf-8")
        self.wsl = FakeWsl()
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch("sys.stdout", new_callable=io.StringIO))
        self.install = stack.enter_context(
            mock.patch.object(
                remote,
                "install",
                side_effect=lambda selected, directory, name="bazel-remote": directory / name,
            )
        )
        stack.enter_context(mock.patch.object(remote, "_wsl", self.wsl))
        stack.enter_context(mock.patch.object(remote, "_git_ca_bundle", return_value=bundle))
        self.popen = stack.enter_context(mock.patch.object(remote.subprocess, "Popen"))
        self.status = stack.enter_context(mock.patch.object(remote, "_status", return_value={"NumFiles": 0}))
        stack.enter_context(mock.patch.object(remote.socket, "socket"))
        stack.enter_context(mock.patch.object(remote.time, "sleep"))

    def start(self, *, upload: bool = True, **options) -> None:
        environ = {**CREDENTIALS, "PATH": "C:/Windows", "WSLENV": "R2_SECRET_ACCESS_KEY"}
        remote.start(self.state, self.output, "Windows", "X64", upload=upload, environ=environ, **options)

    def script(self, action: str) -> str:
        return next(arguments for name, arguments, *_ in self.wsl.calls if name == action)[-1]

    def test_the_distribution_is_replaced_before_the_verified_binary_starts_in_it(self):
        self.start()
        self.popen.assert_not_called()
        self.assertEqual(
            self.wsl.actions(),
            [
                "unregister",
                "distribution list",
                "import",
                "path translation",
                "path translation",
                "proxy installation",
                "proxy start",
                "proxy pid read",
                "proxy liveness check",
            ],
        )
        self.assertEqual(self.wsl.calls[1][1], _LIST_ARGUMENTS)
        self.assertEqual(
            self.wsl.calls[2][1],
            [
                "--import",
                remote.WSL_DISTRO,
                str(self.state / "wsl"),
                str(self.state / "bin" / "wsl-rootfs"),
                "--version",
                "2",
            ],
        )
        self.assertEqual(
            json.loads(self.wsl.marker_at_import),
            {"host": "wsl", "upload": False, "key_prefix": remote.KEY_PREFIX},
        )
        self.assertEqual((self.state / "ca-bundle.crt").read_text(encoding="utf-8"), "trust")
        self.assertIn("cp /mnt/d/state/bazel-remote /opt/bazel-remote/bazel-remote", self.script("proxy installation"))
        self.assertIn(
            "cp /mnt/d/state/ca-bundle.crt /opt/bazel-remote/ca-bundle.crt", self.script("proxy installation")
        )
        start = self.script("proxy start")
        self.assertIn(
            "export SSL_CERT_FILE=/opt/bazel-remote/ca-bundle.crt; setsid /opt/bazel-remote/bazel-remote ", start
        )
        self.assertIn(" --dir /opt/bazel-remote/data ", start)
        self.assertIn(" --s3.prefix bazel-v1 ", start)
        self.assertIn(" --num_uploaders 100 ", start)
        for name in ("proxy installation", "proxy start"):
            # wsl.exe reinterprets these characters before sh sees the script.
            self.assertNotRegex(self.script(name), r'["\\]')
        self.assertEqual(
            json.loads((self.state / remote.SERVER_NAME).read_text(encoding="utf-8")),
            {"pid": 77, "upload": True, "key_prefix": remote.KEY_PREFIX, "host": "wsl"},
        )
        self.assertFalse((self.state / f"{remote.SERVER_NAME}.partial").exists())
        self.assertEqual(self.output.read_text(encoding="utf-8"), "url=grpc://127.0.0.1:9092\n")

    def test_credentials_enter_only_the_start_call_through_wslenv(self):
        self.start(upload=False)
        secrets = {
            "BAZEL_REMOTE_S3_ENDPOINT": "account.r2.cloudflarestorage.com",
            "BAZEL_REMOTE_S3_ACCESS_KEY_ID": "key-id",
            "BAZEL_REMOTE_S3_SECRET_ACCESS_KEY": "secret-key",
        }
        for action, arguments, environ, echo in self.wsl.calls:
            with self.subTest(action=action):
                for secret in (*CREDENTIALS.values(), *secrets.values()):
                    self.assertFalse(any(secret in argument for argument in arguments), secret)
                if action == "proxy start":
                    self.assertEqual(environ, {"PATH": "C:/Windows", **secrets, "WSLENV": ":".join(secrets)})
                    self.assertFalse(echo)
                else:
                    self.assertEqual(environ, {"PATH": "C:/Windows"})
        self.assertIn(" --num_uploaders 0 ", self.script("proxy start"))

    def test_a_proxy_that_never_answers_is_removed_without_logging_credentials(self):
        self.status.return_value = None
        self.wsl.log = " ".join((*CREDENTIALS.values(), "account.r2.cloudflarestorage.com"))
        with self.assertRaises(remote.RemoteCacheError) as raised:
            self.start(ready_seconds=0)
        for secret in (*CREDENTIALS.values(), "account.r2.cloudflarestorage.com"):
            self.assertNotIn(secret, str(raised.exception))
        self.assertEqual(self.wsl.actions()[-3:], ["proxy log read", "unregister", "distribution list"])
        self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_an_exited_proxy_cannot_publish_even_when_an_http_server_answers(self):
        self.wsl.alive = False
        with self.assertRaisesRegex(remote.RemoteCacheError, "did not become ready"):
            self.start()
        self.assertEqual(self.wsl.actions()[-2:], ["unregister", "distribution list"])
        self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")

    def test_a_failed_installation_leaves_no_distribution_behind(self):
        # Every step sits inside the same rollback try; import (which also
        # creates the disk) plus one later action cover the unregister path.
        for action in ("import", "proxy start"):
            self.wsl.calls.clear()
            self.wsl.fail = {action}
            with self.subTest(action=action), self.assertRaisesRegex(remote.RemoteCacheError, f"WSL {action} failed"):
                self.start()
            # Import is inside the rollback try, so a failed import unregisters too.
            self.assertEqual(self.wsl.actions()[-2:], ["unregister", "distribution list"])
            self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)
            self.assertFalse((self.state / remote.SERVER_NAME).exists())
        self.status.assert_not_called()

    def test_a_failed_import_rolls_back_and_rethrows_the_original_error(self):
        original = remote.RemoteCacheError("WSL import failed with exit code 4")
        self.wsl.errors["import"] = original
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr), self.assertRaises(remote.RemoteCacheError) as raised:
            self.start()
        self.assertIs(raised.exception, original)
        self.assertEqual(stderr.getvalue(), "")
        self.assertEqual(self.wsl.actions()[-2:], ["unregister", "distribution list"])
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_failed_removal_preserves_the_disk_and_ownership_before_startup(self):
        disk = self.state / "wsl"
        disk.mkdir()
        sentinel = disk / "sentinel"
        sentinel.write_text("keep", encoding="utf-8")
        prior = {"host": "wsl", "upload": False, "key_prefix": "prior-prefix"}
        marker = self.state / remote.SERVER_NAME
        marker.write_text(json.dumps(prior), encoding="utf-8")
        scenarios = {
            "reported success but still listed": (
                "remains registered after unregister \\(exit code 0\\)",
                {"sticky": True, "unregister_code": 0, "registered": {remote.WSL_DISTRO: "Running"}},
            ),
            "nonzero unregister and still listed": (
                "remains registered after unregister \\(exit code 1\\)",
                {"sticky": True, "unregister_code": 1, "registered": {remote.WSL_DISTRO: "Running"}},
            ),
            "case-insensitive name": (
                "remains registered",
                {"sticky": True, "unregister_code": 0, "registered": {"Aobus-Bazel-Remote": "Running"}},
            ),
            "transitional registration": (
                "remains registered",
                {"sticky": True, "unregister_code": 0, "registered": {remote.WSL_DISTRO: "Installing"}},
            ),
            "distribution list failure": ("WSL distribution list failed", {"fail": {"distribution list"}}),
            "undecodable distribution list": ("undecodable", {"list_text": "\ufffd\n"}),
            "unregister timeout": ("WSL unregister did not finish", {"timeouts": {"unregister"}}),
        }
        for name, (pattern, fields) in scenarios.items():
            with self.subTest(name=name):
                self.wsl.calls.clear()
                self.wsl.fail = set()
                self.wsl.timeouts = set()
                self.wsl.registered = {}
                self.wsl.sticky = False
                self.wsl.unregister_code = None
                self.wsl.list_text = None
                for key, value in fields.items():
                    setattr(self.wsl, key, value)
                with self.assertRaisesRegex(remote.RemoteCacheError, pattern):
                    self.start()
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")
                self.assertEqual(json.loads(marker.read_text(encoding="utf-8")), prior)
                self.assertNotIn("import", self.wsl.actions())
                if name == "unregister timeout":
                    self.assertNotIn("distribution list", self.wsl.actions())
                else:
                    self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)

    def test_a_nonzero_unregister_of_an_absent_distribution_still_replaces_the_disk(self):
        disk = self.state / "wsl"
        disk.mkdir()
        (disk / "old").write_text("gone", encoding="utf-8")
        self.wsl.unregister_code = 1
        self.wsl.registered = {"Ubuntu": "Running"}
        self.start()
        self.assertFalse((disk / "old").exists())
        self.assertTrue(disk.is_dir())
        self.assertEqual(self.wsl.calls[1][1], _LIST_ARGUMENTS)
        self.assertIn(remote.WSL_DISTRO, self.wsl.registered)
        self.assertEqual(json.loads((self.state / remote.SERVER_NAME).read_text(encoding="utf-8"))["pid"], 77)

    def test_transitional_registration_is_visible_only_because_list_requests_all(self):
        self.wsl.sticky = True
        self.wsl.registered = {remote.WSL_DISTRO: "Installing"}
        with self.assertRaisesRegex(remote.RemoteCacheError, "remains registered"):
            remote._remove_wsl({"PATH": "C:/Windows"})
        listed = next(arguments for name, arguments, *_ in self.wsl.calls if name == "distribution list")
        self.assertEqual(listed, _LIST_ARGUMENTS)

    def test_a_rollback_unlink_error_keeps_the_marker_and_the_original_import_error(self):
        original = remote.RemoteCacheError("WSL import failed with exit code 9")
        self.wsl.errors["import"] = original
        stderr = io.StringIO()
        original_unlink = Path.unlink

        def unlink(path, missing_ok=False):
            if path.name == remote.SERVER_NAME:
                raise OSError(errno.EBUSY, "marker busy")
            return original_unlink(path, missing_ok=missing_ok)

        with (
            mock.patch("sys.stderr", stderr),
            mock.patch.object(Path, "unlink", unlink),
            self.assertRaises(remote.RemoteCacheError) as raised,
        ):
            self.start()
        self.assertIs(raised.exception, original)
        self.assertIn("WSL startup rollback failed; ownership retained at", stderr.getvalue())
        self.assertIn("marker busy", stderr.getvalue())
        self.assertNotIn("secret-key", stderr.getvalue())
        self.assertNotIn("account.r2.cloudflarestorage.com", stderr.getvalue())
        self.assertTrue((self.state / remote.SERVER_NAME).is_file())

    def test_a_partial_import_rollback_failure_keeps_a_cleanup_marker_for_a_later_stop(self):
        prefix = "validation.ci/42"
        original = remote.RemoteCacheError("WSL import failed with exit code 1:\npartial")
        self.wsl.partial_import = True
        self.wsl.sticky = True
        self.wsl.errors["import"] = original
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr), self.assertRaises(remote.RemoteCacheError) as raised:
            self.start(key_prefix=prefix)
        self.assertIs(raised.exception, original)
        diagnostic = stderr.getvalue()
        self.assertIn(
            f"WSL startup rollback failed; ownership retained at {self.state / remote.SERVER_NAME}", diagnostic
        )
        self.assertNotIn("secret-key", diagnostic)
        self.assertNotIn("account.r2.cloudflarestorage.com", diagnostic)
        self.assertEqual(
            json.loads((self.state / remote.SERVER_NAME).read_text(encoding="utf-8")),
            {"host": "wsl", "upload": False, "key_prefix": prefix},
        )
        self.assertEqual((self.state / "wsl" / "ext4.vhdx").read_bytes(), b"partial-import")
        self.assertEqual(
            [arguments for name, arguments, *_ in self.wsl.calls if name == "distribution list"],
            [_LIST_ARGUMENTS, _LIST_ARGUMENTS],
        )
        report = self.state / "receipt.json"
        self.status.reset_mock()
        with (
            mock.patch("sys.stderr", stderr),
            mock.patch.dict(remote.os.environ, {"PATH": "C:/Windows"}, clear=True),
            mock.patch.object(remote, "drain") as drain,
        ):
            self.assertEqual(remote.main(["stop", "--state", str(self.state), "--report", str(report)]), 1)
        drain.assert_not_called()
        self.status.assert_not_called()
        self.assertFalse(report.exists())
        self.assertNotIn("proxy log read", self.wsl.actions())
        self.assertNotIn("proxy stop", self.wsl.actions())
        self.assertEqual(
            json.loads((self.state / remote.SERVER_NAME).read_text(encoding="utf-8"))["key_prefix"], prefix
        )
        self.wsl.sticky = False
        self.wsl.registered.clear()
        with mock.patch.dict(remote.os.environ, {"PATH": "C:/Windows"}, clear=True):
            self.assertEqual(remote.main(["stop", "--state", str(self.state), "--report", str(report)]), 0)
        self.assertFalse((self.state / remote.SERVER_NAME).exists())
        self.assertEqual(
            json.loads(report.read_text(encoding="utf-8")),
            {
                "schemaVersion": 1,
                "bucket": "aobus-sccache",
                "keyPrefix": prefix,
                "started": False,
                "upload": False,
                "activity": {
                    "queued": 0,
                    "uploaded": 0,
                    "pending": 0,
                    "uploadFailures": 0,
                    "downloaded": 0,
                    "downloadMisses": 0,
                },
                "lostUploads": 0,
            },
        )


class WslStopTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.state = Path(temporary.name)
        self.wsl = FakeWsl()
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.stdout = stack.enter_context(mock.patch("sys.stdout", new_callable=io.StringIO))
        stack.enter_context(mock.patch.object(remote, "_wsl", self.wsl))
        self.terminate = stack.enter_context(mock.patch.object(remote, "_terminate"))
        self.status = stack.enter_context(mock.patch.object(remote, "_status", return_value=None))
        stack.enter_context(mock.patch.object(remote.time, "sleep"))
        (self.state / remote.SERVER_NAME).write_text(
            json.dumps({"pid": 77, "upload": True, "key_prefix": remote.KEY_PREFIX, "host": "wsl"}), encoding="utf-8"
        )

    def test_the_log_is_drained_from_the_distribution_before_it_is_removed(self):
        self.wsl.log = "\n".join([WRITE, UPLOADED, DOWNLOADED])
        self.wsl.alive = False
        with mock.patch.dict(remote.os.environ, {**CREDENTIALS, "PATH": "C:/Windows"}, clear=True):
            self.assertTrue(remote.stop(self.state))
        self.assertEqual(
            self.wsl.actions(),
            ["proxy log read", "proxy stop", "proxy liveness check", "unregister", "distribution list"],
        )
        self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)
        self.assertEqual(self.wsl.calls[1][1][-3:], ["kill", "-TERM", "77"])
        for _, _, environ, _ in self.wsl.calls:
            self.assertEqual(environ, {"PATH": "C:/Windows"})
        self.terminate.assert_not_called()
        self.assertIn("1 download(s), 0 miss(es), 1 of 1 queued upload(s)", self.stdout.getvalue())
        self.assertFalse((self.state / remote.SERVER_NAME).exists())
        report = self.state / "receipt.json"
        self.wsl.alive = False
        self.wsl.calls.clear()
        (self.state / remote.SERVER_NAME).write_text(
            json.dumps({"pid": 77, "upload": True, "key_prefix": remote.KEY_PREFIX, "host": "wsl"}),
            encoding="utf-8",
        )
        with mock.patch.dict(remote.os.environ, {"PATH": "C:/Windows"}, clear=True):
            self.assertTrue(remote.stop(self.state, report=report))
        self.assertEqual(
            json.loads(report.read_text(encoding="utf-8")),
            {
                "schemaVersion": 1,
                "bucket": "aobus-sccache",
                "keyPrefix": remote.KEY_PREFIX,
                "started": True,
                "upload": True,
                "activity": {
                    "queued": 1,
                    "uploaded": 1,
                    "pending": 0,
                    "uploadFailures": 0,
                    "downloaded": 1,
                    "downloadMisses": 0,
                },
                "lostUploads": 0,
            },
        )

    def test_a_vanished_distribution_is_still_unregistered(self):
        self.wsl.fail = {"proxy log read", "proxy stop"}
        with self.assertRaisesRegex(remote.RemoteCacheError, "WSL proxy log read failed"):
            remote.stop(self.state)
        self.assertEqual(self.wsl.actions(), ["proxy log read", "proxy stop", "unregister", "distribution list"])
        self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_a_signal_error_is_acceptable_only_after_verified_absence(self):
        self.wsl.fail = {"proxy stop"}
        report = self.state / "receipt.json"
        with mock.patch.dict(remote.os.environ, {"PATH": "C:/Windows"}, clear=True):
            self.assertTrue(remote.stop(self.state, report=report))
        self.assertNotIn("proxy liveness check", self.wsl.actions())
        self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)
        self.assertFalse((self.state / remote.SERVER_NAME).exists())
        self.assertTrue(json.loads(report.read_text(encoding="utf-8"))["started"])

    def test_a_still_live_proxy_is_acceptable_only_after_verified_absence(self):
        self.wsl.alive = True
        with mock.patch.dict(remote.os.environ, {"PATH": "C:/Windows"}, clear=True):
            self.assertTrue(remote.stop(self.state))
        self.assertEqual(self.wsl.actions().count("proxy liveness check"), 20)
        self.assertEqual(self.wsl.actions()[-2:], ["unregister", "distribution list"])
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_verified_absence_is_required_even_after_the_proxy_exits(self):
        self.wsl.alive = False
        self.wsl.sticky = True
        self.wsl.unregister_code = 0
        self.wsl.registered = {remote.WSL_DISTRO: "Running"}
        before = (self.state / remote.SERVER_NAME).read_text(encoding="utf-8")
        with self.assertRaisesRegex(remote.RemoteCacheError, "remains registered after unregister \\(exit code 0\\)"):
            remote.stop(self.state)
        self.assertEqual((self.state / remote.SERVER_NAME).read_text(encoding="utf-8"), before)
        self.assertIn("proxy liveness check", self.wsl.actions())
        self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)

    def test_failed_stop_cleanup_keeps_the_marker_and_writes_no_receipt(self):
        marker = {"host": "wsl", "upload": False, "key_prefix": remote.KEY_PREFIX}
        scenarios = {
            "still listed": {
                "sticky": True,
                "unregister_code": 1,
                "registered": {remote.WSL_DISTRO: "Uninstalling"},
            },
            "list failure": {"fail": {"distribution list"}},
            "timeout": {"timeouts": {"unregister"}},
        }
        for name, fields in scenarios.items():
            with self.subTest(name=name):
                self.wsl.calls.clear()
                self.wsl.fail = set()
                self.wsl.timeouts = set()
                self.wsl.sticky = False
                self.wsl.unregister_code = None
                self.wsl.registered = {}
                self.wsl.list_text = None
                for key, value in fields.items():
                    setattr(self.wsl, key, value)
                path = self.state / remote.SERVER_NAME
                path.write_text(json.dumps(marker), encoding="utf-8")
                report = self.state / f"{name.replace(' ', '-')}.json"
                stderr = io.StringIO()
                self.status.reset_mock()
                with (
                    mock.patch("sys.stderr", stderr),
                    mock.patch.dict(remote.os.environ, {**CREDENTIALS, "PATH": "C:/Windows"}, clear=True),
                    mock.patch.object(remote, "drain") as drain,
                ):
                    code = remote.main(["stop", "--state", str(self.state), "--report", str(report)])
                self.assertEqual(code, 1)
                drain.assert_not_called()
                self.status.assert_not_called()
                self.assertFalse(report.exists())
                self.assertEqual(json.loads(path.read_text(encoding="utf-8")), marker)
                self.assertNotIn("proxy log read", self.wsl.actions())
                self.assertNotIn("proxy stop", self.wsl.actions())
                self.assertNotIn("secret-key", stderr.getvalue())
                if name == "timeout":
                    self.assertNotIn("distribution list", self.wsl.actions())
                else:
                    self.assertEqual(self.wsl.calls[-1][1], _LIST_ARGUMENTS)


class ActivityTest(unittest.TestCase):
    def test_every_accepted_write_queues_one_upload(self):
        log = "\n".join([WRITE, AC_PUT, CAS_PUT, SKIPPED, CONTAINS, UPLOADED, UPLOAD_DENIED, DOWNLOADED, MISSED])
        self.assertEqual(
            remote.activity(log),
            remote.Activity(queued=3, uploaded=2, upload_failures=1, downloaded=1, download_misses=1),
        )
        self.assertEqual(remote.activity(log).pending, 1)

    def test_ac_and_cas_completions_with_the_same_digest_remain_distinct(self):
        result = remote.activity(f"{AC_PUT}\n{UPLOADED}")
        self.assertEqual((result.queued, result.uploaded, result.pending), (1, 0, 1))

    def test_compressed_bytestream_writes_with_instance_and_metadata_match_the_cas_object(self):
        write = WRITE.replace("uploads/1b4a/blobs/", "instance/uploads/1b4a/compressed-blobs/zstd/") + "/metadata"
        result = remote.activity(f"{UPLOADED}\n{write}")
        # Backend completion can precede the successful client-reply log line.
        self.assertEqual((result.queued, result.uploaded, result.pending), (1, 1, 0))

    def test_selected_prefix_matches_exactly_including_literal_dots_and_path_boundaries(self):
        prefix = "validation.ci/42"
        for other in ("validationXci/42", "validation.ci/420", "validation.ci/42/child", remote.KEY_PREFIX):
            log = "\n".join([WRITE, UPLOADED, DOWNLOADED, MISSED]).replace(remote.KEY_PREFIX, other)
            with self.subTest(other=other):
                result = remote.activity(log, key_prefix=prefix)
                self.assertEqual(result, remote.Activity(1, 0, 0, 0, 0))
                self.assertEqual(result.pending, 1)
        selected = "\n".join([WRITE, UPLOADED, DOWNLOADED, MISSED]).replace(remote.KEY_PREFIX, prefix)
        self.assertEqual(remote.activity(selected, key_prefix=prefix), remote.Activity(1, 1, 0, 1, 1))

    def test_other_prefix_upload_errors_do_not_contaminate_selected_evidence(self):
        prefix = "validation/42"
        selected = UPLOADED.replace(remote.KEY_PREFIX, prefix)
        result = remote.activity("\n".join([WRITE, selected, UPLOAD_DENIED]), key_prefix=prefix)
        self.assertEqual((result.pending, result.upload_failures), (0, 0))


class DrainTest(unittest.TestCase):
    def drain(
        self, logs: list[str], *, key_prefix: str = remote.KEY_PREFIX, **options: float
    ) -> tuple[remote.Activity, float]:
        now = [0.0]
        reads = iter(logs)
        latest = [logs[0]]

        def read_log() -> str:
            latest[0] = next(reads, latest[0])
            return latest[0]

        def sleep(seconds: float) -> None:
            now[0] += seconds

        result = remote.drain(read_log, key_prefix=key_prefix, clock=lambda: now[0], sleep=sleep, **options)
        return result, now[0]

    def test_waits_until_every_queued_write_is_uploaded(self):
        queued = "\n".join([WRITE, AC_PUT])
        result, elapsed = self.drain([queued, queued, f"{queued}\n{UPLOADED}", f"{queued}\n{UPLOADED}\n{AC_UPLOADED}"])
        self.assertEqual(result.pending, 0)
        self.assertEqual(elapsed, 3)

    def test_an_upload_without_a_successful_client_reply_cannot_complete_another_object(self):
        initial = "\n".join([CANCELLED, OTHER_UPLOADED, WRITE])
        result, elapsed = self.drain([initial, initial, f"{initial}\n{UPLOADED}"])
        self.assertEqual((result.queued, result.uploaded, result.pending), (1, 1, 0))
        self.assertEqual(elapsed, 2)

    def test_only_the_selected_prefix_can_finish_the_drain(self):
        prefix = "validation.ci/42"
        initial = f"{WRITE}\n{UPLOADED}"
        selected = UPLOADED.replace(remote.KEY_PREFIX, prefix)
        result, elapsed = self.drain([initial, f"{initial}\n{selected}"], key_prefix=prefix)
        self.assertEqual((result.queued, result.uploaded, result.pending), (1, 1, 0))
        self.assertEqual(elapsed, 1)

    def test_stalled_uploads_end_after_the_quiet_period(self):
        result, elapsed = self.drain([f"{WRITE}\n{AC_PUT}\n{UPLOADED}"], quiet=5)
        self.assertEqual(result.pending, 1)
        self.assertEqual(elapsed, 5)

    def test_progress_never_outlasts_the_timeout(self):
        logs = ["\n".join([WRITE] * 100 + [UPLOADED] * count) for count in range(100)]
        result, elapsed = self.drain(logs, timeout=10, quiet=5)
        self.assertGreater(result.pending, 0)
        self.assertEqual(elapsed, 10)


class StopTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.state = Path(temporary.name)
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.stdout = stack.enter_context(mock.patch("sys.stdout", new_callable=io.StringIO))
        self.terminate = stack.enter_context(mock.patch.object(remote, "_terminate"))
        stack.enter_context(mock.patch.object(remote, "_status", return_value=None))

    def stop(self, *, upload: bool, log: str, key_prefix: str | None = None, report: Path | None = None) -> bool:
        server = {"pid": 4242, "upload": upload}
        if key_prefix is not None:
            server["key_prefix"] = key_prefix
        (self.state / remote.SERVER_NAME).write_text(json.dumps(server), encoding="utf-8")
        (self.state / remote.LOG_NAME).write_text(log, encoding="utf-8")
        with mock.patch.object(remote, "drain", wraps=remote.drain) as drain:
            stopped = remote.stop(self.state, timeout=0, report=report)
        self.drained = drain.called
        return stopped

    def test_without_a_server_there_is_nothing_to_stop(self):
        self.assertTrue(remote.stop(self.state))
        self.terminate.assert_not_called()

    def test_upload_disabled_runs_do_not_wait_for_backend_writes(self):
        self.assertTrue(self.stop(upload=False, log="\n".join([WRITE, AC_PUT, DOWNLOADED])))
        self.assertFalse(self.drained)
        self.terminate.assert_called_once_with(4242)
        self.assertIn("1 download(s), 0 miss(es), uploads disabled", self.stdout.getvalue())
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_lost_or_failed_uploads_are_reported_after_stopping(self):
        report = self.state / "receipt.json"
        self.assertFalse(
            self.stop(upload=True, log="\n".join([WRITE, AC_PUT, CAS_PUT, UPLOADED, UPLOAD_DENIED]), report=report)
        )
        self.assertTrue(self.drained)
        self.terminate.assert_called_once_with(4242)
        self.assertIn("::warning::1 upload(s) were still queued and 1 failed", self.stdout.getvalue())
        receipt = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(receipt["keyPrefix"], remote.KEY_PREFIX)
        self.assertEqual(receipt["lostUploads"], 1)
        self.assertEqual(receipt["activity"]["pending"], 1)
        self.assertEqual(receipt["activity"]["uploadFailures"], 1)

    def test_an_unacknowledged_object_cannot_hide_a_lost_upload(self):
        self.assertFalse(self.stop(upload=True, log="\n".join([CANCELLED, OTHER_UPLOADED, WRITE])))
        self.terminate.assert_called_once_with(4242)
        self.assertIn("::warning::1 upload(s) were still queued", self.stdout.getvalue())

    def test_log_read_failure_still_cleans_up_the_owned_server(self):
        (self.state / remote.SERVER_NAME).write_text(json.dumps({"pid": 4242, "upload": True}), encoding="utf-8")
        with self.assertRaises(FileNotFoundError):
            remote.stop(self.state)
        self.terminate.assert_called_once_with(4242)
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_report_write_failure_occurs_after_owned_server_cleanup(self):
        not_directory = self.state / "not-a-directory"
        not_directory.write_text("fixture", encoding="utf-8")
        with self.assertRaises(OSError):
            self.stop(upload=True, log=f"{WRITE}\n{UPLOADED}", report=not_directory / "receipt.json")
        self.terminate.assert_called_once_with(4242)
        self.assertFalse((self.state / remote.SERVER_NAME).exists())

    def test_a_complete_drain_succeeds(self):
        self.assertTrue(self.stop(upload=True, log="\n".join([WRITE, UPLOADED])))
        self.assertIn("1 of 1 queued upload(s), 0 failed upload(s)", self.stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
