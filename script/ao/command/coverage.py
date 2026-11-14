"""ao coverage — strict, fresh native Bazel coverage with retained evidence."""

import argparse

from ..core import bazel_coverage, bazel_native, bazel_workflows
from ..core.proc import die

NAME = "coverage"
HELP = "Measure Linux coverage with strict native/stock/combined validation"
REQUIRES_BUILD_ENV = False


def requires_build_environment(args: argparse.Namespace) -> bool:
    bazel_coverage.validate_request(bazel_native.native_platform(), "debug")
    if args.filter:
        raise die("coverage does not accept Catch2 filters: suite selection must preserve the measured denominator")
    return True


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    parser.add_argument("filter", nargs="?", help="retired Catch2 coverage filter; rejected before SDK preparation")
    parser.add_argument("-p", "--path", help="external Bazel coverage tree (honors BUILD_DIR)")
    parser.add_argument("-j", "--jobs", type=bazel_workflows.positive_jobs, default=2)
    parser.add_argument("--scope", action="append", default=[], help="report source prefix; does not narrow execution")
    parser.add_argument("--analysis-only", action="store_true", help="verify instrumentation/inputs without execution")
    suites = parser.add_mutually_exclusive_group()
    suites.add_argument(
        "--suite", choices=("core", "application", "backend", "cli", "tui", "gtk", "all"), dest="coverage_suite"
    )
    for suite in ("core", "application", "backend", "cli", "tui", "gtk", "all"):
        suites.add_argument("--" + suite, dest="coverage_suite", action="store_const", const=suite)
    bazel_workflows.add_remote_cache_argument(parser, "opt in to a trusted native action cache")
    parser.set_defaults(func=run_command, config="debug", vcpkg_prefix=None, coverage_suite="default", targets=[])


def run_command(args: argparse.Namespace) -> int:
    try:
        return bazel_coverage.run(args)
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error
