"""ao run — launch configured native products with their declared runfiles."""

import argparse
import os
import subprocess
from collections.abc import Generator
from contextlib import ExitStack, contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path

from ..core import bazel_workflows, builddir, phases, winui
from ..core.bazel_native import BazelWorkspaceError
from ..core.bazel_workflows import ExecutableArtifact
from ..core.proc import die

HELP = "Build and run a native application or an explicit executable target"
NAME = "run"
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = False


def requires_build_environment(args: argparse.Namespace) -> bool:
    """Even --no-build needs positive native SDK/host analysis admission."""
    return True


@dataclass(frozen=True)
class AppSpec:
    target: str


APPS = {
    "appkit": AppSpec("//app/macos-appkit:aobus-appkit"),
    "cli": AppSpec("//app/cli:aobus"),
    "tui": AppSpec("//app/tui:aobus-tui"),
    "gtk": AppSpec("//app/linux-gtk:aobus-gtk"),
    "winui": AppSpec("//app/windows-winui:aobus-winui"),
}

EPILOG = """\
examples:
  ./ao run cli                       # build and run the debug CLI
  ./ao run tui release               # use the governed optimized build
  ./ao run cli --no-build            # analyze and launch existing outputs only
  ./ao run appkit --main-thread-checker  # diagnostic; requires full Xcode
  ./ao run //test:utility_test        # build and run an explicit executable
  ./ao run tui -- --library ~/Music  # preserve application argv after --

--path (or BUILD_DIR) selects a Bazel tree outside the checkout; old build trees
are not executable search paths. --no-build never builds missing outputs.
WinUI requires an interactive desktop and the admitted Windows App Runtime.
"""


def _application(value: str) -> str:
    apps = builddir.platform_profile().apps
    if value in apps or value.startswith("//"):
        return value
    raise argparse.ArgumentTypeError(f"choose an application ({', '.join(apps)}) or a //package:target label")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    profile = builddir.platform_profile()
    parser = subparsers.add_parser(
        NAME,
        help=HELP,
        description=HELP,
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("app", type=_application, help=f"application ({', '.join(profile.apps)}) or executable label")
    bazel_workflows.add_arguments(parser)
    parser.add_argument(
        "-n", "--no-build", action="store_true", help="analyze and run existing outputs without building"
    )
    parser.add_argument(
        "--main-thread-checker",
        action="store_true",
        help="inject Xcode's Main Thread Checker into the macOS AppKit application (diagnostic only)",
    )
    parser.add_argument("app_args", nargs="*", help="application arguments; put option flags after --")
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


@contextmanager
def _prepare_launch(args: argparse.Namespace) -> Generator[tuple[ExecutableArtifact, dict[str, str]], None, None]:
    _config, host = bazel_workflows.validate_request(args)
    profile = builddir.platform_profile()
    target = APPS[args.app].target if args.app in APPS else args.app
    app = next((name for name, spec in APPS.items() if spec.target == target), None)
    if (app is not None and app not in profile.apps) or (app is None and not target.startswith("//")):
        available = ", ".join(profile.apps)
        raise die(f"application '{args.app}' is unavailable on {host}. Available applications: {available}.")
    if app == "gtk" and getattr(args, "gtk", None) in (False, "off", "false"):
        raise die("the GTK application is disabled by --gtk=off.")
    checker_env = None
    if args.main_thread_checker:
        if host != "macos" or app != "appkit":
            raise die("--main-thread-checker is available only for the macOS appkit application.")
        checker_env = _main_thread_checker_environment()
    if app == "winui":
        try:
            winui.require_runtime()
            winui.require_interactive_session()
        except RuntimeError as exc:
            raise die(str(exc)) from exc
    with bazel_workflows.prepare_artifacts(args, [target], no_build=args.no_build) as artifacts:
        (artifact,) = artifacts
        environment = {**(checker_env if checker_env is not None else os.environ), **artifact.environment()}
        yield artifact, environment


def _run_application(command: list[str], environment: dict[str, str]) -> int:
    """Settle the returned child before its caller releases configured artifacts."""
    child: subprocess.Popen[bytes] | None = None
    try:
        # Install ownership handling before Popen returns, not after launch.
        # Popen itself owns a construction failure before it returns a handle.
        child = subprocess.Popen(command, env=environment)
        return child.wait()
    except BaseException as original:
        if child is None:
            raise
        # Repeated Ctrl-C must not release the artifacts before the child is reaped.
        while True:
            try:
                with suppress(ProcessLookupError):
                    child.kill()
                child.wait()
                break
            except (KeyboardInterrupt, InterruptedError):
                continue
        if isinstance(original, KeyboardInterrupt):
            return 130
        raise


def run_command(args: argparse.Namespace) -> int:
    try:
        # Supervision retains the artifact lock until exit without capturing
        # stdin/stdout/stderr. Terminal frontends keep the caller's real terminal,
        # and space-bearing argv is not reparsed through a shell or exec-on-CRT.
        with ExitStack() as launch:
            with phases.measure("launch-preparation", command=NAME):
                artifact, environment = launch.enter_context(_prepare_launch(args))
            with phases.measure("application", command=NAME) as phase:
                phase.status = _run_application([str(artifact.executable), *args.app_args], environment)
                return phase.status
    except BazelWorkspaceError as error:
        raise die(str(error)) from error
