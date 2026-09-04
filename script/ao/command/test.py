"""ao test — incrementally build and run registered development test suites."""

import argparse
import os
import random
import re
import shlex
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from collections.abc import Generator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Literal

from ..core import builddir, buildlock, linttest, proc, tooltest
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run
from . import build

HELP = "Build incrementally and run C++ and tooling test suites with optional Catch2 filters"
NAME = "test"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True

SHARD_ENV = "AOBUS_TEST_SHARDS"
# The core suite keeps getting faster past this point -- 34.0s to 23.7s between
# eight and sixteen shards on a sixteen-vCPU Windows guest, 8.6s to 5.9s between
# eight and thirty-one on a thirty-two core Linux host -- so the cap is not
# where the curve flattens. It is a bound on process count: unlike a compiler
# job, a shard is a whole test process with its own fixtures, temporary tree,
# and, for the GTK suite, its own X server. Sixteen keeps nearly all of the win
# without letting a large machine start a hundred of them for a suite of a few
# hundred tests. AOBUS_TEST_SHARDS overrides it in both directions.
SHARD_CAP = 16


def requires_build_environment(arguments: Sequence[str]) -> bool:
    """Return whether this invocation can build before running tests."""
    portal_arguments = arguments[: arguments.index("--")] if "--" in arguments else arguments
    return not any(argument in {"-n", "--no-build"} for argument in portal_arguments)


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
    profile = builddir.platform_profile()
    if tsan and not profile.tsan_suites:
        raise die("ThreadSanitizer is unavailable on this platform.")

    if tsan and selection in ("default", "all", "concurrency"):
        selection = "tsan"
    groups = suite_groups()
    suites = groups.get(selection, (selection,))
    unavailable = [suite for suite in suites if suite not in profile.all_suites]
    if unavailable:
        available = ", ".join(profile.all_suites)
        raise die(f"suite '{unavailable[0]}' is unavailable on this platform. Available suites: {available}.")
    return suites


def _start_xvfb() -> "subprocess.Popen[str]":
    try:
        return subprocess.Popen(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except FileNotFoundError as exc:
        raise die(
            "GTK tests require Xvfb. Enter the project nix-shell, or run through ./ao after shell.nix is updated."
        ) from exc


def _gtk_display_environment(display: str) -> dict[str, str]:
    # GTK may select its accessibility and input-method backends before the
    # test binary reaches main().  Set the complete headless profile on the
    # child process rather than relying on GtkTestMain's fallback defaults.
    return {
        "DISPLAY": display,
        "GTK_A11Y": "test",
        "GTK_IM_MODULE": "simple",
        "GDK_BACKEND": "x11",
        "GDK_DISABLE": "gl,vulkan",
        "GSK_RENDERER": "cairo",
    }


@contextmanager
def virtual_gtk_displays(count: int = 1) -> Generator[list[dict[str, str]], None, None]:
    """Start one Xvfb per GTK test process.

    Sharing one display across shards is not merely untidy, it is unreliable.
    Several tests present a window and then drain only the events already
    pending, so a popover that is still waiting on an X round trip has not been
    created yet when the assertion runs. One busy server serving eight clients
    made that race real: eight shards on a shared display failed 5 of 13 runs,
    and 0 of 14 with a display each.
    """
    servers: list[subprocess.Popen[str]] = []

    try:
        # Started inside the block: a failure on the third Xvfb must still shut
        # down the first two.
        for _ in range(count):
            servers.append(_start_xvfb())

        displays = []
        for server in servers:
            assert server.stdout is not None
            display_number = server.stdout.readline().strip()
            if not display_number:
                output = server.stdout.read()
                raise die(f"Xvfb failed to start.{(' Output: ' + output.strip()) if output.strip() else ''}")
            displays.append(f":{display_number}")

        print(f"GTK display{'s' if count > 1 else ''}: Xvfb {' '.join(displays)}")
        yield [_gtk_display_environment(display) for display in displays]
    finally:
        for server in servers:
            server.terminate()
        for server in servers:
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
    sanitizers.add_argument(
        "--asan", action="store_true", help="test the AddressSanitizer build tree (plus UBSan where available)"
    )
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


def shard_count(*, environ: Mapping[str, str] | None = None) -> int:
    """Return how many parallel shards one Catch2 suite runs."""
    environment = os.environ if environ is None else environ
    configured = environment.get(SHARD_ENV)
    if configured is None:
        return min(SHARD_CAP, max(1, (os.cpu_count() or 1) - 1))
    try:
        shards = int(configured)
    except ValueError:
        raise die(f"{SHARD_ENV} must be a positive integer.") from None
    if shards < 1:
        raise die(f"{SHARD_ENV} must be a positive integer.")
    return shards


def suite_shards(*, list_only: bool = False, repeat: int = 1, tsan: bool = False, concurrency: bool = False) -> int:
    """Return the shard count for one invocation, or 1 where sharding would distort it.

    Listing has nothing to parallelize. The other three exclusions are about
    meaning rather than speed: concurrency and repeat runs measure how the code
    behaves under contention, and TSan needs one process to own the machine, so
    filling every core with sibling test processes would change the very thing
    those runs exist to observe.
    """
    if list_only or repeat > 1 or tsan or concurrency:
        return 1
    return shard_count()


_OVERALL_KEYS = ("successes", "failures", "expectedFailures", "skips")


@dataclass
class _Shard:
    """One shard process, how it was started, and the files it writes."""

    index: int
    argv: Sequence[str]
    environment: Mapping[str, str]
    process: "subprocess.Popen[bytes]"
    sink: IO[bytes]
    output: Path
    report: Path


def _read_shard_totals(report: Path) -> tuple[dict[str, int], dict[str, int]] | None:
    """Return one shard's (assertion, test case) tallies, or None when unreadable."""
    try:
        root = ElementTree.parse(report).getroot()
    except (OSError, ElementTree.ParseError):
        return None

    assertions = root.find("OverallResults")
    cases = root.find("OverallResultsCases")
    if assertions is None or cases is None:
        return None

    try:
        return (
            {key: int(assertions.get(key, "0")) for key in _OVERALL_KEYS},
            {key: int(cases.get(key, "0")) for key in _OVERALL_KEYS},
        )
    except ValueError:
        return None


def _shard_summary(label: str, assertions: dict[str, int], cases: dict[str, int], shards: int) -> str:
    """Render the combined Catch2 tally the individual shards no longer print."""
    ran = cases["successes"] + cases["failures"] + cases["expectedFailures"]
    checks = assertions["successes"] + assertions["failures"] + assertions["expectedFailures"]
    parts = [f"{ran} test cases", f"{checks} assertions", f"{assertions['failures']} failed"]
    if cases["skips"]:
        parts.append(f"{cases['skips']} skipped")
    return f"{label}: {', '.join(parts)} across {shards} shards"


def _echo(text: bytes) -> None:
    sys.stdout.flush()
    sys.stdout.buffer.write(text)
    sys.stdout.buffer.flush()


def _shard_argv(command: Sequence[str], shard: int, shards: int, seed: int, report: Path) -> list[str]:
    return [
        *command,
        # Catch2 shards the run order, and that order defaults to random, so the
        # shards only partition the suite when every process draws the same one.
        # Left to their own seeds, eight core shards ran 1773 of 2682 tests --
        # 692 of them twice, 909 not at all -- while still summing to 2682 cases
        # and looking exactly like a complete run.
        "--rng-seed",
        str(seed),
        # A filter can match fewer tests than there are shards, which leaves the
        # trailing shards empty. Empty shards are expected here, so the caller
        # checks the combined tally instead of each process's own opinion.
        "--allow-running-no-tests",
        "--shard-count",
        str(shards),
        "--shard-index",
        str(shard),
        "--reporter",
        "console::out=-",
        "--reporter",
        f"xml::out={report}",
    ]


def _run_sharded(
    command: Sequence[str],
    *,
    label: str,
    environments: Sequence[dict[str, str]],
    log: Path | None,
    allow_no_tests: bool,
) -> int:
    """Run one Catch2 binary as parallel shards and report a single combined tally.

    Each shard also writes an XML report, so the totals are summed exactly rather
    than scraped back out of the console summaries. Every shard's console output
    reaches the log, which is where a post-mortem looks, but only failing shards
    are replayed to the terminal: eight passing summaries per suite would bury
    the one line a reader came for.
    """
    shards = len(environments)
    # Catch2 reserves 0 for "pick one for me", which is what the shards must not do.
    seed = random.randrange(1, 2**32)
    print(f"Randomness seeded to: {seed}")

    with tempfile.TemporaryDirectory(prefix="aobus-shard-") as directory:
        root = Path(directory)
        running: list[_Shard] = []
        sinks: list[IO[bytes]] = []
        try:
            for index, environment in enumerate(environments):
                report = root / f"shard-{index}.xml"
                output = root / f"shard-{index}.out"
                argv = _shard_argv(command, index, shards, seed, report)
                sink = output.open("wb")
                sinks.append(sink)
                process = subprocess.Popen(
                    argv,
                    cwd=PROJECT_ROOT,
                    env={**os.environ, **environment} if environment else None,
                    stdout=sink,
                    stderr=subprocess.STDOUT,
                )
                running.append(_Shard(index, argv, environment, process, sink, output, report))
        finally:
            # Also reached when a later Popen fails: the shards already started
            # must be collected before this function can report anything.
            for shard in running:
                shard.process.wait()
            # Closed from `sinks` rather than `running`, because the sink of a
            # failed Popen never reached `running`. Windows cannot remove a
            # directory holding an open handle, so leaving one open would fail
            # the cleanup of this TemporaryDirectory and replace the launch
            # exception with an unrelated WinError 32.
            for opened in sinks:
                opened.close()

        status = next((shard.process.returncode for shard in running if shard.process.returncode != 0), 0)

        assertions = dict.fromkeys(_OVERALL_KEYS, 0)
        cases = dict.fromkeys(_OVERALL_KEYS, 0)
        unreported = []
        for shard in running:
            _report_shard_output(shard, shards, log=log)
            totals = _read_shard_totals(shard.report)
            if totals is None:
                unreported.append(shard.index)
                continue
            for accumulator, shard_totals in zip((assertions, cases), totals, strict=True):
                for key in _OVERALL_KEYS:
                    accumulator[key] += shard_totals[key]

    if unreported:
        # A shard that produced no readable report cannot be counted, so say so
        # rather than print a total that silently omits the tests it ran.
        indices = ", ".join(str(index + 1) for index in unreported)
        print(f"{label}: no readable report from shard(s) {indices} of {shards}; the tally below is incomplete")
        status = status or 1

    print(_shard_summary(label, assertions, cases, shards))

    if not allow_no_tests and not unreported and not sum(cases.values()):
        raise die(f"no {label} tests matched the supplied filter.")
    return status


def _repro_command(shard: _Shard) -> str:
    """Return the command that reruns this shard.

    Printed rather than described because the reader would otherwise have to
    translate "shard 3 of 8" into Catch2's zero-based --shard-index 2. The
    reporter arguments are dropped: they only redirect output the rerun wants on
    the terminal anyway.

    The shard's environment is carried along, because part of it decides whether
    the failure reproduces at all rather than merely how the run is configured:
    on an ASan tree UBSAN_OPTIONS is what makes undefined behaviour halt instead
    of log and continue. DISPLAY is the deliberate exception -- it names an Xvfb
    that this run tears down on the way out, so the rerun picks up whatever
    display the caller has.
    """
    argv = list(shard.argv)
    kept = [
        argument
        for index, argument in enumerate(argv)
        if argument != "--reporter" and (index == 0 or argv[index - 1] != "--reporter")
    ]
    if builddir.platform_profile().name == "windows":
        # No prefix to add: the GTK suite is Linux-only and every sanitizer
        # helper returns nothing on Windows, so a Windows shard runs with the
        # inherited environment. cmd.exe would also not read the single quotes
        # shlex.join emits as quoting, and a Windows build path would come back
        # as a program named "'C:\...'".
        return subprocess.list2cmdline(kept)

    exported = {key: value for key, value in shard.environment.items() if key != "DISPLAY"}
    prefix = ["env", *(f"{key}={value}" for key, value in sorted(exported.items()))] if exported else []
    return shlex.join([*prefix, *kept])


def _report_shard_output(shard: _Shard, shards: int, *, log: Path | None) -> None:
    """Log every shard's output; echo only the shards that failed."""
    banner = f"--- shard {shard.index + 1} of {shards} (exit {shard.process.returncode}) ---\n".encode()
    with shard.output.open("rb") as source:
        lines = [line for line in source if not proc.is_suppressed_output(line)]

    if log is not None:
        with log.open("ab") as sink:
            sink.write(banner)
            sink.writelines(lines)

    if shard.process.returncode != 0:
        repro = f"rerun this shard: {_repro_command(shard)}\n".encode()
        _echo(banner + b"".join(lines) + repro)


_LSAN_SUPP_PATH = Path(__file__).resolve().parent.parent / "lsan.supp"
_TSAN_SUPP_PATH = Path(__file__).resolve().parent.parent / "tsan.supp"
_TSAN_MERGED_SUPP_PATH = Path("/tmp") / f"aobus-tsan-{os.getpid()}.supp"
_SANITIZER_OPTION_SEPARATOR = re.compile(r":(?=[A-Za-z_][A-Za-z0-9_]*=)")


def _lsan_env(build_dir: Path, *, enabled: bool = False) -> dict[str, str]:
    """Return Linux LeakSanitizer options when *build_dir* is an ASan tree."""
    if builddir.platform_profile().name != "linux" or (not enabled and "asan" not in build_dir.name):
        return {}
    return {"LSAN_OPTIONS": f"suppressions={_LSAN_SUPP_PATH}"}


def _split_sanitizer_options(value: str) -> list[str]:
    return [option for option in _SANITIZER_OPTION_SEPARATOR.split(value.strip(":")) if option]


def _macos_tui_asan_env(name: str, build_dir: Path, *, enabled: bool = False) -> dict[str, str]:
    """Return the macOS TUI container-annotation compatibility option."""
    if name != "tui" or builddir.platform_profile().name != "macos" or (not enabled and "asan" not in build_dir.name):
        return {}

    # The vcpkg FTXUI archive is not ASan-instrumented, while Aobus is. Both
    # instantiate libc++ deque operations over FTXUI's event queue, so mixed
    # container annotations produce a false container-overflow report. Keep the
    # suppression in this one process until both sides use matching instrumentation.
    retained_options = []
    for option in _split_sanitizer_options(os.environ.get("ASAN_OPTIONS", "")):
        key, _, _ = option.partition("=")
        if key != "detect_container_overflow":
            retained_options.append(option)

    return {"ASAN_OPTIONS": ":".join((*retained_options, "detect_container_overflow=0"))}


def _ubsan_env(build_dir: Path, *, enabled: bool = False) -> dict[str, str]:
    """Preserve caller options while making ASan/UBSan test trees fail closed."""
    if builddir.platform_profile().name == "windows" or (not enabled and "asan" not in build_dir.name):
        return {}

    retained_options = []
    for option in _split_sanitizer_options(os.environ.get("UBSAN_OPTIONS", "")):
        key, _, _ = option.partition("=")
        if key not in {"halt_on_error", "print_stacktrace"}:
            retained_options.append(option)

    required_options = ["halt_on_error=1", "print_stacktrace=1"]
    return {"UBSAN_OPTIONS": ":".join((*retained_options, *required_options))}


def _tsan_env(build_dir: Path, *, enabled: bool) -> dict[str, str]:
    """Apply dependency suppressions and fail on the first TSan report."""
    if not enabled and "tsan" not in build_dir.name:
        return {}

    existing_options = _split_sanitizer_options(os.environ.get("TSAN_OPTIONS", ""))
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
    asan: bool = False,
    tsan: bool = False,
    log: Path | None = None,
    shards: int = 1,
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

    sharded = shards > 1 and not list_only
    print("=====================================")
    print(f"Running {spec.label} Tests")
    print(f"CMD: {' '.join(command)}" + (f" (in {shards} shards)" if sharded else ""))
    print("=====================================")

    sanitizer_env = {
        **_macos_tui_asan_env(name, build_dir, enabled=asan),
        **_lsan_env(build_dir, enabled=asan),
        **_ubsan_env(build_dir, enabled=asan),
        **_tsan_env(build_dir, enabled=tsan),
    }

    def execute(environments: list[dict[str, str]]) -> int:
        if sharded:
            return _run_sharded(
                command, label=spec.label, environments=environments, log=log, allow_no_tests=allow_no_tests
            )
        return run(command, env=environments[0] or None, log=log, append=log is not None)

    if name == "gtk" and not list_only:
        with virtual_gtk_displays(shards if sharded else 1) as displays:
            return execute([{**sanitizer_env, **display} for display in displays])

    return execute([sanitizer_env] * (shards if sharded else 1))


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
    asan: bool = False,
    tsan: bool = False,
    concurrency: bool = False,
    log: Path | None = None,
) -> int:
    shards = suite_shards(list_only=list_only, repeat=repeat, tsan=tsan, concurrency=concurrency)
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
                    asan=asan,
                    tsan=tsan,
                    log=log,
                    shards=shards,
                )
                if spec.kind == "catch2"
                else run_non_catch2_suite(name, build_dir, list_only=list_only, log=log)
            )
            if status != 0:
                return status

    return 0


def run_command(args: argparse.Namespace) -> int:
    build.validate_build_options(args)
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
            build_cmd += build.parallel_build_arguments()
            build_cmd += ["--target", *targets]
            with buildlock.build_tree_lock(build_dir):
                if run(build_cmd) != 0:
                    raise die("test build failed.")

    options = {
        "test_filter": test_filter,
        "list_only": args.list,
        "repeat": args.repeat,
        "asan": args.asan,
        "tsan": args.tsan,
    }
    if args.suite == "concurrency":
        options["allow_no_tests"] = True
        options["concurrency"] = True
    return run_suites(suites, build_dir, **options)
