"""ao setup — perform explicit, scoped host setup actions."""

import argparse

from ..core import winui
from ..core.proc import die

HELP = "Perform an explicit, scoped development-host setup action"
NAME = "setup"
REQUIRES_BUILD_ENV = False


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument("component", choices=("winui-runtime",), help="host component to install")
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    if args.component != "winui-runtime":
        raise die(f"unsupported setup component {args.component!r}")
    try:
        installed = winui.setup_runtime()
    except RuntimeError as exc:
        raise die(str(exc)) from exc
    print(f"Windows App Runtime {installed.version} {installed.architecture} is ready.")
    return 0
