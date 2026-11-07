"""Contracts for the CI-only producer/fresh-consumer R2 proof, without R2 I/O."""

import errno
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from urllib.error import HTTPError

from ao.core import bazel_cache_probe as probe

CREDENTIALS = {
    "R2_ENDPOINT": "https://private-account.r2.cloudflarestorage.com",
    "R2_ACCESS_KEY_ID": "private-key-id",
    "R2_SECRET_ACCESS_KEY": "private-secret-key",
}
PREFIX = "bazel-ci-validation/42/3/proof"
TARGET = "//test/lint:cache_clean"
OUTPUT = "bazel-out/k8-dbg/bin/test/lint/_objs/cache_clean/CacheClean.pic.o"


def build_events():
    """Small independent Bazel 9 BEP fixture, including stream completion."""
    action_id = {"actionCompleted": {"label": TARGET, "primaryOutput": OUTPUT}}
    return [
        {
            "id": {"started": {}},
            "started": {"uuid": "current-build", "command": "build", "buildToolVersion": "9.2.0- (@non-git)"},
            "children": [action_id, {"buildFinished": {}}],
        },
        {"id": action_id, "action": {"type": "CppCompile", "label": TARGET, "success": True}},
        {"id": {"buildFinished": {}}, "finished": {"exitCode": {"name": "SUCCESS"}}, "lastMessage": True},
    ]


def spawn_entry(*, runner="remote cache hit", label=TARGET, mnemonic="CppCompile", cache_hit=True):
    # SpawnExec JSON, not an invented BEP cache field. Bazel's JSON writer
    # explicitly includes default fields and concatenates objects without separators.
    return {
        "mnemonic": mnemonic,
        "targetLabel": label,
        "runner": runner,
        "cacheHit": cache_hit,
        "status": "",
        "exitCode": 0,
    }


def write_spawns(path, spawns):
    path.write_text("".join(json.dumps(spawn, indent=2) for spawn in spawns), encoding="utf-8")


def write_events(path, events):
    path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")


def proxy_receipt(*, upload, downloaded=0):
    return {
        "schemaVersion": 1,
        "bucket": "aobus-sccache",
        "keyPrefix": PREFIX,
        "started": True,
        "upload": upload,
        "lostUploads": 0,
        "activity": {
            "queued": 2 if upload else 0,
            "uploaded": 2 if upload else 0,
            "pending": 0,
            "uploadFailures": 0,
            "downloaded": downloaded,
            "downloadMisses": 0,
        },
    }


class CompletedBuildEvidenceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.bep = Path(temporary.name) / "build.bep.json"

    def validate(self, events):
        write_events(self.bep, events)
        return probe.validate_bep(self.bep)

    def test_complete_successful_bep_is_independent_of_execution_cache_evidence(self):
        self.assertIsNone(self.validate(build_events()))

    def test_missing_truncated_and_nonobject_streams_are_not_evidence(self):
        with self.assertRaises(OSError):
            probe.validate_bep(self.bep)
        for text in ("", '{"id":', "[]\n", '{"id":{},"id":{}}\n'):
            with self.subTest(text=text):
                self.bep.write_text(text, encoding="utf-8")
                with self.assertRaises((ValueError, probe.ProofError)):
                    probe.validate_bep(self.bep)

    def test_last_message_and_successful_finished_event_are_both_required(self):
        no_last = build_events()
        del no_last[-1]["lastMessage"]
        no_finished = build_events()[:-1]
        no_finished[-1]["lastMessage"] = True
        early_last = build_events()
        early_last[0]["lastMessage"] = True
        failed = build_events()
        failed[-1]["finished"]["exitCode"] = {"name": "BUILD_FAILURE", "code": 1}
        false_success = build_events()
        false_success[-1]["finished"]["overallSuccess"] = False
        for events in (no_last, no_finished, early_last, failed, false_success):
            with self.subTest(events=events), self.assertRaises(probe.ProofError):
                self.validate(events)

    def test_missing_announced_or_duplicate_events_fail_even_with_a_last_message(self):
        missing = build_events()
        missing[0]["children"].append({"progress": {"opaqueCount": 7}})
        duplicate = build_events()
        duplicate.insert(2, duplicate[1])
        aborted = build_events()
        aborted[1]["aborted"] = {"reason": "INCOMPLETE"}
        for events in (missing, duplicate, aborted):
            with self.subTest(events=events), self.assertRaises(probe.ProofError):
                self.validate(events)

    def test_another_bazel_version_or_nonbuild_invocation_is_not_current_evidence(self):
        for update in ({"buildToolVersion": "8.4.0"}, {"command": "aquery"}, {"uuid": ""}):
            events = build_events()
            events[0]["started"].update(update)
            with self.subTest(update=update), self.assertRaises(probe.ProofError):
                self.validate(events)


class SpawnEvidenceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.log = Path(temporary.name) / "spawns.json"

    def hits(self, spawns):
        write_spawns(self.log, spawns)
        return probe.compile_hits(self.log)

    def test_pretty_object_sequence_selects_only_native_compile_remote_hits(self):
        self.assertEqual(
            self.hits([spawn_entry(mnemonic="AobusLint"), spawn_entry(label="@@//test/lint:cache_clean")]),
            [
                {
                    "targetLabel": "@@//test/lint:cache_clean",
                    "mnemonic": "CppCompile",
                    "runner": "remote cache hit",
                    "cacheHit": True,
                    "exitCode": 0,
                }
            ],
        )

    def test_local_disk_unrelated_or_failed_hits_cannot_satisfy_evidence(self):
        cases = [
            spawn_entry(runner="local"),
            spawn_entry(runner="disk cache hit"),
            spawn_entry(runner="remote"),
            spawn_entry(label="//other:library"),
            spawn_entry(mnemonic="AobusLint"),
            spawn_entry(cache_hit=False),
            {**spawn_entry(), "status": "NON_ZERO_EXIT"},
            {**spawn_entry(), "exitCode": 1},
            {**spawn_entry(), "exitCode": False},
            {**spawn_entry(), "cacheHit": 1},
            {"mnemonic": "CppCompile", "targetLabel": TARGET, "remoteCacheHit": True},
        ]
        for spawn in cases:
            with self.subTest(spawn=spawn):
                self.assertEqual(self.hits([spawn]), [])

    def test_missing_empty_truncated_or_nonobject_execution_logs_fail(self):
        with self.assertRaises(OSError):
            probe.compile_hits(self.log)
        for text in ("", " \n", '{"mnemonic":', "[]\n", '{"runner":"local","runner":"remote cache hit"}'):
            with self.subTest(text=text):
                self.log.write_text(text, encoding="utf-8")
                with self.assertRaises((ValueError, probe.ProofError)):
                    probe.compile_hits(self.log)

    def test_truncated_trailing_spawn_invalidates_an_earlier_valid_hit(self):
        write_spawns(self.log, [spawn_entry()])
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write('{"mnemonic":')
        with self.assertRaises(ValueError):
            probe.compile_hits(self.log)


class LiveBoundary:
    """Discriminate the proxy and portal boundary; keep all proof logic real."""

    def __init__(self, test, root):
        self.test = test
        self.root = root
        self.active = None
        self.calls = []
        self.exits = {"producer": 0, "consumer": 0}
        self.events = {"producer": build_events(), "consumer": build_events()}
        self.spawns = {"producer": [spawn_entry(runner="local", cache_hit=False)], "consumer": [spawn_entry()]}
        self.reports = {"producer": proxy_receipt(upload=True), "consumer": proxy_receipt(upload=False, downloaded=3)}
        self.stop_results = {"producer": True, "consumer": True}
        self.start_url = "grpc://127.0.0.1:9092"
        self.start_error = None
        self.build_error = None
        self.stop_error = None
        self.environments = []
        self.failures = []

    def checked(self, method):
        def invoke(*args, **kwargs):
            try:
                return method(*args, **kwargs)
            except AssertionError as exc:
                # Production intentionally sanitizes external exceptions. Do not
                # let that hide a rejected fake invocation in a negative test.
                self.failures.append(str(exc))
                raise

        return invoke

    def start(self, state, output, runner_os, runner_arch, *, upload, key_prefix, environ):
        phase = state.parent.name
        self.test.assertIn(phase, ("producer", "consumer"))
        self.test.assertEqual(state, self.root / phase / "proxy")
        self.test.assertEqual(output, self.root / phase / "proxy.output")
        self.test.assertEqual((runner_os, runner_arch), ("Linux", "X64"))
        self.test.assertEqual(upload, phase == "producer")
        self.test.assertEqual(key_prefix, PREFIX)
        self.test.assertIsNone(self.active)
        self.test.assertEqual(list(state.iterdir()), [])
        self.test.assertEqual(list((state.parent / "client-state").iterdir()), [])
        self.test.assertFalse((state.parent / "build").exists())
        initial = json.loads((state.parent / "receipt.json").read_text(encoding="utf-8"))
        self.test.assertIsNone(initial["buildExit"])
        self.test.assertIsNone(initial["finishedAt"])
        self.test.assertTrue(initial["startedAt"])
        for key, value in CREDENTIALS.items():
            self.test.assertEqual(environ[key], value)
        (state / "data").mkdir()
        (state / "data" / "owned-marker").write_text(phase, encoding="utf-8")
        self.active = phase
        self.calls.append(("start", phase, upload))
        if self.start_error is not None:
            raise self.start_error
        if self.start_url is not None:
            output.write_text(f"url={self.start_url}\n", encoding="utf-8")

    def run(self, argv, *, cwd, env, log):
        phase = self.active
        self.test.assertIn(phase, ("producer", "consumer"))
        directory = self.root / phase
        self.test.assertEqual(
            argv,
            [
                str(probe.PROJECT_ROOT / "ao"),
                "bazel",
                "build",
                "--path",
                str(directory / "build"),
                "--jobs",
                "2",
                "--",
                "//test/lint:cache_clean",
                f"--build_event_json_file={directory / 'build.bep.json'}",
                "--build_event_publish_all_actions",
                "--experimental_build_event_upload_strategy=local",
                "--remote_cache_async=false",
                "--remote_download_outputs=all",
                f"--execution_log_json_file={directory / 'spawns.json'}",
            ]
            + (["--noremote_upload_local_results"] if phase == "consumer" else []),
        )
        self.test.assertEqual(cwd, probe.PROJECT_ROOT)
        self.test.assertEqual(log, directory / "build.log")
        self.test.assertEqual(env["AOBUS_STATE_ROOT"], str(directory / "client-state"))
        self.test.assertEqual(env["AOBUS_BAZEL_REMOTE_CACHE"], "grpc://127.0.0.1:9092")
        self.test.assertEqual(dict(os.environ), env)
        self.test.assertFalse([key for key in env if key.startswith(("R2_", "AWS_", "BAZEL_REMOTE_S3_"))])
        self.test.assertNotIn("GITHUB_OUTPUT", env)
        self.environments.append(dict(env))
        persisted = json.loads((directory / "receipt.json").read_text(encoding="utf-8"))
        self.test.assertTrue(persisted["buildStartedAt"])
        self.test.assertIsNone(persisted["buildExit"])
        (directory / "build").mkdir()
        (directory / "client-state" / "client-marker").write_text(phase, encoding="utf-8")
        log.write_text("remote cache hit in stdout is not evidence\n", encoding="utf-8")
        events = self.events[phase]
        if events is not None:
            write_events(directory / "build.bep.json", events)
        spawns = self.spawns[phase]
        if spawns is not None:
            write_spawns(directory / "spawns.json", spawns)
        self.calls.append(("build", phase))
        if self.build_error is not None:
            raise self.build_error
        return self.exits[phase]

    def stop(self, state, *, report):
        phase = state.parent.name
        self.test.assertEqual(state, self.root / phase / "proxy")
        self.test.assertEqual(report, self.root / phase / "proxy.receipt.json")
        self.test.assertEqual(self.active, phase)
        persisted = json.loads((state.parent / "receipt.json").read_text(encoding="utf-8"))
        if any(call == ("build", phase) for call in self.calls) and self.build_error is None:
            self.test.assertEqual(persisted["buildExit"], self.exits[phase])
            self.test.assertTrue(persisted["buildFinishedAt"])
        self.active = None
        self.calls.append(("stop", phase))
        if self.stop_error is not None:
            raise self.stop_error
        report.write_text(json.dumps(self.reports[phase]), encoding="utf-8")
        return self.stop_results[phase]


class ProofLifecycleTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.parent = Path(temporary.name)
        self.root = self.parent / "proof"
        github_output = self.parent / "real-github-output"
        github_output.write_text("untouched\n", encoding="utf-8")
        self.environment = {
            **CREDENTIALS,
            "PATH": os.environ.get("PATH", ""),
            "GITHUB_OUTPUT": str(github_output),
            "AWS_SESSION_TOKEN": "private-aws-token",
            "BAZEL_REMOTE_S3_ENDPOINT": "private-override",
            "R2_OTHER_SETTING": "private-other",
            "AOBUS_STATE_ROOT": "/previous/client",
            "AOBUS_BAZEL_REMOTE_CACHE": "grpc://previous-proxy:9092",
        }
        self.enterContext(mock.patch.dict(os.environ, self.environment, clear=True))
        self.stderr = self.enterContext(mock.patch("sys.stderr", new_callable=io.StringIO))
        self.enterContext(mock.patch.object(probe.platform, "system", return_value="Linux"))
        self.enterContext(mock.patch.object(probe.platform, "machine", return_value="x86_64"))
        self.enterContext(mock.patch.object(probe.sys, "platform", "linux"))
        self.boundary = LiveBoundary(self, self.root)
        self.start = self.enterContext(
            mock.patch.object(probe.cache, "start", side_effect=self.boundary.checked(self.boundary.start))
        )
        self.stop = self.enterContext(
            mock.patch.object(probe.cache, "stop", side_effect=self.boundary.checked(self.boundary.stop))
        )
        self.enterContext(mock.patch.object(probe.proc, "run", side_effect=self.boundary.checked(self.boundary.run)))
        self.install = self.enterContext(
            mock.patch.object(probe.cache, "install", side_effect=AssertionError("install"))
        )

    def tearDown(self):
        self.assertEqual(self.boundary.failures, [])

    def prove(self, **kwargs):
        return probe.prove(self.root, jobs=2, key_prefix=PREFIX, **kwargs)

    def receipt(self, phase=None):
        return json.loads(
            (self.root / phase if phase else self.root).joinpath("receipt.json").read_text(encoding="utf-8")
        )

    def test_producer_then_empty_consumer_use_separate_clients_builds_and_proxy_data(self):
        self.assertEqual(self.prove(), 0)
        self.assertEqual(
            self.boundary.calls,
            [
                ("start", "producer", True),
                ("build", "producer"),
                ("stop", "producer"),
                ("start", "consumer", False),
                ("build", "consumer"),
                ("stop", "consumer"),
            ],
        )
        for phase in ("producer", "consumer"):
            receipt = self.receipt(phase)
            self.assertEqual(receipt["buildExit"], 0)
            self.assertIs(receipt["proxyStopped"], True)
            self.assertIs(receipt["success"], True)
            self.assertTrue(receipt["finishedAt"])
            self.assertEqual((self.root / phase / "proxy/data/owned-marker").read_text(encoding="utf-8"), phase)
            self.assertTrue((self.root / phase / "build.log").is_file())
            self.assertEqual(receipt["buildLog"], "build.log")
            self.assertEqual(receipt["portalBuildLog"], "build/build.log")
        receipt = self.receipt()
        self.assertEqual(receipt["schemaVersion"], 1)
        self.assertEqual(receipt["scope"], "same-runner-native-linux")
        self.assertIs(receipt["success"], True)
        self.assertEqual(len(receipt["phases"]["consumer"]["nativeCompileRemoteHits"]), 1)
        for secret in (*CREDENTIALS.values(), "private-aws-token", "private-override", "private-other"):
            self.assertNotIn(secret, json.dumps(receipt))
        self.assertEqual(dict(os.environ), self.environment)
        self.assertEqual((self.parent / "real-github-output").read_text(encoding="utf-8"), "untouched\n")

    def test_every_r2_credential_is_required_before_creating_state(self):
        for missing in CREDENTIALS:
            environment = {key: value for key, value in self.environment.items() if key != missing}
            with self.subTest(missing=missing), self.assertRaisesRegex(probe.ProofError, "complete R2 credentials"):
                self.prove(environ=environment)
            self.assertFalse(self.root.exists())
        self.start.assert_not_called()
        self.stop.assert_not_called()
        self.install.assert_not_called()

    def test_invalid_prefix_is_rejected_by_the_helper_before_any_side_effect(self):
        for prefix in ("../escape", "/absolute", "a//b", "a/./b", "a\nsecret"):
            with self.subTest(prefix=prefix), self.assertRaises(probe.cache.RemoteCacheError):
                probe.prove(self.root, jobs=2, key_prefix=prefix)
        self.assertFalse(self.root.exists())
        self.start.assert_not_called()
        self.install.assert_not_called()

    def test_non_linux_and_unsupported_native_hosts_do_not_create_state(self):
        with mock.patch.object(probe.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(probe.ProofError, "native Linux"):
                self.prove()
        with mock.patch.object(probe.platform, "machine", return_value="aarch64"):
            with self.assertRaises(probe.cache.RemoteCacheError):
                self.prove()
        self.assertFalse(self.root.exists())
        self.start.assert_not_called()

    def test_nonpositive_job_count_is_rejected_before_creating_state(self):
        for jobs in (0, -1):
            with self.subTest(jobs=jobs), self.assertRaisesRegex(probe.ProofError, "jobs must be positive"):
                probe.prove(self.root, jobs=jobs, key_prefix=PREFIX)
        self.assertFalse(self.root.exists())
        self.start.assert_not_called()

    def test_existing_root_is_preserved_instead_of_reused_or_deleted(self):
        self.root.mkdir()
        marker = self.root / "old-cache"
        marker.write_text("keep", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            self.prove()
        self.assertEqual(list(self.root.iterdir()), [marker])
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
        self.start.assert_not_called()

    def test_producer_native_failure_preserves_numeric_exit_log_and_cleanup(self):
        self.boundary.exits["producer"] = 7
        self.assertEqual(probe.main(["--path", str(self.root), "--jobs", "2", "--key-prefix", PREFIX]), 7)
        self.assertEqual(self.receipt("producer")["buildExit"], 7)
        self.assertEqual(self.receipt("producer")["failure"], {"operation": "build", "exceptionType": "ProofError"})
        self.assertEqual(self.receipt("producer")["error"], "native build failed")
        self.assertIs(self.receipt("producer")["proxyStopped"], True)
        self.assertFalse((self.root / "consumer").exists())
        self.assertTrue((self.root / "producer/build.log").is_file())
        self.assertIs(self.receipt()["success"], False)

    def test_unfinished_or_failed_uploads_prevent_starting_a_consumer(self):
        for changes in ({"pending": 1, "uploaded": 1}, {"uploadFailures": 1}, {"queued": 0, "uploaded": 0}):
            with self.subTest(changes=changes):
                with tempfile.TemporaryDirectory() as temporary:
                    self.root = Path(temporary) / "proof"
                    self.boundary.root = self.root
                    self.boundary.reports["producer"] = proxy_receipt(upload=True)
                    self.boundary.reports["producer"]["activity"].update(changes)
                    self.assertEqual(self.prove(), 1)
                    self.assertFalse((self.root / "consumer").exists())
                    self.assertIs(self.receipt("producer")["success"], False)

    def test_stop_false_prevents_a_consumer_even_if_the_receipt_claims_completed_uploads(self):
        self.boundary.stop_results["producer"] = False
        self.assertEqual(self.prove(), 1)
        self.assertFalse((self.root / "consumer").exists())
        self.assertIs(self.receipt("producer")["proxyStopped"], False)
        self.assertEqual(self.receipt("producer")["failure"]["operation"], "verifyProxy")

    def test_consumer_failure_still_stops_its_own_proxy_and_preserves_exit(self):
        self.boundary.exits["consumer"] = 9
        self.assertEqual(self.prove(), 9)
        self.assertEqual(self.receipt("consumer")["buildExit"], 9)
        self.assertEqual(self.boundary.calls[-1], ("stop", "consumer"))
        self.assertIs(self.receipt()["success"], False)

    def test_local_or_unrelated_hits_fail_even_with_downloads_and_hit_stdout(self):
        for spawn in (
            spawn_entry(runner="local"),
            spawn_entry(mnemonic="AobusLint"),
            spawn_entry(label="//other:library"),
        ):
            with self.subTest(spawn=spawn), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary) / "proof"
                self.boundary.root = self.root
                self.boundary.spawns["consumer"] = [spawn]
                self.assertEqual(self.prove(), 1)
                self.assertIs(self.receipt("consumer")["success"], False)
                self.assertEqual(self.receipt("consumer")["failure"]["operation"], "verifyExecution")
                self.assertEqual(
                    self.receipt("consumer")["error"], "consumer has no native CppCompile remote cache hit"
                )
                self.assertEqual(self.boundary.calls[-1], ("stop", "consumer"))

    def test_missing_or_incomplete_consumer_bep_cannot_reuse_producer_evidence(self):
        for events in (None, build_events()[:-1]):
            with self.subTest(events=events), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary) / "proof"
                self.boundary.root = self.root
                self.boundary.events["consumer"] = events
                self.assertEqual(self.prove(), 1)
                self.assertIs(self.receipt("consumer")["success"], False)
                self.assertEqual(self.receipt("consumer")["failure"]["operation"], "verifyBep")
                self.assertEqual(self.boundary.calls[-1], ("stop", "consumer"))

    def test_missing_consumer_execution_log_is_not_replaced_by_bep_or_stdout(self):
        self.boundary.spawns["consumer"] = None
        self.assertEqual(self.prove(), 1)
        self.assertIs(self.receipt("consumer")["success"], False)
        self.assertEqual(self.boundary.calls[-1], ("stop", "consumer"))

    def test_proxy_receipt_must_identify_the_owned_started_phase(self):
        for changes in (
            {"keyPrefix": "other/prefix"},
            {"started": False},
            {"bucket": "other-bucket"},
            {"upload": True},
            {"schemaVersion": 2},
        ):
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary) / "proof"
                self.boundary.root = self.root
                self.boundary.reports["consumer"] = proxy_receipt(upload=False, downloaded=3)
                self.boundary.reports["consumer"].update(changes)
                self.assertEqual(self.prove(), 1)
                self.assertIs(self.receipt("consumer")["success"], False)

    def test_start_failure_still_stops_owned_proxy_without_leaking_backend_errors(self):
        self.boundary.start_error = probe.cache.RemoteCacheError(CREDENTIALS["R2_ENDPOINT"])
        self.assertEqual(self.prove(), 1)
        self.assertEqual(self.boundary.calls, [("start", "producer", True), ("stop", "producer")])
        self.assertIsNone(self.receipt("producer")["buildExit"])
        self.assertNotIn(CREDENTIALS["R2_ENDPOINT"], json.dumps(self.receipt()))

    def test_start_failures_distinguish_address_errors_from_http_errors_without_secret_text(self):
        private = "private-url-header-credential-body"
        address = OSError(errno.EADDRINUSE, private, CREDENTIALS["R2_ENDPOINT"])
        wrapped_address = probe.cache.RemoteCacheError(private)
        wrapped_address.__cause__ = address
        http = HTTPError(CREDENTIALS["R2_ENDPOINT"], 403, private, {"Authorization": private}, None)
        wrapped_http = probe.cache.RemoteCacheError(private)
        wrapped_http.__cause__ = http
        for error, expected in (
            (address, {"exceptionType": "OSError", "errno": errno.EADDRINUSE}),
            (wrapped_address, {"exceptionType": "RemoteCacheError", "causeType": "OSError", "errno": errno.EADDRINUSE}),
            (http, {"exceptionType": "HTTPError", "httpStatus": 403}),
            (wrapped_http, {"exceptionType": "RemoteCacheError", "causeType": "HTTPError", "httpStatus": 403}),
        ):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary) / "proof"
                self.boundary.root = self.root
                self.boundary.start_error = error
                self.stderr.seek(0)
                self.stderr.truncate()
                self.assertEqual(self.prove(), 1)
                receipt = self.receipt("producer")
                self.assertEqual(receipt["failure"], {"operation": "startProxy", **expected})
                self.assertIsNone(receipt["buildExit"])
                self.assertIs(receipt["proxyStopped"], True)
                summary = self.stderr.getvalue()
                self.assertIn("R2 cache proof producer failed: operation=startProxy", summary)
                for key, value in expected.items():
                    self.assertIn(f"{key}={value}", summary)
                for secret in (private, *CREDENTIALS.values()):
                    self.assertNotIn(secret, json.dumps(self.receipt()))
                    self.assertNotIn(secret, summary)

    def test_start_operation_and_category_survive_a_different_cleanup_failure(self):
        address = OSError(errno.EADDRINUSE, CREDENTIALS["R2_ENDPOINT"])
        self.boundary.start_error = probe.cache.RemoteCacheError(CREDENTIALS["R2_SECRET_ACCESS_KEY"])
        self.boundary.start_error.__cause__ = address
        self.boundary.stop_error = HTTPError(CREDENTIALS["R2_ENDPOINT"], 502, "private-http-error", {}, None)
        self.assertEqual(self.prove(), 1)
        receipt = self.receipt("producer")
        self.assertEqual(
            receipt["failure"],
            {
                "operation": "startProxy",
                "exceptionType": "RemoteCacheError",
                "causeType": "OSError",
                "errno": errno.EADDRINUSE,
            },
        )
        self.assertEqual(
            receipt["cleanupFailure"], {"operation": "stopProxy", "exceptionType": "HTTPError", "httpStatus": 502}
        )
        self.assertIsNone(receipt["buildExit"])
        self.assertIn("operation=startProxy exceptionType=RemoteCacheError", self.stderr.getvalue())
        self.assertIn("cleanup operation=stopProxy exceptionType=HTTPError httpStatus=502", self.stderr.getvalue())
        for secret in (*CREDENTIALS.values(), "private-http-error"):
            self.assertNotIn(secret, json.dumps(self.receipt()))
            self.assertNotIn(secret, self.stderr.getvalue())

    def test_failure_metadata_does_not_inspect_implicit_exception_context(self):
        error = RuntimeError(CREDENTIALS["R2_SECRET_ACCESS_KEY"])
        error.__context__ = HTTPError(CREDENTIALS["R2_ENDPOINT"], 401, "private", {}, None)
        self.boundary.start_error = error
        self.assertEqual(self.prove(), 1)
        self.assertEqual(
            self.receipt("producer")["failure"], {"operation": "startProxy", "exceptionType": "RuntimeError"}
        )
        self.assertNotIn(CREDENTIALS["R2_SECRET_ACCESS_KEY"], self.stderr.getvalue())
        self.assertNotIn("httpStatus", self.stderr.getvalue())

    def test_execution_hits_without_backend_downloads_fail(self):
        self.boundary.reports["consumer"]["activity"]["downloaded"] = 0
        self.assertEqual(self.prove(), 1)
        self.assertIs(self.receipt("consumer")["success"], False)

    def test_consumer_uploads_fail_even_if_the_uploader_claims_to_be_disabled(self):
        self.boundary.reports["consumer"]["activity"].update({"queued": 1, "uploaded": 1})
        self.assertEqual(self.prove(), 1)
        self.assertIs(self.receipt("consumer")["success"], False)

    def test_missing_or_non_loopback_url_has_no_local_or_previous_proxy_fallback(self):
        for url in (None, "grpc://previous-proxy:9092"):
            with self.subTest(url=url), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary) / "proof"
                self.boundary.root = self.root
                self.boundary.start_url = url
                self.assertEqual(self.prove(), 1)
                self.assertIsNone(self.receipt("producer")["buildExit"])
                self.assertEqual(self.receipt("producer")["failure"]["operation"], "readProxyUrl")
                self.assertEqual(self.boundary.calls[-1], ("stop", "producer"))
                self.assertFalse((self.root / "producer/build").exists())

    def test_unknown_native_exit_is_not_invented_and_client_environment_is_restored(self):
        self.boundary.build_error = ChildProcessError("status unavailable")
        self.assertEqual(self.prove(), 1)
        self.assertIsNone(self.receipt("producer")["buildExit"])
        self.assertIs(self.receipt("producer")["proxyStopped"], True)
        self.assertEqual(
            self.receipt("producer")["failure"], {"operation": "build", "exceptionType": "ChildProcessError"}
        )
        self.assertEqual(self.receipt("producer")["portalBuildLog"], "build/build.log")
        self.assertFalse((self.root / "producer/build/build.log").exists())
        self.assertEqual(dict(os.environ), self.environment)

    def test_interrupt_records_unknown_exit_and_stops_proxy_after_foreground_settlement(self):
        self.boundary.build_error = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.prove()
        self.assertIsNone(self.receipt("producer")["buildExit"])
        self.assertIs(self.receipt("producer")["proxyStopped"], True)
        self.assertTrue(self.receipt("producer")["finishedAt"])
        self.assertTrue(self.receipt()["finishedAt"])
        self.assertEqual(
            self.receipt("producer")["failure"], {"operation": "build", "exceptionType": "KeyboardInterrupt"}
        )
        self.assertEqual(self.boundary.calls[-1], ("stop", "producer"))
        self.assertEqual(dict(os.environ), self.environment)

    def test_cleanup_failure_does_not_replace_native_failure_or_leak_backend_errors(self):
        self.boundary.exits["producer"] = 7
        self.boundary.stop_error = probe.cache.RemoteCacheError(CREDENTIALS["R2_ENDPOINT"])
        self.assertEqual(self.prove(), 7)
        receipt = self.receipt("producer")
        self.assertEqual(receipt["cleanupError"], "proxy stop failed")
        self.assertEqual(receipt["failure"], {"operation": "build", "exceptionType": "ProofError"})
        self.assertEqual(receipt["cleanupFailure"], {"operation": "stopProxy", "exceptionType": "RemoteCacheError"})
        self.assertNotIn(CREDENTIALS["R2_ENDPOINT"], json.dumps(self.receipt()))
        self.assertNotIn(CREDENTIALS["R2_ENDPOINT"], self.stderr.getvalue())

    def test_cli_missing_credentials_fails_without_printing_any_values(self):
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch("sys.stderr", new_callable=io.StringIO) as stderr:
            self.assertEqual(probe.main(["--path", str(self.root), "--jobs", "2", "--key-prefix", PREFIX]), 1)
        self.assertIn("complete R2 credentials", stderr.getvalue())
        self.assertFalse(self.root.exists())
        self.start.assert_not_called()


@unittest.skipUnless(sys.platform == "linux", "native Linux process boundary")
class ClientProcessTest(unittest.TestCase):
    def test_real_foreground_client_inherits_no_backend_credentials_and_has_its_own_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            phase = root / "phase"
            phase.mkdir()
            client = root / "ao"
            client.write_text(
                f"#!{sys.executable}\n"
                "import json, os, sys\n"
                "assert sys.argv[1:3] == ['bazel', 'build']\n"
                "assert '//test/lint:cache_clean' in sys.argv\n"
                "print(json.dumps(dict(os.environ)))\n",
                encoding="utf-8",
            )
            client.chmod(0o755)
            original = {**CREDENTIALS, "AWS_SESSION_TOKEN": "private-token", "BAZEL_REMOTE_S3_REGION": "auto"}
            environment = probe._client_environment(original, phase / "client-state", "grpc://127.0.0.1:9092")
            with mock.patch.object(probe, "PROJECT_ROOT", root), mock.patch.dict(os.environ, original, clear=True):
                with io.TextIOWrapper(io.BytesIO()) as stdout, mock.patch("sys.stdout", stdout):
                    self.assertEqual(probe._build(phase, 2, environment), 0)
                self.assertEqual(dict(os.environ), original)
            received = json.loads((phase / "build.log").read_text(encoding="utf-8"))
            self.assertFalse([key for key in received if key.startswith(("R2_", "AWS_", "BAZEL_REMOTE_S3_"))])
            self.assertEqual(received["AOBUS_STATE_ROOT"], str(phase / "client-state"))
            self.assertEqual(received["AOBUS_BAZEL_REMOTE_CACHE"], "grpc://127.0.0.1:9092")


if __name__ == "__main__":
    unittest.main()
