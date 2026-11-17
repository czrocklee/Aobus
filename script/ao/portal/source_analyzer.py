"""Path-sensitive native analysis over a frozen source scope."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.policy import tidy_config
from ao.portal import bazel_lint, git_files, source_scope, tidy_engine
from ao.portal.source_scope import Selection, SourceScope

ALL_FOLDERS = ("lib", "app", "include", "test", "tool/lint")


@dataclass(frozen=True)
class AnalyzerRequest:
    path: str | None = None
    jobs: int = field(default_factory=tidy_engine.default_jobs)
    tidy_check: str | None = None
    output: str | None = None
    debug: bool = False
    tidy_args: tuple[str, ...] = ()
    alpha: bool = False
    include_external_diagnostics: bool = False
    report_only: bool = False

    @property
    def check(self) -> str | None:
        return self.tidy_check

    @property
    def tidy_arg(self) -> tuple[str, ...]:
        return self.tidy_args

    @property
    def fail_on_diagnostics(self) -> bool:
        return not self.report_only


def validate(request: AnalyzerRequest) -> None:
    """Admit diagnostic options before source discovery or SDK preparation."""
    tidy_config.validate_diagnostic_arguments(list(request.tidy_args))
    bazel_lint._validate_lint_configuration(request.tidy_check, None, request.tidy_args, alpha=request.alpha)
    bazel_lint._jobs(request.jobs)
    bazel_lint._path_argument(request.path, "path")
    bazel_lint._output_path(request.output)


def classify(path: Path) -> bool:
    try:
        relative = absolute_path(path).relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        return True
    if os.name == "nt":
        relative = relative.lower()
    return not relative.startswith("test/lint/")


def analyzable_scope(scope: SourceScope) -> SourceScope:
    source_scope.require_files(scope)
    files: list[str] = []
    explicit: list[str] = []
    seen: set[Path] = set()
    explicit_paths = {absolute_path(PROJECT_ROOT / name) for name in scope.explicit_files}
    for name in scope.files:
        path = absolute_path(PROJECT_ROOT / name)
        if path.suffix not in git_files.CPP_SUFFIXES or path in seen or not classify(path):
            continue
        seen.add(path)
        files.append(str(path))
        if path in explicit_paths:
            explicit.append(str(path))
    return SourceScope(tuple(files), tuple(explicit))


def select(selection: Selection) -> SourceScope:
    return analyzable_scope(source_scope.resolve(selection, top_dirs=ALL_FOLDERS))


def run(scope: SourceScope, request: AnalyzerRequest) -> int:
    validate(request)
    selected = analyzable_scope(scope)
    if not selected.files:
        print("No analyzable C++ or Objective-C++ sources found.", file=sys.stderr)
        return 0
    if request.output:
        absolute_path(Path(request.output)).parent.mkdir(parents=True, exist_ok=True)
    return bazel_lint.run_analyzer(request, selected.files, explicit_files=selected.explicit_files)
