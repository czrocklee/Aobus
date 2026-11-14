"""ao analyze — Clang Static Analyzer over native Bazel compile contexts."""

import argparse
import os
import sys
from pathlib import Path

from ..core import bazel_lint, buildenv, gitfiles, tidyconfig, tidyengine
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.proc import die

HELP = "Run the Clang Static Analyzer (report-only unless --fail-on-diagnostics)"
NAME = "analyze"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


EPILOG = """\
Analyzer diagnostics are report-only by default; tool failures still return non-zero.

examples:
  ./ao analyze                               # changed files
  ./ao analyze --folder lib --folder app     # production code
  ./ao analyze --all -o /tmp/analyzer.txt    # whole repo, report to a file
  ./ao analyze --all --fail-on-diagnostics   # gate once the baseline is clean
  ./ao analyze --alpha --folder lib          # include experimental checks
"""

ALL_FOLDERS = ["lib", "app", "include", "test", "tool/lint"]


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    tidyengine.add_scope_arguments(parser, verb="analyze")
    parser.add_argument("--fail-on-diagnostics", action="store_true", help="return non-zero when diagnostics are found")
    parser.add_argument(
        "--include-external-diagnostics",
        action="store_true",
        help="include diagnostics whose primary location is outside the repo",
    )
    parser.add_argument("--alpha", action="store_true", help="also enable clang-analyzer-alpha.* checks")
    parser.add_argument(
        "--tidy-arg",
        action="append",
        default=[],
        type=tidyconfig.diagnostic_argument,
        metavar="<arg>",
        help="extra diagnostic-only clang-tidy argument (repeatable; no fixes, exports or raw response files)",
    )
    parser.set_defaults(func=run_command)


def requires_build_environment(args: argparse.Namespace) -> bool:
    if args.jobs < 1:
        raise die("-j must be at least 1.")
    files, _explicit = analyzable_sources(args)
    return buildenv.requires_source_build_env(args, files)


def classify(path: Path) -> bool:
    """Skip the checkout's intentional lint negatives after native normalization."""
    path = absolute_path(path)
    try:
        rel = path.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        return True

    if os.name == "nt":
        rel = rel.lower()
    return not rel.startswith("test/lint/")


def analyzable_sources(args: argparse.Namespace) -> tuple[list[str], bool]:
    """Resolve the analyzer scope to existing files, excluding checkout lint fixtures."""
    candidates, explicit = tidyengine.resolve_scope(args, ALL_FOLDERS, "Analyzing")
    files: list[str] = []
    seen: set[Path] = set()
    for name in candidates:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            if explicit:
                raise die(f"explicitly selected analyzer path does not exist or is not a file: {name}")
            continue
        if path.suffix not in gitfiles.CPP_SUFFIXES:
            continue
        path = absolute_path(path)
        if path not in seen and classify(path):
            seen.add(path)
            files.append(str(path))
    return files, explicit


def run_command(args: argparse.Namespace) -> int:
    # Argparse rejects these during parsing. This remains the guard for a
    # programmatic Namespace that did not pass through the option type.
    try:
        tidyconfig.validate_diagnostic_arguments(args.tidy_arg)
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if args.jobs < 1:
        raise die("-j must be at least 1.")
    files, explicit = analyzable_sources(args)
    if not files:
        print("No analyzable C++ or Objective-C++ sources found.", file=sys.stderr)
        return 0
    return bazel_lint.run_analyzer(args, files, explicit=explicit)
