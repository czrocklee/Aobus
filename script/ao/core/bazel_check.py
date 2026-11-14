"""Native completion: audits, test ownership, compile contracts, build and unattended tests.

The BUILD graph owns products, unattended tests and explicitly manual harnesses.
This module orchestrates those roots; hygiene and documentation remain separate.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections.abc import Callable, Sequence
from contextlib import AbstractContextManager, nullcontext, redirect_stderr, redirect_stdout
from dataclasses import asdict
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from . import bazel_contract, bazel_test_ownership, bazel_workspace, guardrails
from . import bazel_native as native
from .bazel_lint import parse_query_xml
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT

_TEST_ROOT = "//:check_tests"
_NATIVE_TEST_ROOT = "//:check_native_tests"
_PRODUCT_ROOT = "//:check_products"
_RUN_ROOT = "//:check_runs"
_COMPATIBILITY = 'str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))'


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


def _configured_roots(document: Any, labels: Sequence[str]) -> frozenset[bazel_test_ownership.ConfiguredTarget]:
    """Require each named seed before a dependency walk can hide an omission."""
    expected = frozenset(labels)
    if not expected or len(expected) != len(labels):
        raise ValueError("configured roots require nonempty unique labels")
    roots = bazel_test_ownership.extract_configured_targets(document)
    observed = frozenset(target.label for target in roots)
    if observed != expected or len(roots) != len(expected):
        raise ValueError(
            "configured root labels/configurations disagree: "
            f"missing={sorted(expected - observed)}, unexpected={sorted(observed - expected)}, "
            f"identities={sorted(roots)}"
        )
    return roots


def _configured_set(labels: Sequence[str]) -> str:
    return "config(set(" + " ".join(json.dumps(label) for label in labels) + "), target)"


def _first_party_labels(text: str) -> frozenset[str]:
    """Keep project labels from a cquery label listing that also names rule wrappers."""
    return frozenset(
        line.removeprefix("@@").removeprefix("@")
        for line in text.splitlines()
        if line.startswith(("//", "@//", "@@//"))
    )


def _incompatible_labels(text: str, expected: Sequence[str]) -> list[str]:
    """Read label|True/False rows; every expected owner must be reported consistently."""
    statuses: dict[str, bool] = {}
    for row in text.splitlines():
        label, separator, status = row.rpartition("|")
        label = label.removeprefix("@@").removeprefix("@")
        if (
            not separator
            or status not in {"True", "False"}
            or statuses.setdefault(label, status == "True") != (status == "True")
        ):
            raise ValueError(f"malformed or conflicting configured compatibility row: {row!r}")
    if set(statuses) != set(expected):
        raise ValueError("configured compatibility rows disagree with the declared test owners")
    return sorted(label for label, incompatible in statuses.items() if incompatible)


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
            "scope": {
                "tests": _TEST_ROOT,
                "native_tests": _NATIVE_TEST_ROOT,
                "products": _PRODUCT_ROOT,
                "native_runs": _RUN_ROOT,
                "dependency_admission": "governed repository admission during configured analysis and build",
                "compile_contracts": "unchanged default-test and Linux SPA analysis scope",
                "excluded": [
                    "hygiene",
                    "documentation",
                    "interactive GUI runs",
                    "performance runs",
                    "sanitizers",
                    "coverage",
                    "distribution",
                ],
            },
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
            return subprocess.run(
                argv,
                cwd=PROJECT_ROOT,
                env=native.child_environment(isolate_rc=True),
                stdout=out,
                stderr=err,
                check=False,
            ).returncode

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


def _complete(args: argparse.Namespace, tree: Path, attempt: _Check, prepared: Path, executable: Path) -> None:
    declared_path = attempt.bazel(
        "declared-graph", executable, prepared, "query", ['kind("rule", //...)', "--output=xml"]
    )
    native_selection = attempt.bazel(
        "native-test-selection",
        executable,
        prepared,
        "cquery",
        [
            f"labels(srcs, config({_NATIVE_TEST_ROOT}, target))",
            "--output=starlark",
            "--starlark:expr=str(target.label)",
        ],
    )
    native_tests = _labels(native_selection.read_text(encoding="utf-8"), allow_empty=True)
    attempt.receipt["native_tests"] = native_tests
    selection_path = attempt.bazel(
        "test-selection", executable, prepared, "query", [f"tests({_TEST_ROOT})", "--output=label"]
    )
    selected_tests = _labels("\n".join([*_labels(selection_path.read_text(encoding="utf-8")), *native_tests]))
    declared_graph = parse_query_xml(declared_path)
    # Bare patterns can include configurations retained from previous commands;
    # config(..., target) pins each root to its own current configuration.
    root_labels = [_PRODUCT_ROOT, _RUN_ROOT, *selected_tests]
    root_expression = _configured_set(root_labels)
    roots_path = attempt.bazel(
        "completion-roots", executable, prepared, "cquery", [root_expression, "--output=jsonproto"]
    )
    configured_roots = _configured_roots(json.loads(roots_path.read_text(encoding="utf-8")), root_labels)
    compatibility_path = attempt.bazel(
        "compatibility",
        executable,
        prepared,
        "cquery",
        [root_expression, "--output=starlark", f"--starlark:expr={_COMPATIBILITY}"],
    )
    roots = configured_roots - bazel_test_ownership.extract_compatibility(
        compatibility_path.read_text(encoding="utf-8"), configured_roots
    )
    selected_test_targets = frozenset(target for target in roots if target.label in selected_tests)
    registrations = bazel_test_ownership.discover_registration_sources(PROJECT_ROOT)
    owners = bazel_test_ownership.registration_owners(declared_graph, registrations)
    reached_path = attempt.bazel(
        "reached-executables",
        executable,
        prepared,
        "cquery",
        [
            f'kind("cc_test|cc_binary", deps({root_expression}))',
            "--output=starlark",
            "--starlark:expr=str(target.label)",
        ],
    )
    reached = _first_party_labels(reached_path.read_text(encoding="utf-8"))
    incompatible_owners: list[str] = []
    if owners:
        owner_path = attempt.bazel(
            "owner-compatibility",
            executable,
            prepared,
            "cquery",
            [_configured_set(owners), "--output=starlark", f"--starlark:expr={_COMPATIBILITY}"],
        )
        incompatible_owners = _incompatible_labels(owner_path.read_text(encoding="utf-8"), owners)

    def ownership() -> int:
        errors = bazel_test_ownership.ownership_errors(
            declared_graph,
            registrations,
            reached=reached,
            executed=[target.label for target in selected_test_targets],
            incompatible=incompatible_owners,
        )
        path = attempt.directory / "test-ownership.json"
        native.write_json(
            path,
            {
                "schema": 3,
                "registration_files": len(registrations),
                "declarations": sum(len(cases) for cases in registrations.values()),
                "owners": owners,
                "incompatible_owners": incompatible_owners,
                "selected_tests": [asdict(target) for target in sorted(selected_test_targets)],
                "errors": errors,
            },
        )
        attempt.receipt["ownership"] = str(path)
        for error in errors:
            print(error)
        print(
            f"Test registration ownership: {len(registrations)} files, "
            f"{len(incompatible_owners)} owners incompatible with this host."
        )
        return 1 if errors else 0

    attempt.python("test-ownership", ownership)
    attempt.python(
        "compile-contracts",
        lambda: bazel_contract.verify(executable, tree, prepared, args.config, jobs=args.jobs, isolate_rc=True),
    )
    attempt.bazel(
        "build",
        executable,
        prepared,
        "build",
        [
            _PRODUCT_ROOT,
            _TEST_ROOT,
            *native_tests,
            f"--build_event_json_file={attempt.directory / 'build.bep.json'}",
        ],
        remote_cache=args.remote_cache,
    )
    attempt.bazel(
        "tests",
        executable,
        prepared,
        "test",
        [
            _TEST_ROOT,
            *native_tests,
            f"--build_event_json_file={attempt.directory / 'tests.bep.json'}",
        ],
        remote_cache=args.remote_cache,
    )

    def test_results() -> int:
        results = [
            result | {"executed": result["runs"] - result["cached"]}
            for result in _test_results(attempt.directory / "tests.bep.json", selected_test_targets, fresh=args.fresh)
        ]
        attempt.receipt["test_results"] = results
        attempt.receipt["test_summary"] = {
            "passed": len(results),
            "executed_runs": sum(result["executed"] for result in results),
            "cached_runs": sum(result["cached"] for result in results),
        }
        return 0

    attempt.python("test-results", test_results)
    runs_path = attempt.bazel(
        "native-run-selection",
        executable,
        prepared,
        "cquery",
        [
            f"labels(srcs, config({_RUN_ROOT}, target))",
            "--output=starlark",
            "--starlark:expr=str(target.label)",
        ],
    )
    run_targets = _labels(runs_path.read_text(encoding="utf-8"), allow_empty=True)
    # bazel run rejects a non-executable target itself; the reviewed
    # //:check_runs list owns which executables are safe to run unattended.
    for index, target in enumerate(run_targets):
        attempt.bazel(f"native-run-{index}", executable, prepared, "run", [target], remote_cache=args.remote_cache)
    attempt.receipt["native_runs"] = run_targets


def run(args: argparse.Namespace) -> int:
    tree = bazel_workspace.workspace_path(args.path, args.config)
    platform = native.native_platform()
    with build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        attempt = _Check(tree, args.config, platform, args.jobs)
        attempt.receipt["test_cache_policy"] = "fresh" if args.fresh else "auto"
        try:
            native.require_supported_source()

            def audit() -> int:
                result = guardrails.run(PROJECT_ROOT, attempt.directory, platform)
                attempt.receipt["guardrails"] = str(result.report_path)
                for finding in (*result.errors, *result.warnings):
                    print(finding)
                return result.exit_code

            attempt.python("guardrails", audit)
            from .bazel_workflows import feature_options

            base = bazel_workspace.prepare_workspace(
                tree, args.config, prefix=args.vcpkg_prefix, graph_options=feature_options(args)
            )
            executable = bazel_workspace.bazel_executable()
            attempt.receipt.update(base_prepared=str(base), executable=str(executable))
            attempt.save()

            def complete() -> int:
                services: AbstractContextManager[list[dict[str, str]]]
                if platform == "linux":
                    from ..command.test import virtual_gtk_displays
                    from .bazel_test_services import stable_service_root

                    services = virtual_gtk_displays(1, ipc_root=stable_service_root(tree, "gtk-bus"))
                else:
                    services = nullcontext([dict[str, str]()])
                with services as environments:
                    audio: AbstractContextManager[dict[str, str]]
                    if platform == "linux":
                        from .bazel_test_services import private_pipewire

                        audio = private_pipewire(
                            attempt.directory, environments[0], runtime_root=stable_service_root(tree, "pipewire")
                        )
                    else:
                        audio = nullcontext(environments[0])
                    with audio as environment:
                        prepared = _context_configuration(base, attempt.directory, environment, fresh=args.fresh)
                        attempt.receipt["prepared"] = str(prepared)
                        attempt.save()
                        _complete(args, tree, attempt, prepared, executable)
                return 0

            attempt.python("native-context", complete)
            attempt.receipt.update(status="passed", exit=0)
            summary = attempt.receipt["test_summary"]
            print(
                f"Configured test summaries: {summary['passed']} passed; "
                f"{summary['executed_runs']} executed runs, {summary['cached_runs']} cached runs."
            )
            print(f"Bazel completion passed ({args.config}); receipt: {attempt.path}")
            return 0
        except _PhaseFailure as exc:
            attempt.receipt.update(status="failed", exit=exc.status, error=str(exc))
            print(f"Bazel completion failed: {exc}; receipt: {attempt.path}", file=sys.stderr)
            return exc.status
        except (SystemExit, KeyboardInterrupt) as exc:
            status = _controlled_exit_status(exc)
            attempt.receipt.update(
                status="interrupted" if isinstance(exc, KeyboardInterrupt) else "failed",
                exit=status,
                error=str(exc) or type(exc).__name__,
            )
            if isinstance(exc, KeyboardInterrupt):
                attempt.receipt["writer_state"] = "not established by an interrupted controller"
            print(f"Bazel completion stopped ({status}); receipt: {attempt.path}", file=sys.stderr)
            return status
        except (
            native.BazelWorkspaceError,
            OSError,
            subprocess.SubprocessError,
            ValueError,
            KeyError,
            TypeError,
            ET.ParseError,
            RecursionError,
        ) as exc:
            attempt.receipt.update(status="error", exit=1, error=str(exc))
            print(f"Bazel completion: {exc}; receipt: {attempt.path}", file=sys.stderr)
            return 1
        finally:
            if attempt.receipt["status"] == "running":
                attempt.receipt.update(status="error", exit=1, error="completion stopped before a terminal result")
            attempt.receipt["finished_at"] = _timestamp()
            attempt.save()
