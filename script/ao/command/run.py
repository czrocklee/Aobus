"""ao run — run Aobus applications (cli or gtk)."""

import argparse
import os
from pathlib import Path

from ..core import builddir
from ..core.proc import die
from . import build

HELP = "Run Aobus applications (cli or gtk)"

EPILOG = """\
examples:
  ./ao run cli              # build and run the CLI client built in debug mode
  ./ao run gtk              # build and run the GTK desktop client built in debug mode
  ./ao run cli -n           # run the CLI client without rebuilding
  ./ao run cli release      # build and run the CLI client built in release mode
  ./ao run gtk --clang      # build and run the GTK client built using clang compiler
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "run", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("app", choices=["cli", "gtk"], help="the application to run (cli or gtk)")
    build.add_build_arguments(parser)
    parser.add_argument("-n", "--no-build", action="store_true", help="skip building the target")
    parser.add_argument("app_args", nargs="*", help="non-option arguments forwarded to the application")
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    target = "aobus" if args.app == "cli" else "aobus-gtk"

    if not args.no_build:
        build.do_build(args, [target])

    build_dir = (
        Path(args.path)
        if getattr(args, "path", None)
        else builddir.build_dir(args.flavor, clang=args.clang, asan=args.asan, tsan=args.tsan)
    )

    if args.app == "cli":
        executable = build_dir / "app/cli/aobus"
    else:
        executable = build_dir / "app/linux-gtk/aobus-gtk"

    if not executable.exists():
        raise die(f"Executable not found at {executable}. Did you build the project? Run './ao build' first.")

    # Replaces the current process with the target executable
    os.execvp(str(executable), [str(executable), *args.app_args])
