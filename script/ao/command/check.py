"""ao check — native build, dependency, and registered-suite validation."""

import argparse
import copy
from pathlib import Path

from ..core import builddir, compiler_cache, dependency_policy
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
    if "AOBUS_BUILD_TESTS" in cache and not test.cmake_option_enabled(cache, "AOBUS_BUILD_TESTS", build_dir):
        raise die(f"Test targets are not enabled in {build_dir} (AOBUS_BUILD_TESTS=ON is required).")
    if args.gtk is not None:
        gtk_enabled = args.gtk == "on"
    elif "AOBUS_BUILD_GTK" in cache:
        gtk_enabled = test.cmake_option_enabled(cache, "AOBUS_BUILD_GTK", build_dir)
    else:
        gtk_enabled = True
    if not gtk_enabled:
        return tuple(suite for suite in suites if suite != "gtk")
    return suites


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

    print("Verifying dependency resolution...")
    try:
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
