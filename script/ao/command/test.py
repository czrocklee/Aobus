"""ao test — incrementally build and run registered development test suites."""

import argparse
import os
import subprocess
from collections.abc import Generator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from ..core import builddir, linttest, tooltest
from ..core.proc import die, run

HELP = "Build incrementally and run C++ and tooling test suites with optional Catch2 filters"
NAME = "test"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


EPILOG = """\
Pass any valid Catch2 filter string as the last argument. Quote it to avoid shell
globbing, e.g. "[layout],[model]" (OR logic) or "[audio][backend]" (AND logic).
Filters and --list apply to Catch2 suites; non-Catch2 suites report their suite name.

examples:
  ./ao test                          # build and run the native default suites
  ./ao test --all                    # build and run every native suite
  ./ao test -n                       # run the default suites without building
  ./ao test --core "[audio][backend]"
  ./ao test --tui "[layout]"
  ./ao test --tooling                # test the ao development tooling
  ./ao test --core --list "[audio]"  # list matching core tests
  ./ao test --concurrency             # run concurrency contracts across Catch2 suites
  ./ao test --tsan --repeat 20        # repeat the TSan-safe suite group
"""


@dataclass(frozen=True)
class SuiteSpec:
    label: str
    kind: Literal["catch2", "tooling", "lint"]
    target: str | None = None


SUITES = {
    "core": SuiteSpec("Core", "catch2", "ao_core_test"),
    "tui": SuiteSpec("TUI", "catch2", "ao_tui_test"),
    "cli": SuiteSpec("CLI", "catch2", "ao_cli_test"),
    "gtk": SuiteSpec("GTK", "catch2", "ao_gtk_test"),
    "integration": SuiteSpec("Integration", "catch2", "ao_integration_test"),
    "council": SuiteSpec("Council", "catch2", "ao_council_test"),
    "tooling": SuiteSpec("Tooling Tests", "tooling"),
    "lint": SuiteSpec("Lint Integration", "lint", "AobusLintPlugin"),
}

SUITE_TARGETS = {
    name: [spec.target] for name, spec in SUITES.items() if spec.kind == "catch2" and spec.target is not None
}

SUITE_GROUPS = {
    "default": builddir.LINUX_PROFILE.default_suites,
    "all": builddir.LINUX_PROFILE.all_suites,
    "tsan": builddir.LINUX_PROFILE.tsan_suites,
    "concurrency": tuple(name for name, spec in SUITES.items() if spec.kind == "catch2"),
}


def suite_groups() -> dict[str, tuple[str, ...]]:
    """Return suite groups containing only targets enabled by the native profile."""
    profile = builddir.platform_profile()
    catch2_suites = tuple(name for name in profile.all_suites if SUITES[name].kind == "catch2")
    return {
        "default": profile.default_suites,
        "all": profile.all_suites,
        "tsan": profile.tsan_suites,
        "concurrency": catch2_suites,
    }


def suites_for(selection: str, *, tsan: bool = False) -> tuple[str, ...]:
    if tsan and selection in ("default", "all", "concurrency"):
        selection = "tsan"
    groups = suite_groups()
    suites = groups.get(selection, (selection,))
    if not suites:
        raise die("ThreadSanitizer suites are unavailable on this platform.")
    unavailable = [suite for suite in suites if suite not in builddir.platform_profile().all_suites]
    if unavailable:
        available = ", ".join(builddir.platform_profile().all_suites)
        raise die(f"suite '{unavailable[0]}' is unavailable on this platform. Available suites: {available}.")
    return suites


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
    profile = builddir.platform_profile()
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("filter", nargs="?", default="", help="Catch2 test filter")
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument(
        "--suite",
        choices=(*profile.all_suites, *SUITE_GROUPS),
        default="default",
        help="test suite or group (default: default)",
    )
    for name in profile.all_suites:
        suite.add_argument(
            f"--{name}",
            dest="suite",
            action="store_const",
            const=name,
            help=f"shortcut for --suite {name}",
        )
    suite.add_argument(
        "--default",
        dest="suite",
        action="store_const",
        const="default",
        help=f"run {', '.join(profile.default_suites)} suites",
    )
    suite.add_argument("--all", dest="suite", action="store_const", const="all", help="run every native suite")
    suite.add_argument(
        "--concurrency",
        dest="suite",
        action="store_const",
        const="concurrency",
        help="run [concurrency] tests across every native Catch2 suite",
    )
    parser.add_argument("-p", "--path", metavar="<dir>", help="override the native test build directory")
    parser.add_argument("--clang", action="store_true", help="test the clang build tree")
    sanitizers = parser.add_mutually_exclusive_group()
    sanitizers.add_argument("--asan", action="store_true", help="test the ASan/UBSan build tree")
    sanitizers.add_argument("--tsan", action="store_true", help="test the TSan build tree")
    parser.add_argument("-l", "--list", action="store_true", help="list matching tests instead of running them")
    parser.add_argument("-n", "--no-build", action="store_true", help="skip the incremental build")
    parser.add_argument(
        "--repeat",
        type=_positive_int,
        default=1,
        metavar="N",
        help="repeat selected tests N times and stop on the first failure",
    )
    parser.set_defaults(func=run_command)


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("repeat count must be at least 1")
    return parsed


_LSAN_SUPPRESSIONS = """\
# Known leaks in third-party libraries (fontconfig, pango, gtk, etc.)
# These are one-time allocations or internal caches not explicitly freed on exit.

leak:libfontconfig.so
leak:libfontconfig
leak:FcValueSave
leak:FcPatternObjectAddWithBinding

leak:libpango-1.0.so
leak:libpangocairo-1.0.so
leak:libpangoft2-1.0.so
leak:libpango
leak:pango_font_map_load_fontset
leak:pango_cairo_fc_font_map_fontset_key_substitute

leak:libgtk-4.so
leak:libgdk-4.so
leak:gtk_widget_realize
leak:gtk_text_get_scroll_limits

leak:libglib-2.0.so
leak:libgobject-2.0.so
leak:g_signal_emit
"""

_LSAN_SUPP_PATH = Path("/tmp/aobus-lsan.supp")
_TSAN_SUPP_PATH = Path(__file__).resolve().parent.parent / "tsan.supp"
_TSAN_MERGED_SUPP_PATH = Path("/tmp") / f"aobus-tsan-{os.getpid()}.supp"


def _lsan_env(build_dir: Path) -> dict[str, str]:
    """Return LSAN_OPTIONS env when *build_dir* is an ASan tree."""
    if "asan" not in build_dir.name:
        return {}
    _LSAN_SUPP_PATH.write_text(_LSAN_SUPPRESSIONS)
    return {"LSAN_OPTIONS": f"suppressions={_LSAN_SUPP_PATH}"}


def _tsan_env(build_dir: Path, *, enabled: bool) -> dict[str, str]:
    """Apply dependency suppressions and fail on the first TSan report."""
    if not enabled and "tsan" not in build_dir.name:
        return {}

    existing_options = [option for option in os.environ.get("TSAN_OPTIONS", "").strip(":").split(":") if option]
    suppression_paths: list[Path] = []
    retained_options: list[str] = []

    for option in existing_options:
        key, separator, value = option.partition("=")
        if key != "suppressions":
            retained_options.append(option)
            continue
        if not separator or not value:
            raise die("TSAN_OPTIONS contains an empty suppressions path.")
        suppression_paths.append(Path(value))

    suppression_path = _TSAN_SUPP_PATH
    if suppression_paths:
        contents: list[str] = []
        for path in (*suppression_paths, _TSAN_SUPP_PATH):
            try:
                contents.append(path.read_text(encoding="utf-8").rstrip())
            except OSError as exc:
                raise die(f"cannot read TSan suppression file {path}: {exc}") from exc

        try:
            _TSAN_MERGED_SUPP_PATH.write_text("\n\n".join(contents) + "\n", encoding="utf-8")
        except OSError as exc:
            raise die(f"cannot write merged TSan suppression file {_TSAN_MERGED_SUPP_PATH}: {exc}") from exc
        suppression_path = _TSAN_MERGED_SUPP_PATH

    required_options = [f"suppressions={suppression_path}", "halt_on_error=1", "second_deadlock_stack=1"]
    return {"TSAN_OPTIONS": ":".join((*retained_options, *required_options))}


def run_suite(
    name: str,
    build_dir: Path,
    *,
    test_filter: str = "",
    list_only: bool = False,
    allow_no_tests: bool = False,
    tsan: bool = False,
    log: Path | None = None,
) -> int:
    spec = SUITES[name]
    if spec.kind != "catch2" or spec.target is None:
        raise ValueError(f"{name} is not a Catch2 suite")

    binary = builddir.executable(build_dir / "test" / spec.target)
    if not binary.is_file():
        raise die(f"{name} test binary not found at {binary}. Build first, e.g. with ./ao build.")

    command = [str(binary)]
    if list_only:
        command += ["--list-tests", "--verbosity", "high"]
    if allow_no_tests:
        command.append("--allow-running-no-tests")
    if test_filter:
        command.append(test_filter)

    print("=====================================")
    print(f"Running {spec.label} Tests")
    print(f"CMD: {' '.join(command)}")
    print("=====================================")

    sanitizer_env = {**_lsan_env(build_dir), **_tsan_env(build_dir, enabled=tsan)}

    if name == "gtk" and not list_only:
        with virtual_gtk_display() as env:
            return run(command, env={**sanitizer_env, **env}, log=log, append=log is not None)

    return run(command, env=sanitizer_env or None, log=log, append=log is not None)


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
    allow_no_tests: bool = False,
    repeat: int = 1,
    tsan: bool = False,
    log: Path | None = None,
) -> int:
    iterations = 1 if list_only else repeat
    for iteration in range(iterations):
        if iterations > 1:
            print(f"Concurrency/stress repetition {iteration + 1}/{iterations}")

        for index, name in enumerate(suites):
            if index:
                print()

            spec = SUITES[name]
            status = (
                run_suite(
                    name,
                    build_dir,
                    test_filter=test_filter,
                    list_only=list_only,
                    allow_no_tests=allow_no_tests,
                    tsan=tsan,
                    log=log,
                )
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

    if args.suite == "concurrency" and args.filter:
        raise die("--concurrency supplies the [concurrency] filter; do not also pass a positional filter.")

    suites = suites_for(args.suite, tsan=args.tsan)
    test_filter = "[concurrency]" if args.suite == "concurrency" else args.filter

    if not args.no_build:
        targets = [target for suite in suites if (target := SUITES[suite].target) is not None]
        if targets:
            if not build_dir.is_dir():
                raise die(f"build directory {build_dir} does not exist. Run ./ao build first to configure the project.")
            print("=====================================")
            print(f"Building {', '.join(targets)} in {build_dir}...")
            print("=====================================")
            build_cmd = ["cmake", "--build", str(build_dir)]
            if getattr(args, "asan", False) or getattr(args, "tsan", False):
                jobs = max(1, (os.cpu_count() or 1) // 2)
                build_cmd += ["--parallel", str(jobs)]
            else:
                build_cmd.append("--parallel")
            build_cmd += ["--target", *targets]
            if run(build_cmd) != 0:
                raise die("test build failed.")

    options = {
        "test_filter": test_filter,
        "list_only": args.list,
        "repeat": args.repeat,
        "tsan": args.tsan,
    }
    if args.suite == "concurrency":
        options["allow_no_tests"] = True
    return run_suites(suites, build_dir, **options)
