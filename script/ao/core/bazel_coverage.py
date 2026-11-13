"""Linux Debug Bazel coverage with checked per-test/profile provenance.

This explicit route does not replace the CMake coverage command. Test filters
are deliberately unavailable until genuine per-leaf Catch2 XML proves matches.
Only current BEP artifacts and this invocation's strict collection receipts can
contribute to the production-only, cross-object line report.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Callable, Mapping, Sequence
from contextlib import ExitStack
from datetime import UTC, datetime
from functools import cache
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import unquote, urlparse
from urllib.request import url2pathname

from . import bazel_check, bazel_contract, bazel_test_ownership, bazel_workspace
from . import bazel_native as native
from .bazel_compiler_probe import _compiler_identity
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT

_SUITES = {"default", "all", "core", "application", "backend", "tui", "cli", "gtk"}
_PRODUCTION = ("app/", "lib/", "include/")
_PROFILE_ENV = {"LLVM_PROFILE_FILE", "GCOV_PREFIX", "GCOV_PREFIX_STRIP", "IGNORE_COVERAGE_COLLECTION_FAILURES"}
_SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".inc", ".cpp", ".cc", ".c", ".cxx", ".m", ".mm"}


def validate_request(platform: str, config: str = "debug") -> None:
    """Pure admission, callable by the CLI before selecting any environment/tree."""
    if platform != "linux":
        raise native.BazelWorkspaceError("Bazel coverage is supported only on Linux")
    if config != "debug":
        raise native.BazelWorkspaceError("Bazel coverage requires Debug without ASan/TSan")


def _environment(java_home: Path) -> dict[str, str]:
    # The stock Java launcher honors JAVABIN before its selected runtime.
    environment = {
        key: value
        for key, value in native.child_environment(isolate_rc=True).items()
        if key not in _PROFILE_ENV and key != "JAVABIN"
    }
    return environment | {"JAVA_HOME": str(java_home), "JAVABIN": str(java_home / "bin/java")}


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _java_runtime(server_home: Path) -> Path:
    """Derive a complete runtime home inside the already selected Nix output."""
    selected = server_home.resolve(strict=True)
    executable = (server_home / "bin/java").resolve(strict=True)
    home = executable.parent.parent
    if not home.is_relative_to(selected):
        raise native.BazelWorkspaceError("coverage Java runtime escapes the selected server JDK output")
    if not (home / "bin/java").is_file() or not (home / "lib/jrt-fs.jar").is_file():
        raise native.BazelWorkspaceError("selected coverage Java runtime lacks bin/java or lib/jrt-fs.jar")
    if (home / "bin/java").resolve(strict=True) != executable:
        raise native.BazelWorkspaceError("coverage Java runtime differs from the selected server executable")
    return home


def _source_identity(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): _sha(path)
        for top in ("app", "lib", "include")
        for path in (root / top).rglob("*")
        if path.is_file() and path.suffix in _SOURCE_SUFFIXES
    }


def _source(value: str, root: Path, mappings: Mapping[str, str]) -> str | None:
    value = mappings.get(value, value)
    path = PurePosixPath(value)
    if ".." in path.parts or "_virtual_includes" in path.parts:
        raise ValueError(f"unsafe or unmapped virtual coverage source: {value}")
    if path.is_absolute():
        try:
            value = path.relative_to(root.as_posix()).as_posix()
        except ValueError:
            return None
    if not value.startswith(_PRODUCTION):
        return None
    if value != PurePosixPath(value).as_posix():
        raise ValueError(f"noncanonical coverage source: {value}")
    return value


def parse_lcov(text: str) -> dict[str, dict[int, int]]:
    """Strict line LCOV parser; branch/function records are not report metrics."""
    result: dict[str, dict[int, int]] = {}
    source: str | None = None
    lines: dict[int, int] = {}
    totals: dict[str, int] = {}
    metadata = re.compile(
        r"(?:TN:[^\r\n]*|VER:[^\r\n]+|FN:(?:0|[1-9]\d*),(?:(?:0|[1-9]\d*),)?[^\r\n]+|"
        r"FNDA:\d+,[^\r\n]+|(?:FNF|FNH|BRF|BRH):\d+|BRDA:[1-9]\d*,\d+,\d+,(?:\d+|-))"
    )
    for row in text.splitlines():
        if row.startswith("SF:"):
            if source is not None or not row[3:] or any(char in row[3:] for char in "\x00\r\n"):
                raise ValueError("malformed or unterminated LCOV source record")
            source, lines, totals = row[3:], {}, {}
        elif row.startswith("DA:"):
            match = re.fullmatch(r"DA:([1-9]\d*),(\d+)(?:,[A-Za-z0-9+/=]+)?", row)
            if source is None or match is None:
                raise ValueError(f"malformed LCOV line record: {row}")
            number, hits = map(int, match.groups()[:2])
            lines[number] = lines.get(number, 0) + hits
        elif row.startswith(("LF:", "LH:")):
            key, value = row.split(":", 1)
            if source is None or key in totals or not value.isdigit():
                raise ValueError("malformed LCOV line summary")
            totals[key] = int(value)
        elif row == "end_of_record":
            # Compiler-generated class members can have function counters but
            # no line counters. Require explicit zero line summaries rather
            # than accepting an empty/truncated source record.
            if source is None or (not lines and totals != {"LF": 0, "LH": 0}):
                raise ValueError("LCOV record has no source/executable lines")
            if totals.get("LF", len(lines)) != len(lines) or totals.get(
                "LH", sum(v > 0 for v in lines.values())
            ) != sum(v > 0 for v in lines.values()):
                raise ValueError("LCOV line summaries disagree with records")
            target = result.setdefault(source, {})
            for number, hits in lines.items():
                target[number] = target.get(number, 0) + hits
            source = None
        elif metadata.fullmatch(row) is None:
            raise ValueError(f"unknown or malformed LCOV record: {row}")
    if source is not None or not any(result.values()):
        raise ValueError("empty or incomplete LCOV report")
    return result


def production_report(
    reports: Sequence[Mapping[str, Mapping[int, int]]],
    identities: Mapping[str, str],
    root: Path,
    scopes: Sequence[str] = (),
    *,
    mappings: Mapping[str, str] | None = None,
    require_application: bool = False,
) -> dict[str, dict[int, int]]:
    merged: dict[str, dict[int, int]] = {}
    for report in reports:
        for source, lines in report.items():
            relative = _source(source, root, mappings or {})
            if relative is None:
                continue
            path = root / relative
            if relative not in identities or _sha(path) != identities[relative]:
                raise ValueError(f"missing/stale source identity: {relative}")
            length = len(path.read_text(encoding="utf-8").splitlines())
            target = merged.setdefault(relative, {})
            for number, hits in lines.items():
                if type(number) is not int or not 0 < number <= length or type(hits) is not int or hits < 0:
                    raise ValueError(f"malformed/stale source line: {relative}:{number}")
                target[number] = target.get(number, 0) + hits
    merged = {source: lines for source, lines in merged.items() if lines}
    if not merged:
        raise ValueError("no executable production coverage data")
    if require_application and not any(
        source.startswith("app/") and source.endswith(".cpp") and any(hits > 0 for hits in lines.values())
        for source, lines in merged.items()
    ):
        raise ValueError("application coverage has no real app implementation counters")
    normalized = []
    for scope in scopes:
        scope_path = PurePosixPath(scope)
        if not scope or scope_path.is_absolute() or ".." in scope_path.parts or str(scope_path) != scope.rstrip("/"):
            raise ValueError(f"invalid coverage scope: {scope}")
        prefix = scope.rstrip("/")
        if not prefix.startswith(_PRODUCTION) and prefix not in {"app", "lib", "include"}:
            raise ValueError(f"scope is not a production prefix: {scope}")
        if not any(source == prefix or source.startswith(prefix + "/") for source in merged):
            raise ValueError(f"no executable coverage data for scope: {scope}")
        normalized.append(prefix)
    return {
        source: lines
        for source, lines in merged.items()
        if not normalized or any(source == scope or source.startswith(scope + "/") for scope in normalized)
    }


def _lcov(report: Mapping[str, Mapping[int, int]]) -> str:
    rows: list[str] = []
    for source, lines in sorted(report.items()):
        rows.extend(["TN:", f"SF:{source}"])
        rows.extend(f"DA:{number},{hits}" for number, hits in sorted(lines.items()))
        rows.extend([f"LF:{len(lines)}", f"LH:{sum(value > 0 for value in lines.values())}", "end_of_record"])
    return "\n".join(rows) + "\n"


def _bep_producer(event: Mapping[str, Any]) -> None:
    # These two fields retain Bazel 9.2's internal byte strings. A future
    # producer may use a different encoding even when a URI round-trips.
    started = event.get("started")
    version = started.get("buildToolVersion") if isinstance(started, dict) else None
    if version not in ("9.2.0", "9.2.0- (@non-git)"):
        raise ValueError(f"unsupported coverage BEP producer version: {version!r}")


def _bep_native_path(value: str) -> str:
    """Invert the pinned producer's UTF-8 bytes stored as Latin-1 characters."""
    if not isinstance(value, str) or "\x00" in value:
        raise ValueError("malformed Bazel 9.2 BEP path")
    try:
        return value.encode("latin-1").decode("utf-8")
    except UnicodeError as error:
        raise ValueError("invalid or lossy Bazel 9.2 BEP path encoding") from error


def _artifact(file: Mapping[str, Any], *, root: Path | None = None) -> Path:
    uri = file.get("uri")
    if not isinstance(uri, str):
        raise ValueError("coverage requires current local BEP artifact file URIs")
    parsed = urlparse(uri)
    if parsed.scheme != "file" or parsed.netloc or not parsed.path.startswith("/"):
        raise ValueError("coverage requires current local BEP artifact file URIs")
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
    path = Path(_bep_native_path(url2pathname(parsed.path)))
    if root is not None and (not path.is_relative_to(root) or not path.resolve().is_relative_to(root.resolve())):
        raise ValueError(f"BEP coverage artifact is outside the owned tree: {path}")
    if not path.is_file():
        raise ValueError(f"missing BEP coverage artifact: {path}")
    return path


def _events(path: Path) -> list[dict[str, Any]]:
    result = [json.loads(row) for row in path.read_text(encoding="utf-8").split("\n") if row.strip(" \t\r")]
    if not result or not all(isinstance(item, dict) for item in result) or result[-1].get("lastMessage") is not True:
        raise ValueError("incomplete coverage BEP")
    starts = [
        index for index, event in enumerate(result) if isinstance(event.get("id"), dict) and "started" in event["id"]
    ]
    if len(starts) != 1 or any("testResult" in event or "workspaceInfo" in event for event in result[: starts[0]]):
        raise ValueError("coverage BEP producer start is missing, duplicated or out of order")
    _bep_producer(result[starts[0]])
    return result


def _retain_archives(
    path: Path, directory: Path, tree: Path, expected: frozenset[bazel_test_ownership.ConfiguredTarget]
) -> dict[str, Any]:
    """Snapshot completed artifacts from a known-exited writer's valid BEP prefix.

    Prefix events are diagnostic evidence only, never completion/test-success
    evidence. Strict parsing and reconciliation must independently admit them.
    """
    retained: list[dict[str, Any]] = []
    errors: list[str] = []
    archive_index = 0
    producer_seen = False
    result: dict[str, Any] = {
        "status": "diagnostic-only",
        "prefix_events": 0,
        "archives": retained,
        "errors": errors,
    }
    try:
        with path.open(encoding="utf-8") as source:
            for number, row in enumerate(source, 1):
                if not row.strip(" \t\r\n"):
                    continue
                try:
                    event = json.loads(row)
                    if not isinstance(event, dict):
                        raise ValueError("BEP event is not an object")
                    identifier = event.get("id")
                    if isinstance(identifier, dict) and "started" in identifier:
                        if producer_seen:
                            raise ValueError("duplicate coverage BEP producer start")
                        _bep_producer(event)
                        producer_seen = True
                    if not producer_seen and ("testResult" in event or "workspaceInfo" in event):
                        raise ValueError("coverage BEP path event precedes its producer start")
                except ValueError as error:
                    errors.append(f"BEP prefix stopped at line {number}: {error}")
                    break
                result["prefix_events"] += 1
                if "testResult" not in event:
                    continue
                try:
                    identity = event["id"]["testResult"]
                    target = bazel_test_ownership.ConfiguredTarget(
                        identity["label"].removeprefix("@@"), identity["configuration"]["id"]
                    )
                    if target not in expected:
                        raise ValueError(f"unexpected configured diagnostic test result: {target}")
                    for file in event["testResult"].get("testActionOutput", []):
                        try:
                            if not str(file.get("name", "")).endswith("outputs.zip"):
                                continue
                            destination = directory / f"test-output-{archive_index:04d}.zip"
                            archive_index += 1
                            artifact = _artifact(file, root=tree)
                            # Keep failed copies too, without overwriting an
                            # earlier snapshot when a later artifact is invalid.
                            with artifact.open("rb") as incoming, destination.open("xb") as output:
                                shutil.copyfileobj(incoming, output)
                            retained.append(
                                {
                                    "source": str(artifact),
                                    "retained": str(destination),
                                    "sha256": _sha(destination),
                                    "diagnostic_only": True,
                                }
                            )
                        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
                            errors.append(f"BEP artifact at line {number}: {error}")
                except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
                    errors.append(f"BEP artifact at line {number}: {error}")
    except (OSError, ValueError) as error:
        errors.append(f"BEP diagnostic retention: {error}")
    return result


_COMBINED_REPORT = "bazel-out/_coverage/_coverage_report.dat"
_REPORT_LOCATION = re.compile(r"^INFO: LCOV coverage report is located at (.+)\n and execpath is (.+)$", re.MULTILINE)


def _combined_artifact(events: Sequence[dict[str, Any]], diagnostics: Path, tree: Path) -> Path:
    """Bind Bazel's combined LCOV report to this invocation.

    Bazel 9 declares the combined report only in its invocation output, not in
    BEP. The BEP still supplies the execroot, the executed CoverageReport
    actions and the invocation window that the report's write must fall inside.
    """
    roots = [event["workspaceInfo"]["localExecRoot"] for event in events if "workspace" in event.get("id", {})]
    started = [int(event["started"]["startTimeMillis"]) for event in events if "started" in event.get("id", {})]
    finished = [
        int(event["finished"]["finishTimeMillis"]) for event in events if "buildFinished" in event.get("id", {})
    ]
    executed = [
        int(action.get("actionsExecuted", 0))
        for event in events
        if "buildMetrics" in event
        for action in event["buildMetrics"]["actionSummary"].get("actionData", [])
        if action.get("mnemonic") == "CoverageReport"
    ]
    if len(roots) != 1 or len(started) != 1 or len(finished) != 1 or len(executed) != 1 or executed[0] < 1:
        raise ValueError("BEP does not bind an executed combined coverage report action to this invocation")
    report = Path(_bep_native_path(roots[0])) / _COMBINED_REPORT
    declared = [
        location
        for location, execpath in _REPORT_LOCATION.findall(diagnostics.read_text(encoding="utf-8"))
        if execpath == _COMBINED_REPORT
    ]
    if declared != [str(report)]:
        raise ValueError("Bazel did not declare exactly one current combined coverage report")
    if not report.is_relative_to(tree) or not report.resolve().is_relative_to(tree.resolve()) or not report.is_file():
        raise ValueError(f"combined coverage report is missing or outside the owned tree: {report}")
    if not started[0] <= report.stat().st_mtime_ns // 1_000_000 <= finished[0]:
        raise ValueError("combined coverage report was not written during this invocation")
    return report


def _test_artifacts(
    events: Sequence[dict[str, Any]],
    expected: frozenset[bazel_test_ownership.ConfiguredTarget],
    *,
    root: Path | None = None,
) -> dict[bazel_test_ownership.ConfiguredTarget, tuple[Path, Path, str | None]]:
    observed = {}
    for event in events:
        if "testResult" not in event:
            continue
        identity = event.get("id", {}).get("testResult", {})
        target = bazel_test_ownership.ConfiguredTarget(
            identity["label"].removeprefix("@@"), identity["configuration"]["id"]
        )
        result = event["testResult"]
        if target not in expected or target in observed:
            raise ValueError(f"unexpected/duplicate coverage test result: {target}")
        execution = result.get("executionInfo", {})
        if not isinstance(execution, dict):
            raise ValueError(f"malformed coverage test executionInfo: {target}")
        strategy = execution.get("strategy")
        if "strategy" in execution and (not isinstance(strategy, str) or not strategy.strip()):
            raise ValueError(f"malformed coverage test execution strategy: {target}")
        if (
            identity.get("run") != 1
            or identity.get("shard") != 1
            or identity.get("attempt") != 1
            or result.get("cachedLocally", False)
            or execution.get("cachedRemotely", False)
            or result.get("status") != "PASSED"
        ):
            raise ValueError(f"coverage test failed, sharded, retried or cached: {target}")
        files = [
            file for file in result.get("testActionOutput", []) if str(file.get("name", "")).endswith("outputs.zip")
        ]
        stock = [file for file in result.get("testActionOutput", []) if file.get("name") == "test.lcov"]
        if len(files) != 1 or len(stock) != 1:
            raise ValueError(f"missing/ambiguous strict archive or stock per-test LCOV artifact: {target}")
        observed[target] = (_artifact(files[0], root=root), _artifact(stock[0], root=root), strategy)
    if set(observed) != expected:
        raise ValueError("missing selected coverage test artifacts")
    return observed


def _collection(
    artifact: Path,
    directory: Path,
    attempt: str,
    label: str,
    *,
    gcov_sha256: str | None = None,
    expected_source_mappings: Mapping[str, str] | None = None,
) -> tuple[dict[str, dict[int, int]], dict[str, str]]:
    directory.mkdir()
    if artifact.suffix != ".zip":
        raise ValueError("coverage requires Bazel's declared zipped undeclared-test outputs")
    shutil.copyfile(artifact, directory / "outputs.zip")
    with zipfile.ZipFile(artifact) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("duplicate collection archive member")
        for item in archive.infolist():
            path = PurePosixPath(item.filename)
            if path.is_absolute() or ".." in path.parts or "\\" in item.filename:
                raise ValueError("unsafe collection archive member")
            if not item.filename.startswith("aobus-coverage/") or item.is_dir():
                continue
            if item.file_size > 512 * 1024 * 1024:
                raise ValueError("oversized collection archive member")
            destination = directory / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(item) as source, destination.open("xb") as output:
                shutil.copyfileobj(source, output)
    collected = directory / "aobus-coverage"
    receipt = native.read_json(collected / "receipt.json")
    test = native.read_json(collected / "test.json")
    if (
        receipt.get("schema") != 2
        or receipt.get("source_mapping_contract") != "declared-standard"
        or receipt.get("status") != "passed"
        or receipt.get("attempt") != attempt
        or receipt.get("test") != label
        or test.get("attempt") != attempt
        or test.get("exit") != 0
    ):
        raise ValueError("failed, stale or incompatible collection receipt")
    if gcov_sha256 is not None and receipt.get("gcov_sha256") != gcov_sha256:
        raise ValueError("collection reader differs from the selected compiler SDK identity")
    standard = receipt["standard_source_mappings"]
    mappings = dict(standard["mappings"])
    if standard["sha256"] is not None:
        standard_file = collected / "source-mappings.txt"
        if standard["sha256"] != _sha(standard_file):
            raise ValueError("changed standard source mappings")
        rows = [row.split(":") for row in standard_file.read_text(encoding="utf-8").splitlines()]
        if any(len(row) != 2 or not all(row) for row in rows) or len(rows) != len(mappings) or dict(rows) != mappings:
            raise ValueError("standard source mapping receipt differs from retained metadata")
    elif mappings or standard["path"] is not None:
        raise ValueError("missing standard source mapping metadata")
    for name, value in mappings.items():
        if not isinstance(name, str) or not isinstance(value, str):
            raise ValueError("malformed standard source mapping receipt")
        if (
            expected_source_mappings is not None
            and "_virtual_includes" in PurePosixPath(name).parts
            and expected_source_mappings.get(name) != value
        ):
            raise ValueError("standard source mapping differs from configured Symlink metadata")
    if receipt["source_mappings"] != mappings:
        raise ValueError("source mappings differ from retained standard metadata")
    profiles = receipt.get("profiles", [])
    if (
        not profiles
        or receipt.get("consumed_profiles") != len(profiles)
        or len(receipt.get("observed_profiles", [])) != len(profiles)
    ):
        raise ValueError("collection receipt did not consume every observed profile")
    for index, profile in enumerate(profiles):
        pair = collected / f"profile-{index:05d}"
        if (
            profile.get("status") != "passed"
            or profile.get("exit") != 0
            or profile.get("notes_sha256") != _sha(pair / "object.gcno")
            or profile.get("profile_sha256") != _sha(pair / "object.gcda")
        ):
            raise ValueError("missing, changed or failed profile extraction receipt")
        if not profile.get("reports"):
            raise ValueError("profile receipt has no extraction reports")
        for report in profile["reports"]:
            path = PurePosixPath(report["path"])
            if path.is_absolute() or ".." in path.parts or report["sha256"] != _sha(collected / path):
                raise ValueError("missing/changed raw extraction report")
    report = collected / "coverage.lcov"
    if receipt.get("lcov_sha256") != _sha(report):
        raise ValueError("changed raw per-test LCOV")
    if receipt.get("manifest_sha256") != _sha(collected / "instrumented-files.txt"):
        raise ValueError("changed instrumentation manifest")
    return parse_lcov(report.read_text(encoding="utf-8")), receipt["sources"]


def _flags() -> list[str]:
    return [
        "--collect_code_coverage",
        "--instrumentation_filter=^//(app|lib|include|test)(/|:)",
        "--instrument_test_targets",
        "--noexperimental_use_llvm_covmap",
        "--coverage_support=//bazel/coverage:coverage_support",
        "--coverage_output_generator=//bazel/coverage:merger",
        "--coverage_report_generator=//bazel/coverage:merger",
        "--run_under=//bazel/coverage:coverage_support",
        "--incompatible_bazel_test_exec_run_under",
        "--combined_report=lcov",
        "--test_timeout=300,600,1200,3600",
        "--//bazel:asan_enabled=false",
        "--//bazel:tsan_enabled=false",
        "--nocache_test_results",
        "--test_sharding_strategy=disabled",
        "--runs_per_test=1",
        "--flaky_test_attempts=1",
        "--experimental_split_coverage_postprocessing=false",
        "--zip_undeclared_test_outputs",
    ]


class _Attempt:
    def __init__(self, tree: Path, args: argparse.Namespace):
        parent = tree / "coverage"
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
        self.path = self.directory / "receipt.json"
        self.tree = tree
        self.args = args
        self.java_home: Path | None = None
        self.server_java_home: Path | None = None
        self.receipt: dict[str, Any] = {
            "schema": 1,
            "attempt": self.directory.name,
            "status": "running",
            "exit": None,
            "started_at": datetime.now(UTC).isoformat(),
            "source": str(PROJECT_ROOT),
            "config": "debug",
            "suite": args.coverage_suite,
            "scope": list(args.scope or []),
            "phases": [],
        }
        if getattr(args, "analysis_only", False):
            self.receipt.update(
                mode="analysis-only",
                evidence=(
                    "Configured coverage actions, compilation-context mapping propagation, source/gcno declarations "
                    "and merger runfiles membership only; native generated mapping/manifest contents are unavailable "
                    "via Bazel 9.2 analysis; no build, test execution, runtime fidelity, "
                    "profile measurement or report proof."
                ),
            )
        native.write_json(self.directory / "request.json", self.receipt)
        self.save()

    def save(self) -> None:
        native.write_json(self.path, self.receipt)

    def bazel(
        self,
        name: str,
        executable: Path,
        prepared: Path,
        action: str,
        arguments: Sequence[str],
        *,
        tolerate_failure: bool = False,
    ) -> Path:
        if getattr(self.args, "analysis_only", False) and action not in {"query", "cquery", "aquery", "info"}:
            raise native.BazelWorkspaceError(f"coverage analysis-only cannot execute Bazel {action}")
        if self.java_home is None or self.server_java_home is None:
            raise native.BazelWorkspaceError("coverage has no admitted Java runtime/server selection")
        argv = [*bazel_workspace.invocation(executable, prepared, isolate_rc=True), action]
        if f"--server_javabase={self.server_java_home}" not in argv:
            raise native.BazelWorkspaceError("coverage server Java home differs from the captured startup selection")
        if action != "query":
            argv.append(f"--jobs={self.args.jobs}")
        if self.args.remote_cache and action != "query":
            argv.append(f"--remote_cache={self.args.remote_cache}")
        argv.extend(arguments)
        output, errors = self.directory / f"{name}.stdout.log", self.directory / f"{name}.stderr.log"
        record: dict[str, Any] = {
            "name": name,
            "argv": argv,
            "stdout": str(output),
            "stderr": str(errors),
            "exit": None,
        }
        self.receipt["phases"].append(record)
        self.save()
        try:
            with output.open("xb") as out, errors.open("xb") as err:
                self.receipt["writer_state"] = "unknown"
                self.save()
                status = subprocess.run(
                    argv, cwd=PROJECT_ROOT, env=_environment(self.java_home), stdout=out, stderr=err, check=False
                ).returncode
            record["exit"] = status
            self.receipt["writer_state"] = "exited"
            if status and not tolerate_failure:
                raise native.BazelWorkspaceError(f"coverage phase {name} exited {status}; diagnostics: {errors}")
            return output
        finally:
            if self.receipt.get("writer_state") == "unknown":
                self.receipt["artifact_retention"] = {
                    "status": "not-attempted",
                    "reason": "writer exit is unknown; outputs may still be changing",
                }
                self.receipt["retry_requires"] = "owner confirmation that all profile writers are idle"
            self.save()
            if record["exit"] is not None:
                with (self.tree / "build.log").open("ab") as log:
                    log.write(f"\n=== coverage: {name} ===\n".encode())
                    for path in (output, errors):
                        if path.is_file():
                            with path.open("rb") as source:
                                shutil.copyfileobj(source, log)


def _label(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("target label must be a string")
    match = re.fullmatch(r"(?:@@?[A-Za-z0-9._+~-]+)?//([A-Za-z0-9._+~/-]*):([A-Za-z0-9._+~/-]+)", value)
    if match is None or any(
        part in {"", ".", ".."} for path in (match[1], match[2]) if path for part in path.split("/")
    ):
        raise ValueError("invalid target label: " + value)
    return value


def _identifier(value: object) -> str:
    if isinstance(value, bool) or not isinstance(value, (str, int)) or not re.fullmatch(r"[1-9][0-9]*", str(value)):
        raise ValueError("aquery ID must be a positive integer")
    return str(value)


def _records(document: dict[str, Any], name: str) -> list[dict[str, Any]]:
    values = document.get(name, [])
    if not isinstance(values, list) or any(not isinstance(value, dict) for value in values):
        raise ValueError(name + " must be a list of objects")
    return values


def _strings(value: object) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item or "\x00" in item for item in value):
        raise ValueError("expected a list of nonempty strings")
    return value


def _expand(arguments: list[str], params: dict[str, Any], active: tuple[str, ...] = ()) -> tuple[str, ...]:
    """Inline declared aquery response files; an unknown or recursive one fails closed."""
    result: list[str] = []
    for argument in arguments:
        if argument.startswith("@"):
            path = argument[1:]
            if path not in params or path in active:
                raise ValueError("unexpanded or recursive response file: " + argument)
            result.extend(_expand(params[path], params, (*active, path)))
        else:
            result.append(argument)
    return tuple(result)


def _action_proof(document: dict[str, Any], require_application: bool) -> dict[str, int]:
    compiles = bazel_contract.extract_compile_actions(document)
    owned = [action for action in compiles if action.label.startswith(("//app/", "//lib/", "//include", "//test:"))]
    target = [action for action in owned if not action.is_tool]
    if not target:
        raise ValueError("coverage analysis has no production/test compile actions")
    instrumentation = {"-fprofile-arcs", "-ftest-coverage"}

    def coverage_option(argument: str) -> bool:
        option = argument.lstrip("-")
        return argument.startswith("-") and (
            option == "coverage"
            or option.startswith(
                (
                    "fprofile-",
                    "fno-profile-",
                    "ftest-coverage",
                    "fno-test-coverage",
                    "fcoverage-",
                    "fno-coverage-",
                    "coverage-",
                    "no-coverage-",
                    "gcov-",
                    "no-gcov-",
                )
            )
        )

    def reject_forwarded(argv: Sequence[str], label: str) -> None:
        # Extraction already expands response files. Inspect forwarding payloads
        # too, but never treat a forwarded driver flag as effective atomic proof.
        # Relevant overrides fail closed; unrelated compiler/forwarding flags do
        # not need an argument grammar here.
        forwarded = False
        for argument in argv[1:]:
            wrapper, separator, payload = argument.partition("=")
            is_wrapper = wrapper in {"-Xclang", "-Xpreprocessor", "-Xassembler", "-Xlinker", "-mllvm"} or (
                wrapper.startswith("-Xarch_")
            )
            if argument.startswith(("-Wp,", "-Wl,", "-Wa,")):
                relevant = any(coverage_option(token) for token in argument.split(",")[1:])
            elif is_wrapper and separator:
                relevant = coverage_option(payload)
            else:
                relevant = forwarded and coverage_option(argument)
            if relevant:
                raise ValueError(f"forwarded coverage option cannot prove safe counters: {label}: {argument}")
            forwarded = is_wrapper and not separator

    for action in compiles:
        flags = set(action.argv)
        reject_forwarded(action.argv, action.label)
        if action.is_tool and any(coverage_option(argument) for argument in action.argv):
            raise ValueError(f"coverage leaked into exec compilation: {action.label}")
        if action in target and not instrumentation <= flags:
            raise ValueError(f"owned target compile is not gcov instrumented: {action.label}")
        measured = bool(flags & (instrumentation | {"--coverage"}))
        if measured and not action.is_tool:
            # This literal contract is independent of the toolchain producer.
            # Presence alone is insufficient: early or late overrides, including
            # cc1/backend spellings, must not silently select non-atomic updates.
            atomic = "-fprofile-update=atomic"
            for argument in action.argv:
                option = argument.lstrip("-")
                if (
                    (option.startswith(("fprofile-update", "fno-profile-update")) and argument != atomic)
                    or option.startswith(("coverage-atomic-counter", "no-coverage-atomic-counter"))
                    or option.startswith(("gcov-atomic-counter", "no-gcov-atomic-counter"))
                    or argument in {"-fno-profile-arcs", "-fno-test-coverage"}
                ):
                    raise ValueError(f"unsafe gcov counter option: {action.label}: {argument}")
            if atomic not in flags:
                raise ValueError(f"gcov compile lacks explicit atomic counter updates: {action.label}")
    if require_application and not any(
        action.label.startswith("//app/")
        and any(arg.startswith("app/") and arg.endswith(".cpp") for arg in action.argv)
        for action in target
    ):
        raise ValueError("coverage analysis has no actual app implementation compile")
    configurations = bazel_contract._unique_pairs(
        (_identifier(item["id"]), item) for item in document.get("configuration", [])
    )
    targets = bazel_contract._unique_pairs(
        (_identifier(item["id"]), item["label"]) for item in document.get("targets", [])
    )
    links = 0
    for action in document.get("actions", []):
        mnemonic = action.get("mnemonic")
        if mnemonic not in {"CppLink", "CppArchive"}:
            continue
        configuration_id = _identifier(action.get("configurationId"))
        if configuration_id not in configurations:
            raise ValueError(f"unknown coverage link/archive configuration: {configuration_id}")
        configuration = configurations[configuration_id]
        is_tool = configuration.get("isTool", False)
        if type(is_tool) is not bool:
            raise ValueError("coverage link/archive configuration isTool must be a boolean")
        target_id = _identifier(action.get("targetId"))
        if target_id not in targets:
            raise ValueError(f"unknown coverage link/archive target: {target_id}")
        params = bazel_contract._unique_pairs(
            (_strings([item["execPath"]])[0], _strings(item["arguments"])) for item in _records(action, "paramFiles")
        )
        argv = _expand(_strings(action.get("arguments", [])), params)
        if not argv:
            raise ValueError("coverage link/archive has no command line")
        reject_forwarded(argv, targets[target_id])
        if mnemonic == "CppArchive" and any(coverage_option(flag) or flag.startswith("-fprofile") for flag in argv):
            raise ValueError("coverage flags passed to static archiver")
        if mnemonic == "CppLink" and is_tool and any(coverage_option(flag) for flag in argv):
            raise ValueError(f"coverage leaked into exec link: {targets[target_id]}")
        if mnemonic == "CppLink" and not is_tool and "--coverage" in argv:
            links += 1
    if not links:
        raise ValueError("coverage analysis has no instrumented link action")
    return {"target_compiles": len(target), "exec_compiles": sum(action.is_tool for action in compiles), "links": links}


def _action_graph(
    document: dict[str, Any],
) -> tuple[
    Callable[[str, str | int], dict[str, Any]],
    Callable[[str | int], str],
    Callable[[str], frozenset[str]],
]:
    """Resolve declared artifacts and transitive inputs without executing actions."""
    tables: dict[str, dict[str, dict[str, Any]]] = {
        name: bazel_contract._unique_pairs((str(item["id"]), item) for item in document.get(name, []))
        for name in ("targets", "configuration", "pathFragments", "artifacts", "depSetOfFiles")
    }
    active_fragments: set[str] = set()
    active_depsets: set[str] = set()

    def lookup(table: str, identifier: str | int) -> dict[str, Any]:
        item = tables[table].get(str(identifier))
        if item is None:
            raise ValueError(f"unknown aquery {table} ID: {identifier}")
        return item

    @cache
    def fragment(identifier: str) -> str:
        if identifier in active_fragments:
            raise ValueError("recursive aquery path fragment")
        active_fragments.add(identifier)
        try:
            item = lookup("pathFragments", identifier)
            label = item["label"]
            if (
                not isinstance(label, str)
                or not label
                or label in {".", ".."}
                or any(char in label for char in "/\\:")
                or any(ord(char) < 32 or ord(char) == 127 for char in label)
            ):
                raise ValueError("unsafe aquery path fragment")
            parent = item.get("parentId", 0)
            return (fragment(str(parent)) + "/" if parent not in (0, "0") else "") + label
        finally:
            active_fragments.remove(identifier)

    def artifact(identifier: str | int) -> str:
        return fragment(str(lookup("artifacts", identifier)["pathFragmentId"]))

    @cache
    def inputs(identifier: str) -> frozenset[str]:
        if identifier in active_depsets:
            raise ValueError("recursive aquery input depset")
        active_depsets.add(identifier)
        try:
            item = lookup("depSetOfFiles", identifier)
            direct = [str(value) for value in item.get("directArtifactIds", [])]
            children = [str(value) for value in item.get("transitiveDepSetIds", [])]
            if len(direct) != len(set(direct)) or len(children) != len(set(children)):
                raise ValueError("duplicate aquery input depset reference")
            return frozenset(artifact(value) for value in direct).union(*(inputs(value) for value in children))
        finally:
            active_depsets.remove(identifier)

    return lookup, artifact, inputs


def _action_mappings(document: dict[str, Any]) -> dict[str, str]:
    """Resolve target Symlink aliases from IDs only, never from path spelling."""
    lookup, artifact, inputs = _action_graph(document)
    result: dict[str, str] = {}
    for action in document.get("actions", []):
        if action.get("mnemonic") != "Symlink":
            continue
        lookup("targets", action["targetId"])
        configuration = lookup("configuration", action["configurationId"])
        if type(configuration.get("isTool", False)) is not bool:
            raise ValueError("malformed Symlink configuration")
        if configuration.get("isTool", False):
            continue
        outputs = [artifact(value) for value in action.get("outputIds", [])]
        if len(outputs) != 1:
            raise ValueError("Symlink requires exactly one declared output")
        if "primaryOutputId" in action and str(action["primaryOutputId"]) != str(action["outputIds"][0]):
            raise ValueError("inconsistent Symlink primary output")
        if "_virtual_includes" not in PurePosixPath(outputs[0]).parts:
            continue
        dependencies = [str(value) for value in action.get("inputDepSetIds", [])]
        if len(dependencies) != len(set(dependencies)):
            raise ValueError("duplicate Symlink input depset reference")
        sources = frozenset().union(*(inputs(value) for value in dependencies))
        if len(sources) != 1:
            raise ValueError("virtual header Symlink requires exactly one input")
        alias, source = outputs[0], next(iter(sources))
        if (
            PurePosixPath(alias).parts[0] != "bazel-out"
            or len(PurePosixPath(source).parts) < 2
            or PurePosixPath(source).parts[0] not in {"app", "lib", "include", "test", "external", "bazel-out"}
            or "_virtual_includes" in PurePosixPath(source).parts
        ):
            raise ValueError("unsupported virtual header Symlink source namespace")
        if result.setdefault(alias, source) != source:
            raise ValueError(f"conflicting configured virtual header Symlink: {alias}")
    return result


def _runtime_provider_facts(
    attempt: _Attempt,
    prepared: Path,
    executable: Path,
    flags: Sequence[str],
    document: dict[str, Any],
    expected: frozenset[bazel_test_ownership.ConfiguredTarget],
) -> dict[str, Any]:
    """Observe independent public imports in the actual consumers' merger configuration."""
    lookup, artifact, inputs = _action_graph(document)
    observed = set()
    configurations = set()
    writes = [
        action
        for action in document.get("actions", [])
        if action.get("mnemonic") == "FileWrite"
        and lookup("targets", action["targetId"])["label"] == "//bazel/coverage:merger"
    ]
    for action in document.get("actions", []):
        if action.get("mnemonic") != "TestRunner":
            continue
        config = lookup("configuration", action["configurationId"])
        consumer = bazel_test_ownership.ConfiguredTarget(
            bazel_test_ownership._label(lookup("targets", action["targetId"])["label"]),
            bazel_test_ownership._configuration(config["checksum"]),
        )
        if consumer not in expected:
            continue
        if consumer in observed or config.get("isTool", False) is not False:
            raise ValueError("ambiguous configured coverage provider consumer")
        observed.add(consumer)
        bound = frozenset().union(*(inputs(str(value)) for value in action.get("inputDepSetIds", [])))
        mergers = [write for write in writes if any(artifact(value) in bound for value in write.get("outputIds", []))]
        if len(mergers) != 1:
            raise ValueError("selected consumer lacks one configured coverage merger")
        merger_config = lookup("configuration", mergers[0]["configurationId"])
        if merger_config.get("isTool", False) is not True:
            raise ValueError("coverage merger provider configuration is not exec")
        configurations.add(bazel_test_ownership._configuration(merger_config["checksum"]))
    if not expected or observed != expected or len(configurations) != 1:
        raise ValueError("coverage public providers require one selected merger exec configuration")
    configuration = next(iter(configurations))
    universe = ",".join(sorted({target.label for target in expected}))
    inventories = {}
    queries = {}
    expression = (
        'json.encode({"label": str(target.label), "files": sorted([file.path for file in target.files.to_list()])})'
    )
    for name, target in (
        ("java-runtime", "@bazel_tools//tools/jdk:jdk"),
        ("stock-payload", "@remote_coverage_tools//:all_lcov_merger_lib"),
    ):
        output = attempt.bazel(
            "declared-" + name,
            executable,
            prepared,
            "cquery",
            [
                f"config({target}, {configuration})",
                "--universe_scope=" + universe,
                "--output=starlark",
                "--starlark:expr=" + expression,
                *flags,
            ],
        )
        rows = output.read_text(encoding="utf-8").splitlines()
        if len(rows) != 1:
            raise ValueError(f"coverage {name} requires one configured provider record")
        row = json.loads(rows[0], object_pairs_hook=bazel_contract._unique_pairs)
        if not isinstance(row, dict) or set(row) != {"label", "files"}:
            raise ValueError("malformed configured coverage provider record")
        resolved_label = _label(row["label"])
        files = _strings(row["files"])
        if not files or len(files) != len(set(files)):
            raise ValueError(f"empty/duplicate declared coverage {name} files")
        for path in files:
            bazel_test_ownership._path(path)
            if not path.startswith("external/"):
                raise ValueError(f"coverage {name} must expose imported source files")
        inventories[name] = sorted(files)
        queries[name] = {
            "target": target,
            "resolved_label": resolved_label,
            "configuration": configuration,
            "universe": sorted({target.label for target in expected}),
            "path": str(output),
            "sha256": _sha(output),
        }
    java = [path for path in inventories["java-runtime"] if path.endswith("/bin/java")]
    payload = inventories["stock-payload"]
    if len(java) != 1 or len(payload) != 1 or PurePosixPath(payload[0]).name != "all_lcov_merger_tools_deploy.jar":
        raise ValueError("public coverage imports lack one declared Java executable/original stock deploy JAR")
    facts = {
        "java": java[0],
        "runtime_files": inventories["java-runtime"],
        "stock_payload": payload[0],
        "queries": queries,
        "scope": (
            "DefaultInfo source-file declarations of the standard JDK and stock runtime JAR imports in the same "
            "prepared coverage context and actual bound merger exec configuration/selected consumer universe. "
            "Not complete JDK physical-input identity, materialization or execution."
        ),
    }
    attempt.receipt["coverage_runtime_providers"] = facts
    attempt.save()
    return facts


def _coverage_contexts(
    attempt: _Attempt,
    prepared: Path,
    executable: Path,
    flags: Sequence[str],
    expected: frozenset[bazel_test_ownership.ConfiguredTarget],
) -> dict[bazel_test_ownership.ConfiguredTarget, dict[str, Any]]:
    """Read the pinned exposed context mapping and public instrumentation declarations.

    Native standard-map and manifest write actions expose no content through
    Bazel 9.2 analysis. This observes their upstream declarations, not their bytes.
    """
    context = '[value.compilation_context for key, value in providers(target).items() if key.endswith("%CcInfo")][0]'
    expression = (
        'json.encode({"label": str(target.label), "mappings": [list(pair) for pair in '
        + context
        + '._virtual_to_original_headers.to_list()], "instrumented_files": sorted([file.path for file in '
        'providers(target)["InstrumentedFilesInfo"].instrumented_files.to_list()]), '
        '"metadata_files": sorted([file.path for file in '
        'providers(target)["InstrumentedFilesInfo"].metadata_files.to_list()])})'
    )
    result = {}
    queries: list[dict[str, Any]] = []
    for checksum in sorted({target.configuration for target in expected}):
        batch = frozenset(target for target in expected if target.configuration == checksum)
        labels = sorted(target.label for target in batch)
        output = attempt.bazel(
            f"coverage-context-{len(queries):04d}",
            executable,
            prepared,
            "cquery",
            [
                "config(set(" + " ".join(labels) + f"), {checksum})",
                "--output=starlark",
                "--starlark:expr=" + expression,
                *flags,
            ],
        )
        observed = set()
        for text in output.read_text(encoding="utf-8").splitlines():
            row = json.loads(text, object_pairs_hook=bazel_contract._unique_pairs)
            if not isinstance(row, dict) or set(row) != {"label", "mappings", "instrumented_files", "metadata_files"}:
                raise ValueError("malformed configured coverage context record")
            target = bazel_test_ownership.ConfiguredTarget(bazel_test_ownership._label(row["label"]), checksum)
            if target not in batch or target in observed:
                raise ValueError("unexpected/duplicate configured coverage context")
            observed.add(target)
            pairs = row["mappings"]
            if not isinstance(pairs, list) or any(
                not isinstance(pair, list) or len(pair) != 2 or any(not isinstance(value, str) for value in pair)
                for pair in pairs
            ):
                raise ValueError("malformed compilation-context source mapping")
            for pair in pairs:
                for value in pair:
                    bazel_test_ownership._path(value)
            row["mappings"] = bazel_contract._unique_pairs((pair[0], pair[1]) for pair in pairs)
            for name in ("instrumented_files", "metadata_files"):
                files = _strings(row[name])
                if len(files) != len(set(files)):
                    raise ValueError("duplicate configured coverage context files")
                for path in files:
                    bazel_test_ownership._path(path)
            result[target] = row
        if observed != batch:
            raise ValueError("missing selected configured coverage contexts")
        queries.append({"configuration": checksum, "labels": labels, "path": str(output), "sha256": _sha(output)})
    attempt.receipt["coverage_context_queries"] = queries
    attempt.save()
    return result


def _analysis_proof(
    document: dict[str, Any],
    expected: frozenset[bazel_test_ownership.ConfiguredTarget],
    mappings: Mapping[str, str],
    runtime_facts: Mapping[str, Any],
    contexts: Mapping[bazel_test_ownership.ConfiguredTarget, Mapping[str, Any]],
) -> dict[str, Any]:
    """Join declared metadata and merger runfiles to the selected test consumers.

    Context mappings and instrumentation declarations do not prove native
    generated metadata contents. The private seam is an existing product edge;
    its aliases come from configured Symlink actions, not a header inventory.
    """
    lookup, artifact, inputs = _action_graph(document)
    actions = document.get("actions", [])

    def label(action: dict[str, Any]) -> str:
        return str(lookup("targets", action["targetId"])["label"]).removeprefix("@@")

    def configured(action: dict[str, Any], *, tool: bool) -> bazel_test_ownership.ConfiguredTarget:
        configuration = lookup("configuration", action["configurationId"])
        if type(configuration.get("isTool", False)) is not bool or configuration.get("isTool", False) != tool:
            raise ValueError("coverage analysis consumer has the wrong target/exec configuration")
        return bazel_test_ownership.ConfiguredTarget(
            bazel_test_ownership._label(label(action)),
            bazel_test_ownership._configuration(configuration["checksum"]),
        )

    def dependencies(action: dict[str, Any]) -> frozenset[str]:
        identifiers = [str(value) for value in action.get("inputDepSetIds", [])]
        if len(identifiers) != len(set(identifiers)):
            raise ValueError("duplicate coverage analysis input depset")
        return frozenset().union(*(inputs(value) for value in identifiers))

    producers: dict[str, list[dict[str, Any]]] = {}
    for action in actions:
        for identifier in action.get("outputIds", []):
            producers.setdefault(artifact(identifier), []).append(action)

    def producer(
        path: str, mnemonic: str, owner: bazel_test_ownership.ConfiguredTarget, *, tool: bool = False
    ) -> dict[str, Any]:
        candidates = producers.get(path, [])
        if len(candidates) != 1:
            raise ValueError(f"missing/ambiguous coverage analysis producer: {path}")
        action = candidates[0]
        if (
            action.get("mnemonic") != mnemonic
            or configured(action, tool=tool) != owner
            or [artifact(value) for value in action.get("outputIds", [])] != [path]
        ):
            raise ValueError(f"coverage analysis producer is not owned by its configured consumer: {path}")
        if "primaryOutputId" in action and artifact(action["primaryOutputId"]) != path:
            raise ValueError("inconsistent coverage analysis primary output")
        return action

    def contents(action: dict[str, Any]) -> str:
        value = action.get("fileContents")
        if not isinstance(value, str):
            raise ValueError("coverage analysis requires FileWrite contents")
        return value

    def checksum(value: str) -> str:
        return hashlib.sha256(value.encode("utf-8")).hexdigest()

    private_owner = "//lib/lmdb:private_headers"
    library_present = any(label(action) == "//lib/library:library" for action in actions)
    private_aliases = {
        artifact(identifier): mappings[artifact(identifier)]
        for action in actions
        if action.get("mnemonic") == "Symlink"
        and label(action) == private_owner
        and not lookup("configuration", action["configurationId"]).get("isTool", False)
        for identifier in action.get("outputIds", [])
        if artifact(identifier) in mappings
    }
    if library_present and not private_aliases:
        raise ValueError("library closure lacks private implementation header aliases")

    consumers = []
    observed: set[bazel_test_ownership.ConfiguredTarget] = set()
    private_consumers = []
    mergers: dict[str, dict[str, Any]] = {}
    for action in actions:
        if action.get("mnemonic") != "TestRunner":
            continue
        target = configured(action, tool=False)
        if target not in expected:
            continue
        if target in observed:
            raise ValueError("duplicate configured coverage TestRunner")
        observed.add(target)
        bound = dependencies(action)
        metadata = {}
        for name, suffix in (("mapping", "/reported_to_actual_sources.txt"), ("manifest", ".instrumented_files")):
            paths = [path for path in bound if path.endswith(suffix)]
            if len(paths) != 1:
                raise ValueError(f"selected TestRunner lacks one bound {name} FileWrite")
            path = paths[0]
            producer(path, "FileWrite", target)
            metadata[name] = {"path": path}
        context = contexts[target]
        declared = context["mappings"]
        if any(mappings.get(alias) != source for alias, source in declared.items()):
            raise ValueError("compilation-context mapping differs from configured Symlink metadata")
        sources = set(context["instrumented_files"])
        notes = set(context["metadata_files"])
        compile_notes = {
            artifact(identifier)
            for compile in actions
            if compile.get("mnemonic") == "CppCompile"
            and not lookup("configuration", compile["configurationId"]).get("isTool", False)
            for identifier in compile.get("outputIds", [])
        }
        if (
            not sources
            or not notes
            or any(not path.endswith(".gcno") for path in notes)
            or not notes <= bound & compile_notes
        ):
            raise ValueError("instrumentation declarations lack source/gcno inputs from actual target compiles")
        if private_aliases and private_aliases.items() <= declared.items() and set(private_aliases.values()) <= sources:
            private_consumers.append({"label": target.label, "configuration": target.configuration})

        runners = [
            path
            for path in bound
            if any(
                item.get("mnemonic") == "FileWrite" and label(item) == "//bazel/coverage:merger"
                for item in producers.get(path, [])
            )
        ]
        if len(runners) != 1:
            raise ValueError("selected TestRunner lacks one bound declared coverage merger")
        runner = runners[0]
        merger = producers[runner][0]
        owner = configured(merger, tool=True)
        merger = producer(runner, "FileWrite", owner, tool=True)
        tree_path = runner + ".runfiles"
        if tree_path not in bound:
            raise ValueError("selected TestRunner does not consume its merger runfiles tree")
        tree = producer(tree_path, "RunfilesTree", owner, tool=True)
        members = dependencies(tree)
        command = contents(merger)
        keys = []
        for assignment in ("export JAVABIN", "export JAVA_HOME", "payload"):
            values = re.findall(r"^" + assignment + r"=\"\$runfiles/([^\"\n]+)\"$", command, re.MULTILINE)
            if len(values) != 1:
                raise ValueError("coverage merger lacks an unambiguous declared runfiles key")
            keys.append(bazel_test_ownership._path(values[0]))
        java_key, home_key, _ = keys
        java_path, home_path, payload_path = ("external/" + key for key in keys)
        required = {
            home_path + "/" + suffix
            for suffix in ("bin/java", "lib/modules", "lib/server/libjvm.so", "conf/security/java.security", "release")
        }
        if (
            merger.get("isExecutable") is not True
            or not java_key.endswith("/bin/java")
            or java_key != home_key + "/bin/java"
            or java_path != runtime_facts["java"]
            or payload_path != runtime_facts["stock_payload"]
            or not {runner, java_path, payload_path, *required, *runtime_facts["runtime_files"]} <= members
            or 'exec "$JAVABIN" -cp "$payload" com.google.devtools.coverageoutputgenerator.Main "$@"' not in command
        ):
            raise ValueError("coverage merger runtime/payload runfiles membership or runner keys disagree")
        mergers[runner] = {
            "label": owner.label,
            "configuration": owner.configuration,
            "runner": runner,
            "runner_sha256": checksum(command),
            "runfiles_tree": tree_path,
            "java": java_path,
            "stock_payload": payload_path,
            "declared_runtime_files": sorted(path for path in members if path.startswith(home_path + "/")),
        }
        consumers.append(
            {
                "label": target.label,
                "configuration": target.configuration,
                "standard_source_mapping": metadata["mapping"],
                "instrumentation_manifest": metadata["manifest"],
                "context_mapping_entries": len(declared),
                "instrumented_sources": len(sources),
                "metadata_files": len(notes),
                "merger": runner,
            }
        )
    if not expected or observed != expected:
        raise ValueError("coverage analysis lacks every selected configured TestRunner")
    if library_present and not private_consumers:
        raise ValueError("no selected test context propagates all private implementation header mappings/sources")
    return {
        "consumers": consumers,
        "private_implementation_mapping": {
            "status": "context-propagation-passed" if library_present else "not-applicable",
            "evidence_scope": (
                "CcCompilationContext mapping propagation and InstrumentedFilesInfo source declarations only"
            ),
            "owner": private_owner,
            "aliases": private_aliases,
            "consumers": private_consumers,
            "library_in_closure": library_present,
        },
        "native_metadata_content_inspection": "unavailable via Bazel 9.2 analysis; runtime only",
        "mergers": list(mergers.values()),
        "runtime_scope": (
            "Declared analysis/runfiles membership, including all independently queried public JDK files; "
            "not complete JDK physical-input identity, materialization or runtime execution."
        ),
    }


def _analyze(
    args: argparse.Namespace, attempt: _Attempt, prepared: Path, executable: Path, identity: dict[str, str]
) -> tuple[frozenset[bazel_test_ownership.ConfiguredTarget], dict[str, str], Path, str]:
    root = f"//:coverage_{args.coverage_suite}_tests"
    flags = _flags()
    labels_path = attempt.bazel("suite-selection", executable, prepared, "query", [f"tests({root})", "--output=label"])
    labels = bazel_check._labels(labels_path.read_text(encoding="utf-8"))
    selection = attempt.bazel(
        "selection", executable, prepared, "cquery", ["set(" + " ".join(labels) + ")", "--output=jsonproto", *flags]
    )
    expected = bazel_test_ownership.extract_configured_targets(json.loads(selection.read_text(encoding="utf-8")))
    if not expected:
        raise ValueError("coverage selected no configured test executables")
    attempt.receipt["selected_tests"] = [
        {"label": target.label, "configuration": target.configuration} for target in sorted(expected)
    ]
    if getattr(args, "analysis_only", False):
        attempt.receipt["configured_selection"] = {"path": str(selection), "sha256": _sha(selection)}
    require_application = args.coverage_suite in {"default", "all", "application", "cli", "tui", "gtk"}
    actions = attempt.bazel(
        "producer-actions",
        executable,
        prepared,
        "aquery",
        [
            "deps(set(" + " ".join(labels) + "))",
            "--output=jsonproto",
            "--include_artifacts",
            "--include_commandline",
            "--include_param_files",
            *(["--include_file_write_contents"] if getattr(args, "analysis_only", False) else []),
            *flags,
        ],
    )
    document = json.loads(actions.read_text(encoding="utf-8"))
    actions_sha256 = _sha(actions)
    attempt.receipt["producer_actions"] = _action_proof(document, require_application)
    expected_mappings = _action_mappings(document)
    # This is an independent controller-side oracle, never a test/merger input.
    # The declared standard metadata must supply every observed virtual source.
    attempt.receipt["source_mapping_expectations"] = {
        "aliases": len(expected_mappings),
        "aquery": str(actions),
        "aquery_sha256": actions_sha256,
    }
    if getattr(args, "analysis_only", False):
        runtime_facts = _runtime_provider_facts(attempt, prepared, executable, flags, document, expected)
        contexts = _coverage_contexts(attempt, prepared, executable, flags, expected)
        attempt.receipt["analysis_proof"] = _analysis_proof(
            document, expected, expected_mappings, runtime_facts, contexts
        )
        evidence = [*runtime_facts["queries"].values(), *attempt.receipt["coverage_context_queries"]]
        if any(_sha(Path(query["path"])) != query["sha256"] for query in evidence):
            raise ValueError("declared coverage runtime provider evidence changed during analysis")
    attempt.save()
    if _source_identity(PROJECT_ROOT) != identity:
        raise ValueError("authored sources changed during coverage analysis")
    if _sha(actions) != actions_sha256:
        raise ValueError("configured source mapping evidence changed before coverage")
    return expected, expected_mappings, actions, actions_sha256


def _measure(
    args: argparse.Namespace, attempt: _Attempt, prepared: Path, executable: Path, identity: dict[str, str]
) -> None:
    root = f"//:coverage_{args.coverage_suite}_tests"
    flags = _flags()
    require_application = args.coverage_suite in {"default", "all", "application", "cli", "tui", "gtk"}
    expected, expected_mappings, actions, actions_sha256 = _analyze(args, attempt, prepared, executable, identity)
    bep = attempt.directory / "coverage.bep.json"
    attempt.bazel(
        "coverage",
        executable,
        prepared,
        "coverage",
        [root, *flags, "--keep_going", f"--build_event_json_file={bep}"],
        tolerate_failure=True,
    )
    attempt.receipt["bazel_exit"] = attempt.receipt["phases"][-1]["exit"]
    # A known-exited writer can leave completed results before a truncated BEP
    # tail. Snapshot those archives before strict reconciliation rejects it.
    retention = _retain_archives(bep, attempt.directory, attempt.tree, expected)
    attempt.receipt["artifact_retention"] = retention
    attempt.receipt["test_output_archives"] = retention["archives"]
    attempt.save()
    failure = f"Bazel coverage exited {attempt.receipt['bazel_exit']}; see diagnostic artifact retention"
    try:
        events = _events(bep)
    except (OSError, ValueError, TypeError) as error:
        attempt.receipt["bep_error"] = str(error)
        attempt.save()
        if attempt.receipt["bazel_exit"]:
            raise native.BazelWorkspaceError(failure) from error
        raise
    if attempt.receipt["bazel_exit"]:
        raise native.BazelWorkspaceError(failure)
    if retention["errors"]:
        raise ValueError("coverage diagnostic archive retention was incomplete; see receipt")
    if _sha(actions) != actions_sha256 or _source_identity(PROJECT_ROOT) != identity:
        raise ValueError("configured source mapping evidence or authored sources changed during coverage")
    results = bazel_check._test_results(bep, expected, fresh=True)
    if any(result["runs"] != 1 for result in results):
        raise ValueError("coverage tests did not execute exactly once")
    artifacts = _test_artifacts(events, expected, root=attempt.tree)
    attempt.receipt["test_results"] = [
        result
        | {
            "execution_strategy": artifacts[
                bazel_test_ownership.ConfiguredTarget(result["label"], result["configuration"])
            ][2]
        }
        for result in results
    ]
    attempt.save()
    reports = []
    readers = [
        item["sha256"]
        for item in attempt.receipt["compiler_identity"]["files"]
        if Path(item["path"]).name == "llvm-cov"
    ]
    if len(readers) != 1:
        raise ValueError("selected compiler SDK identity lacks one exact llvm-cov reader")
    measured_sources: dict[str, str] = {}
    snapshots = {item["source"]: item for item in retention["archives"]}
    stock_reports: dict[bazel_test_ownership.ConfiguredTarget, Path] = {}
    attempt.receipt["stock_test_reports"] = []
    attempt.receipt["strict_test_reports"] = []
    for index, (target, (artifact, stock, _)) in enumerate(sorted(artifacts.items())):
        if str(artifact) not in snapshots:
            raise ValueError("current configured coverage artifact has no retained snapshot")
        stock_reports[target] = attempt.directory / f"test-stock-{index:04d}.lcov"
        shutil.copyfile(stock, stock_reports[target])
        attempt.receipt["stock_test_reports"].append(
            {
                "label": target.label,
                "configuration": target.configuration,
                "source": str(stock),
                "retained": str(stock_reports[target]),
                "sha256": _sha(stock_reports[target]),
            }
        )
    retention["status"] = "current-complete-bep"
    for artifact, _, _ in artifacts.values():
        snapshots[str(artifact)]["diagnostic_only"] = False
    attempt.save()
    stock_mappings: dict[str, str] = {}
    for index, (target, (artifact, _, _)) in enumerate(sorted(artifacts.items())):
        directory = attempt.directory / f"test-{index:04d}"
        data, sources = _collection(
            Path(snapshots[str(artifact)]["retained"]),
            directory,
            attempt.directory.name,
            target.label,
            gcov_sha256=readers[0],
            expected_source_mappings=expected_mappings,
        )
        for source, checksum in sources.items():
            relative = _source(source, PROJECT_ROOT, {})
            if relative is not None:
                if identity.get(relative) != checksum:
                    raise ValueError(f"source changed since coverage request: {relative}")
                measured_sources[relative] = checksum
        collection = native.read_json(directory / "aobus-coverage/receipt.json")
        mappings = dict(collection["source_mappings"])
        execroot = Path(collection["root"])
        if not execroot.is_absolute() or any(
            not isinstance(k, str) or not isinstance(v, str) for k, v in mappings.items()
        ):
            raise ValueError("malformed strict source mapping metadata")
        expanded = [(str(execroot / name), value) for name, value in mappings.items()]
        expanded.extend((str(execroot / name), name) for name in sources if not Path(name).is_absolute())
        for name, value in expanded:
            if mappings.setdefault(name, value) != value:
                raise ValueError(f"conflicting absolute source mapping: {name}")
        for name, value in mappings.items():
            if stock_mappings.setdefault(name, value) != value:
                raise ValueError(f"conflicting reported source mapping: {name}")
        strict = production_report([data], measured_sources, PROJECT_ROOT)
        stock_measurement = production_report(
            [parse_lcov(stock_reports[target].read_text(encoding="utf-8"))],
            measured_sources,
            PROJECT_ROOT,
            mappings=mappings,
        )
        if strict != stock_measurement:
            raise ValueError(f"stock per-test LCOV disagrees with strict profile measurements: {target.label}")
        strict_path = directory / "aobus-coverage/coverage.lcov"
        attempt.receipt["strict_test_reports"].append(
            {
                "label": target.label,
                "configuration": target.configuration,
                "path": str(strict_path),
                "sha256": _sha(strict_path),
                "source_mapping_contract": collection["source_mapping_contract"],
                "standard_source_mappings_sha256": collection["standard_source_mappings"]["sha256"],
                "stock_agreement": True,
            }
        )
        reports.append(data)
    if _sha(actions) != actions_sha256 or _source_identity(PROJECT_ROOT) != identity:
        raise ValueError("configured source mapping evidence or authored sources changed during collection")
    merged = production_report(reports, measured_sources, PROJECT_ROOT, require_application=require_application)
    combined = _combined_artifact(events, attempt.directory / "coverage.stderr.log", attempt.tree)
    raw = attempt.directory / "combined.raw.lcov"
    shutil.copyfile(combined, raw)
    attempt.receipt["stock_combined_report"] = {"source": str(combined), "retained": str(raw), "sha256": _sha(raw)}
    # The combined report also merges every instrumented target's baseline
    # records. Those name request-time sources, possibly without a test profile,
    # and carry no lines; only measured lines take part in the comparison.
    combined_data = production_report(
        [parse_lcov(raw.read_text(encoding="utf-8"))],
        identity,
        PROJECT_ROOT,
        mappings=stock_mappings,
    )
    if merged != combined_data:
        raise ValueError("combined LCOV disagrees with checked per-test line measurements")
    selected = production_report([merged], measured_sources, PROJECT_ROOT, args.scope or [])
    report_path = attempt.directory / "coverage.lcov"
    report_path.write_text(_lcov(selected), encoding="utf-8")
    rows = [
        {
            "source": source,
            "lines": len(lines),
            "covered": sum(hits > 0 for hits in lines.values()),
            "sha256": measured_sources[source],
        }
        for source, lines in sorted(selected.items())
    ]
    total = sum(len(lines) for lines in selected.values())
    covered = sum(hits > 0 for lines in selected.values() for hits in lines.values())
    native.write_json(
        attempt.directory / "sources.json", {"metric": "line", "lines": total, "covered": covered, "sources": rows}
    )
    attempt.receipt.update(report=str(report_path), report_sha256=_sha(report_path), lines=total, covered=covered)
    print(f"Bazel production line coverage: {covered / total:.2%} ({covered}/{total}); report: {report_path}")


def run(args: argparse.Namespace) -> int:
    # Admission precedes tree selection, environment preparation and SDK work.
    validate_request(native.native_platform(), args.config)
    if args.vcpkg_prefix:
        raise native.BazelWorkspaceError("Linux Bazel coverage does not accept --vcpkg-prefix")
    if getattr(args, "filter", None):
        raise native.BazelWorkspaceError(
            "Bazel coverage filters require genuine Catch2 XML proof and are not yet supported"
        )
    if getattr(args, "analysis_only", False) and args.scope:
        raise native.BazelWorkspaceError("Bazel coverage --analysis-only does not accept report-only --scope")
    if args.coverage_suite not in _SUITES:
        raise native.BazelWorkspaceError(f"unsupported coverage suite: {args.coverage_suite}")
    if args.jobs < 1:
        raise native.BazelWorkspaceError("coverage jobs must be positive")
    tree = bazel_workspace.workspace_path(args.path, "coverage-debug")
    with build_tree_lock(tree):
        attempt = _Attempt(tree, args)
        try:
            server_java_home = bazel_workspace._linux_server_javabase()
            java_home = _java_runtime(server_java_home)
            java = java_home / "bin/java"
            attempt.server_java_home = server_java_home
            attempt.java_home = java_home
            attempt.receipt["java_runtime"] = {
                "source_home": str(server_java_home),
                "home": str(java_home),
                "source_executable": str(server_java_home / "bin/java"),
                "executable": str(java),
                "realpath": str(java.resolve(strict=True)),
                "sha256": _sha(java),
                "identity_scope": "selected bin/java bytes, not the complete JDK",
                "selection": (
                    "declared local Java runtime selected by coverage-only repository JAVA_HOME; "
                    "Java compilation toolchain unchanged"
                ),
            }
            identity = _source_identity(PROJECT_ROOT)
            native.write_json(attempt.directory / "source-identity.json", identity)
            attempt.receipt["compiler_identity"] = _compiler_identity("linux")
            native.write_json(attempt.directory / "compiler-identity.json", attempt.receipt["compiler_identity"])
            base = bazel_workspace.prepare_workspace(tree, "debug")
            executable = bazel_workspace.bazel_executable()
            attempt.receipt.update(base_prepared=str(base), executable=str(executable))
            analysis_only = getattr(args, "analysis_only", False)
            with ExitStack() as stack:
                environment = {}
                if not analysis_only:
                    from ..command.test import virtual_gtk_displays
                    from .bazel_test_services import private_pipewire

                    displays = stack.enter_context(virtual_gtk_displays(1))
                    environment = stack.enter_context(private_pipewire(attempt.directory, displays[0]))
                frozen = environment | {
                    "AOBUS_COVERAGE_ATTEMPT": attempt.directory.name,
                    "AOBUS_COVERAGE_SOURCE_ROOT": str(PROJECT_ROOT),
                    "BAZEL_CC_COVERAGE_TOOL": "GCOV",
                }
                prepared = bazel_check._context_configuration(base, attempt.directory, frozen, fresh=True)
                # Select the admitted local runtime before repository evaluation;
                # its standard JavaRuntimeInfo.files become merger action inputs.
                # No global Java compilation/runtime toolchain is overridden.
                with (prepared / "aobus.bazelrc").open("a", encoding="utf-8") as rc:
                    rc.write("common " + json.dumps(f"--repo_env=JAVA_HOME={java_home}") + "\n")
                    rc.write("build --//bazel/coverage:runtime_enabled=true\n")
                attempt.receipt["prepared"] = str(prepared)
                attempt.save()
                if analysis_only:
                    _analyze(args, attempt, prepared, executable, identity)
                else:
                    _measure(args, attempt, prepared, executable, identity)
            attempt.receipt.update(status="analysis-passed" if analysis_only else "passed", exit=0)
            if analysis_only:
                print(f"Bazel coverage analysis passed (no execution or report); receipt: {attempt.path}")
            return 0
        except (SystemExit, KeyboardInterrupt) as error:
            status = bazel_check._controlled_exit_status(error)
            attempt.receipt.update(status="interrupted", exit=status, error=str(error) or type(error).__name__)
            return status
        except (
            native.BazelWorkspaceError,
            OSError,
            ValueError,
            KeyError,
            TypeError,
            subprocess.SubprocessError,
            zipfile.BadZipFile,
        ) as error:
            attempt.receipt.update(status="failed", exit=1, error=str(error))
            print(f"Bazel coverage: {error}; receipt: {attempt.path}", file=sys.stderr)
            return 1
        finally:
            if attempt.receipt["status"] == "running":
                attempt.receipt.update(status="incomplete", exit=1)
            attempt.receipt["finished_at"] = datetime.now(UTC).isoformat()
            attempt.save()
