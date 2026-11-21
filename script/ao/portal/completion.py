"""Run Bazel validation and verify that the requested checks passed."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, NamedTuple

from ao.core import bazel_native as native
from ao.core.build_lock import build_tree_lock
from ao.portal import bazel_ci_summary, bazel_sanitizers, bazel_workflows, bazel_workspace


@dataclass(frozen=True, kw_only=True)
class Request(bazel_workflows.Request):
    ci: bool = False
    fresh: bool = False
    clean: bool = False


def validate(request: Request) -> str:
    """Reject unsupported requests before selecting or creating a build tree."""
    config, _host = bazel_workflows.validate_request(request)
    if config == "profile":
        raise native.BazelWorkspaceError("sampling is not a validation profile; use ao measure performance")
    if request.ci and config != "fastbuild":
        raise native.BazelWorkspaceError("daily CI uses fastbuild; select other profiles explicitly outside CI")
    return config


_CI_REPOSITORY_SUITE = "//test/script:ci_repository_checks"
_COMMON_CHECKS = {"//test/script:" + name for name in ("docs", "format_docs", "ruff", "name_audit", "test_audit")}
_WORKFLOWS = "//test/script:workflows"


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
        suites={_CI_REPOSITORY_SUITE},
        required=_COMMON_CHECKS | {_WORKFLOWS},
        optional=set(),
        targets=(_CI_REPOSITORY_SUITE,),
        options=("--keep_going",),
        attempt_prefix="repository-",
    )


def _selection(platform: str, request: Request) -> _CheckSelection:
    # Require published checks even if one is accidentally dropped from its suite.
    suites = {
        "//:check_tests",
        "//test/script:native_repository_checks" if request.ci else "//test/script:repository_checks",
    }
    required = {
        "//:native_policy",
        "//test/lint:production_lint",
        "//test/lint:lint_test",
        "//test:backend_test",
        "//test:media_test",
        "//test:tui_help_smoke",
    }
    required.update("//test/script:" + name for name in ("format_sources", "guardrails", "bazel_lint", "mypy"))
    optional: set[str] = set()
    if not request.ci:
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
        if request.ci:
            required.add("//test:winui_uia_test")
    else:
        optional.update(windows_probes | windows_analysis)
    # The ordinary suite expands these Linux-only tests on every host.
    linux_only = {"//test:dictionary_publication_test", "//bazel/header_share:sparse_headers_test"}
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
    if platform == "macos":
        required.update(
            (
                "//test/script:test_bazel_appkit_deps",
                "//test/script:test_bazel_environment",
                "//test/script:test_bazel_remote_cache",
            )
        )
    for feature, label in (("gtk", "//test:gtk_test"), ("system_media", "//test:systemmedia_test")):
        if platform == "linux" and bazel_workflows.feature_enabled(getattr(request, feature), feature):
            required.add(label)
        else:
            optional.add(label)

    if request.ci:
        return _CheckSelection(
            suites, required, optional, targets=("--", "//...", "-" + _CI_REPOSITORY_SUITE), options=("--config=ci",)
        )
    return _CheckSelection(suites, required, optional)


def _main_label(label: str) -> str:
    return label.lstrip("@") if label.startswith(("@@//", "@//")) else label


def _verify_required_checks(path: Path, selection: _CheckSelection, *, fresh: bool) -> None:
    required = selection.required.copy()
    summaries: dict[str, list[dict[str, Any]]] = {}
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
            _verify_required_checks(bep, selection, fresh=request.fresh)
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
    return _run(request, config, _selection(native.native_platform(), request))


def _run(request: Request, config: str, selection: _CheckSelection) -> int:
    normal_config = (
        "debug"
        if request.config == "debug"
        else "fastbuild"
        if config in bazel_sanitizers.SANITIZER_CONFIGS
        else config
    )
    tree = bazel_workspace.workspace_path(request.path, normal_config)
    with build_tree_lock(tree):
        native.require_supported_source()
        tree.mkdir(parents=True, exist_ok=True)
        if status := _test_graph(request, tree, normal_config, selection):
            return status
        if config in bazel_sanitizers.SANITIZER_CONFIGS:
            instrumented = replace(request, path=str(tree), config=config, asan=False, tsan=False)
            return bazel_sanitizers.run(instrumented)
        return 0
