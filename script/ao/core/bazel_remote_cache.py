"""Run CI's Bazel remote cache: a pinned bazel-remote proxy over the shared R2 bucket.

Bazel reaches a loopback bazel-remote whose local directory fronts the bucket
the compiler cache uses, under a Bazel-only key prefix. The caller controls
which trusted jobs receive credentials; secretless runs keep their disk cache.
Neither the uploader flag nor an isolated validation prefix is a backend
authorization boundary. The gRPC action-cache dependency check
treats a result whose blobs have expired from R2 as a miss, so an age-based
lifecycle rule on the prefix costs rebuilds, not failures. bazel-remote's
graceful shutdown abandons queued proxy uploads, so ``stop`` drains them,
accounted from its access log, before terminating it.

bazel-remote refuses Windows builds, so a Windows runner hosts the pinned Linux
binary in a throwaway WSL2 distribution imported from a pinned base rootfs.
WSL forwards the distribution's loopback listeners to Windows, which keeps the
URL identical. That rootfs carries no trust store; the runner image's Git for
Windows CA bundle supplies it. Only the three backend variables named in
``WSLENV`` enter the distribution.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
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
from pathlib import Path, PurePath, PurePosixPath
from typing import Any
from urllib.parse import urlsplit

if __package__:
    from .r2_credentials import R2_BUCKET, R2Credentials, r2_credentials
else:
    # CI invokes this file with the runner's Python before the portal's environment exists.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from ao.core.r2_credentials import R2_BUCKET, R2Credentials, r2_credentials

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
    # WSL hosts the Linux binary on Windows.
    ("Windows", "X64"): "linux-x86_64",
}
WSL_DISTRO = "aobus-bazel-remote"
_WSL_HOME = PurePosixPath("/opt/bazel-remote")
_WSL_LOG = "/var/log/bazel-remote.log"
_WSL_PID = "/run/bazel-remote.pid"
_WSL_SECRETS = ("BAZEL_REMOTE_S3_ENDPOINT", "BAZEL_REMOTE_S3_ACCESS_KEY_ID", "BAZEL_REMOTE_S3_SECRET_ACCESS_KEY")
_WSL_SECONDS = 120
_SHA256 = re.compile(r"[0-9a-f]{64}")


class RemoteCacheError(RuntimeError):
    pass


@dataclass(frozen=True)
class Artifact:
    url: str
    sha256: str


@dataclass(frozen=True)
class _Server:
    """The started proxy as its host exposes it."""

    pid: int
    exited: Callable[[], bool]
    read_log: Callable[[], str]
    abandon: Callable[[], None]


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


def validated_key_prefix(value: str) -> str:
    """Validate a relative R2 object prefix before any proxy or proof side effect."""
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9._-]+(?:/[A-Za-z0-9._-]+)*", value):
        raise RemoteCacheError("Bazel remote-cache key prefix must be a nonempty relative ASCII path")
    if any(segment in {".", ".."} for segment in value.split("/")):
        raise RemoteCacheError("Bazel remote-cache key prefix must not contain . or .. segments")
    return value


def artifact(runner_os: str, runner_arch: str, manifest: Path = MANIFEST_PATH) -> Artifact:
    """Select the pinned release binary for a GitHub runner OS and architecture."""
    key = _ARTIFACT_KEYS.get((runner_os, runner_arch))
    if key is None:
        raise RemoteCacheError(f"no pinned bazel-remote for {runner_os} {runner_arch}")
    return _pinned(_manifest(manifest)["artifacts"][key])


def wsl_rootfs(manifest: Path = MANIFEST_PATH) -> Artifact:
    """Select the pinned base rootfs of the distribution that hosts the proxy on Windows."""
    return _pinned(_manifest(manifest)["wslRootfs"])


def _manifest(manifest: Path) -> dict[str, Any]:
    values = json.loads(manifest.read_text(encoding="utf-8"))
    if values.get("schemaVersion") != 1:
        raise RemoteCacheError(f"unsupported bazel-remote manifest schema in {manifest}")
    return dict(values)


def _pinned(entry: dict[str, str]) -> Artifact:
    return Artifact(entry["url"], entry["sha256"])


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
    selected: Artifact,
    directory: Path,
    *,
    name: str = "bazel-remote",
    fetch: Callable[[str, Path], object] = urllib.request.urlretrieve,
) -> Path:
    """Reuse or download the verified artifact into the state directory."""
    binary = directory / f"{name}-{selected.sha256[:16]}"
    if binary.is_file() and _file_sha256(binary) == selected.sha256:
        return binary
    directory.mkdir(parents=True, exist_ok=True)
    partial = binary.with_name(binary.name + ".partial")
    fetch(selected.url, partial)
    if _file_sha256(partial) != selected.sha256:
        partial.unlink()
        raise RemoteCacheError(f"downloaded {name} does not match {MANIFEST_PATH.name}")
    partial.chmod(0o755)
    os.replace(partial, binary)
    return binary


def server_command(binary: PurePath, data: PurePath, *, upload: bool, key_prefix: str = KEY_PREFIX) -> list[str]:
    key_prefix = validated_key_prefix(key_prefix)
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
        key_prefix,
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


def activity(log: str, *, key_prefix: str = KEY_PREFIX) -> Activity:
    key_prefix = validated_key_prefix(key_prefix)
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
            rf" S3 (UPLOAD|DOWNLOAD) {re.escape(R2_BUCKET)} {re.escape(key_prefix)}/"
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
                if sys.platform == "win32":
                    # Windows lets SO_REUSEADDR share a live listener's address.
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                else:
                    # Match the server's bind policy: closed connections in TIME_WAIT
                    # are reusable, but a live listener still owns its address.
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
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


def _start_native(
    log: Path, binary: Path, data: Path, environment: Mapping[str, str], upload: bool, key_prefix: str
) -> _Server:
    with log.open("wb") as output:
        process = subprocess.Popen(
            server_command(binary, data, upload=upload, key_prefix=key_prefix),
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=environment,
            # Later steps use the server; the job's orphan cleanup ends it if stop never runs.
            start_new_session=True,
        )

    def abandon() -> None:
        if process.poll() is None:
            process.kill()
        process.wait()

    return _Server(
        process.pid,
        lambda: process.poll() is not None,
        lambda: log.read_text(encoding="utf-8", errors="replace"),
        abandon,
    )


def _wsl_environment(environ: Mapping[str, str]) -> dict[str, str]:
    """Let wsl.exe inherit Windows' own variables; WSLENV alone decides what enters the distribution."""
    return {
        key: value
        for key, value in environ.items()
        if key.upper() != "WSLENV" and not key.upper().startswith(("R2_", "AWS_", "BAZEL_REMOTE_"))
    }


def _wsl(
    action: str, arguments: Sequence[str], environ: Mapping[str, str], *, check: bool = True, echo: bool = True
) -> tuple[int, str]:
    """Run wsl.exe; ``echo`` false keeps a failed call's output out of the error."""
    try:
        completed = subprocess.run(
            ["wsl.exe", *arguments],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env={**environ, "WSL_UTF8": "1"},
            timeout=_WSL_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RemoteCacheError(f"WSL {action} did not finish within {_WSL_SECONDS} seconds") from exc
    # Releases that ignore WSL_UTF8 answer management commands in UTF-16.
    raw = completed.stdout
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-16-le" if b"\0" in raw else "utf-8-sig"
    output = raw.decode(encoding, errors="replace")
    if check and completed.returncode:
        detail = f":\n{output.strip()}" if echo and output.strip() else ""
        raise RemoteCacheError(f"WSL {action} failed with exit code {completed.returncode}{detail}")
    return completed.returncode, output


def _wsl_exec(
    action: str, command: Sequence[str], environ: Mapping[str, str], *, check: bool = True, echo: bool = True
) -> tuple[int, str]:
    return _wsl(action, ["-d", WSL_DISTRO, "--exec", *command], environ, check=check, echo=echo)


def _git_ca_bundle(environ: Mapping[str, str]) -> Path:
    git = shutil.which("git", path=environ.get("PATH"))
    if git:
        for root in list(Path(git).resolve().parents)[:3]:
            for relative in ("usr/ssl/certs/ca-bundle.crt", "mingw64/etc/ssl/certs/ca-bundle.crt"):
                if (root / relative).is_file():
                    return root / relative
    raise RemoteCacheError("Git for Windows' ca-bundle.crt was not found beside git")


def _remove_wsl(environ: Mapping[str, str]) -> None:
    # A nonzero unregister can mean already absent; only a successful list can
    # distinguish that from a failed teardown. Include transitional registrations
    # with --all: the default list can hide an importing/unregistering disk.
    code, _ = _wsl("unregister", ["--unregister", WSL_DISTRO], environ, check=False)
    _, output = _wsl("distribution list", ["--list", "--quiet", "--all"], environ)
    if "\ufffd" in output:
        raise RemoteCacheError("WSL distribution list contains undecodable output")
    if WSL_DISTRO.casefold() in {name.strip().casefold() for name in output.splitlines()}:
        raise RemoteCacheError(f"WSL distribution remains registered after unregister (exit code {code})")


def _start_in_wsl(
    state: Path,
    binary: Path,
    secrets: Mapping[str, str],
    *,
    upload: bool,
    key_prefix: str,
    environ: Mapping[str, str],
) -> _Server:
    """Import the pinned distribution, install the verified binary and start it detached."""
    base = _wsl_environment(environ)
    rootfs = install(wsl_rootfs(), state / "bin", name="wsl-rootfs")
    bundle = state / "ca-bundle.crt"
    shutil.copyfile(_git_ca_bundle(environ), bundle)
    disk = state / "wsl"
    _remove_wsl(base)
    shutil.rmtree(disk, ignore_errors=True)
    disk.mkdir(parents=True)
    # Import can register a distribution before failing. Keep a cleanup-only
    # ownership record until either readiness or verified rollback replaces it.
    server_file = state / SERVER_NAME
    server_file.write_text(json.dumps({"host": "wsl", "upload": False, "key_prefix": key_prefix}), encoding="utf-8")
    try:
        _wsl("import", ["--import", WSL_DISTRO, str(disk), str(rootfs), "--version", "2"], base)
        # These sh scripts stay free of double quotes and backslashes, which
        # wsl.exe's own command-line parsing would reinterpret.
        sources = [
            shlex.quote(_wsl_exec("path translation", ["wslpath", "-a", path.as_posix()], base)[1].strip())
            for path in (binary, bundle)
        ]
        _wsl_exec(
            "proxy installation",
            [
                "sh",
                "-c",
                f"mkdir -p {_WSL_HOME}/data && cp {sources[0]} {_WSL_HOME}/bazel-remote && "
                f"cp {sources[1]} {_WSL_HOME}/ca-bundle.crt && chmod 755 {_WSL_HOME}/bazel-remote && : > {_WSL_LOG}",
            ],
            base,
        )
        command = shlex.join(
            server_command(_WSL_HOME / "bazel-remote", _WSL_HOME / "data", upload=upload, key_prefix=key_prefix)
        )
        _wsl_exec(
            "proxy start",
            [
                "sh",
                "-c",
                # The session must outlive setsid's detach, or WSL ends the server with it.
                f"export SSL_CERT_FILE={_WSL_HOME}/ca-bundle.crt; "
                f"setsid {command} > {_WSL_LOG} 2>&1 < /dev/null & echo $! > {_WSL_PID}; sleep 1",
            ],
            {**base, **secrets, "WSLENV": ":".join(_WSL_SECRETS)},
            # Shell diagnostics from the one call that carries credentials stay unprinted.
            echo=False,
        )
        pid = _wsl_exec("proxy pid read", ["cat", _WSL_PID], base)[1].strip()
        if not pid.isascii() or not pid.isdigit():
            raise RemoteCacheError("WSL did not report the proxy pid")
    except BaseException:
        try:
            _remove_wsl(base)
            server_file.unlink(missing_ok=True)
        except BaseException as cleanup:
            print(
                f"Error: WSL startup rollback failed; ownership retained at {server_file}: {cleanup}", file=sys.stderr
            )
        raise
    return _Server(
        int(pid),
        lambda: _wsl_exec("proxy liveness check", ["kill", "-0", pid], base, check=False)[0] != 0,
        lambda: _wsl_exec("proxy log read", ["cat", _WSL_LOG], base)[1],
        lambda: _remove_wsl(base),
    )


def _terminate_in_wsl(pid: int, environ: Mapping[str, str]) -> None:
    try:
        if _wsl_exec("proxy stop", ["kill", "-TERM", str(pid)], environ, check=False)[0] == 0:
            for _ in range(20):
                time.sleep(0.5)
                if _wsl_exec("proxy liveness check", ["kill", "-0", str(pid)], environ, check=False)[0] != 0:
                    break
    finally:
        _remove_wsl(environ)


def start(
    state: Path,
    github_output: Path,
    runner_os: str,
    runner_arch: str,
    *,
    upload: bool,
    key_prefix: str = KEY_PREFIX,
    environ: Mapping[str, str] = os.environ,
    ready_seconds: float = 60,
) -> None:
    key_prefix = validated_key_prefix(key_prefix)
    credentials = r2_credentials(environ)
    if credentials is None:
        print("R2 credentials unavailable; Bazel keeps its local disk cache.")
        return
    environment = server_environment(environ, credentials)
    binary = install(artifact(runner_os, runner_arch), state / "bin")
    _require_free_addresses()
    hosted = runner_os == "Windows"
    if hosted:
        server = _start_in_wsl(
            state.resolve(),
            binary.resolve(),
            {name: environment[name] for name in _WSL_SECRETS},
            upload=upload,
            key_prefix=key_prefix,
            environ=environ,
        )
    else:
        server = _start_native(state / LOG_NAME, binary, state / "data", environment, upload, key_prefix)
    try:
        deadline = time.monotonic() + ready_seconds
        while True:
            status = _status()
            # Check our child even when an HTTP server answered successfully.
            if server.exited() or time.monotonic() >= deadline:
                tail = "\n".join(server.read_log().splitlines()[-20:])
                for secret in (*vars(credentials).values(), environment["BAZEL_REMOTE_S3_ENDPOINT"]):
                    tail = tail.replace(secret, "<redacted>")
                raise RemoteCacheError("bazel-remote did not become ready:\n" + tail)
            if status is not None:
                break
            time.sleep(0.5)
        record: dict[str, object] = {"pid": server.pid, "upload": upload, "key_prefix": key_prefix}
        if hosted:
            record["host"] = "wsl"
        # Failed readiness publication must not corrupt a WSL cleanup marker.
        pending = state / f"{SERVER_NAME}.partial"
        pending.write_text(json.dumps(record), encoding="utf-8")
        os.replace(pending, state / SERVER_NAME)
        # Each Bazel step maps this output explicitly; other steps never see it.
        _write_github_output(github_output, {"url": f"grpc://{GRPC_ADDRESS}"})
    except BaseException:
        try:
            server.abandon()
            (state / SERVER_NAME).unlink(missing_ok=True)
        except BaseException as cleanup:
            print(
                f"Error: bazel-remote startup rollback failed; ownership retained at {state / SERVER_NAME}: {cleanup}",
                file=sys.stderr,
            )
        raise
    mode = "uploads from this trusted run" if upload else "uploads disabled"
    print(f"Bazel remote cache ready ({mode}): {R2_BUCKET}/{key_prefix} through grpc://{GRPC_ADDRESS}")


def drain(
    read_log: Callable[[], str],
    *,
    key_prefix: str = KEY_PREFIX,
    timeout: float = 900,
    quiet: float = 30,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> Activity:
    """Wait until every queued upload finished, or until uploads stall for the quiet period."""
    deadline = clock() + timeout
    current = activity(read_log(), key_prefix=key_prefix)
    last_progress = clock()
    while current.pending > 0 and clock() < deadline and clock() - last_progress < quiet:
        sleep(1)
        observed = activity(read_log(), key_prefix=key_prefix)
        if observed.uploaded != current.uploaded:
            last_progress = clock()
        current = observed
    return current


def _terminate(pid: int) -> None:
    if sys.platform == "win32":
        raise RemoteCacheError("Bazel remote-cache termination requires native Linux/macOS")
    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(20):
            time.sleep(0.5)
            os.kill(pid, 0)
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def stop(state: Path, *, timeout: float = 900, report: Path | None = None) -> bool:
    """Drain and stop the owned server, optionally recording versioned traffic evidence."""
    server_file = state / SERVER_NAME
    started = server_file.is_file()
    key_prefix = KEY_PREFIX
    upload = False
    result = Activity(0, 0, 0, 0, 0)
    if not started:
        print("No Bazel remote cache was started.")
    else:
        server = json.loads(server_file.read_text(encoding="utf-8"))
        log = state / LOG_NAME
        wsl = _wsl_environment(os.environ) if server.get("host") == "wsl" else None
        partial = wsl is not None and "pid" not in server
        if partial:
            started = False

        def read_log() -> str:
            if partial:
                return ""
            if wsl is not None:
                return _wsl_exec("proxy log read", ["cat", _WSL_LOG], wsl)[1]
            return log.read_text(encoding="utf-8", errors="replace")

        try:
            key_prefix = validated_key_prefix(server.get("key_prefix", KEY_PREFIX))
            upload = server["upload"]
            # A server with uploaders disabled queues nothing for R2.
            result = (
                drain(read_log, key_prefix=key_prefix, timeout=timeout)
                if upload
                else activity(read_log(), key_prefix=key_prefix)
            )
            if not partial and (status := _status()):
                print(f"bazel-remote local cache: {status.get('NumFiles')} file(s), {status.get('CurrSize')} byte(s)")
            uploads = f"{result.uploaded} of {result.queued} queued upload(s)" if upload else "uploads disabled"
            print(
                f"R2 {R2_BUCKET}/{key_prefix}: {result.downloaded} download(s), {result.download_misses} miss(es), "
                f"{uploads}, {result.upload_failures} failed upload(s)"
            )
        finally:
            if wsl is not None:
                if partial:
                    _remove_wsl(wsl)
                else:
                    _terminate_in_wsl(int(server["pid"]), wsl)
            else:
                _terminate(int(server["pid"]))
            server_file.unlink(missing_ok=True)
    lost = max(result.pending, 0) if upload else 0
    if report is not None:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "bucket": R2_BUCKET,
                    "keyPrefix": key_prefix,
                    "started": started,
                    "upload": upload,
                    "activity": {
                        "queued": result.queued,
                        "uploaded": result.uploaded,
                        "pending": result.pending,
                        "uploadFailures": result.upload_failures,
                        "downloaded": result.downloaded,
                        "downloadMisses": result.download_misses,
                    },
                    "lostUploads": lost,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
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
    start_parser.add_argument("--key-prefix", default=KEY_PREFIX, help="relative S3 object prefix (not authorization)")
    stop_parser = subparsers.add_parser("stop", help="drain queued uploads and stop the proxy")
    stop_parser.add_argument("--state", type=Path, required=True)
    stop_parser.add_argument("--report", type=Path, help="write a versioned JSON receipt of observed proxy traffic")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "start":
            args.state.mkdir(parents=True, exist_ok=True)
            start(
                args.state,
                args.github_output,
                args.runner_os,
                args.runner_arch,
                upload=args.trusted_writer == "true",
                key_prefix=args.key_prefix,
            )
            return 0
        return 0 if stop(args.state, report=args.report) else 1
    except (RemoteCacheError, OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
