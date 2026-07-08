"""Aobus development portal: `./ao <command> [options]`.

Each command module registers an argparse subparser; `./ao help` lists them all.
The `ao` shim at the repository root re-enters nix-shell before dispatching here.
"""

import argparse
import sys

from .command import analyze, build, check, council, coverage, hygiene, run, test, test_audit, tidy
from .command import format as format_command

COMMAND_MODULES = (build, check, test, test_audit, coverage, tidy, analyze, format_command, hygiene, run, council)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ao",
        description="Aobus development portal. Run './ao <command> --help' for details.",
    )
    subparsers = parser.add_subparsers(dest="command", metavar="<command>", required=True)
    for module in COMMAND_MODULES:
        module.register(subparsers)
    return parser


def parse_arguments(parser: argparse.ArgumentParser, arguments: list[str]) -> argparse.Namespace:
    # `ao run` forwards everything after `--` straight to the application. Strip that tail
    # before argparse runs so option-like app flags (e.g. `--library`) are not mistaken for
    # ao's own options or for the optional `flavor` positional, then re-attach them to app_args.
    forwarded: list[str] = []
    if arguments and arguments[0] == "run" and "--" in arguments:
        split = arguments.index("--")
        arguments, forwarded = arguments[:split], arguments[split + 1 :]
    args = parser.parse_args(arguments)
    if forwarded:
        args.app_args = list(args.app_args) + forwarded
    return args


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
    args = parse_arguments(parser, arguments)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
