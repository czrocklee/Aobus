"""ao test — incrementally build and run registered development test suites."""

import argparse
import subprocess
from collections.abc import Generator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from ..core import builddir, linttest, tooltest
from ..core.proc import die, run

HELP = "Build incrementally and run C++ and tooling test suites with optional Catch2 filters"

EPILOG = """\
Pass any valid Catch2 filter string as the last argument. Quote it to avoid shell
globbing, e.g. "[layout],[model]" (OR logic) or "[audio][backend]" (AND logic).
Filters and --list apply to Catch2 suites; non-Catch2 suites report their suite name.

examples:
  ./ao test                          # build and run the default core + GTK suites
  ./ao test --all                    # build and run every suite
  ./ao test -n                       # run the default suites without building
  ./ao test --gtk "[layout]"         # GTK tests matching [layout]
  ./ao test --core "[audio][backend]"
  ./ao test --tooling                # test the ao development tooling
  ./ao test --gtk --list "[layout]"  # list matching GTK tests
"""


@dataclass(frozen=True)
class SuiteSpec:
    label: str
    kind: Literal["catch2", "tooling", "lint"]
    target: str | None = None


SUITES = {
    "core": SuiteSpec("Core", "catch2", "ao_core_test"),
    "gtk": SuiteSpec("GTK", "catch2", "ao_gtk_test"),
    "integration": SuiteSpec("Integration", "catch2", "ao_integration_test"),
    "fleet": SuiteSpec("Fleet", "catch2", "ao_fleet_test"),
    "tooling": SuiteSpec("Tooling Tests", "tooling"),
    "lint": SuiteSpec("Lint Integration", "lint", "AobusLintPlugin"),
}

SUITE_TARGETS = {
    name: [spec.target] for name, spec in SUITES.items() if spec.kind == "catch2" and spec.target is not None
}

SUITE_GROUPS = {
    "default": ("core", "gtk"),
    "all": ("core", "gtk", "integration", "fleet", "tooling", "lint"),
}


@contextmanager
def virtual_gtk_display() -> Generator[dict[str, str], None, None]:
    try:
        server = subprocess.Popen(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except FileNotFoundError as exc:
        raise die(
            "GTK tests require Xvfb. Enter the project nix-shell, or run through ./ao after shell.nix is updated."
        ) from exc

    try:
        assert server.stdout is not None
        display_number = server.stdout.readline().strip()
        if not display_number:
            output = server.stdout.read()
            raise die(f"Xvfb failed to start.{(' Output: ' + output.strip()) if output.strip() else ''}")

        display = f":{display_number}"
        print(f"GTK display: Xvfb {display}")
        yield {"DISPLAY": display, "GDK_BACKEND": "x11", "GDK_DISABLE": "gl,vulkan", "GSK_RENDERER": "cairo"}
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "test", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("filter", nargs="?", default="", help="Catch2 test filter")
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument(
        "--suite",
        choices=(*SUITES, *SUITE_GROUPS),
        default="default",
        help="test suite or group (default: default)",
    )
    suite.add_argument("--core", dest="suite", action="store_const", const="core", help="shortcut for --suite core")
    suite.add_argument("--gtk", dest="suite", action="store_const", const="gtk", help="shortcut for --suite gtk")
    suite.add_argument(
        "--integration",
        dest="suite",
        action="store_const",
        const="integration",
        help="shortcut for --suite integration",
    )
    suite.add_argument("--fleet", dest="suite", action="store_const", const="fleet", help="shortcut for --suite fleet")
    suite.add_argument(
        "--tooling", dest="suite", action="store_const", const="tooling", help="shortcut for --suite tooling"
    )
    suite.add_argument("--lint", dest="suite", action="store_const", const="lint", help="shortcut for --suite lint")
    suite.add_argument("--default", dest="suite", action="store_const", const="default", help="run core and GTK suites")
    suite.add_argument("--all", dest="suite", action="store_const", const="all", help="run every suite")
    parser.add_argument("-p", "--path", metavar="<dir>", help="build directory (default: /tmp/build/debug)")
    parser.add_argument("--clang", action="store_true", help="test the clang build tree")
    sanitizers = parser.add_mutually_exclusive_group()
    sanitizers.add_argument("--asan", action="store_true", help="test the ASan/UBSan build tree")
    sanitizers.add_argument("--tsan", action="store_true", help="test the TSan build tree")
    parser.add_argument("-l", "--list", action="store_true", help="list matching tests instead of running them")
    parser.add_argument("-n", "--no-build", action="store_true", help="skip the incremental build")
    parser.set_defaults(func=run_command)


def run_suite(
    name: str,
    build_dir: Path,
    *,
    test_filter: str = "",
    list_only: bool = False,
    log: Path | None = None,
) -> int:
    spec = SUITES[name]
    if spec.kind != "catch2" or spec.target is None:
        raise ValueError(f"{name} is not a Catch2 suite")

    binary = build_dir / "test" / spec.target
    if not binary.is_file():
        raise die(f"{name} test binary not found at {binary}. Build first, e.g. with ./ao build.")

    command = [str(binary)]
    if list_only:
        command += ["--list-tests", "--verbosity", "high"]
    if test_filter:
        command.append(test_filter)

    print("=====================================")
    print(f"Running {spec.label} Tests")
    print(f"CMD: {' '.join(command)}")
    print("=====================================")

    if name == "gtk" and not list_only:
        with virtual_gtk_display() as env:
            return run(command, env=env, log=log, append=log is not None)

    return run(command, log=log, append=log is not None)


def run_non_catch2_suite(name: str, build_dir: Path, *, list_only: bool = False, log: Path | None = None) -> int:
    spec = SUITES[name]
    print("=====================================")
    print(f"{'Listing' if list_only else 'Running'} {spec.label}")
    print("=====================================")

    if list_only:
        print("This suite does not expose Catch2 test cases.")
        return 0

    if spec.kind == "tooling":
        return tooltest.run(log=log)

    if spec.kind == "lint":
        return linttest.run(build_dir, log=log)

    raise ValueError(f"unknown suite kind: {spec.kind}")


def run_suites(
    suites: tuple[str, ...],
    build_dir: Path,
    *,
    test_filter: str = "",
    list_only: bool = False,
    log: Path | None = None,
) -> int:
    for index, name in enumerate(suites):
        if index:
            print()

        spec = SUITES[name]
        status = (
            run_suite(name, build_dir, test_filter=test_filter, list_only=list_only, log=log)
            if spec.kind == "catch2"
            else run_non_catch2_suite(name, build_dir, list_only=list_only, log=log)
        )
        if status != 0:
            return status

    return 0


def run_command(args: argparse.Namespace) -> int:
    build_dir = (
        Path(args.path) if args.path else builddir.build_dir("debug", clang=args.clang, asan=args.asan, tsan=args.tsan)
    )

    suites = SUITE_GROUPS.get(args.suite, (args.suite,))

    if not args.no_build:
        targets = [target for suite in suites if (target := SUITES[suite].target) is not None]
        if targets:
            if not build_dir.is_dir():
                raise die(f"build directory {build_dir} does not exist. Run ./ao build first to configure the project.")
            print("=====================================")
            print(f"Building {', '.join(targets)} in {build_dir}...")
            print("=====================================")
            if run(["cmake", "--build", str(build_dir), "--parallel", "--target", *targets]) != 0:
                raise die("test build failed.")

    return run_suites(suites, build_dir, test_filter=args.filter, list_only=args.list)
