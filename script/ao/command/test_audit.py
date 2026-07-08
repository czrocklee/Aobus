"""ao test-audit - advisory Catch2 naming and tag checks."""

import argparse
import sys

from ..core import testaudit

HELP = "Report Catch2 test names and tags that drift from project conventions"

EPILOG = """\
The audit is advisory by default: it reports issues and exits zero. Use
--fail-on-issue when running it as a review gate for a selected path.

examples:
  ./ao test-audit
  ./ao test-audit test/unit/query
  ./ao test-audit --fail-on-issue test/unit/query/ParserTest.cpp
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "test-audit",
        help=HELP,
        description=HELP,
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("paths", nargs="*", help="test files or directories to audit; defaults to all tests")
    parser.add_argument("--fail-on-issue", action="store_true", help="return non-zero when issues are found")
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    issues = testaudit.audit_paths(args.paths)
    for issue in issues:
        print(issue.format(), file=sys.stderr)

    print(f"test-audit: {len(issues)} issue(s)")
    return 1 if issues and args.fail_on_issue else 0
