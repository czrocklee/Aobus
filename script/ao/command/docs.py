"""ao docs — validate and manage the Aobus documentation system."""

import argparse
import sys

from ..core import doccheck

HELP = "Validate the Aobus documentation structure and internal links"
NAME = "docs"
REQUIRES_BUILD_ENV = False


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    actions = parser.add_subparsers(dest="docs_action", metavar="<action>", required=True)

    check = actions.add_parser(
        "check", help="validate metadata, RFC dependencies, links, anchors, and index reachability"
    )
    check.set_defaults(func=run_check)


def run_check(_args: argparse.Namespace) -> int:
    issues = doccheck.check_tree()
    for issue in issues:
        print(issue.format(), file=sys.stderr)

    if issues:
        print(f"Documentation check failed: {len(issues)} issue(s).", file=sys.stderr)
        return 1

    print(f"Documentation check passed ({len(doccheck.discover_markdown())} Markdown files).")
    return 0
