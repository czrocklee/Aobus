"""Read native BEP evidence and prepare the test context for coverage."""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from dataclasses import asdict
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse
from urllib.request import url2pathname

from ao.portal import bazel_test_ownership


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


def _labels(text: str) -> list[str]:
    labels = [line.removeprefix("@@").removeprefix("@") for line in text.splitlines()]
    if not labels or len(set(labels)) != len(labels):
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
