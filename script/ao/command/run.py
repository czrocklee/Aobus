"""ao run — run Aobus applications (cli, tui, or gtk)."""

import argparse
import os
from dataclasses import dataclass
from pathlib import Path

from ..core import builddir
from ..core.proc import die
from . import build

HELP = "Run Aobus applications (cli, tui, or gtk)"


@dataclass(frozen=True)
class AppSpec:
    target: str
    executable: Path


APPS = {
    "cli": AppSpec("aobus", Path("app/cli/aobus")),
    "tui": AppSpec("aobus-tui", Path("app/tui/aobus-tui")),
    "gtk": AppSpec("aobus-gtk", Path("app/linux-gtk/aobus-gtk")),
}

EPILOG = """\
examples:
  ./ao run cli              # build and run the CLI client built in debug mode
  ./ao run tui              # build and run the terminal client built in debug mode
  ./ao run gtk              # build and run the GTK desktop client built in debug mode
  ./ao run cli -n           # run the CLI client without rebuilding
  ./ao run cli release      # build and run the CLI client built in release mode
  ./ao run gtk --clang      # build and run the GTK client built using clang compiler
  ./ao run tui -- --library ~/Music   # forward option flags to the application after --
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "run", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("app", choices=APPS.keys(), help="the application to run (cli, tui, or gtk)")
    build.add_build_arguments(parser)
    parser.add_argument("-n", "--no-build", action="store_true", help="skip building the target")
    parser.add_argument(
        "app_args",
        nargs="*",
        help="arguments forwarded to the application; put option flags after `--`",
    )
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    app = APPS[args.app]

    if not args.no_build:
        build.do_build(args, [app.target])

    build_dir = (
        Path(args.path)
        if getattr(args, "path", None)
        else builddir.build_dir(args.flavor, clang=args.clang, asan=args.asan, tsan=args.tsan)
    )
    executable = build_dir / app.executable

    if not executable.exists():
        raise die(f"Executable not found at {executable}. Did you build the project? Run './ao build' first.")

    # Replaces the current process with the target executable
    os.execvp(str(executable), [str(executable), *args.app_args])
