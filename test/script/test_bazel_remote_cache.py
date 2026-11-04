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
from ao.core import compiler_cache

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
    def manifest(self, root: Path, entry: dict[str, str]) -> Path:
        path = root / "bazel-remote.json"
        path.write_text(
            json.dumps({"schemaVersion": 1, "version": "2.6.4", "artifacts": {"linux-x86_64": entry}}),
            encoding="utf-8",
        )
        return path

    def test_every_pinned_artifact_downloads_the_manifest_version(self):
        manifest = json.loads(remote.MANIFEST_PATH.read_text(encoding="utf-8"))
        for runner in (("Linux", "X64"), ("macOS", "X64"), ("macOS", "ARM64")):
            with self.subTest(runner=runner):
                selected = remote.artifact(*runner)
                self.assertIn(f"/v{manifest['version']}/", selected.url)

    def test_windows_has_no_pinned_proxy(self):
        # bazel-remote refuses Windows builds upstream.
        with self.assertRaisesRegex(remote.RemoteCacheError, "no pinned bazel-remote for Windows X64"):
            remote.artifact("Windows", "X64")

    def test_windows_termination_refuses_unsupported_signals(self):
        with (
            mock.patch.object(remote.sys, "platform", "win32"),
            mock.patch.object(remote.os, "kill") as kill,
            self.assertRaisesRegex(remote.RemoteCacheError, "requires native Linux/macOS"),
        ):
            remote._terminate(4242)
        kill.assert_not_called()

    def test_entries_need_an_https_url_and_a_lowercase_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            for entry in (
                {"url": "http://example.com/bazel-remote", "sha256": HASH},
                {"url": "https://example.com/bazel-remote", "sha256": HASH.upper()},
                {"url": "https://example.com/bazel-remote"},
            ):
                with self.subTest(entry=entry), self.assertRaisesRegex(remote.RemoteCacheError, "https url"):
                    remote.artifact("Linux", "X64", self.manifest(Path(temporary), entry))


class EndpointTest(unittest.TestCase):
    def test_the_compiler_cache_secret_spellings_select_the_host(self):
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
        # The compiler cache's bucket, under the Bazel-only prefix.
        self.assertEqual(argv[argv.index("--s3.bucket") + 1], compiler_cache.R2_BUCKET)
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
        for prefix in (
            "",
            "/",
            "/absolute",
            ".",
            "..",
            "a/./b",
            "a/../b",
            "a//b",
            "a/",
            "a\\b",
            "a:b",
            "a b",
            "a\nb",
            "a[0]",
            "a?b",
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

    def test_stop_cli_reports_versioned_counts_for_the_recorded_prefix_only(self):
        prefix = "validation.ci/42"
        report = self.state / "reports/receipt.json"
        (self.state / remote.SERVER_NAME).write_text(
            json.dumps({"pid": 4242, "upload": True, "key_prefix": prefix}), encoding="utf-8"
        )
        selected = "\n".join([WRITE, UPLOADED, DOWNLOADED, MISSED]).replace(remote.KEY_PREFIX, prefix)
        (self.state / remote.LOG_NAME).write_text(f"{selected}\n{UPLOAD_DENIED}\n{DOWNLOADED}", encoding="utf-8")
        self.assertEqual(remote.main(["stop", "--state", str(self.state), "--report", str(report)]), 0)
        self.terminate.assert_called_once_with(4242)
        self.assertFalse((self.state / remote.SERVER_NAME).exists())
        self.assertIn(f"R2 aobus-sccache/{prefix}:", self.stdout.getvalue())
        self.assertEqual(
            json.loads(report.read_text(encoding="utf-8")),
            {
                "schemaVersion": 1,
                "bucket": "aobus-sccache",
                "keyPrefix": prefix,
                "started": True,
                "upload": True,
                "activity": {
                    "queued": 1,
                    "uploaded": 1,
                    "pending": 0,
                    "uploadFailures": 0,
                    "downloaded": 1,
                    "downloadMisses": 1,
                },
                "lostUploads": 0,
            },
        )

    def test_upload_disabled_consumer_reports_downloads_for_its_recorded_prefix(self):
        prefix = "validation.ci/42"
        report = self.state / "consumer.json"
        selected = DOWNLOADED.replace(remote.KEY_PREFIX, prefix)
        self.assertTrue(self.stop(upload=False, log=f"{selected}\n{DOWNLOADED}", key_prefix=prefix, report=report))
        receipt = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(receipt["upload"])
        self.assertEqual(receipt["activity"]["downloaded"], 1)
        self.assertEqual(receipt["lostUploads"], 0)
        self.assertFalse(self.drained)

    def test_no_server_report_does_not_claim_remote_cache_activity(self):
        report = self.state / "receipt.json"
        self.assertTrue(remote.stop(self.state, report=report))
        receipt = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(receipt["started"])
        self.assertFalse(receipt["upload"])
        self.assertEqual(receipt["keyPrefix"], remote.KEY_PREFIX)
        self.assertEqual(
            receipt["activity"],
            {
                "queued": 0,
                "uploaded": 0,
                "pending": 0,
                "uploadFailures": 0,
                "downloaded": 0,
                "downloadMisses": 0,
            },
        )
        self.assertEqual(receipt["lostUploads"], 0)
        self.terminate.assert_not_called()

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
