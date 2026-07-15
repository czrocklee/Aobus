"""ao run — run Aobus applications (cli, tui, or gtk)."""

import argparse
import os
from dataclasses import dataclass
from pathlib import Path

from ..core import builddir
from ..core.proc import die
from . import build

HELP = "Build and run an application enabled by the native profile"
NAME = "run"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


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

WINDOWS_EPILOG = """\
examples:
  ao.bat run cli                         # build and run the CLI in debug mode
  ao.bat run tui                         # build and run the TUI in debug mode
  ao.bat run tui -n                      # run without rebuilding
  ao.bat run tui release                 # build and run the release TUI
  ao.bat run tui -- --library C:\\Music  # forward application options after --
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    profile = builddir.platform_profile()
    parser = subparsers.add_parser(
        NAME,
        help=HELP,
        description=HELP,
        epilog=WINDOWS_EPILOG if builddir.platform_profile().name == "windows" else EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("app", choices=profile.apps, help=f"application to run ({', '.join(profile.apps)})")
    build.add_build_arguments(parser)
    parser.add_argument("-n", "--no-build", action="store_true", help="skip building the target")
    parser.add_argument(
        "app_args",
        nargs="*",
        help="arguments forwarded to the application; put option flags after `--`",
    )
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    profile = build.validate_build_options(args)
    if args.app not in profile.apps:
        available = ", ".join(profile.apps)
        raise die(f"application '{args.app}' is unavailable on {profile.name}. Available applications: {available}.")

    app = APPS[args.app]

    if not args.no_build:
        build.do_build(args, [app.target])

    build_dir = (
        Path(args.path)
        if getattr(args, "path", None)
        else builddir.build_dir(args.flavor, clang=args.clang, asan=args.asan, tsan=args.tsan)
    )
    executable = builddir.executable(build_dir / app.executable)

    if not executable.exists():
        raise die(f"Executable not found at {executable}. Did you build the project? Run './ao build' first.")

    # Replaces the current process with the target executable
    os.execvp(str(executable), [str(executable), *args.app_args])
