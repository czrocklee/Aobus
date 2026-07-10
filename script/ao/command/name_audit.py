"""ao name-audit - report class and file naming drift."""

import argparse
import sys

from ..core import nameaudit

HELP = "Report class/file names that drift from project role vocabulary"
NAME = "name-audit"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = False


EPILOG = """\
The audit is advisory by default: it reports issues and exits zero. Use
--fail-on-issue when running it as a review or commit gate.

examples:
  ./ao name-audit
  ./ao name-audit app/include/ao/uimodel
  ./ao name-audit --fail-on-issue app/linux-gtk/track/TrackListModel.h
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME,
        help=HELP,
        description=HELP,
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("paths", nargs="*", help="files or directories to audit; defaults to all C++ sources")
    parser.add_argument("--fail-on-issue", action="store_true", help="return non-zero when issues are found")
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    issues = nameaudit.audit_paths(args.paths)
    for issue in issues:
        print(issue.format(), file=sys.stderr)

    print(f"name-audit: {len(issues)} issue(s)")
    return 1 if issues and args.fail_on_issue else 0
