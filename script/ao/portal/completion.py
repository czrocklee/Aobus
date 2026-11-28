"""Run Bazel validation and verify that the requested checks passed."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, NamedTuple

from ao.core import bazel_native as native
from ao.core.build_lock import build_tree_lock
from ao.portal import (
    bazel_attempt,
    bazel_ci_summary,
    bazel_lint,
    bazel_test_ownership,
    bazel_workflows,
    bazel_workspace,
    lint_scope,
)
from ao.portal.source_scope import SourceScope


@dataclass(frozen=True, kw_only=True)
class Request(bazel_workflows.Request):
    extend: bool = False
    lint_base: str | None = None
    prune_lint_inputs: bool = True
    native_only: bool = False
    desktop_tests: bool = False
    fresh: bool = False
    clean: bool = False


def validate(request: Request) -> str:
    """Reject unsupported requests before selecting or creating a build tree."""
    config, host = bazel_workflows.validate_request(request)
    if config == "profile":
        raise native.BazelWorkspaceError("sampling is not a validation profile; use ao measure performance")
    if config not in ("fastbuild", "debug", "release"):
        raise native.BazelWorkspaceError(f"{config} is not a completion profile; select a native Bazel test instead")
    if request.extend and request.lint_base is not None:
        raise native.BazelWorkspaceError("--extend and --lint-base select different lint scopes")
    if request.desktop_tests and host != "windows":
        raise native.BazelWorkspaceError("--desktop-tests requires Windows")
    return config


_STANDALONE_SUITE = "//test/script:standalone_repository_checks"
_COMMON_CHECKS = {"//test/script:" + name for name in ("docs", "format_docs", "ruff", "name_audit", "test_audit")}
_WORKFLOWS = "//test/script:workflows"
_NATIVE_POLICY = "//:native_policy"
_PRODUCTION_LINT = "//test/lint:production_lint"


class _CheckSelection(NamedTuple):
    suites: set[str]
    required: set[str]
    optional: set[str]
    targets: tuple[str, ...] = ("//...",)
    options: tuple[str, ...] = ()
    attempt_prefix: str = "check-"


def validate_repository() -> None:
    if native.native_platform() != "linux":
        raise native.BazelWorkspaceError("check repository requires Linux")


def _repository_selection() -> _CheckSelection:
    return _CheckSelection(
        suites={_STANDALONE_SUITE},
        required=_COMMON_CHECKS | {_WORKFLOWS},
        optional=set(),
        targets=(_STANDALONE_SUITE,),
        options=("--keep_going",),
        attempt_prefix="repository-",
    )


def _selection(platform: str, request: Request, *, scoped_lint: bool = False) -> _CheckSelection:
    # Require published checks even if one is accidentally dropped from its suite.
    suites = {
        "//:check_tests",
        "//test/script:native_repository_checks" if request.native_only else "//test/script:repository_checks",
    }
    required = {
        _NATIVE_POLICY,
        "//test/lint:lint_test",
        "//test:backend_test",
        "//test:media_test",
        "//test:tui_help_smoke",
    }
    if not scoped_lint:
        required.add(_PRODUCTION_LINT)
    required.update("//test/script:" + name for name in ("format_sources", "guardrails", "bazel_lint", "mypy"))
    optional: set[str] = set()
    if not request.native_only:
        required.update(_COMMON_CHECKS)
        (required if platform == "linux" else optional).add(_WORKFLOWS)
    windows_probes = {"//test:winui_localization_probe_test", "//test:winui_track_item_probe_test"}
    # Require these Windows-only analysis tests independently of suite expansion.
    windows_analysis = {
        "//test/winui/projection:winui_projection_test",
        "//bazel/windows_header_share:mode_interfaces_test",
    }
    if platform == "windows":
        required.update(windows_probes | windows_analysis)
        if request.desktop_tests:
            required.update({"//test:winui_uia_test", "//test:winui_uia_selection_test"})
    else:
        optional.update(windows_probes | windows_analysis)
    # The ordinary suite expands these Linux-only tests on every host.
    linux_only = {
        "//bazel/header_share:sparse_headers_test",
        "//test:dictionary_publication_test",
        "//test:library_import_apply_cancellation_test",
        "//test:scan_directory_spelling_fallback_test",
    }
    if platform == "linux":
        required.update(linux_only)
        suites.add("//:instrumentation_tests")
        required.update(("//:native_driver_policy", "//bazel/header_share:selector_test"))
        required.update(
            "//:instrumentation_tests_" + case
            for case in ("coverage", "asan_build", "asan_compile", "tsan_build", "tsan_compile")
        )
    else:
        optional.update(linux_only)
    appkit_tests = {"//test:appkit_cli_test", "//test:appkit_editor_test"}
    if platform == "macos":
        required.update(
            appkit_tests
            | {
                "//test/script:test_appkit_scenarios",
                "//test/script:test_bazel_appkit_deps",
                "//test/script:test_bazel_environment",
                "//test/script:test_bazel_remote_cache",
            }
        )
    else:
        optional.update(appkit_tests)
    for feature, label in (("gtk", "//test:gtk_test"), ("system_media", "//test:systemmedia_test")):
        if platform == "linux" and bazel_workflows.feature_enabled(getattr(request, feature), feature):
            required.add(label)
        else:
            optional.add(label)

    exclusions = ["-" + _STANDALONE_SUITE] if request.native_only else []
    if scoped_lint:
        # The wildcard also builds non-tests: the public alias and result/log providers reach the gate.
        exclusions.append("-//:production_lint")
        exclusions.extend("-" + _PRODUCTION_LINT + suffix for suffix in ("", ".gate", ".gate_result", ".gate_log"))
    options: tuple[str, ...] = ("--keep_going",) if request.native_only else ()
    if request.native_only and platform == "windows" and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted":
        # Short tests have timed out at 60 s during cold Windows builds. Keep the other three class limits.
        options += ("--test_timeout=300,300,900,3600",)
    return _CheckSelection(
        suites,
        required,
        optional,
        targets=("--", "//...", *exclusions) if exclusions else ("//...",),
        options=options,
    )


def _main_label(label: str) -> str:
    return label.lstrip("@") if label.startswith(("@@//", "@//")) else label


def _registered_test_labels(results: list[dict[str, Any]], tree: Path) -> tuple[str, set[str]]:
    """Read the ownership audit's output from this invocation, under its tree lock."""
    if len(results) != 1:
        raise ValueError(f"expected one {_NATIVE_POLICY} testResult, got {len(results)}")
    event = results[0]
    result = event.get("testResult")
    if not isinstance(result, dict) or result.get("status") != "PASSED":
        raise ValueError(f"expected a passing {_NATIVE_POLICY} testResult")
    # The caller selected this event by its id.testResult.label.
    configuration = event["id"]["testResult"].get("configuration")
    config_id = configuration.get("id") if isinstance(configuration, dict) else None
    if not isinstance(config_id, str) or not config_id:
        raise ValueError(f"missing configuration id on {_NATIVE_POLICY} testResult")
    outputs = result.get("testActionOutput")
    if not isinstance(outputs, list) or not all(isinstance(out, dict) and out.get("name") for out in outputs):
        raise ValueError(f"expected named testActionOutput entries from {_NATIVE_POLICY}")
    name = "test.outputs/" + bazel_test_ownership.REGISTERED_TEST_LABELS_FILE
    files = [out for out in outputs if out["name"] == name]
    if len(files) != 1:
        raise ValueError(f"expected one {name} output from {_NATIVE_POLICY}")
    manifest = bazel_attempt.test_output_file(files[0], root=tree)
    labels = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(labels, list) or not labels:
        raise ValueError(f"expected a nonempty list of public execution labels in {manifest}")
    return config_id, {bazel_test_ownership._label(label) for label in labels}


def _verify_required_checks(path: Path, selection: _CheckSelection, *, tree: Path, fresh: bool) -> None:
    required = selection.required.copy()
    summaries: dict[str, list[dict[str, Any]]] = {}
    policy_results: list[dict[str, Any]] = []
    skipped: dict[str, set[str]] = {}
    expanded: set[str] = set()
    finished = last = False
    try:
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                event = json.loads(line)
                if last or not isinstance(event, dict) or not isinstance(event.get("id"), dict):
                    raise ValueError("invalid event or data after lastMessage")
                identity = event["id"]
                last = event.get("lastMessage") is True
                if "buildFinished" in identity:
                    if finished or event.get("finished", {}).get("overallSuccess") is not True:
                        raise ValueError("missing or conflicting successful build finish")
                    finished = True
                for suite in event.get("expanded", {}).get("testSuiteExpansions", []):
                    name = _main_label(suite["suiteLabel"])
                    if name in selection.suites:
                        if not suite.get("testLabels") or name in expanded:
                            raise ValueError(f"empty or ambiguous test suite expansion: {name}")
                        expanded.add(name)
                        required.update(map(_main_label, suite["testLabels"]))
                if "testResult" in identity and _main_label(identity["testResult"]["label"]) == _NATIVE_POLICY:
                    policy_results.append(event)
                if "testSummary" in identity:
                    label = _main_label(identity["testSummary"]["label"])
                    summaries.setdefault(label, []).append(event)
                completed = identity.get("targetCompleted")
                aborted = event.get("aborted")
                # Aspect completions are not the test label's own result.
                if (
                    isinstance(completed, dict)
                    and not completed.get("aspect")
                    and isinstance(aborted, dict)
                    and aborted.get("reason") == "SKIPPED"
                ):
                    label = _main_label(completed["label"])
                    detail = f"{label} [{completed['configuration']['id']}]"
                    if isinstance(aborted.get("description"), str) and aborted["description"]:
                        detail += f": {aborted['description']}"
                    skipped.setdefault(label, set()).add(detail)
        if not (finished and last):
            raise ValueError("incomplete event stream (missing build finish or lastMessage)")
        if missing_suites := selection.suites - expanded:
            raise ValueError("missing required test suites: " + ", ".join(sorted(missing_suites)))
        required -= selection.optional
        registered: set[str] = set()
        policy_config = None
        if _NATIVE_POLICY in required and _NATIVE_POLICY in summaries:
            policy_config, registered = _registered_test_labels(policy_results, tree)
            # These owners are compatible in the actual graph, even if an optional
            # suite member or a separately maintained requirement says otherwise.
            required.update(registered)
        unsummarized = required - summaries.keys()
        skipped_labels = unsummarized & skipped.keys()
        missing = unsummarized - skipped_labels
        if unsummarized:
            clauses = []
            if missing:
                clauses.append("missing required passing checks: " + ", ".join(sorted(missing)))
            if skipped_labels:
                details = [detail for label in sorted(skipped_labels) for detail in sorted(skipped[label])]
                clauses.append("skipped required checks: " + "; ".join(details))
            raise ValueError("; ".join(clauses))
        for label in sorted(required):
            events = summaries[label]
            if len(events) != 1 or not events[0]["id"]["testSummary"].get("configuration", {}).get("id"):
                raise ValueError(f"ambiguous configured test summary: {label}")
            if label == _NATIVE_POLICY or label in registered:
                if events[0]["id"]["testSummary"]["configuration"]["id"] != policy_config:
                    raise ValueError(f"required check did not run in the audited configuration: {label}")
            summary = events[0]["testSummary"]
            if summary.get("overallStatus") != "PASSED":
                raise ValueError(f"required check did not pass: {label}")
            runs, cached = summary.get("totalRunCount"), summary.get("totalNumCached", 0)
            if type(runs) is not int or type(cached) is not int or runs < 1 or not 0 <= cached <= runs:
                raise ValueError(f"invalid required check run counts: {label}")
            if fresh and cached:
                raise ValueError(f"required check used a cached result with --fresh: {label}")
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise native.BazelWorkspaceError(f"completion evidence rejected: {path}: {error}") from error


def _report_wasapi(bep: Path, tree: Path) -> None:
    note = bazel_ci_summary.wasapi_case_note(bep, tree)
    print(note, flush=True)
    try:
        (bep.parent / "wasapi.txt").write_text(note + "\n", encoding="utf-8")
    except OSError as error:
        print(f"Cannot save WASAPI note for the CI summary: {error}", file=sys.stderr)


def _test_graph(request: Request, tree: Path, config: str, selection: _CheckSelection) -> int:
    executable = bazel_workspace.bazel_executable()
    prepared = bazel_workspace.prepare_workspace(
        tree, config, prefix=request.vcpkg_prefix, graph_options=bazel_workflows.feature_options(request)
    )
    if request.clean and (
        status := bazel_workspace.execute(
            executable, tree, prepared, "clean", config, [], isolate_rc=True, log=tree / "clean.log"
        )
    ):
        return status
    directory = tree / "checks" / config
    directory.mkdir(parents=True, exist_ok=True)
    attempt = Path(tempfile.mkdtemp(prefix=selection.attempt_prefix, dir=directory))
    options = [
        "--lockfile_mode=error",
        f"--build_event_json_file={attempt / 'tests.bep.json'}",
        "--experimental_build_event_upload_strategy=local",
        f"--profile={attempt / 'profile.json.gz'}",
        *selection.options,
    ]
    if _NATIVE_POLICY in selection.required:
        options.append("--nozip_undeclared_test_outputs")
    if request.fresh:
        options.append("--nocache_test_results")
    print(f"Bazel test graph ({config}); evidence: {attempt}", flush=True)
    log = attempt / "test.log"
    try:
        status = bazel_workspace.execute(
            executable,
            tree,
            prepared,
            "test",
            config,
            list(selection.targets),
            jobs=request.jobs,
            remote_cache=request.remote_cache,
            isolate_rc=True,
            options=options,
            log=log,
        )
        bep = attempt / "tests.bep.json"
        if native.native_platform() == "windows":
            _report_wasapi(bep, tree)
        if status == 0:
            _verify_required_checks(bep, selection, tree=tree, fresh=request.fresh)
        return status
    finally:
        if log.is_file():
            shutil.copyfile(log, tree / "build.log")


def run_repository(*, path: str | None, jobs: int) -> int:
    validate_repository()
    request = Request(path=path, jobs=jobs)
    return _run(request, validate(request), _repository_selection())


def run(request: Request) -> int:
    config = validate(request)
    plan = lint_scope.Plan(None, "--extend") if request.extend else lint_scope.plan(request.lint_base)
    files = plan.files
    scope = "full graph" if files is None else f"{len(files)} scoped file(s)"
    print(f"Native lint: {scope} ({plan.reason})", flush=True)
    selection = _selection(native.native_platform(), request, scoped_lint=files is not None)
    return _run(request, config, selection, scoped_files=files or ())


def _run(request: Request, config: str, selection: _CheckSelection, *, scoped_files: tuple[str, ...] = ()) -> int:
    tree = bazel_workspace.workspace_path(request.path, config)
    with build_tree_lock(tree):
        native.require_supported_source()
        tree.mkdir(parents=True, exist_ok=True)
        status = _test_graph(request, tree, config, selection)
        if status or not scoped_files:
            return status
        # Keep both phases on this locked tree; another invocation may request --clean.
        return bazel_lint.run(replace(request, path=str(tree)), SourceScope(scoped_files), lockfile_readonly=True)
