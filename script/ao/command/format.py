"""Source-rewriting format command; verification belongs to ``check format``."""

import argparse

from ao.core.proc import die
from ao.portal import source_format

from . import options

NAME = "format"
HELP = "Rewrite source formatting for selected files or directories"
REQUIRES_BUILD_ENV = False


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    options.add_source_arguments(parser)
    parser.set_defaults(func=run_command)


def requires_build_environment(args: argparse.Namespace) -> bool:
    try:
        return options.needs_formatter_environment(options.resolved_scope(args))
    except ValueError as error:
        raise die(str(error)) from error


def requires_python_tools(args: argparse.Namespace) -> bool:
    try:
        return options.has_python_sources(options.resolved_scope(args))
    except ValueError as error:
        raise die(str(error)) from error


REQUIRES_PYTHON_TOOLS = requires_python_tools


def run_command(args: argparse.Namespace) -> int:
    try:
        return source_format.rewrite(options.resolved_scope(args))
    except (OSError, ValueError) as error:
        raise die(str(error)) from error
