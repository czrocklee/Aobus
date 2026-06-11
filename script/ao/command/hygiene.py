"""ao hygiene - the check-only commit gate: format --check, then tidy.

Deliberately never modifies files: rewriting sources mid-session disturbs in-flight work,
and most clang-tidy findings have no safe auto-fix. The gate reports; fixes are applied
explicitly (./ao format for formatting, manual edits for lint findings).
"""

import argparse
import sys
from collections.abc import Callable

from ..core import tidyengine
from . import format as format_command
from . import tidy

HELP = "Run the commit gate: format --check, then tidy (check-only, never edits files)"

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


def run_command(args: argparse.Namespace) -> int:
    print("=== format --check ===")
    format_failed = format_command.run_command(_format_args(args)) != 0

    print()
    print("=== tidy ===")
    tidy_failed = tidy.run_command(_tidy_args(args)) != 0

    if not (format_failed or tidy_failed):
        return 0

    print("Hygiene issues found. This gate is check-only; fix and re-run:", file=sys.stderr)
    if format_failed:
        print("  - Formatting: run ./ao format on the same scope, then review and re-stage the diff.", file=sys.stderr)
    if tidy_failed:
        print("  - Lint findings: fix them manually, re-run scoped validation, then ./ao hygiene.", file=sys.stderr)
    return 1
