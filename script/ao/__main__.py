"""Aobus development portal: `./ao <command> [options]`.

Each command module registers an argparse subparser; `./ao help` lists them all.
Platform launchers prepare the native environment before dispatching a parsed command.
"""

import argparse
import os
import sys

from .command import COMMAND_MODULES
from .core import buildenv, phases


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
    return parser.parse_args(arguments)


def parse_invocation(arguments: list[str]) -> argparse.Namespace | None:
    # Child processes write straight to the underlying fd; line-buffer our own prints so
    # status lines interleave with subprocess output in the right order when piped.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    parser = make_parser()
    if not arguments or arguments[0] == "help":
        parser.print_help()
        return None
    return parse_arguments(parser, arguments)


def dispatch(args: argparse.Namespace, *, build_environment_required: bool | None = None) -> int:
    """Execute an already parsed invocation, including any prepared source scope."""
    # Native commands own their governed Bazel tree. No implicit configure,
    # source projection or alternate build graph runs before the selected command.
    with phases.measure("command", command=args.command) as phase:
        status: int = args.func(args) or 0
        phase.status = status
        return status


def main(argv: list[str] | None = None) -> int:
    args = parse_invocation(list(sys.argv[1:] if argv is None else argv))
    if args is None:
        return 0
    buildenv.load_source_scope(args, os.environ.pop("AOBUS_PREFLIGHT_SCOPE", None))
    return dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
