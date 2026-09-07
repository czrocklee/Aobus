"""ao hygiene - the check-only commit gate: format, naming audits, and tidy checks.

Deliberately never modifies files: rewriting sources mid-session disturbs in-flight work,
and most clang-tidy findings have no safe auto-fix. The gate reports; fixes are applied
explicitly (./ao format for formatting, manual edits for lint findings).

Stop on formatting failure before collecting diagnostics whose locations would
be invalidated by formatting. Every stage uses the same resolved source scope.
"""

import argparse
import sys
from collections.abc import Callable, Sequence

from ..core import buildenv, gitfiles, tidyengine
from . import format as format_command
from . import name_audit, test_audit, tidy

HELP = "Run the commit gate: format --check, naming audits, then tidy (check-only, never edits files)"
NAME = "hygiene"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = True


EPILOG = """\
With no paths, checks files changed since the merge base with local main, plus
working tree, staged, and untracked files. Empty stage subsets are skipped.

examples:
  ./ao hygiene
  ./ao hygiene --all
  ./ao hygiene script/ao/command/hygiene.py
"""

Register = Callable[["argparse._SubParsersAction[argparse.ArgumentParser]"], None]


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
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


def _format_args() -> argparse.Namespace:
    return _subcommand_defaults(
        format_command.register,
        "format",
        check=True,
    )


def _tidy_args(args: argparse.Namespace) -> argparse.Namespace:
    return _subcommand_defaults(
        tidy.register,
        "tidy",
        jobs=args.jobs,
        path=args.path,
    )


def _test_audit_args(paths: list[str]) -> argparse.Namespace:
    return _subcommand_defaults(test_audit.register, "test-audit", paths=paths, fail_on_issue=True)


def _name_audit_args(paths: list[str]) -> argparse.Namespace:
    return _subcommand_defaults(name_audit.register, "name-audit", paths=paths, fail_on_issue=True)


def requires_build_environment(arguments: Sequence[str]) -> bool:
    args = buildenv.parse_command_arguments(NAME, arguments)
    return buildenv.requires_source_build_env(args, format_command.resolve_files(args))


def run_command(args: argparse.Namespace) -> int:
    files = format_command.resolve_files(args)
    if not files:
        print("No files to check.")
        return 0

    print("=== format --check ===")
    if format_command.run_command(_format_args(), files=files) != 0:
        print("Hygiene stopped after formatting failure; this gate is check-only.", file=sys.stderr)
        print("Run ./ao format on the same scope, review the diff, then rerun ./ao hygiene.", file=sys.stderr)
        return 1

    cpp_files = [name for name in files if name.endswith(gitfiles.CPP_SUFFIXES) and format_command._file_exists(name)]
    test_files = [name for name in cpp_files if name.endswith("Test.cpp")]

    print()
    print("=== test-audit ===")
    test_audit_failed = bool(test_files) and test_audit.run_command(_test_audit_args(test_files)) != 0
    if not test_files:
        print("No applicable test files in scope.")

    print()
    print("=== name-audit ===")
    name_audit_failed = bool(cpp_files) and name_audit.run_command(_name_audit_args(cpp_files)) != 0
    if not cpp_files:
        print("No C++ files in scope.")

    print()
    print("=== tidy ===")
    tidy_failed = tidy.run_command(_tidy_args(args), resolved_scope=(files, bool(args.files))) != 0

    if not (test_audit_failed or name_audit_failed or tidy_failed):
        return 0

    print("Hygiene issues found. This gate is check-only; fix and re-run.", file=sys.stderr)
    if tidy_failed:
        print("  - Lint findings: formatting is already clean, so line numbers are stable -", file=sys.stderr)
        print("    fix the findings manually, re-run scoped validation, then ./ao hygiene.", file=sys.stderr)

    if test_audit_failed:
        print(
            "  - Test names/tags: fix the reported TEST_CASE names or tags, then rerun "
            "./ao test-audit --fail-on-issue.",
            file=sys.stderr,
        )
    if name_audit_failed:
        print(
            "  - Class/file names: fix the reported role or catch-all names, then rerun "
            "./ao name-audit --fail-on-issue.",
            file=sys.stderr,
        )
    return 1
