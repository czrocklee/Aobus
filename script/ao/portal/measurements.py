"""Measurement and instrumentation admission over existing native engines."""

from dataclasses import dataclass, replace
from pathlib import Path

from ao.core import bazel_native as native
from ao.core.build_lock import build_tree_lock
from ao.portal import bazel_concepts, bazel_coverage, bazel_workspace, concept_report, dependency_checks


@dataclass(frozen=True, kw_only=True)
class CoverageRequest:
    path: str | None = None
    jobs: int = 2
    compiler: str = "gcc"
    scope: tuple[str, ...] = ()
    coverage_suite: str = "default"
    remote_cache: str | None = None
    config: str = "debug"
    vcpkg_prefix: str | None = None
    analysis_only: bool = False


def validate_coverage_request(request: CoverageRequest) -> None:
    """Admit selection before the caller prepares a native environment."""
    bazel_coverage.validate_options(request)


def measure_coverage(request: CoverageRequest) -> int:
    if request.analysis_only:
        raise native.BazelWorkspaceError("coverage measurement cannot request analysis-only")
    validate_coverage_request(request)
    return bazel_coverage.run(request)


def check_instrumentation(request: CoverageRequest) -> int:
    request = replace(request, analysis_only=True)
    validate_coverage_request(request)
    return bazel_coverage.run(request)


def measure_concepts(request: dependency_checks.Request, *, json_output: Path | None = None) -> int:
    dependency_checks.validate_request(request)
    tree = bazel_workspace.workspace_path(request.path, request.config)
    # Keep admission facts and fresh observations in the same locked tree.
    with build_tree_lock(tree):
        sdk_facts = dependency_checks.verified_report(request)
        report = bazel_concepts.write_bazel_report(request, sdk_facts=sdk_facts)
    print(concept_report.format_summary(report))
    dependency_checks.write_report(report, json_output)
    return 0
