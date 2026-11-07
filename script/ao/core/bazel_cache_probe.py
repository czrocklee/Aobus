"""CI-only, same-runner native Linux proof of R2 writes and fresh-client reads.

Run only from a trusted checkout with step-local R2 credentials. Separate object
prefixes and disabled consumer uploaders are behavior, not backend authorization.
Publish only receipts, BEPs, execution logs and build logs; raw proxy logs can
contain endpoints.
This deliberately makes no cross-machine cache-portability claim.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import sys
from collections.abc import Mapping, Sequence
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from urllib.error import HTTPError

from . import bazel_remote_cache as cache
from . import proc
from .compiler_cache import R2_BUCKET, r2_credentials
from .paths import PROJECT_ROOT

TARGET = "//test/lint:cache_clean"
REMOTE_CACHE_ENVIRONMENT = "AOBUS_BAZEL_REMOTE_CACHE"


class ProofError(RuntimeError):
    """Missing or inconsistent proof evidence; messages must not contain secrets."""


def _now() -> str:
    return datetime.now(UTC).isoformat()


def _write_receipt(path: Path, value: Mapping[str, Any]) -> None:
    temporary = path.with_suffix(".json.tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def _failure_details(operation: str, error: BaseException) -> dict[str, str | int]:
    """Keep categories and numeric codes only, never exception messages or URLs."""
    details: dict[str, str | int] = {"operation": operation, "exceptionType": type(error).__name__}
    cause = error.__cause__
    if cause is not None:
        details["causeType"] = type(cause).__name__
    # Only an explicit cause belongs to this failure; do not inspect implicit
    # context, HTTP headers/body, URLError.reason or other arbitrary payloads.
    for exception in (error, cause):
        if isinstance(exception, HTTPError) and type(exception.code) is int and 100 <= exception.code <= 599:
            details.setdefault("httpStatus", exception.code)
        if isinstance(exception, OSError) and type(exception.errno) is int:
            details.setdefault("errno", exception.errno)
    return details


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProofError("duplicate JSON field")
        result[key] = value
    return result


def _object(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ProofError("expected a JSON object")
    return value


def _event_key(value: Any) -> str:
    identity = _object(value)
    if len(identity) != 1 or not isinstance(next(iter(identity.values())), dict):
        raise ProofError("invalid BEP event identity")
    return json.dumps(identity, sort_keys=True)


def validate_bep(bep: Path) -> None:
    """Require a complete, successful BEP from the current pinned Bazel build."""
    events = [
        _object(json.loads(line, object_pairs_hook=_unique_object))
        for line in bep.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if not events or events[-1].get("lastMessage") is not True or any(e.get("lastMessage") for e in events[:-1]):
        raise ProofError("BEP is incomplete")
    if events[0].get("id") != {"started": {}}:
        raise ProofError("BEP has no build start")
    started = _object(events[0].get("started"))
    version = (PROJECT_ROOT / ".bazelversion").read_text(encoding="utf-8").strip()
    tool_version = started.get("buildToolVersion", "")
    if (
        not isinstance(tool_version, str)
        or tool_version.split("-", 1)[0] != version
        or started.get("command") != "build"
        or not isinstance(started.get("uuid"), str)
        or not started["uuid"]
    ):
        raise ProofError("BEP is not a current pinned Bazel build")
    identities: set[str] = set()
    announced: set[str] = set()
    finished: list[dict[str, Any]] = []
    for event in events:
        identity = _object(event.get("id"))
        key = _event_key(identity)
        if key in identities:
            raise ProofError("duplicate BEP event identity")
        identities.add(key)
        children = event.get("children", [])
        if not isinstance(children, list):
            raise ProofError("invalid BEP children")
        announced.update(_event_key(child) for child in children)
        if "aborted" in event:
            raise ProofError("aborted BEP event")
        if "finished" in event:
            if identity != {"buildFinished": {}}:
                raise ProofError("invalid BEP completion identity")
            finished.append(_object(event["finished"]))
    if announced - identities:
        raise ProofError("BEP is missing announced events")
    if len(finished) != 1:
        raise ProofError("BEP has no unique completion")
    exit_code = _object(finished[0].get("exitCode"))
    if (
        exit_code.get("name") != "SUCCESS"
        or type(exit_code.get("code", 0)) is not int
        or exit_code.get("code", 0) != 0
        or finished[0].get("overallSuccess", True) is not True
    ):
        raise ProofError("BEP build did not succeed")


def compile_hits(log: Path) -> list[dict[str, Any]]:
    """Read Bazel 9 SpawnExec's sequence of pretty-printed JSON objects.

    The pinned BEP ActionExecuted payload has no cache-hit field and its
    strategy_details is not populated by ActionExecutedEvent. The execution
    log's SpawnExec (src/main/protobuf/spawn.proto in Bazel 9.2.0) supplies the
    structured runner and cache_hit evidence instead. Disk hits are not R2 hits.
    """
    text = log.read_text(encoding="utf-8")
    decoder = json.JSONDecoder(object_pairs_hook=_unique_object)
    position = count = 0
    hits: list[dict[str, Any]] = []
    while position < len(text):
        if text[position].isspace():
            position += 1
            continue
        value, position = decoder.raw_decode(text, position)
        spawn = _object(value)
        count += 1
        if (
            spawn.get("mnemonic") == "CppCompile"
            and spawn.get("targetLabel") in (TARGET, "@@" + TARGET)
            and spawn.get("runner") == "remote cache hit"
            and spawn.get("cacheHit") is True
            and spawn.get("status", "") == ""
            and type(spawn.get("exitCode", 0)) is int
            and spawn.get("exitCode", 0) == 0
        ):
            hits.append(
                {
                    "targetLabel": spawn["targetLabel"],
                    "mnemonic": "CppCompile",
                    "runner": "remote cache hit",
                    "cacheHit": True,
                    "exitCode": 0,
                }
            )
    if count == 0:
        raise ProofError("execution log is empty")
    return hits


def _proxy_evidence(path: Path, *, upload: bool, key_prefix: str) -> dict[str, Any]:
    receipt = _object(json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object))
    if (
        receipt.get("schemaVersion") != 1
        or receipt.get("bucket") != R2_BUCKET
        or receipt.get("keyPrefix") != key_prefix
        or receipt.get("started") is not True
        or receipt.get("upload") is not upload
    ):
        raise ProofError("proxy receipt does not identify this phase")
    activity = _object(receipt.get("activity"))
    for key in ("queued", "uploaded", "pending", "uploadFailures", "downloaded", "downloadMisses"):
        if type(activity.get(key)) is not int or activity[key] < 0:
            raise ProofError("proxy receipt has invalid activity")
    if (
        type(receipt.get("lostUploads")) is not int
        or receipt["lostUploads"] != 0
        or activity["pending"] != 0
        or activity["uploadFailures"] != 0
        or activity["queued"] != activity["uploaded"]
    ):
        raise ProofError("proxy uploads did not finish successfully")
    if upload:
        if activity["queued"] <= 0:
            raise ProofError("producer did not upload to R2")
    elif activity["downloaded"] <= 0 or activity["queued"] != 0 or activity["uploaded"] != 0:
        raise ProofError("consumer needs R2 downloads and no uploads")
    return receipt


def _client_environment(environ: Mapping[str, str], state: Path, url: str) -> dict[str, str]:
    environment = {
        key: value
        for key, value in environ.items()
        if not key.startswith(("R2_", "AWS_", "BAZEL_REMOTE_S3_")) and key != "GITHUB_OUTPUT"
    }
    environment["AOBUS_STATE_ROOT"] = str(state)
    environment[REMOTE_CACHE_ENVIRONMENT] = url
    return environment


def _build(phase: Path, jobs: int, environment: dict[str, str], *, upload: bool = True) -> int:
    command = [
        str(PROJECT_ROOT / "ao"),
        "bazel",
        "build",
        "--path",
        str(phase / "build"),
        "--jobs",
        str(jobs),
        "--",
        TARGET,
        f"--build_event_json_file={phase / 'build.bep.json'}",
        "--build_event_publish_all_actions",
        # BEP uploads are independent of remote_upload_local_results; keep them
        # local so the upload-disabled consumer makes no incidental CAS writes.
        "--experimental_build_event_upload_strategy=local",
        "--remote_cache_async=false",
        "--remote_download_outputs=all",
        f"--execution_log_json_file={phase / 'spawns.json'}",
    ]
    if not upload:
        command.append("--noremote_upload_local_results")
    # proc.run overlays os.environ rather than replacing it. This standalone,
    # single-threaded CI entry point removes credentials for the entire owned
    # foreground invocation, then restores the proxy's environment snapshot.
    original = dict(os.environ)
    try:
        os.environ.clear()
        os.environ.update(environment)
        return proc.run(command, cwd=PROJECT_ROOT, env=environment, log=phase / "build.log")
    finally:
        os.environ.clear()
        os.environ.update(original)


def _run_phase(
    root: Path,
    name: str,
    receipt: dict[str, Any],
    *,
    jobs: int,
    key_prefix: str,
    runner_arch: str,
    environ: Mapping[str, str],
    upload: bool,
) -> None:
    phase = root / name
    phase.mkdir()
    state = phase / "client-state"
    state.mkdir()
    proxy = phase / "proxy"
    proxy.mkdir()
    output = phase / "proxy.output"
    receipt.update(
        {
            "schemaVersion": 1,
            "phase": name,
            "startedAt": _now(),
            "finishedAt": None,
            "buildExit": None,
            "proxyStopped": None,
            "success": False,
            "buildLog": "build.log",
            "portalBuildLog": "build/build.log",
            "bep": "build.bep.json",
            "executionLog": "spawns.json",
            "proxyReceipt": "proxy.receipt.json",
            "clientState": "client-state",
            "buildPath": "build",
            "proxyState": "proxy",
            "proxyData": "proxy/data",
        }
    )
    _write_receipt(phase / "receipt.json", receipt)
    operation = "startProxy"
    try:
        try:
            cache.start(proxy, output, "Linux", runner_arch, upload=upload, key_prefix=key_prefix, environ=environ)
            operation = "readProxyUrl"
            urls = [
                line.removeprefix("url=")
                for line in output.read_text(encoding="utf-8").splitlines()
                if line.startswith("url=")
            ]
            if urls != [f"grpc://{cache.GRPC_ADDRESS}"]:
                raise ProofError("proxy did not publish its owned loopback URL")
            operation = "build"
            receipt["buildStartedAt"] = _now()
            _write_receipt(phase / "receipt.json", receipt)
            receipt["buildExit"] = _build(phase, jobs, _client_environment(environ, state, urls[0]), upload=upload)
            receipt["buildFinishedAt"] = _now()
            _write_receipt(phase / "receipt.json", receipt)
        finally:
            try:
                receipt["proxyStopped"] = cache.stop(proxy, report=phase / "proxy.receipt.json")
            except Exception as exc:
                receipt["cleanupError"] = "proxy stop failed"
                receipt["cleanupFailure"] = _failure_details("stopProxy", exc)
        if type(receipt["buildExit"]) is not int:
            raise ProofError("native build exit is unavailable")
        if receipt["buildExit"] != 0:
            raise ProofError("native build failed")
        operation = "verifyProxy"
        if receipt["proxyStopped"] is not True:
            raise ProofError("proxy did not stop with drained uploads")
        _proxy_evidence(phase / "proxy.receipt.json", upload=upload, key_prefix=key_prefix)
        operation = "verifyBep"
        validate_bep(phase / "build.bep.json")
        operation = "verifyExecution"
        hits = compile_hits(phase / "spawns.json")
        receipt["nativeCompileRemoteHits"] = hits
        if not upload and not hits:
            raise ProofError("consumer has no native CppCompile remote cache hit")
        receipt["success"] = True
    except ProofError as exc:
        receipt["error"] = str(exc)
        receipt["failure"] = _failure_details(operation, exc)
    except KeyboardInterrupt as exc:
        receipt["error"] = "interrupted; native exit may be unavailable"
        receipt["failure"] = _failure_details(operation, exc)
        raise
    except Exception as exc:
        receipt["error"] = "phase execution or evidence read failed"
        receipt["failure"] = _failure_details(operation, exc)
    finally:
        receipt["finishedAt"] = _now()
        _write_receipt(phase / "receipt.json", receipt)
        if "failure" in receipt:
            summary = " ".join(f"{key}={value}" for key, value in receipt["failure"].items())
            if "cleanupFailure" in receipt:
                summary += "; cleanup " + " ".join(f"{key}={value}" for key, value in receipt["cleanupFailure"].items())
            print(f"R2 cache proof {name} failed: {summary}", file=sys.stderr)


def prove(root: Path, *, jobs: int, key_prefix: str, environ: Mapping[str, str] | None = None) -> int:
    """Create a new proof root; never reuse or delete an existing cache or tree."""
    environment = dict(os.environ if environ is None else environ)
    key_prefix = cache.validated_key_prefix(key_prefix)
    if sys.platform != "linux" or platform.system() != "Linux":
        raise ProofError("R2 cache proof requires native Linux")
    if jobs < 1:
        raise ProofError("jobs must be positive")
    credentials = r2_credentials(environment)
    if credentials is None:
        raise ProofError("complete R2 credentials are required")
    cache.s3_endpoint(credentials.endpoint)
    runner_arch = {"x86_64": "X64", "aarch64": "ARM64"}.get(platform.machine(), "unsupported")
    cache.artifact("Linux", runner_arch)  # Validate the pinned host before creating any state.
    root = root.absolute()
    root.mkdir(parents=True, exist_ok=False)
    receipt: dict[str, Any] = {
        "schemaVersion": 1,
        "scope": "same-runner-native-linux",
        "bucket": R2_BUCKET,
        "keyPrefix": key_prefix,
        "target": TARGET,
        "jobs": jobs,
        "startedAt": _now(),
        "finishedAt": None,
        "success": False,
        "phases": {},
    }
    _write_receipt(root / "receipt.json", receipt)
    try:
        for name, upload in (("producer", True), ("consumer", False)):
            phase_receipt: dict[str, Any] = {}
            receipt["phases"][name] = phase_receipt
            _run_phase(
                root,
                name,
                phase_receipt,
                jobs=jobs,
                key_prefix=key_prefix,
                runner_arch=runner_arch,
                environ=environment,
                upload=upload,
            )
            _write_receipt(root / "receipt.json", receipt)
            if not phase_receipt["success"]:
                code = phase_receipt["buildExit"]
                return code if type(code) is int and code != 0 else 1
        receipt["success"] = True
        return 0
    finally:
        receipt["finishedAt"] = _now()
        _write_receipt(root / "receipt.json", receipt)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", type=Path, required=True, help="new proof root; must not already exist")
    parser.add_argument("--jobs", type=int, required=True)
    parser.add_argument("--key-prefix", required=True, help="isolated R2 prefix, not an authorization boundary")
    args = parser.parse_args(argv)
    try:
        return prove(args.path, jobs=args.jobs, key_prefix=args.key_prefix)
    except ProofError as exc:
        print(f"R2 cache proof failed: {exc}", file=sys.stderr)
    except (OSError, ValueError, cache.RemoteCacheError):
        print("R2 cache proof setup failed; no fallback is permitted", file=sys.stderr)
    except KeyboardInterrupt:
        return 130
    return 1


if __name__ == "__main__":
    sys.exit(main())
