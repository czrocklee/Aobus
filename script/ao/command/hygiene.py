"""ao hygiene - the check-only commit gate: format, test naming, and tidy checks.

Deliberately never modifies files: rewriting sources mid-session disturbs in-flight work,
and most clang-tidy findings have no safe auto-fix. The gate reports; fixes are applied
explicitly (./ao format for formatting, manual edits for lint findings).

Resolution order matters: format before acting on tidy findings. clang-format shifts
line numbers, so lint findings collected against unformatted code go stale once you
format, which forces another expensive clang-tidy pass. Formatting first holds
clang-tidy to two runs (discover + verify); the failure message below spells this
out.
"""

import argparse
import sys
from collections.abc import Callable

from ..core import tidyengine
from . import format as format_command
from . import test_audit, tidy

HELP = "Run the commit gate: format --check, test-audit, then tidy (check-only, never edits files)"

EPILOG = """\
With no paths, checks files changed against local main + working tree + staged + untracked.

examples:
  ./ao hygiene
  ./ao hygiene --all
  ./ao hygiene script/ao/command/hygiene.py
"""

Register = Callable[["argparse._SubParsersAction[argparse.ArgumentParser]"], None]


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "hygiene", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("files", nargs="*", metavar="file", help="explicit files to check")
    parser.add_argument("--all", action="store_true", help="check every source in the project folders")
    parser.add_argument(
        "--folder", action="append", default=[], metavar="<dir>", help="all files under <dir> (repeatable)"
    )
    parser.add_argument("--commit", metavar="<rev>", help="changed files since <rev> + working tree + untracked")
    parser.add_argument("-p", "--path", metavar="<dir>", help="build directory with compile_commands.json")
    parser.add_argument("-j", "--jobs", type=int, default=tidyengine.default_jobs(), help="parallel jobs")
    parser.set_defaults(func=run_command)


def _subcommand_defaults(register_command: Register, name: str, **overrides: object) -> argparse.Namespace:
    """Parse the subcommand's real defaults so hygiene can never drift from its parser."""
    parser = argparse.ArgumentParser()
    register_command(parser.add_subparsers())
    namespace = parser.parse_args([name])
    for key, value in overrides.items():
        if not hasattr(namespace, key):
            raise AttributeError(f"{name} parser has no argument {key!r}")
        setattr(namespace, key, value)
    return namespace


def _format_args(args: argparse.Namespace) -> argparse.Namespace:
    return _subcommand_defaults(
        format_command.register,
        "format",
        files=args.files,
        all=args.all,
        folder=args.folder,
        commit=args.commit,
        check=True,
    )


def _tidy_args(args: argparse.Namespace) -> argparse.Namespace:
    return _subcommand_defaults(
        tidy.register,
        "tidy",
        files=args.files,
        all=args.all,
        folder=args.folder,
        commit=args.commit,
        jobs=args.jobs,
        path=args.path,
    )


def _test_audit_args() -> argparse.Namespace:
    return _subcommand_defaults(test_audit.register, "test-audit", paths=[], fail_on_issue=True)


def run_command(args: argparse.Namespace) -> int:
    print("=== format --check ===")
    format_failed = format_command.run_command(_format_args(args)) != 0

    print()
    print("=== test-audit ===")
    test_audit_failed = test_audit.run_command(_test_audit_args()) != 0

    print()
    print("=== tidy ===")
    tidy_failed = tidy.run_command(_tidy_args(args)) != 0

    if not (format_failed or test_audit_failed or tidy_failed):
        return 0

    print("Hygiene issues found. This gate is check-only; fix and re-run.", file=sys.stderr)
    if format_failed and tidy_failed:
        print("  Order matters - format FIRST, then lint:", file=sys.stderr)
        print("    1. Run ./ao format on the same scope, then review and re-stage the diff.", file=sys.stderr)
        print("       Formatting shifts line numbers; fixing lint first strands the tidy", file=sys.stderr)
        print("       findings on stale lines and forces an extra clang-tidy pass.", file=sys.stderr)
        print("    2. Fix the lint findings manually against the now-current lines.", file=sys.stderr)
        print("  Then re-run ./ao hygiene to verify.", file=sys.stderr)
    elif format_failed:
        print("  - Formatting: run ./ao format on the same scope, then review and re-stage the diff.", file=sys.stderr)
    elif tidy_failed:
        print("  - Lint findings: formatting is already clean, so line numbers are stable -", file=sys.stderr)
        print("    fix the findings manually, re-run scoped validation, then ./ao hygiene.", file=sys.stderr)

    if test_audit_failed:
        print(
            "  - Test names/tags: fix the reported TEST_CASE names or tags, then rerun "
            "./ao test-audit --fail-on-issue.",
            file=sys.stderr,
        )
    return 1
