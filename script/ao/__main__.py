"""Aobus development portal: `./ao <command> [options]`.

Each command module registers an argparse subparser; `./ao help` lists them all.
Platform launchers prepare the native environment before dispatching a parsed command.
"""

import argparse
import os
import sys
from contextlib import nullcontext

from .command import COMMAND_MODULES
from .core import buildenv, compiler_cache, msbuild_cache, phases, workspace_cache


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
    previous_tracking = os.environ.get(msbuild_cache.TRACKING_ENV)
    try:
        if os.name == "nt":
            os.environ[msbuild_cache.TRACKING_ENV] = "0"
        if args.command != "bazel" and (
            (
                buildenv.requires_parsed_build_env(args)
                if build_environment_required is None
                else build_environment_required
            )
            or workspace_cache.command_uses_context(args)
        ):
            try:
                compiler_cache.activate_local()
            except compiler_cache.CompilerCacheError as exc:
                print(f"Warning: compiler cache is disabled: {exc}", file=sys.stderr)
        # run measures preparation itself: a successful exec cannot close a root span.
        measurement = phases.measure("command", command=args.command) if args.command != "run" else nullcontext()
        with measurement as phase:
            try:
                # Bazel owns its external tree and has no CMake workspace/cache views to prepare.
                context = nullcontext() if args.command == "bazel" else workspace_cache.command_context(args)
                with context:
                    status: int = args.func(args) or 0
                if phase is not None:
                    phase.status = status
                return status
            except workspace_cache.WorkspaceCacheError as exc:
                print(f"Error: {exc}", file=sys.stderr)
                if phase is not None:
                    phase.status = 1
                else:
                    # Context entry/cleanup failures are not successful launch preparation.
                    with phases.measure("launch-error", command="run") as error_phase:
                        error_phase.status = 1
                return 1
    finally:
        if os.name == "nt":
            if previous_tracking is None:
                os.environ.pop(msbuild_cache.TRACKING_ENV, None)
            else:
                os.environ[msbuild_cache.TRACKING_ENV] = previous_tracking


def main(argv: list[str] | None = None) -> int:
    args = parse_invocation(list(sys.argv[1:] if argv is None else argv))
    if args is None:
        return 0
    buildenv.load_source_scope(args, os.environ.pop("AOBUS_PREFLIGHT_SCOPE", None))
    return dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
