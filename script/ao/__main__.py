"""Aobus development portal: `./ao <command> [options]`.

Each command module registers an argparse subparser; `./ao help` lists them all.
The `ao` shim at the repository root re-enters nix-shell before dispatching here.
"""

import argparse
import sys

from .command import analyze, build, check, coverage, hygiene, run, test, tidy
from .command import format as format_command

COMMAND_MODULES = (build, check, test, coverage, tidy, analyze, format_command, hygiene, run)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ao",
        description="Aobus development portal. Run './ao <command> --help' for details.",
    )
    subparsers = parser.add_subparsers(dest="command", metavar="<command>", required=True)
    for module in COMMAND_MODULES:
        module.register(subparsers)
    return parser


def main(argv: list[str] | None = None) -> int:
    # Child processes write straight to the underlying fd; line-buffer our own prints so
    # status lines interleave with subprocess output in the right order when piped.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    arguments = list(sys.argv[1:] if argv is None else argv)
    parser = make_parser()
    if not arguments or arguments[0] == "help":
        parser.print_help()
        return 0
    args = parser.parse_args(arguments)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
