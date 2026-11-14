"""ao tidy — diagnostic-only C++, Python and Bazel checks over native contexts."""

import argparse
import sys
from collections.abc import Sequence
from contextlib import AbstractContextManager, nullcontext
from pathlib import Path
from typing import Any, TextIO

from ..core import (
    bazel_lint,
    bazel_native,
    bazel_workflows,
    buildenv,
    buildifier,
    gitfiles,
    pythoncheck,
    tidyconfig,
    tidyengine,
)
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.proc import die
from ..core.tidypolicy import classify_relative
from ..core.tidypolicy import find_stale_nolint_suppressions as find_stale_nolint_suppressions

NAME = "tidy"
HELP = "Run native Bazel clang-tidy, Python Ruff/mypy and Bazel Buildifier checks"
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = True
ALL_FOLDERS = ["lib", "app", "asset", "bazel", "include", "script", "test", "tool"]


def expected_aobus_checks(fixture_dir: Path | None = None) -> frozenset[str]:
    """Read nonblank working-tree fixture inventory, including new fixtures."""
    from ..core.linttest import FIXTURE_DIR, discover_fixtures

    root = FIXTURE_DIR if fixture_dir is None else fixture_dir
    return frozenset(
        fixture.check
        for fixture in discover_fixtures(root)
        if fixture.check.startswith("aobus-") and fixture.path.read_text(encoding="utf-8", errors="replace").strip()
    )


class _RemovedFixFlag(argparse.Action):
    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: str | Sequence[Any] | None,
        option_string: str | None = None,
    ) -> None:
        parser.error("--fix was removed; ao tidy is diagnostic-only and does not apply replacements")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    tidyengine.add_scope_arguments(parser, "check")
    parser.add_argument("--config", choices=("debug", "release"), default="debug")
    parser.add_argument("--vcpkg-prefix", help="existing governed native SDK prefix")
    parser.add_argument("--target", action="append", dest="owner_targets", default=[], help="native compile owner")
    parser.add_argument(
        "-n", "--no-build", action="store_true", help="retired compile-database option; explicitly rejected"
    )
    parser.add_argument("--fix", nargs=0, action=_RemovedFixFlag, dest=argparse.SUPPRESS, help=argparse.SUPPRESS)
    parser.add_argument(
        "--tidy-arg", action="append", dest="clang_tidy_arg", default=[], type=tidyconfig.diagnostic_argument
    )
    parser.add_argument("--header-filter", help="override the cache-keyed diagnostic header filter")
    bazel_workflows.add_remote_cache_argument(parser, "opt in to a trusted native action cache")
    parser.set_defaults(func=run_command)


def _target_only(args: argparse.Namespace) -> bool:
    return bool(
        args.owner_targets
        and not (args.files or args.all or args.folder or args.commit or getattr(args, "_resolved_sources", None))
    )


def requires_build_environment(args: argparse.Namespace) -> bool:
    _validate(args)
    if _target_only(args):
        return True
    files, _explicit = tidyengine.resolve_scope(args, ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES)
    return buildenv.requires_source_build_env(args, files)


def _validate(args: argparse.Namespace) -> None:
    if args.no_build:
        raise die(
            "tidy --no-build is retired: Bazel must admit the checker and produce keyed analysis reports; "
            "cached actions remain supported"
        )


def classify(path: Path, explicit: bool) -> str:
    try:
        relative = absolute_path(path).relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        return "STRICT"
    return classify_relative(relative, explicit)


def classify_existing(files: list[str], explicit: bool) -> dict[str, list[Path]]:
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    seen: set[Path] = set()
    for name in files:
        path = absolute_path(PROJECT_ROOT / name)
        if not path.is_file() or path in seen:
            continue
        seen.add(path)
        mode = classify(path, explicit)
        if mode != "IGNORE":
            buckets[mode].append(path)
    return buckets


def split_existing(files: list[str]) -> tuple[list[str], list[str]]:
    cpp, python = [], []
    seen: set[Path] = set()
    for name in files:
        path = absolute_path(PROJECT_ROOT / name)
        if not path.is_file() or path in seen:
            continue
        seen.add(path)
        try:
            relative = path.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
        except ValueError:
            relative = path.as_posix()
        if path.suffix in gitfiles.CPP_SUFFIXES:
            cpp.append(relative)
        elif path.suffix in gitfiles.PYTHON_SUFFIXES:
            python.append(relative)
    return cpp, python


def _lint_args(args: argparse.Namespace, sources: list[str], explicit: bool) -> argparse.Namespace:
    """Use tidy's defaults for both standalone and hygiene's frozen C++ scope."""
    parser = argparse.ArgumentParser()
    register(parser.add_subparsers())
    selected = parser.parse_args([NAME])
    for key in ("config", "path", "vcpkg_prefix", "jobs", "remote_cache"):
        setattr(selected, key, getattr(args, key))
    selected.files = list(sources) if explicit else []
    selected._resolved_sources = sources
    return selected


def run_lint_stages(args: argparse.Namespace, files: list[str], explicit: bool, *, python: Path | None = None) -> int:
    """Gate the same frozen scope for standalone tidy and hygiene's lint stage."""
    cpp_files, python_files = split_existing(files)
    # A named C++ check must not silently run unrelated language checks.
    if getattr(args, "check", None):
        python_files = []
    output = getattr(args, "output", None)
    destination: AbstractContextManager[TextIO | None]
    if output:
        report = absolute_path(Path(output))
        report.parent.mkdir(parents=True, exist_ok=True)
        destination = report.open("w", encoding="utf-8")
    else:
        destination = nullcontext(None)
    with destination as sink:
        return _run_lint_stages(args, files, explicit, cpp_files, python_files, python=python, sink=sink)


def _run_lint_stages(
    args: argparse.Namespace,
    files: list[str],
    explicit: bool,
    cpp_files: list[str],
    python_files: list[str],
    *,
    python: Path | None,
    sink: TextIO | None,
) -> int:
    failed = False
    if cpp_files:
        lint_args = _lint_args(args, cpp_files, explicit)
        for name in ("check", "header_filter", "output", "debug", "clang_tidy_arg", "owner_targets"):
            if hasattr(args, name):
                setattr(lint_args, name, getattr(args, name))
        # The native consumer writes its complete diagnostics to output first;
        # append remaining language diagnostics after it finishes.
        if sink is not None:
            sink.flush()
        failed = bazel_lint.run(lint_args) != 0 or failed
        if sink is not None:
            sink.seek(0, 2)
    if python_files:
        print(f"=== Python hygiene — {len(python_files)} file(s) ===")
        failed = pythoncheck.run_paths(python_files, python=python, sink=sink) != 0 or failed
    bazel_files = [name for name in files if gitfiles.is_bazel_file(name) and (PROJECT_ROOT / name).is_file()]
    if bazel_files and not getattr(args, "check", None):
        try:
            failed = buildifier.run(bazel_files, check=True, lint=True, sink=sink) != 0 or failed
        except buildifier.BuildifierError as error:
            raise die(str(error)) from error
    elif bazel_files:
        print("Skipping Bazel checks: --check selects only C++ diagnostics.", file=sys.stderr)
    return int(failed)


def run_command(args: argparse.Namespace, *, resolved_scope: tuple[list[str], bool] | None = None) -> int:
    _validate(args)
    try:
        if resolved_scope is None and _target_only(args):
            return bazel_lint.run(args)
        files, explicit = resolved_scope or tidyengine.resolve_scope(
            args, ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES
        )
        return run_lint_stages(args, files, explicit)
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
