"""Shared native attempt helpers: retained phases, receipts and BEP results.

`ao check` runs the Bazel test graph directly (see `ao.command.check`); this
module is not that command. The managed sanitizer and coverage routes reuse
the attempt directory, the frozen context configuration and the test-result
binding implemented here.
"""

from __future__ import annotations

import json
import re
import shutil
import sys
import tempfile
import time
from collections.abc import Callable, Mapping, Sequence
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import asdict
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse
from urllib.request import url2pathname

from ao.core import bazel_native as native
from ao.core import proc
from ao.core.paths import PROJECT_ROOT
from ao.portal import bazel_test_ownership, bazel_workspace

_COMPATIBILITY = 'str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))'


def bep_native_path(value: str) -> str:
    """Invert the pinned producer's UTF-8 bytes stored as Latin-1 characters."""
    if not isinstance(value, str) or "\x00" in value:
        raise ValueError("malformed Bazel 9.2 BEP path")
    try:
        return value.encode("latin-1").decode("utf-8")
    except UnicodeError as error:
        raise ValueError("invalid or lossy Bazel 9.2 BEP path encoding") from error


def test_output_file(file: Mapping[str, Any], *, root: Path | None = None) -> Path:
    """Resolve a locally materialized testActionOutput from the pinned BEP producer."""
    uri = file.get("uri")
    if not isinstance(uri, str):
        raise ValueError("expected a local BEP test output URI")
    parsed = urlparse(uri)
    if parsed.scheme != "file" or parsed.netloc or not parsed.path.startswith("/"):
        raise ValueError("expected a local BEP test output URI")
    internal = unquote(parsed.path, errors="strict")
    # TestAttempt uses FileUriPathConverter on an internal string, unlike
    # the lossy named-set producer. Match java.net.URI's path quoting exactly.
    quoted = "".join(
        char
        if (char.isascii() and (char.isalnum() or char in "/:@&=+$,;-_.!~*'()")) or 0xA1 <= ord(char) <= 0xFF
        else "".join(f"%{byte:02X}" for byte in char.encode("utf-8"))
        for char in internal
    )
    if uri != "file://" + quoted:
        raise ValueError("noncanonical Bazel 9.2 BEP artifact file URI")
    # Convert the original quoted path to the host spelling, never unquote
    # the resulting native string again (it may contain a literal '%20').
    path = Path(bep_native_path(url2pathname(parsed.path)))
    if root is not None and (not path.is_relative_to(root) or not path.resolve().is_relative_to(root.resolve())):
        raise ValueError(f"BEP test output is outside the owned tree: {path}")
    if not path.is_file():
        raise ValueError(f"missing BEP test output: {path}")
    return path


def _timestamp() -> str:
    return datetime.now(UTC).isoformat()


def _labels(text: str, *, allow_empty: bool = False) -> list[str]:
    labels = [line.removeprefix("@@").removeprefix("@") for line in text.splitlines()]
    if (not labels and not allow_empty) or len(set(labels)) != len(labels):
        raise ValueError("completion selection requires unique target labels")
    for label in labels:
        if not re.fullmatch(r"//[A-Za-z0-9._+~/-]*:[A-Za-z0-9._+~/-]+", label):
            raise ValueError(f"invalid completion target: {label!r}")
        if any(part in {"", ".", "..", "..."} for path in label[2:].split(":") if path for part in path.split("/")):
            raise ValueError(f"unsafe completion target: {label}")
    return sorted(labels)


def _test_results(
    path: Path, expected: frozenset[bazel_test_ownership.ConfiguredTarget], *, fresh: bool
) -> list[dict[str, Any]]:
    """Bind passing BEP summaries and their execution/cache counts to the exact selection."""
    if not expected:
        raise ValueError("completion selected no compatible test executables")
    # BEP records use LF; Unicode separators can occur inside JSON strings.
    events = [json.loads(line) for line in path.read_text(encoding="utf-8").split("\n") if line.strip(" \t\r")]
    observed: dict[bazel_test_ownership.ConfiguredTarget, dict[str, Any]] = {}
    for event in events:
        if "testSummary" not in event:
            continue
        summary = event["testSummary"]
        identity = event["id"]["testSummary"]
        target = bazel_test_ownership.ConfiguredTarget(_labels(identity["label"])[0], identity["configuration"]["id"])
        if summary.get("overallStatus") != "PASSED":
            raise ValueError(f"completion test did not pass: {target}: {summary.get('overallStatus')}")
        runs = summary.get("totalRunCount", 0)
        cached = summary.get("totalNumCached", 0)
        if fresh and cached:
            raise ValueError(f"completion test did not execute freshly: {target}")
        observed[target] = {**asdict(target), "status": "PASSED", "runs": runs, "cached": cached}
    if set(observed) != expected:
        raise ValueError(f"completion test summaries are missing: {sorted(expected - observed.keys())}")
    return [observed[target] for target in sorted(observed)]


def _controlled_exit_status(error: SystemExit | KeyboardInterrupt) -> int:
    if isinstance(error, KeyboardInterrupt):
        return 130
    # An early exit is incomplete even when a helper requests exit zero.
    return error.code if isinstance(error.code, int) and error.code != 0 else 1


class _PhaseFailure(Exception):
    def __init__(self, name: str, status: int):
        super().__init__(f"{name} exited {status}")
        self.status = status


class _Check:
    """One caller-locked attempt; no later attempt reuses its artifact names."""

    def __init__(self, tree: Path, config: str, platform: str, jobs: int):
        directory = tree / "checks" / config
        directory.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="check-", dir=directory))
        self.path = self.directory / "receipt.json"
        self.log = tree / "build.log"
        # Keep child progress on the caller's console even when a phase
        # redirects Python diagnostics to its own log.
        self.console = sys.stderr
        self.receipt: dict[str, Any] = {
            "schema": 1,
            "source": str(PROJECT_ROOT),
            "tree": str(tree),
            "config": config,
            "platform": platform,
            "jobs": jobs,
            "started_at": _timestamp(),
            "status": "running",
            "exit": None,
            "phases": [],
        }
        self.save()
        with self.log.open("a", encoding="utf-8") as sink:
            sink.write(f"\nBazel completion ({config}); receipt: {self.path}\n")
        print(f"Bazel completion ({config}); receipt: {self.path}", flush=True)

    def save(self) -> None:
        native.write_json(self.path, self.receipt)

    def phase(self, name: str, invoke: Callable[[Any, Any], int], *, argv: Sequence[str] | None = None) -> Path:
        stdout = self.directory / f"{name}.stdout.log"
        stderr = self.directory / f"{name}.stderr.log"
        record: dict[str, Any] = {
            "name": name,
            "argv": list(argv) if argv is not None else None,
            "cwd": str(PROJECT_ROOT),
            "started_at": _timestamp(),
            "stdout": str(stdout),
            "stderr": str(stderr),
            "status": "running",
            "exit": None,
        }
        self.receipt["phases"].append(record)
        self.save()
        started = time.monotonic()
        print(f"=== Bazel completion: {name} ===", flush=True)
        try:
            with stdout.open("xb") as out, stderr.open("xb") as err:
                status = invoke(out, err)
            record.update(status="passed" if status == 0 else "failed", exit=status)
        except BaseException as exc:
            record.update(status="error", error=str(exc) or type(exc).__name__)
            if isinstance(exc, _PhaseFailure):
                record.update(status="failed", exit=exc.status)
            elif isinstance(exc, (SystemExit, KeyboardInterrupt)):
                record.update(
                    status="interrupted" if isinstance(exc, KeyboardInterrupt) else "failed",
                    exit=_controlled_exit_status(exc),
                )
            raise
        finally:
            record.update(finished_at=_timestamp(), elapsed_seconds=time.monotonic() - started)
            self.save()
            with self.log.open("ab") as sink:
                sink.write(f"\n=== {name} ===\n".encode())
                for path in (stdout, stderr):
                    if path.is_file():
                        with path.open("rb") as source:
                            shutil.copyfileobj(source, sink)
        if status:
            print(f"{name} failed; stdout: {stdout}; stderr: {stderr}", file=sys.stderr)
            raise _PhaseFailure(name, status)
        return stdout

    def python(self, name: str, invoke: Callable[[], int]) -> None:
        def run(out: Any, err: Any) -> int:
            with (
                Path(out.name).open("a", encoding="utf-8") as text_out,
                Path(err.name).open("a", encoding="utf-8") as text_err,
                redirect_stdout(text_out),
                redirect_stderr(text_err),
            ):
                return invoke()

        self.phase(name, run)

    def bazel(
        self,
        name: str,
        executable: Path,
        prepared: Path,
        action: str,
        arguments: Sequence[str],
        *,
        remote_cache: str | None = None,
    ) -> Path:
        argv = [*bazel_workspace.invocation(executable, prepared, isolate_rc=True), action]
        if action != "query":
            argv.append(f"--jobs={self.receipt['jobs']}")
        if remote_cache:
            argv.append(f"--remote_cache={remote_cache}")
        argv.extend(arguments)

        def invoke(out: Any, err: Any) -> int:
            return proc.run(
                argv,
                cwd=PROJECT_ROOT,
                env=bazel_workspace.client_environment(isolate_rc=True),
                stdout=out,
                log=err,
                console=self.console,
            )

        return self.phase(name, invoke, argv=argv)


def _context_configuration(base: Path, directory: Path, environment: dict[str, str], *, fresh: bool) -> Path:
    """Freeze runtime options before analysis: test_env changes configuration IDs."""
    prepared = directory / "configuration"
    prepared.mkdir()
    # Preserve the original client facts for independent native contract checks.
    # Repository selection still uses its stable watched base request under the
    # same tree lock; a per-attempt path must not force a repository refresh.
    (prepared / "request.json").write_bytes((base / "request.json").read_bytes())
    content = (base / "aobus.bazelrc").read_text(encoding="utf-8")
    cache_option = "--nocache_test_results" if fresh else "--cache_test_results=auto"
    lines = [content.rstrip(), "common --lockfile_mode=error", "build " + cache_option]
    lines.extend(
        "build " + json.dumps(f"--test_env={key}={value}", ensure_ascii=False)
        for key, value in sorted(environment.items())
    )
    (prepared / "aobus.bazelrc").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return prepared
