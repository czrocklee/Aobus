"""ao check — native build, dependency, and registered-suite validation."""

import argparse
import copy

from ..core import builddir, dependency_policy
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


def run_command(args: argparse.Namespace) -> int:
    if args.flavor not in ("debug", "release"):
        print(f"Note: tests only run for debug/release; use ./ao build for {args.flavor}.")
        profile_args = copy.copy(args)
        profile_args.target = ["all", GUARDRAIL_TARGET]
        return build.run_command(profile_args)

    suites = test.suites_for("all", tsan=args.tsan)
    if args.tsan:
        targets = [target for suite in suites if (target := test.SUITES[suite].target) is not None]
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

    print("Running tests...")
    if (status := test.run_suites(suites, result.build_dir, asan=args.asan, tsan=args.tsan, log=result.log)) != 0:
        return status

    build.print_summary(args, result, tests=f"all native suites ({', '.join(suites)})")
    return 0
