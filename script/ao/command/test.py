"""ao test — incrementally build and run registered development test suites."""

import argparse
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ElementTree
from collections.abc import Generator, Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any
from urllib.parse import quote

from ..core import (
    appkitprocess,
    bazel_workflows,
    builddir,
    jobs,
    phases,
    proc,
    tooltest,
    winui,
)
from ..core.bazel_native import BazelWorkspaceError
from ..core.bazel_workflows import ExecutableArtifact
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run

# The suite registry is dependency-light metadata shared with the core; the
# explicit re-export preserves the public ``ao.command.test`` API and identity.
from ..core.test_suites import SUITES as SUITES
from ..core.test_suites import SuiteSpec as SuiteSpec

HELP = "Build incrementally and run C++ and tooling test suites with optional Catch2 filters"
NAME = "test"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


def requires_python_tools(args: argparse.Namespace) -> bool:
    """Tooling is SDK-free but uses the managed Python test dependencies."""
    return "tooling" in suites_for(args.suite, tsan=getattr(args, "tsan", False))


REQUIRES_PYTHON_TOOLS = requires_python_tools

# Preserve these public command constants while sharing the CI export policy.
SHARD_ENV = jobs.SHARD_ENV
SHARD_CAP = jobs.SHARD_CAP


def requires_build_environment(args: argparse.Namespace) -> bool:
    """Native --no-build still needs positive configured SDK admission."""
    suites = suites_for(args.suite, tsan=getattr(args, "tsan", False))
    suites = tuple(name for name in suites if not args.filter or SUITES[name].kind == "catch2")
    if args.list and all(SUITES[name].kind in ("tooling", "lint") for name in suites):
        return False
    return any(SUITES[suite].kind != "tooling" for suite in suites)


EPILOG = """\
Pass any valid Catch2 filter string as the last argument. Quote it to avoid shell
globbing, e.g. "DecoderFactory - *,PlaybackSession - *" (name alternatives).
Commas separate alternatives even inside shell quotes. Escape literal commas as
\\, in the quoted filter, e.g. "FieldCatalog - finds descriptors by canonical name\\, alias\\, and field".
Project tags are [concurrency] and [.manual]; --concurrency excludes hidden probes.
An explicit filter applies only to Catch2 suites; selected tooling and lint suites
are skipped. Across a suite group, individual suites may have no match, but at
least one test must match globally. Tests that match and then skip still count as
matches. Without a filter, --list reports non-Catch2 suite names.

examples:
  ./ao test                          # build and run the native default suites
  ./ao test --all                    # build and run every automatic native suite
  ./ao test -n                       # run the default suites without building
  ./ao test --core "DecoderFactory - *"
  ./ao test --tui "Render - *"
  ./ao test --tooling                # test the ao development tooling
  ./ao test --core --list "DecoderFactory - *"
  ./ao test --concurrency             # run concurrency contracts across Catch2 suites
  ./ao test --tsan --repeat 20        # repeat the TSan-safe suite group
"""

APPKIT_EPILOG = """\
  ./ao test --appkit --scenario desktop --library /tmp/music --state-root /tmp/aobus-appkit-state
"""

APPKIT_SCENARIOS = (
    "desktop",
    "authoring",
    "presentation",
    "media",
    "credits-controls",
    "credits-previews",
    "quality-presentation",
)
APPKIT_SUCCESSOR_TIMEOUT_SECONDS = 60.0
APPKIT_PARENT_TIMEOUT_SECONDS = 120.0
APPKIT_PARENT_TERMINATE_SECONDS = 5.0
APPKIT_RUN_ID_ENV = "AOBUS_APPKIT_TEST_RUN_ID"


def selectable_suites() -> tuple[str, ...]:
    """Return individually selectable suites for the active native profile."""
    profile = builddir.platform_profile()
    if profile.name == "macos":
        return (*profile.all_suites, "appkit")
    if profile.name == "windows":
        # The compiled-but-hidden backend suite stays explicit-only on Windows,
        # exactly like the WinUI audit: neither joins an unattended group.
        return (*profile.all_suites, "backend", "winui")
    return profile.all_suites


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
    available_suites = selectable_suites()
    unavailable = [suite for suite in suites if suite not in available_suites]
    if unavailable:
        available = ", ".join(available_suites)
        raise die(f"suite '{unavailable[0]}' is unavailable on this platform. Available suites: {available}.")
    return suites


def configured_suites_for(
    selection: str, build_dir: Path, *, tsan: bool = False, args: argparse.Namespace | None = None
) -> tuple[str, ...]:
    """Apply explicit graph features; configured queries admit the actual targets.

    The path parameter remains for runtime callers, but old tree caches and
    leftover executables are not configuration authorities.
    """
    suites = suites_for(selection, tsan=tsan)
    options = args if args is not None else argparse.Namespace()
    bazel_workflows.feature_options(options)
    disabled = tuple(
        suite
        for suite, option in (("gtk", "gtk"), ("systemmedia", "system_media"))
        if getattr(options, option, None) in (False, "off", "false") and suite in suites
    )
    if selection in disabled:
        raise die(f"suite '{selection}' is disabled by the requested Bazel graph features in {build_dir}.")
    return tuple(suite for suite in suites if suite not in disabled)


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


def _start_gtk_bus(config: Path) -> "subprocess.Popen[str]":
    try:
        return subprocess.Popen(
            ["dbus-daemon", "--nofork", f"--config-file={config}", "--print-address=1"],
            stdout=subprocess.PIPE,
            # Preserve startup diagnostics in the portal log without an unread pipe.
            stderr=None,
            text=True,
        )
    except FileNotFoundError as exc:
        raise die("GTK tests require dbus-daemon. Enter the project nix-shell.") from exc


def _gtk_daemon_ready(server: "subprocess.Popen[str]", label: str, *, timeout: float = 5) -> str:
    """Read a single startup handshake without retaining an unread pipe."""
    assert server.stdout is not None
    # Both daemons stay in the foreground and write only their startup address
    # to stdout. Killing a silent daemon closes that pipe before joining its
    # reader; stderr is separate so later diagnostics cannot fill the pipe.
    with ThreadPoolExecutor(max_workers=1) as reader:
        try:
            ready = reader.submit(server.stdout.readline)
            line: str = ready.result(timeout=timeout)
        except BaseException as exc:
            # Interruption must also retire the writer before close/join can
            # wait for the buffered reader's lock. Preserve non-timeout errors.
            server.kill()
            server.wait()
            if isinstance(exc, TimeoutError):
                raise die(f"{label} did not report readiness within {timeout:g} seconds.") from exc
            raise
        finally:
            server.stdout.close()
    return line.strip()


def _gtk_display_environment(display: str) -> dict[str, str]:
    # GTK may select its accessibility and input-method backends before the
    # test binary reaches main().  Set the complete headless profile on the
    # child process rather than relying on GtkTestMain's fallback defaults.
    return {
        "DISPLAY": display,
        "AOBUS_OWNED_GTK_DISPLAY": "1",
        "GTK_A11Y": "test",
        "GTK_IM_MODULE": "simple",
        "GDK_BACKEND": "x11",
        "GDK_DISABLE": "gl,vulkan",
        "GSK_RENDERER": "cairo",
        "G_DEBUG": f"{os.environ.get('G_DEBUG', '')},fatal-warnings".lstrip(","),
    }


@contextmanager
def _private_bus_directory(ipc_root: Path | None) -> Generator[Path, None, None]:
    if ipc_root is None:
        # Linux Unix socket names cannot accommodate an arbitrarily deep TMPDIR
        # (notably nested nix-shell directories). Keep only this private IPC root
        # short; do not change the caller's temporary-file or build environment.
        with tempfile.TemporaryDirectory(
            prefix="aobus-gtk-bus-", dir="/tmp" if sys.platform == "linux" else None
        ) as directory:
            yield Path(directory)
        return
    from ..core.bazel_test_services import recreate_private_root

    recreate_private_root(ipc_root)
    try:
        yield ipc_root
    finally:
        shutil.rmtree(ipc_root, ignore_errors=True)


@contextmanager
def virtual_gtk_displays(
    count: int = 1, *, ipc_root: Path | None = None
) -> Generator[list[dict[str, str]], None, None]:
    """Own one Xvfb and private session bus per GTK test process.

    Sharing one display across shards is unreliable: popovers can still be
    waiting for an X round trip when assertions run. Sharing the session bus
    also makes independent MPRIS tests compete for one canonical name. The
    portal owns both daemons beyond the test process, including GTK's retained
    process-global bus clients, and never exposes the caller's desktop bus.

    A stable `ipc_root` gives each bus a fixed socket and a guid-free address,
    so a Bazel caller's hashed test environment repeats across attempts.
    """
    servers: list[subprocess.Popen[str]] = []
    with _private_bus_directory(ipc_root) as directory:
        try:
            # Partial startup must still retire every already-started daemon.
            for _ in range(count):
                servers.append(_start_xvfb())

            displays = []
            for server in servers:
                display_number = _gtk_daemon_ready(server, "Xvfb")
                if not display_number.isdecimal():
                    raise die(f"Xvfb failed to start. Output: {display_number}")
                displays.append(f":{display_number}")

            def session_config(index: int) -> tuple[Path, str | None]:
                if ipc_root is None:
                    listen, address = f"unix:tmpdir={quote(directory.as_posix(), safe='/')}", None
                else:
                    address = f"unix:path={quote((directory / f'bus-{index}').as_posix(), safe='/')}"
                    listen = address
                config = directory / f"session-{index}.conf"
                # No service directories: tests must not activate host desktop
                # services or their mounts on this isolated bus.
                config.write_text(
                    f"<busconfig><type>session</type><listen>{listen}</listen>"
                    '<policy context="default"><allow send_destination="*"/>'
                    '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>',
                    encoding="utf-8",
                )
                return config, address

            environments = []
            for index, display in enumerate(displays):
                config, fixed_address = session_config(index)
                bus = _start_gtk_bus(config)
                servers.append(bus)
                address = _gtk_daemon_ready(bus, "GTK private D-Bus")
                if not address.startswith("unix:"):
                    raise die(f"GTK private D-Bus failed to start. Output: {address}")
                if fixed_address is not None:
                    if address.split(",guid=", 1)[0] != fixed_address:
                        raise die(f"GTK private D-Bus listened elsewhere: {address}")
                    # Clients need no guid; dbus-daemon draws a new one per start.
                    address = fixed_address
                environments.append(
                    _gtk_display_environment(display)
                    | {"DBUS_SESSION_BUS_ADDRESS": address, "AOBUS_OWNED_GTK_BUS": address}
                )

            print(f"GTK display{'s' if count > 1 else ''}: Xvfb {' '.join(displays)} (private session buses)")
            yield environments
        finally:
            for server in servers:
                server.terminate()
            for server in servers:
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                if server.stdout is not None:
                    server.stdout.close()


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    profile = builddir.platform_profile()
    selectable = selectable_suites()
    parser = subparsers.add_parser(
        NAME,
        help=HELP,
        description=HELP,
        epilog=EPILOG + APPKIT_EPILOG if profile.name == "macos" else EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("filter", nargs="?", default="", help="Catch2 test filter")
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument(
        "--suite",
        choices=(*selectable, *suite_groups()),
        default="default",
        help="test suite or group (default: default)",
    )
    for name in selectable:
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
        help="run non-hidden [concurrency] tests across every native Catch2 suite",
    )
    # --gtk is the retained suite shortcut, not a graph-feature value.
    bazel_workflows.add_arguments(parser, flavor=False, gtk_option="--gtk-feature")
    parser.add_argument("-l", "--list", action="store_true", help="list matching tests instead of running them")
    parser.add_argument("-n", "--no-build", action="store_true", help="skip the incremental build")
    parser.add_argument(
        "--repeat",
        type=_positive_int,
        default=1,
        metavar="N",
        help="repeat selected tests N times and stop on the first failure",
    )
    if profile.name == "macos":
        parser.add_argument(
            "--scenario",
            choices=APPKIT_SCENARIOS,
            help="AppKit smoke scenario (default: desktop; valid only with --appkit)",
        )
        parser.add_argument(
            "--library", metavar="<dir>", help="absent or empty disposable AppKit media directory (requires --appkit)"
        )
        parser.add_argument(
            "--state-root", metavar="<dir>", help="absent or empty isolated AppKit state directory (requires --appkit)"
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
        return min(SHARD_CAP, jobs.default_workers(environ=environment))
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


@dataclass(frozen=True)
class SuiteOutcome:
    """One Catch2 suite's process status and reported matching case count."""

    status: int
    matched: int | None = None


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


def _read_xml_root(report: Path) -> ElementTree.Element | None:
    """Return a Catch2 report root, or None when the report is unreadable."""
    try:
        return ElementTree.parse(report).getroot()
    except (OSError, ElementTree.ParseError):
        return None


def _read_shard_totals(report: Path) -> tuple[dict[str, int], dict[str, int]] | None:
    """Return one shard's (assertion, test case) tallies, or None when unreadable."""
    root = _read_xml_root(report)
    if root is None:
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


def _read_matching_tests(report: Path, *, list_only: bool) -> int | None:
    """Return how many cases Catch2 matched in an unsharded XML report."""
    root = _read_xml_root(report)
    if root is None:
        return None
    if list_only:
        if root.tag != "MatchingTests":
            return None
        return len(root.findall("TestCase"))

    cases = root.find("OverallResultsCases")
    if cases is None:
        return None
    try:
        return sum(int(cases.get(key, "0")) for key in _OVERALL_KEYS)
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
        *([] if "--allow-running-no-tests" in command else ["--allow-running-no-tests"]),
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
    test_filter: str,
    environments: Sequence[dict[str, str]],
    log: Path | None,
    allow_no_tests: bool,
) -> SuiteOutcome:
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
            interrupted: KeyboardInterrupt | None = None
            for shard in running:
                while True:
                    try:
                        shard.process.wait()
                        break
                    except KeyboardInterrupt as error:
                        interrupted = error
            # Closed from `sinks` rather than `running`, because the sink of a
            # failed Popen never reached `running`. Windows cannot remove a
            # directory holding an open handle, so leaving one open would fail
            # the cleanup of this TemporaryDirectory and replace the launch
            # exception with an unrelated WinError 32.
            for opened in sinks:
                opened.close()
            if interrupted is not None and sys.exception() is None:
                raise interrupted

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

    matched = None if unreported else sum(cases.values())
    if not allow_no_tests and matched == 0:
        if not test_filter:
            raise die(f"no {label} tests were selected by default. Hidden tests require an explicit filter.")
        raise die(f"no {label} tests matched the supplied filter.")
    return SuiteOutcome(status, matched)


def _repro_command(shard: _Shard) -> str:
    """Return the command that reruns this shard.

    Printed rather than described because the reader would otherwise have to
    translate "shard 3 of 8" into Catch2's zero-based --shard-index 2. The
    reporter arguments are dropped: they only redirect output the rerun wants on
    the terminal anyway.

    The shard's environment is carried along, because part of it decides whether
    the failure reproduces at all rather than merely how the run is configured:
    on an ASan tree UBSAN_OPTIONS is what makes undefined behaviour halt instead
    of log and continue. DISPLAY, its ownership marker, and the private bus
    address are deliberate exceptions: this run retires those daemons. Direct
    GTK reruns must replace the disabled endpoints with fresh isolated services;
    prefer the portal. Explicitly removing the ownership marker also prevents
    native input against the caller's desktop.
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

    exported = {
        key: value
        for key, value in shard.environment.items()
        if key not in {"DISPLAY", "AOBUS_OWNED_GTK_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "AOBUS_OWNED_GTK_BUS"}
    }
    unset = []
    if {"AOBUS_OWNED_GTK_DISPLAY", "AOBUS_OWNED_GTK_BUS"} & shard.environment.keys():
        # Omitting inherited endpoints is not isolation: GIO can even discover
        # the host bus through XDG_RUNTIME_DIR. Disable it explicitly until the
        # reader supplies fresh isolated services (prefer a portal rerun).
        unset = ["-u", "AOBUS_OWNED_GTK_DISPLAY", "-u", "AOBUS_OWNED_GTK_BUS"]
        exported |= {"DISPLAY": "", "DBUS_SESSION_BUS_ADDRESS": "disabled:"}
    prefix = ["env", *unset, *(f"{key}={value}" for key, value in sorted(exported.items()))] if exported else []
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
        if {"AOBUS_OWNED_GTK_DISPLAY", "AOBUS_OWNED_GTK_BUS"} & shard.environment.keys():
            repro += b"GTK endpoints above are disabled for safety; use ./ao test --gtk to recreate owned services.\n"
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


def _sanitizer_env(name: str, build_dir: Path, *, asan: bool, tsan: bool) -> dict[str, str]:
    return {
        **_lsan_env(build_dir, enabled=asan),
        **_ubsan_env(build_dir, enabled=asan),
        **_tsan_env(build_dir, enabled=tsan),
    }


def _require_appkit_marker(marker: Path, run_id: str, phase: str) -> None:
    expected = f"{run_id}\n"
    try:
        observed = marker.read_text(encoding="utf-8")
    except OSError as exc:
        raise die(f"AppKit {phase} did not write a readable marker at {marker}: {exc}") from exc
    if observed != expected:
        raise die(f"AppKit {phase} marker at {marker} does not belong to this invocation.")


def _resolve_appkit_fixtures(library: Path, state_root: Path) -> tuple[Path, Path]:
    try:
        resolved_library = library.resolve()
        resolved_state_root = state_root.resolve()
    except (OSError, RuntimeError) as exc:
        raise die(f"Cannot resolve AppKit smoke fixture paths: {exc}") from exc

    if (
        resolved_state_root == resolved_library
        or resolved_state_root in resolved_library.parents
        or resolved_library in resolved_state_root.parents
    ):
        raise die("AppKit --library and --state-root must not equal or contain one another.")
    try:
        for option, path in (("--library", resolved_library), ("--state-root", resolved_state_root)):
            if path.exists():
                if not path.is_dir():
                    raise die(f"AppKit {option} is not a directory: {path}")
                if next(path.iterdir(), None) is not None:
                    raise die(f"AppKit {option} must be absent or empty: {path}")
    except OSError as exc:
        raise die(f"Cannot inspect AppKit fixture paths: {exc}") from exc
    return resolved_library, resolved_state_root


def _run_appkit_parent(command: list[str], environment: dict[str, str]) -> int:
    parent = subprocess.Popen(command, cwd=PROJECT_ROOT, env={**os.environ, **environment})
    try:
        try:
            return parent.wait(timeout=APPKIT_PARENT_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            print(
                f"Error: AppKit smoke parent exceeded the {APPKIT_PARENT_TIMEOUT_SECONDS:g}-second controller timeout.",
                file=sys.stderr,
            )
            try:
                parent.terminate()
            except ProcessLookupError:
                pass
            try:
                parent.wait(timeout=APPKIT_PARENT_TERMINATE_SECONDS)
            except subprocess.TimeoutExpired:
                parent.kill()
                parent.wait()
            return 1
    except BaseException:
        try:
            parent.kill()
        except ProcessLookupError:
            pass
        while True:
            try:
                parent.wait()
                break
            except KeyboardInterrupt:
                continue
        raise


def _existing_artifact_arguments(build_dir: Path, *, asan: bool = False, tsan: bool = False) -> argparse.Namespace:
    return argparse.Namespace(
        config="tsan" if tsan else "asan" if asan else "debug",
        path=str(build_dir),
        jobs=jobs.default_workers(),
        vcpkg_prefix=None,
        remote_cache=None,
        gtk=None,
        system_media=None,
    )


@contextmanager
def _suite_artifacts(
    args: argparse.Namespace, suites: tuple[str, ...], *, no_build: bool
) -> Generator[dict[str, tuple[ExecutableArtifact, ...]], None, None]:
    native_suites = tuple(name for name in suites if SUITES[name].kind not in ("tooling", "lint"))
    if not native_suites:
        yield {}
        return
    labels = tuple(target for name in native_suites if (target := SUITES[name].target) is not None)
    if len(labels) != len(native_suites):
        raise ValueError("native suites must declare a Bazel target")
    with bazel_workflows.prepare_artifacts(args, labels, no_build=no_build, expand_tests=True) as artifacts:
        grouped = {
            name: tuple(artifact for artifact in artifacts if SUITES[name].target in artifact.requested_by)
            for name in native_suites
        }
        missing = [name for name, members in grouped.items() if not members]
        if missing:
            raise die(f"no compatible configured test executable for suite '{missing[0]}'.")
        yield grouped


def run_appkit_smoke(
    build_dir: Path,
    *,
    scenario: str,
    library: Path,
    state_root: Path,
    asan: bool = False,
    tsan: bool = False,
    artifact: ExecutableArtifact | None = None,
) -> int:
    library, state_root = _resolve_appkit_fixtures(library, state_root)
    if artifact is None:
        args = _existing_artifact_arguments(build_dir, asan=asan, tsan=tsan)
        target = SUITES["appkit"].target
        assert target is not None
        with bazel_workflows.prepare_artifacts(args, [target], no_build=True) as artifacts:
            return run_appkit_smoke(
                build_dir,
                scenario=scenario,
                library=library,
                state_root=state_root,
                asan=asan,
                tsan=tsan,
                artifact=artifacts[0],
            )
    binary = artifact.executable
    command = [
        str(binary),
        "--scenario",
        scenario,
        "--library",
        str(library),
        "--state-root",
        str(state_root),
    ]
    print("=====================================")
    print(f"Running AppKit Smoke ({scenario})")
    print(f"CMD: {' '.join(command)}")
    print("=====================================")
    sanitizer_env = _sanitizer_env("appkit", build_dir, asan=asan, tsan=tsan)
    run_id = uuid.uuid4().hex
    environment = {**sanitizer_env, **artifact.environment(), APPKIT_RUN_ID_ENV: run_id}
    if scenario != "desktop":
        return _run_appkit_parent(command, environment)
    status = appkitprocess.run_desktop(
        command,
        cwd=PROJECT_ROOT,
        environment=environment,
        parent_timeout=APPKIT_PARENT_TIMEOUT_SECONDS,
        successor_timeout=APPKIT_SUCCESSOR_TIMEOUT_SECONDS,
        terminate_timeout=APPKIT_PARENT_TERMINATE_SECONDS,
    )
    if status != 0:
        return status
    _require_appkit_marker(state_root / "desktop-pass.txt", run_id, "desktop parent")
    _require_appkit_marker(state_root / "successor-pass.txt", run_id, "desktop successor")
    return 0


def run_catch2_suite(
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
    report_matches: bool = False,
    artifacts: tuple[ExecutableArtifact, ...] | None = None,
) -> SuiteOutcome:
    spec = SUITES[name]
    if artifacts is None:
        args = _existing_artifact_arguments(build_dir, asan=asan, tsan=tsan)
        with _suite_artifacts(args, (name,), no_build=True) as grouped:
            return run_catch2_suite(
                name,
                build_dir,
                test_filter=test_filter,
                list_only=list_only,
                allow_no_tests=allow_no_tests,
                asan=asan,
                tsan=tsan,
                log=log,
                shards=shards,
                report_matches=report_matches,
                artifacts=grouped[name],
            )
    if not artifacts:
        raise die(f"no configured executable for {name} tests.")
    matched = 0
    counted = report_matches or len(artifacts) > 1 or (shards > 1 and not list_only)
    for artifact in artifacts:
        outcome = _run_catch2_artifact(
            name,
            build_dir,
            artifact=artifact,
            test_filter=test_filter,
            list_only=list_only,
            allow_no_tests=allow_no_tests or len(artifacts) > 1,
            asan=asan,
            tsan=tsan,
            log=log,
            shards=shards,
            report_matches=counted,
        )
        if outcome.status:
            return outcome
        if counted:
            if outcome.matched is None:
                return SuiteOutcome(1)
            matched += outcome.matched
    if counted and not allow_no_tests and matched == 0:
        raise die(f"no {spec.label} tests matched the supplied filter or default selection.")
    return SuiteOutcome(0, matched if counted else None)


def _run_catch2_artifact(
    name: str,
    build_dir: Path,
    *,
    artifact: ExecutableArtifact,
    test_filter: str,
    list_only: bool,
    allow_no_tests: bool,
    asan: bool,
    tsan: bool,
    log: Path | None,
    shards: int,
    report_matches: bool,
) -> SuiteOutcome:
    spec = SUITES[name]
    if spec.kind != "catch2" or spec.target is None:
        raise ValueError(f"{name} is not a Catch2 suite")

    command = [str(artifact.executable)]
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

    sanitizer_env = {**_sanitizer_env(name, build_dir, asan=asan, tsan=tsan), **artifact.environment()}

    def execute(environments: list[dict[str, str]]) -> SuiteOutcome:
        if sharded:
            return _run_sharded(
                command,
                label=spec.label,
                test_filter=test_filter,
                environments=environments,
                log=log,
                allow_no_tests=allow_no_tests,
            )
        if not report_matches:
            return SuiteOutcome(run(command, env=environments[0] or None, log=log, append=log is not None))

        with tempfile.TemporaryDirectory(prefix="aobus-catch2-report-") as directory:
            report = Path(directory) / "suite.xml"
            reported_command = [
                *command,
                "--reporter",
                "console::out=-",
                "--reporter",
                f"xml::out={report}",
            ]
            status = run(
                reported_command,
                env=environments[0] or None,
                log=log,
                append=log is not None,
            )
            matched = _read_matching_tests(report, list_only=list_only)

        if matched is None:
            print(f"{spec.label}: no readable Catch2 XML report; matching tests could not be counted")
            status = status or 1
        return SuiteOutcome(status, matched)

    # The C++ runfiles reader and every fatal-child reentry inherit the real
    # declared data context. Each native invocation gets disposable scratch.
    with tempfile.TemporaryDirectory(prefix="aobus-native-test-") as directory:
        sanitizer_env["TEST_TMPDIR"] = directory
        if name in ("gtk", "backend") and not list_only and builddir.platform_profile().name == "linux":
            with ExitStack() as services:
                displays = services.enter_context(virtual_gtk_displays(shards if sharded else 1))
                if name == "backend":
                    from ..core.bazel_test_services import private_pipewire

                    # One policy-only service context per backend process; no
                    # personal desktop bus or hardware-monitor admission.
                    contexts = [
                        services.enter_context(
                            private_pipewire(artifact.tree / "runtime-services" / uuid.uuid4().hex, display)
                        )
                        for display in displays
                    ]
                else:
                    contexts = displays
                return execute([{**sanitizer_env, **context} for context in contexts])
        return execute([sanitizer_env] * (shards if sharded else 1))


def run_non_catch2_suite(
    name: str,
    build_dir: Path,
    *,
    list_only: bool = False,
    log: Path | None = None,
    no_build: bool = False,
    lint_build_dir: Path | None = None,
    lint_build_dir_explicit: bool = False,
    artifacts: tuple[ExecutableArtifact, ...] | None = None,
    lint_action: bazel_workflows.LintFixtureAction | None = None,
) -> int:
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
        if lint_action is None:
            selected_tree = lint_build_dir if lint_build_dir is not None else build_dir
            args = _existing_artifact_arguments(selected_tree)
            with bazel_workflows.prepare_lint_fixtures(args, no_build=no_build) as selected:
                return selected.run(log=log)
        return lint_action.run(log=log)

    raise ValueError(f"unknown suite kind: {spec.kind}")


def _catch2_suites_for_filter(suites: tuple[str, ...], test_filter: str) -> tuple[str, ...]:
    """Select Catch2 suites, report exclusions, and reject a tooling-only filter."""
    if not test_filter:
        return suites

    excluded = tuple(name for name in suites if SUITES[name].kind != "catch2")
    selected = tuple(name for name in suites if SUITES[name].kind == "catch2")
    if excluded:
        labels = ", ".join(SUITES[name].label for name in excluded)
        print(f"Skipping {labels}: explicit filters apply only to Catch2 suites.")
    if not selected:
        raise die("the supplied filter cannot be applied because no Catch2 suite is selected.")
    return selected


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
    no_build: bool = False,
    lint_build_dir: Path | None = None,
    lint_build_dir_explicit: bool = False,
    artifacts: Mapping[str, tuple[ExecutableArtifact, ...]] | None = None,
    lint_action: bazel_workflows.LintFixtureAction | None = None,
) -> int:
    suites = _catch2_suites_for_filter(suites, test_filter)
    if "lint" in suites and not list_only and lint_action is None:
        selected_tree = lint_build_dir if lint_build_dir is not None else build_dir
        args = _existing_artifact_arguments(selected_tree, asan=asan, tsan=tsan)
        with bazel_workflows.prepare_lint_fixtures(args, no_build=no_build) as selected:
            return run_suites(
                suites,
                build_dir,
                test_filter=test_filter,
                list_only=list_only,
                allow_no_tests=allow_no_tests,
                repeat=repeat,
                asan=asan,
                tsan=tsan,
                concurrency=concurrency,
                log=log,
                no_build=no_build,
                lint_build_dir=lint_build_dir,
                lint_build_dir_explicit=lint_build_dir_explicit,
                artifacts=artifacts,
                lint_action=selected,
            )
    shards = suite_shards(list_only=list_only, repeat=repeat, tsan=tsan, concurrency=concurrency)
    iterations = 1 if list_only else repeat
    for iteration in range(iterations):
        matched_tests = 0
        if iterations > 1:
            print(f"Test repetition {iteration + 1}/{iterations}")

        for index, name in enumerate(suites):
            if index:
                print()

            spec = SUITES[name]
            if spec.kind == "catch2":
                # One suite record per invocation: jobs is the known shard
                # width and count stays 1 because matched case counts are
                # unstable across filters and repeats.
                with phases.measure("suite", tree="native", jobs=shards, count=1, suite=name) as phase:
                    outcome = run_catch2_suite(
                        name,
                        build_dir,
                        test_filter=test_filter,
                        list_only=list_only,
                        allow_no_tests=allow_no_tests or bool(test_filter),
                        asan=asan,
                        tsan=tsan,
                        log=log,
                        shards=shards,
                        report_matches=bool(test_filter),
                        artifacts=artifacts.get(name) if artifacts is not None else None,
                    )
                    phase.status = outcome.status
                if outcome.status != 0:
                    return outcome.status
                if test_filter:
                    if outcome.matched is None:
                        return 1
                    matched_tests += outcome.matched
            else:
                # Lint executes new assertions against the admitted producer;
                # tooling owns no native tree or artifact preparation.
                with phases.measure(
                    "suite",
                    tree="tidy" if spec.kind == "lint" else None,
                    count=1,
                    suite=name,
                ) as phase:
                    status = run_non_catch2_suite(
                        name,
                        build_dir,
                        list_only=list_only,
                        log=log,
                        no_build=no_build,
                        lint_build_dir=lint_build_dir,
                        lint_build_dir_explicit=lint_build_dir_explicit,
                        artifacts=artifacts.get(name) if artifacts is not None else None,
                        lint_action=lint_action,
                    )
                    phase.status = status
                if status != 0:
                    return status

        if test_filter and matched_tests == 0:
            raise die(f"no tests in the selected Catch2 suites matched filter {test_filter!r}.")

    return 0


def _appkit_options(args: argparse.Namespace) -> tuple[str, Path, Path] | None:
    scenario = getattr(args, "scenario", None)
    library = getattr(args, "library", None)
    state_root = getattr(args, "state_root", None)
    if args.suite != "appkit":
        if scenario is not None or library is not None or state_root is not None:
            raise die("--scenario, --library, and --state-root are valid only with --appkit.")
        return None

    if args.filter or args.list or args.repeat != 1:
        raise die("AppKit smoke does not accept Catch2 filters, --list, or --repeat.")
    if library is None or state_root is None:
        missing = [option for option, value in (("--library", library), ("--state-root", state_root)) if value is None]
        raise die(f"AppKit smoke requires {' and '.join(missing)}; no user configuration is used as a fallback.")
    resolved_library, resolved_state_root = _resolve_appkit_fixtures(Path(library), Path(state_root))
    return scenario or "desktop", resolved_library, resolved_state_root


def _run_winui_audit(args: argparse.Namespace) -> int:
    """Run the explicit Debug desktop gate, never silently skip absent admission."""
    if args.filter or args.list or args.repeat != 1 or args.clang or args.asan or args.tsan:
        raise die(
            "WinUI naming audit runs Debug only and rejects Catch2 filters, --list, "
            "--repeat, and compiler/sanitizer switches."
        )
    try:
        winui.require_runtime()
        winui.require_interactive_session()
    except RuntimeError as error:
        raise die(str(error)) from error

    target = SUITES["winui"].target
    assert target is not None
    with bazel_workflows.prepare_artifacts(args, [target], no_build=args.no_build) as artifacts:
        (artifact,) = artifacts
        with phases.measure("suite", tree="winui", count=1, suite="winui") as phase:
            phase.status = run(
                [str(artifact.executable)],
                cwd=artifact.executable.parent,
                env=artifact.environment(),
                log=artifact.tree / "build.log",
                append=True,
            )
        return phase.status


def run_command(args: argparse.Namespace) -> int:
    try:
        return _run_command(args)
    except BazelWorkspaceError as error:
        raise die(str(error)) from error


def _run_command(args: argparse.Namespace) -> int:
    appkit_options = _appkit_options(args)
    if args.suite == "concurrency" and args.filter:
        raise die("--concurrency supplies [concurrency]~[.]; do not also pass a positional filter.")
    # Explicit tags can admit hidden cases; the automatic concurrency group
    # must never promote a manual/hardware probe to unattended execution.
    test_filter = "[concurrency]~[.]" if args.suite == "concurrency" else args.filter
    suites = _catch2_suites_for_filter(suites_for(args.suite, tsan=args.tsan), test_filter)
    options: dict[str, Any] = {
        "test_filter": test_filter,
        "list_only": args.list,
        "repeat": args.repeat,
        "asan": args.asan,
        "tsan": args.tsan,
        "no_build": args.no_build,
        "concurrency": args.suite == "concurrency",
        "allow_no_tests": args.suite == "concurrency",
    }
    # Tooling and listing non-native suite names need neither a native SDK nor a
    # Bazel executable/tree. Keep this branch ahead of all native preparation.
    if suites == ("tooling",) or (args.list and all(SUITES[name].kind in ("tooling", "lint") for name in suites)):
        return run_suites(suites, Path(args.path or "."), **options)
    config, _host = bazel_workflows.validate_request(args)
    from ..core.bazel_workspace import workspace_path

    build_dir = workspace_path(args.path, config)
    configured = configured_suites_for(args.suite, build_dir, tsan=args.tsan, args=args)
    suites = tuple(name for name in suites if name in configured)
    if not suites:
        raise die("no test suites remain after requested graph feature selection.")
    if args.suite == "winui":
        return _run_winui_audit(args)
    if appkit_options is not None:
        scenario, library, state_root = appkit_options
        target = SUITES["appkit"].target
        assert target is not None
        with bazel_workflows.prepare_artifacts(args, [target], no_build=args.no_build) as appkit_artifacts:
            (artifact,) = appkit_artifacts
            return run_appkit_smoke(
                build_dir,
                scenario=scenario,
                library=library,
                state_root=state_root,
                asan=config == "asan",
                tsan=config == "tsan",
                artifact=artifact,
            )
    options["asan"], options["tsan"] = config == "asan", config == "tsan"
    prepared_suites = tuple(name for name in suites if not args.list or SUITES[name].kind == "catch2")
    with ExitStack() as prepared:
        artifacts: Mapping[str, tuple[ExecutableArtifact, ...]] = prepared.enter_context(
            _suite_artifacts(args, prepared_suites, no_build=args.no_build)
        )
        lint_action = (
            prepared.enter_context(bazel_workflows.prepare_lint_fixtures(args, no_build=args.no_build))
            if "lint" in suites and not args.list
            else None
        )
        return run_suites(
            suites,
            build_dir,
            artifacts=artifacts,
            lint_action=lint_action,
            log=build_dir / "build.log",
            **options,
        )
