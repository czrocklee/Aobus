"""Prepare and execute the complete native Bazel validation graph.

The command adapter and the platform launcher share this request admission.
This service does not parse CLI arguments or dispatch other public commands.
"""

from __future__ import annotations

import json
import shutil
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, NamedTuple

from ao.core import bazel_native as native
from ao.core.build_lock import build_tree_lock
from ao.portal import bazel_sanitizers, bazel_workflows, bazel_workspace


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


class _CheckSelection(NamedTuple):
    suites: set[str]
    required: set[str]
    optional: set[str]


def _selection(platform: str, request: Request) -> _CheckSelection:
    # Read suite membership from Bazel's own expansion of //... in this invocation.
    # Keep independent promises for the native harnesses: removing a manual test
    # from its discovery suite must not silently remove it from the evidence gate.
    suites = {"//:check_tests", "//test/script:repository_checks"}
    required = {
        "//:native_policy",
        "//test/lint:production_lint",
        "//test/lint:lint_test",
        "//test:backend_test",
        "//test:media_test",
        "//test:tui_help_smoke",
    }
    # Expansion admits new members automatically. Keep the published minimum
    # independent so deleting a check from the BUILD file cannot lower the bar.
    required.update(
        "//test/script:" + name
        for name in ("docs", "format", "guardrails", "bazel_lint", "mypy", "ruff", "name_audit", "test_audit")
    )
    optional = set()
    windows_probes = {"//test:winui_localization_probe_test", "//test:winui_track_item_probe_test"}
    # Independent of suite expansion: a Windows-only analysis test must not
    # disappear from the evidence gate if discovery stops listing it.
    winui_projection = "//test/winui/projection:winui_projection_test"
    if platform == "windows":
        required.update(windows_probes)
        required.add(winui_projection)
        if request.ci:
            required.add("//test:winui_uia_test")
    else:
        optional.update(windows_probes)
        optional.add(winui_projection)
    if platform == "linux":
        suites.add("//:instrumentation_tests")
        required.update(("//:native_driver_policy", "//bazel/header_share:selector_test", "//test/script:workflows"))
        required.update(
            "//:instrumentation_tests_" + case
            for case in ("coverage", "asan_build", "asan_compile", "tsan_build", "tsan_compile")
        )
    else:
        optional.add("//test/script:workflows")
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

    return _CheckSelection(suites, required, optional)


def _main_label(label: str) -> str:
    return label.lstrip("@") if label.startswith(("@@//", "@//")) else label


def _verify_required_checks(path: Path, selection: _CheckSelection, *, fresh: bool) -> None:
    required = selection.required.copy()
    summaries: dict[str, list[dict[str, Any]]] = {}
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
        if not (finished and last):
            raise ValueError("incomplete event stream (missing build finish or lastMessage)")
        if missing := selection.suites - expanded:
            raise ValueError("missing required test suites: " + ", ".join(sorted(missing)))
        required -= selection.optional
        if missing := required - summaries.keys():
            raise ValueError("missing required passing checks: " + ", ".join(sorted(missing)))
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


def _test_graph(request: Request, tree: Path, config: str) -> int:
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
    attempt = Path(tempfile.mkdtemp(prefix="check-", dir=directory))
    options = [
        "--lockfile_mode=error",
        f"--build_event_json_file={attempt / 'tests.bep.json'}",
        "--experimental_build_event_upload_strategy=local",
        f"--profile={attempt / 'profile.json.gz'}",
    ]
    if request.ci:
        options.append("--config=ci")
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
            ["//..."],
            jobs=request.jobs,
            remote_cache=request.remote_cache,
            isolate_rc=True,
            options=options,
            log=log,
        )
        if status == 0:
            selection = _selection(native.native_platform(), request)
            _verify_required_checks(attempt / "tests.bep.json", selection, fresh=request.fresh)
        return status
    finally:
        if log.is_file():
            shutil.copyfile(log, tree / "build.log")


def run(request: Request) -> int:
    config = validate(request)
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
        if status := _test_graph(request, tree, normal_config):
            return status
        if config in bazel_sanitizers.SANITIZER_CONFIGS:
            instrumented = replace(request, path=str(tree), config=config, asan=False, tsan=False)
            return bazel_sanitizers.run(instrumented)
        return 0
