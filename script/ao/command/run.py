"""ao run — run applications enabled by the native profile."""

import argparse
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..core import builddir, winui, workspace_cache
from ..core.proc import die
from . import build

HELP = "Build and run an application enabled by the native profile"
NAME = "run"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


def requires_build_environment(args: argparse.Namespace) -> bool:
    """Return whether this invocation can build before launching."""
    return not args.no_build


@dataclass(frozen=True)
class AppSpec:
    target: str
    executable: Path


APPS = {
    "appkit": AppSpec("aobus-appkit", Path("app/macos-appkit/aobus-appkit.app/Contents/MacOS/aobus-appkit")),
    "cli": AppSpec("aobus", Path("app/cli/aobus")),
    "tui": AppSpec("aobus-tui", Path("app/tui/aobus-tui")),
    "gtk": AppSpec("aobus-gtk", Path("app/linux-gtk/aobus-gtk")),
    "winui": AppSpec("winui", Path("app/windows-winui")),
}

EPILOG = """\
examples:
  ./ao run cli              # build and run the CLI client built in debug mode
  ./ao run tui              # build and run the terminal client built in debug mode
  ./ao run gtk              # build and run the GTK desktop client built in debug mode
  ./ao run cli -n           # run the CLI client without rebuilding
  ./ao run cli release      # build and run the CLI client with IPO/LTO
  ./ao run gtk --clang      # build and run the GTK client built using clang compiler
  ./ao run appkit           # build and run the native macOS desktop
  ./ao run appkit --main-thread-checker  # requires full Xcode on macOS
  ./ao run tui -- --library ~/Music   # forward option flags to the application after --
"""

WINDOWS_EPILOG = """\
examples:
  ao.bat run cli                         # build and run the CLI in debug mode
  ao.bat run tui                         # build and run the TUI in debug mode
  ao.bat run winui                       # build and run WinUI from an interactive desktop
  ao.bat run tui -n                      # run without rebuilding
  ao.bat run tui release                 # build and run the release TUI with IPO/LTCG
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
        "--main-thread-checker",
        action="store_true",
        help="inject Xcode's Main Thread Checker into a macOS AppKit application (diagnostic only)",
    )
    parser.add_argument(
        "app_args",
        nargs="*",
        help="arguments forwarded to the application; put option flags after `--`",
    )
    parser.set_defaults(func=run_command)


def _main_thread_checker_environment() -> dict[str, str]:
    """Keep the caller's environment and add the explicitly requested Xcode diagnostic."""
    env = os.environ.copy()
    developer = env.get("DEVELOPER_DIR")
    if not developer:
        try:
            developer = subprocess.check_output(
                ["xcode-select", "--print-path"], text=True, stderr=subprocess.STDOUT
            ).strip()
        except (OSError, subprocess.CalledProcessError) as exc:
            raise die("--main-thread-checker requires a full Xcode installation selected by xcode-select.") from exc
    developer_dir = Path(developer)
    # DEVELOPER_DIR also accepts an Xcode.app bundle, like Apple's tools do.
    if developer_dir.suffix == ".app":
        developer_dir = developer_dir / "Contents" / "Developer"
    library = developer_dir / "usr" / "lib" / "libMainThreadChecker.dylib"
    if not library.is_file():
        raise die(
            f"Main Thread Checker not found at {library}. Select a full Xcode installation with "
            "DEVELOPER_DIR; Command Line Tools alone do not provide it."
        )
    libraries = env.get("DYLD_INSERT_LIBRARIES", "").split(":")
    if str(library) not in libraries:
        libraries.append(str(library))
    env["DYLD_INSERT_LIBRARIES"] = ":".join(part for part in libraries if part)
    return env


def run_command(args: argparse.Namespace) -> int:
    profile = build.validate_build_options(args)
    if args.app not in profile.apps:
        available = ", ".join(profile.apps)
        raise die(f"application '{args.app}' is unavailable on {profile.name}. Available applications: {available}.")

    app = APPS[args.app]
    checker_env = None
    if args.main_thread_checker:
        if profile.name != "macos" or args.app != "appkit":
            raise die("--main-thread-checker is available only for the macOS appkit application.")
        checker_env = _main_thread_checker_environment()

    if not args.no_build:
        build.do_build(args, [app.target])

    if args.app == "winui":
        build_dir = Path(args.path) if getattr(args, "path", None) else builddir.winui_build_dir()
        configuration = "Debug" if args.flavor == "debug" else "Release"
        executable = builddir.executable(build_dir / app.executable / configuration / "Aobus")
        try:
            winui.require_runtime()
            winui.require_interactive_session()
        except RuntimeError as exc:
            raise die(str(exc)) from exc
    else:
        build_dir = (
            Path(args.path)
            if getattr(args, "path", None)
            else builddir.build_dir(args.flavor, clang=args.clang, asan=args.asan, tsan=args.tsan)
        )
        executable = builddir.executable(build_dir / app.executable)

    if not executable.exists():
        command = "ao.bat build --target winui" if args.app == "winui" else "./ao build"
        raise die(f"Executable not found at {executable}. Did you build the project? Run '{command}' first.")
    workspace_cache.validate_consumer(build_dir)

    if os.environ.get("AOBUS_WINDOWS_SOURCE_VIEW"):
        with subprocess.Popen([str(executable), *args.app_args]) as child:
            try:
                return child.wait()
            except KeyboardInterrupt as interrupt:
                # Retire the views only after this application has exited.
                # An interrupted build still retains its views for surviving workers.
                try:
                    child.kill()
                    child.wait()
                except OSError as exc:
                    interrupt.add_note(f"Could not reap the application: {exc}")
                    raise interrupt from exc
                return 130
    # Replaces the current process with the target executable
    arguments = [str(executable), *args.app_args]
    if checker_env is not None:
        os.execvpe(str(executable), arguments, checker_env)
    else:
        os.execvp(str(executable), arguments)
