"""ao docs — check documentation integrity and semantic references."""

import argparse
import sys

from ..core.paths import PROJECT_ROOT

HELP = "Check documentation links, reachability, and semantic references"
NAME = "docs"
REQUIRES_BUILD_ENV = False
REQUIRES_PYTHON_TOOLS = True


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    actions = parser.add_subparsers(dest="docs_action", metavar="<action>", required=True)

    check = actions.add_parser(
        "check", help="check optional metadata, links, anchors, reachability, and naming vocabulary"
    )
    check.add_argument("--format", action="store_true", help="also check discovered Markdown with pinned Prettier")
    check.set_defaults(func=run_check)


def run_check(args: argparse.Namespace) -> int:
    # Native preflight imports every command before selecting managed packages.
    from ..core import doccheck

    issues = doccheck.check_tree()
    for issue in issues:
        print(issue.format(), file=sys.stderr)

    if issues:
        print(f"Documentation check failed: {len(issues)} issue(s).", file=sys.stderr)
        return 1

    markdown = doccheck.discover_markdown()
    if getattr(args, "format", False):
        from ..core import prettier

        try:
            status = prettier.run([path.relative_to(PROJECT_ROOT).as_posix() for path in markdown], check=True)
            if status:
                return status
        except prettier.PrettierError as exc:
            print(str(exc), file=sys.stderr)
            return 1
    print(f"Documentation check passed ({len(markdown)} Markdown files).")
    return 0
