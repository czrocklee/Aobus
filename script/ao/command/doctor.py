"""ao doctor — inspect platform development prerequisites without changing them."""

import argparse

from ..core import winui
from ..core.proc import die

HELP = "Inspect native development prerequisites without modifying the host"
NAME = "doctor"
REQUIRES_BUILD_ENV = False


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument("area", choices=("winui",), help="development surface to inspect")
    parser.add_argument(
        "--build-only",
        action="store_true",
        help="omit runtime launch checks when validating a build-only host",
    )
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    if args.area != "winui":
        raise die(f"unsupported doctor area {args.area!r}")
    checks = winui.inspect_host(include_runtime=not args.build_only)
    for check in checks:
        if check.ok:
            marker = "ok"
        elif check.required:
            marker = "FAIL"
        else:
            marker = "warn"
        print(f"[{marker:>4}] {check.label}: {check.detail}")
    if any(check.required and not check.ok for check in checks):
        raise die("WinUI prerequisites are incomplete.")
    print("WinUI build prerequisites are ready.")
    return 0
