"""ao check — native build, dependency, and registered-suite validation."""

import argparse
import copy
from pathlib import Path

from ..core import builddir, compiler_cache, dependency_policy, phases, testaudit
from ..core.paths import absolute_path
from ..core.proc import die
from . import build, perf, test

HELP = "Build everything and run every suite enabled by the native profile"
NAME = "check"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True
GUARDRAIL_TARGET = "aobus_guardrails"


EPILOG = """\
examples:
  ./ao check                # debug build + every registered test suite
  ./ao check release        # same against the release tree
  ./ao check --asan         # debug + address sanitizer, plus undefined sanitizer where available
  ./ao check --tsan         # debug + ThreadSanitizer-safe suites
  ./ao check --clang        # clang build tree
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    build.add_build_arguments(parser)
    parser.set_defaults(func=run_command, target=[])


def _tsan_build_suites(args: argparse.Namespace) -> tuple[str, ...]:
    suites = test.suites_for("all", tsan=True)
    if "gtk" not in suites:
        return suites

    # A TSan check builds individual suite targets. Reused trees retain their
    # feature configuration when no switch is passed; a clean tree uses defaults.
    build_dir = (
        Path(args.path)
        if args.path
        else builddir.build_dir(args.flavor, clang=args.clang, asan=args.asan, tsan=args.tsan)
    )
    cache = compiler_cache.read_cmake_cache(build_dir / "CMakeCache.txt") if not args.clean else {}
    if "AOBUS_BUILD_TESTS" in cache and not build.cmake_option_enabled(cache, "AOBUS_BUILD_TESTS", build_dir):
        raise die(f"Test targets are not enabled in {build_dir} (AOBUS_BUILD_TESTS=ON is required).")
    if args.gtk is not None:
        gtk_enabled = args.gtk == "on"
    elif "AOBUS_BUILD_GTK" in cache:
        gtk_enabled = build.cmake_option_enabled(cache, "AOBUS_BUILD_GTK", build_dir)
    else:
        gtk_enabled = True
    if not gtk_enabled:
        return tuple(suite for suite in suites if suite != "gtk")
    return suites


def _verify_test_source_manifest(build_dir: Path) -> None:
    """Reconcile content discovery with this build's configured CMake evidence."""
    cache = compiler_cache.read_cmake_cache(build_dir / "CMakeCache.txt")
    source_home = cache.get("CMAKE_HOME_DIRECTORY")
    if (
        not source_home
        or not (source_root := absolute_path(source_home)).is_dir()
        or not (source_root / "test").is_dir()
    ):
        raise die(f"Configured test source view is unavailable in {build_dir}.")
    manifest = build_dir / "aobus-test-sources.tsv"
    try:
        rows = manifest.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise die(f"Cannot read configured test source manifest {manifest}: {error}") from error
    if not rows or rows[0] != "source\tstate\towner-or-disabled-reason":
        raise die(f"Invalid configured test source manifest header: {manifest}")

    registered: set[str] = set()
    for line in rows[1:]:
        fields = line.split("\t")
        if len(fields) != 3:
            raise die(f"Invalid configured test source record in {manifest}: {line!r}")
        source, state, owner_or_reason = fields
        relative = Path(source)
        if (
            not source.startswith("test/")
            or ".." in relative.parts
            or state not in ("configured", "disabled")
            or not owner_or_reason
            or source in registered
            or not (source_root / relative).is_file()
        ):
            raise die(f"Invalid or duplicate configured test source record in {manifest}: {line!r}")
        registered.add(source)

    problems: list[str] = []
    declaration_files = 0
    declarations = 0
    for path in testaudit.discover_test_files(source_root):
        cases, lexical_issues = testaudit.scan_test_file(path)
        problems.extend(issue.format(source_root) for issue in lexical_issues)
        placement_issues = testaudit.registration_placement_issues(path, cases)
        problems.extend(issue.format(source_root) for issue in placement_issues)
        if placement_issues or not cases:
            continue
        declaration_files += 1
        declarations += len(cases)
        relative_source = path.relative_to(source_root).as_posix()
        if relative_source not in registered:
            problems.append(f"{relative_source}: registration file has no configured owner or explicit disabled record")
    if declaration_files == 0:
        problems.append("no registration files were discovered in the configured source view")
    if problems:
        raise die("Configured test registration coverage failed:\n  " + "\n  ".join(problems))
    print(f"Test registration ownership: {declaration_files} files, {declarations} declarations accounted for.")


def run_command(args: argparse.Namespace) -> int:
    if args.flavor not in ("debug", "release"):
        print(f"Note: tests only run for debug/release; use ./ao build for {args.flavor}.")
        profile_args = copy.copy(args)
        profile_args.target = ["all", GUARDRAIL_TARGET]
        return build.run_command(profile_args)

    build_suites = _tsan_build_suites(args) if args.tsan else test.suites_for("all")
    if args.tsan:
        targets = [target for suite in build_suites if (target := test.SUITES[suite].target) is not None]
        targets.append(GUARDRAIL_TARGET)
    else:
        targets = ["all", GUARDRAIL_TARGET, perf.TARGET]
    result = build.do_build(args, targets=targets)
    _verify_test_source_manifest(result.build_dir)

    print("Verifying dependency resolution...")
    try:
        with phases.measure("dependency", tree="native"):
            dependency_policy.verified_report(result.build_dir)
    except dependency_policy.DependencyPolicyError as exc:
        raise die(str(exc)) from exc

    profile = builddir.platform_profile()
    if profile.name == "windows" and not args.asan:
        winui_args = copy.copy(args)
        winui_args.path = str(
            builddir.winui_companion_build_dir(
                result.build_dir,
                primary_path_was_explicit=args.path is not None,
            )
        )
        winui_result = build.do_build(winui_args, targets=["winui"])
        print("Verifying WinUI dependency resolution...")
        try:
            with phases.measure("dependency", tree="winui"):
                dependency_policy.verified_report(winui_result.build_dir)
        except dependency_policy.DependencyPolicyError as exc:
            raise die(str(exc)) from exc
        print(f"WinUI {args.flavor} build: {winui_result.build_dir}")
    elif profile.name == "windows" and args.asan:
        print("WinUI build skipped for the MSVC AddressSanitizer profile.")

    suites = test.configured_suites_for("all", result.build_dir, tsan=args.tsan)
    print("Running tests...")
    if (status := test.run_suites(suites, result.build_dir, asan=args.asan, tsan=args.tsan, log=result.log)) != 0:
        return status

    build.print_summary(args, result, tests=f"all native suites ({', '.join(suites)})")
    return 0
