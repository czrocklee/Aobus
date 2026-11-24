"""Scoped native/Python/Bazel diagnostics, source audits and workflow lint.

Formatting and global documentation semantics are separate operations. Named
C++ checker requests bypass every unrelated language and audit stage.
"""

from __future__ import annotations

import os
import subprocess
import sys
from collections.abc import Sequence
from contextlib import AbstractContextManager, nullcontext
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TextIO

from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.policy import tidy_config
from ao.policy.tidy_policy import classify_relative
from ao.portal import (
    bazel_lint,
    bazel_workflows,
    buildifier,
    git_files,
    name_audit,
    python_check,
    source_scope,
    test_audit,
    tidy_engine,
)
from ao.portal.ready_tool import ReadyTool
from ao.portal.source_scope import SourceScope


@dataclass(frozen=True)
class LintRequest:
    config: str = "fastbuild"
    path: str | None = None
    vcpkg_prefix: str | None = None
    jobs: int = field(default_factory=tidy_engine.default_jobs)
    remote_cache: str | None = None
    compiler: str | None = None
    gtk: str | None = None
    system_media: str | None = None
    tidy_check: str | None = None
    header_filter: str | None = None
    output: str | None = None
    debug: bool = False
    tidy_args: tuple[str, ...] = ()
    owner_targets: tuple[str, ...] = ()
    report_only: bool = False

    @property
    def check(self) -> str | None:
        return self.tidy_check

    @property
    def clang_tidy_arg(self) -> tuple[str, ...]:
        return self.tidy_args


@dataclass(frozen=True)
class _NativeLintRequest(LintRequest):
    """Frozen source and file provenance consumed by the native lint engine."""

    files: tuple[str, ...] = ()
    _resolved_sources: tuple[str, ...] | None = None
    all: bool = False
    folder: tuple[str, ...] = ()
    commit: str | None = None


def validate(request: LintRequest) -> None:
    """Admit every native option before discovery, tools, or a build tree."""
    tidy_config.validate_diagnostic_arguments(list(request.tidy_args))
    bazel_lint._common_lint_request(request)
    bazel_lint._owner_targets(request.owner_targets)
    bazel_workflows.validate_request(request)


def split_existing(files: Sequence[str]) -> tuple[list[str], list[str]]:
    cpp: list[str] = []
    python: list[str] = []
    seen: set[Path] = set()
    for name in files:
        path = absolute_path(PROJECT_ROOT / name)
        if not path.is_file() or path in seen:
            continue
        seen.add(path)
        relative = source_scope.spelling(path)
        if path.suffix in git_files.CPP_SUFFIXES:
            cpp.append(relative)
        elif path.suffix in git_files.PYTHON_SUFFIXES:
            python.append(relative)
    return cpp, python


def audit_tests(paths: Sequence[str] = (), *, report_only: bool = False, sink: TextIO | None = None) -> int:
    issues = test_audit.audit_paths(list(paths))
    for issue in issues:
        print(issue.format(), file=sys.stderr if sink is None else sink)
    print(f"test-audit: {len(issues)} issue(s)")
    return int(bool(issues) and not report_only)


def audit_names(paths: Sequence[str] = (), *, report_only: bool = False, sink: TextIO | None = None) -> int:
    issues = name_audit.audit_paths(list(paths))
    for issue in issues:
        print(issue.format(), file=sys.stderr if sink is None else sink)
    print(f"name-audit: {len(issues)} issue(s)")
    return int(bool(issues) and not report_only)


def actionlint_executable() -> str:
    """Select the Nix wrapper that supplies the workflow linter and its helpers."""
    tool = Path(os.environ.get("AO_NIX_ACTIONLINT", ""))
    try:
        available = (
            tool.is_absolute()
            and tool.is_relative_to("/nix/store")
            and tool.resolve(strict=True).is_relative_to("/nix/store")
            and tool.is_file()
            and os.access(tool, os.X_OK)
        )
    except (OSError, ValueError, RuntimeError) as error:
        raise ValueError(f"cannot inspect AO_NIX_ACTIONLINT selection {str(tool)!r}: {error}") from error
    if not available:
        raise ValueError("actionlint is unavailable in AO_NIX_ACTIONLINT from the pinned Nix shell; re-enter ./ao")
    return str(tool)


def ready_actionlint() -> ReadyTool:
    tool = actionlint_executable()
    return ReadyTool((tool,), (Path(tool),))


def check_workflows(
    files: Sequence[str], *, report_only: bool = False, sink: TextIO | None = None, tool: str | None = None
) -> int:
    workflows = [name for name in files if git_files.is_workflow_file(name) and source_scope.file_exists(name)]
    if not workflows:
        return 0
    print("=== workflow lint (actionlint) ===")
    if sys.platform != "linux":
        print("Skipping workflow lint: the pinned Linux Nix environment owns this repository check.")
        return 0
    try:
        selected = actionlint_executable() if tool is None else tool
        result = subprocess.run([selected, "-no-color", *workflows], cwd=PROJECT_ROOT, stdout=sink, stderr=sink)
    except (OSError, ValueError) as error:
        print(f"cannot execute pinned actionlint: {error}", file=sys.stderr)
        return 2
    if result.returncode == 0:
        return 0
    return 2 if result.returncode != 1 else int(not report_only)


def _cpp_scope(scope: SourceScope, files: Sequence[str]) -> SourceScope:
    explicit = {source_scope.spelling(name) for name in scope.explicit_files}
    # A mixed file+directory selection cannot promote scanned fixtures to
    # explicit files when the legacy engine receives its scope-wide flag.
    selected = [name for name in files if classify_relative(name, name in explicit) != "IGNORE"]
    return SourceScope(tuple(selected), tuple(name for name in selected if name in explicit))


def _verdict(status: int, *, report_only: bool) -> int:
    """Keep admission/tool failures fatal; only ordinary findings are advisory."""
    return 0 if report_only and status == 1 else status


def run(scope: SourceScope | None, request: LintRequest, *, python: Path | None = None) -> int:
    """Run diagnostics only over prepared paths; None selects explicit owners."""
    validate(request)
    if scope is None:
        if not request.owner_targets:
            raise ValueError("a lint source scope or explicit native owner is required")
        return bazel_lint.run(_NativeLintRequest(**asdict(request)))
    source_scope.require_files(scope)
    if not scope.files:
        print("No files to check.")
        return 0
    cpp_files, python_files = split_existing(scope.files)
    cpp_scope = _cpp_scope(scope, cpp_files)
    destination: AbstractContextManager[TextIO | None] = nullcontext(None)
    if request.output:
        output = absolute_path(Path(request.output))
        output.parent.mkdir(parents=True, exist_ok=True)
        destination = output.open("w", encoding="utf-8")
    with destination as sink:
        status = 0
        if cpp_scope.files or request.owner_targets:
            native_request = _NativeLintRequest(
                **asdict(request), files=cpp_scope.explicit_files, _resolved_sources=cpp_scope.files
            )
            if sink is not None:
                sink.flush()
            status = max(status, bazel_lint.run(native_request))
            if sink is not None:
                sink.seek(0, 2)
        if request.tidy_check:
            return status
        if cpp_scope.files:
            print("=== test-audit ===")
            test_files = [name for name in cpp_scope.files if test_audit.is_audit_candidate(name)]
            if test_files:
                status = max(status, audit_tests(test_files, report_only=request.report_only, sink=sink))
            else:
                print("No applicable test files in scope.")
            print("=== name-audit ===")
            status = max(status, audit_names(cpp_scope.files, report_only=request.report_only, sink=sink))
        if python_files:
            print(f"=== Python lint — {len(python_files)} file(s) ===")
            status = max(
                status,
                _verdict(
                    python_check.run_paths(python_files, python=python, sink=sink), report_only=request.report_only
                ),
            )
        bazel_files = [name for name in scope.files if git_files.is_bazel_file(name)]
        if bazel_files:
            try:
                status = max(
                    status,
                    _verdict(
                        buildifier.lint_paths(bazel_files, sink=sink, provision=False), report_only=request.report_only
                    ),
                )
            except buildifier.BuildifierError as error:
                print(str(error), file=sys.stderr)
                status = max(status, 2)
        status = max(status, check_workflows(scope.files, report_only=request.report_only, sink=sink))
        return status
