"""Explicit preparation of governed development tools, SDKs and configuration."""

import argparse

from ao.core import bazel_native
from ao.core.proc import die
from ao.portal import setup_actions

NAME = "setup"
HELP = "Prepare an explicitly selected development tool, SDK or repository configuration"
REQUIRES_BUILD_ENV = False


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    components = parser.add_subparsers(dest="component", metavar="<component>", required=True)
    for name in setup_actions.COMPONENTS:
        component = components.add_parser(name, help=f"prepare {name}")
        if name == "deps":
            component.add_argument("-p", "--path", help="external governed SDK tree (honors BUILD_DIR)")
            component.add_argument("--config", choices=("fastbuild", "debug", "release"), default="fastbuild")
        component.set_defaults(func=run_command)


def requires_build_environment(args: argparse.Namespace) -> bool:
    return bool(args.component == "deps")


def requires_provider_tools(args: argparse.Namespace) -> bool:
    return bool(args.component == "deps")


REQUIRES_PROVIDER_TOOLS = requires_provider_tools


def run_command(args: argparse.Namespace) -> int:
    try:
        return setup_actions.setup(
            args.component, path=getattr(args, "path", None), config=getattr(args, "config", "fastbuild")
        )
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
