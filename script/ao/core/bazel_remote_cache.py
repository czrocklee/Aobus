"""Run CI's Bazel remote cache: a pinned bazel-remote proxy over the shared R2 bucket.

Bazel reaches a loopback bazel-remote whose local directory fronts the bucket
the compiler cache uses, under a Bazel-only key prefix. Only trusted main
pushes use writer credentials; other runs require separately configured
backend-read-only credentials, or keep Bazel's disk cache. The uploader flag
is not an authorization boundary. The gRPC action-cache dependency check
treats a result whose blobs have expired from R2 as a miss, so an age-based
lifecycle rule on the prefix costs rebuilds, not failures. bazel-remote's
graceful shutdown abandons queued proxy uploads, so ``stop`` drains them,
accounted from its access log, before terminating it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections import Counter
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

if __package__:
    from .compiler_cache import R2_BUCKET, R2Credentials, r2_credentials
else:
    # CI invokes this file with the runner's Python before the portal's environment exists.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from ao.core.compiler_cache import R2_BUCKET, R2Credentials, r2_credentials

MANIFEST_PATH = Path(__file__).resolve().parents[1] / "bazel-remote.json"
KEY_PREFIX = "bazel-v1"
GRPC_ADDRESS = "127.0.0.1:9092"
HTTP_ADDRESS = "127.0.0.1:9090"
LOCAL_SIZE_GIB = 10
LOG_NAME = "bazel-remote.log"
SERVER_NAME = "bazel-remote.json"
_ARTIFACT_KEYS = {
    ("Linux", "X64"): "linux-x86_64",
    ("macOS", "X64"): "darwin-x86_64",
    ("macOS", "ARM64"): "darwin-arm64",
}
_SHA256 = re.compile(r"[0-9a-f]{64}")


class RemoteCacheError(RuntimeError):
    pass


@dataclass(frozen=True)
class Artifact:
    url: str
    sha256: str


@dataclass(frozen=True)
class Activity:
    """Acknowledged writes and matching backend completions from the access log."""

    queued: int
    uploaded: int
    upload_failures: int
    downloaded: int
    download_misses: int

    @property
    def pending(self) -> int:
        return self.queued - self.uploaded


def artifact(runner_os: str, runner_arch: str, manifest: Path = MANIFEST_PATH) -> Artifact:
    """Select the pinned release binary for a GitHub runner OS and architecture."""
    key = _ARTIFACT_KEYS.get((runner_os, runner_arch))
    if key is None:
        raise RemoteCacheError(f"no pinned bazel-remote for {runner_os} {runner_arch}")
    values = json.loads(manifest.read_text(encoding="utf-8"))
    if values.get("schemaVersion") != 1:
        raise RemoteCacheError(f"unsupported bazel-remote manifest schema in {manifest}")
    entry = values.get("artifacts", {}).get(key, {})
    url, sha256 = entry.get("url"), entry.get("sha256")
    if not isinstance(url, str) or not url.startswith("https://") or not _SHA256.fullmatch(str(sha256)):
        raise RemoteCacheError(f"{manifest} needs an https url and a lowercase SHA-256 for {key}")
    return Artifact(url, str(sha256))


def s3_endpoint(endpoint: str) -> str:
    """Return the host bazel-remote expects from the compiler cache's R2 endpoint secret."""
    parsed = urlsplit(endpoint if "://" in endpoint else f"https://{endpoint}")
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.path.strip("/")
        or parsed.query
        or parsed.fragment
    ):
        # The value is a secret; describe its expected shape instead of echoing it.
        raise RemoteCacheError("R2_ENDPOINT must be an https origin such as https://<account>.r2.cloudflarestorage.com")
    return parsed.netloc


def _file_sha256(path: Path) -> str:
    # The runner's Python may predate hashlib.file_digest.
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def install(
    selected: Artifact, directory: Path, *, fetch: Callable[[str, Path], object] = urllib.request.urlretrieve
) -> Path:
    """Reuse or download the verified binary into the state directory."""
    binary = directory / f"bazel-remote-{selected.sha256[:16]}"
    if binary.is_file() and _file_sha256(binary) == selected.sha256:
        return binary
    directory.mkdir(parents=True, exist_ok=True)
    partial = binary.with_name(binary.name + ".partial")
    fetch(selected.url, partial)
    if _file_sha256(partial) != selected.sha256:
        partial.unlink()
        raise RemoteCacheError(f"downloaded bazel-remote does not match {MANIFEST_PATH.name}")
    partial.chmod(0o755)
    os.replace(partial, binary)
    return binary


def server_command(binary: Path, data: Path, *, upload: bool) -> list[str]:
    return [
        str(binary),
        "--dir",
        str(data),
        "--max_size",
        str(LOCAL_SIZE_GIB),
        "--grpc_address",
        GRPC_ADDRESS,
        "--http_address",
        HTTP_ADDRESS,
        "--s3.bucket",
        R2_BUCKET,
        "--s3.prefix",
        KEY_PREFIX,
        "--s3.auth_method",
        "access_key",
        "--s3.region",
        "auto",
        # Without uploaders bazel-remote drops every proxy write, so this run only reads.
        "--num_uploaders",
        "100" if upload else "0",
        # stop() accounts queued uploads from this log.
        "--access_log_level",
        "all",
    ]


def server_environment(environ: Mapping[str, str], credentials: R2Credentials) -> dict[str, str]:
    """Keep backend overrides and inherited writer keys out of the proxy environment."""
    environment = {
        key: environ[key]
        for key in ("PATH", "HOME", "TMPDIR", "TMP", "TEMP", "SSL_CERT_FILE", "SSL_CERT_DIR", "RUNNER_TRACKING_ID")
        if key in environ
    }
    environment["BAZEL_REMOTE_S3_ENDPOINT"] = s3_endpoint(credentials.endpoint)
    environment["BAZEL_REMOTE_S3_ACCESS_KEY_ID"] = credentials.access_key_id
    environment["BAZEL_REMOTE_S3_SECRET_ACCESS_KEY"] = credentials.secret_access_key
    return environment


def activity(log: str) -> Activity:
    queued: Counter[tuple[str, str]] = Counter()
    uploaded: Counter[tuple[str, str]] = Counter()
    failures = downloaded = misses = 0
    for line in log.splitlines():
        if match := re.search(
            r" GRPC BYTESTREAM WRITE COMPLETED: (?:\S*/)?uploads/[^/]+/"
            r"(?:blobs|compressed-blobs/zstd)/([0-9a-f]{64})/\d+(?:/\S*)?$",
            line,
        ):
            queued["CAS", match.group(1)] += 1
        elif match := re.search(r" GRPC (AC|CAS) PUT ([0-9a-f]{64}) OK$", line):
            queued[match.group(1), match.group(2)] += 1
        elif match := re.search(
            rf" S3 (UPLOAD|DOWNLOAD) {re.escape(R2_BUCKET)} {KEY_PREFIX}/"
            r"(ac|cas\.v2)/[0-9a-f]{2}/([0-9a-f]{64}) (.*)$",
            line,
        ):
            status = match.group(4)
            if match.group(1) == "UPLOAD":
                uploaded["AC" if match.group(2) == "ac" else "CAS", match.group(3)] += 1
                failures += status != "OK"
            else:
                downloaded += status == "OK"
                misses += status == "NOT FOUND"
    # A proxy upload can finish even when replying to its client failed. Only
    # completions for the same kind/hash may satisfy an acknowledged write.
    completed = sum((queued & uploaded).values())
    return Activity(sum(queued.values()), completed, failures, downloaded, misses)


def _require_free_addresses() -> None:
    """Reject pre-existing listeners before starting a child on these fixed ports."""
    for address in (HTTP_ADDRESS, GRPC_ADDRESS):
        host, port = address.rsplit(":", 1)
        with socket.socket() as probe:
            try:
                probe.bind((host, int(port)))
            except OSError as exc:
                raise RemoteCacheError(f"Bazel remote-cache address {address} is unavailable") from exc


def _status() -> dict[str, object] | None:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(f"http://{HTTP_ADDRESS}/status", timeout=2) as response:
            return dict(json.load(response))
    except (OSError, ValueError):
        return None


def _write_github_output(path: Path, outputs: Mapping[str, str]) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        for key, value in outputs.items():
            stream.write(f"{key}={value}\n")


def start(
    state: Path,
    github_output: Path,
    runner_os: str,
    runner_arch: str,
    *,
    upload: bool,
    environ: Mapping[str, str] = os.environ,
    ready_seconds: float = 60,
) -> None:
    credentials = r2_credentials(environ)
    if credentials is None:
        print("R2 credentials unavailable; Bazel keeps its local disk cache.")
        return
    environment = server_environment(environ, credentials)
    binary = install(artifact(runner_os, runner_arch), state / "bin")
    _require_free_addresses()
    log = state / LOG_NAME
    with log.open("wb") as output:
        process = subprocess.Popen(
            server_command(binary, state / "data", upload=upload),
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=environment,
            # Later steps use the server; the job's orphan cleanup ends it if stop never runs.
            start_new_session=True,
        )
    try:
        deadline = time.monotonic() + ready_seconds
        while True:
            status = _status()
            # Check our child even when an HTTP server answered successfully.
            if process.poll() is not None or time.monotonic() >= deadline:
                tail = "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:])
                for secret in (*vars(credentials).values(), environment["BAZEL_REMOTE_S3_ENDPOINT"]):
                    tail = tail.replace(secret, "<redacted>")
                raise RemoteCacheError("bazel-remote did not become ready:\n" + tail)
            if status is not None:
                break
            time.sleep(0.5)
        (state / SERVER_NAME).write_text(json.dumps({"pid": process.pid, "upload": upload}), encoding="utf-8")
        # Each Bazel step maps this output explicitly; other steps never see it.
        _write_github_output(github_output, {"url": f"grpc://{GRPC_ADDRESS}"})
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.wait()
        (state / SERVER_NAME).unlink(missing_ok=True)
        raise
    mode = "uploads from this trusted run" if upload else "read-only"
    print(f"Bazel remote cache ready ({mode}): {R2_BUCKET}/{KEY_PREFIX} through grpc://{GRPC_ADDRESS}")


def drain(
    read_log: Callable[[], str],
    *,
    timeout: float = 900,
    quiet: float = 30,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> Activity:
    """Wait until every queued upload finished, or until uploads stall for the quiet period."""
    deadline = clock() + timeout
    current = activity(read_log())
    last_progress = clock()
    while current.pending > 0 and clock() < deadline and clock() - last_progress < quiet:
        sleep(1)
        observed = activity(read_log())
        if observed.uploaded != current.uploaded:
            last_progress = clock()
        current = observed
    return current


def _terminate(pid: int) -> None:
    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(20):
            time.sleep(0.5)
            os.kill(pid, 0)
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def stop(state: Path, *, timeout: float = 900) -> bool:
    """Drain uploads, report proxy traffic, and terminate the server; False when uploads were lost."""
    server_file = state / SERVER_NAME
    if not server_file.is_file():
        print("No Bazel remote cache was started.")
        return True
    server = json.loads(server_file.read_text(encoding="utf-8"))
    log = state / LOG_NAME

    def read_log() -> str:
        return log.read_text(encoding="utf-8", errors="replace")

    # A read-only server queues nothing for R2.
    result = drain(read_log, timeout=timeout) if server["upload"] else activity(read_log())
    if status := _status():
        print(f"bazel-remote local cache: {status.get('NumFiles')} file(s), {status.get('CurrSize')} byte(s)")
    uploads = f"{result.uploaded} of {result.queued} queued upload(s)" if server["upload"] else "read-only"
    print(
        f"R2 {R2_BUCKET}/{KEY_PREFIX}: {result.downloaded} download(s), {result.download_misses} miss(es), "
        f"{uploads}, {result.upload_failures} failed upload(s)"
    )
    _terminate(int(server["pid"]))
    server_file.unlink()
    lost = max(result.pending, 0) if server["upload"] else 0
    if lost or result.upload_failures:
        print(
            f"::warning::{lost} upload(s) were still queued and {result.upload_failures} failed; "
            "later runs rebuild them."
        )
    return not lost and not result.upload_failures


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="CI Bazel remote cache over the shared R2 bucket")
    subparsers = parser.add_subparsers(dest="command", required=True)
    start_parser = subparsers.add_parser("start", help="start the proxy when R2 credentials exist")
    start_parser.add_argument("--state", type=Path, required=True)
    start_parser.add_argument("--github-output", type=Path, required=True)
    start_parser.add_argument("--runner-os", required=True)
    start_parser.add_argument("--runner-arch", required=True)
    start_parser.add_argument("--trusted-writer", choices=("true", "false"), required=True)
    stop_parser = subparsers.add_parser("stop", help="drain queued uploads and stop the proxy")
    stop_parser.add_argument("--state", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "start":
            args.state.mkdir(parents=True, exist_ok=True)
            start(
                args.state, args.github_output, args.runner_os, args.runner_arch, upload=args.trusted_writer == "true"
            )
            return 0
        return 0 if stop(args.state) else 1
    except (RemoteCacheError, OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
