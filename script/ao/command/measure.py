"""Collect quantitative evidence, rather than running an acceptance gate."""

import argparse
import subprocess
from pathlib import Path

from ao.core import bazel_native
from ao.core.proc import die
from ao.portal import bazel_workflows, dependency_checks, measurements, performance_review

from . import options

NAME = "measure"
HELP = "Collect coverage, performance or public-concept measurements"
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = False


def _nonnegative(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return number


def register(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    metrics = parser.add_subparsers(dest="measure_action", metavar="<metric>", required=True)
    coverage = metrics.add_parser("coverage", help="execute instrumented tests and collect production line coverage")
    options.add_coverage_arguments(coverage, measurement=True)
    performance = metrics.add_parser("performance", help="build and execute the performance review workload")
    bazel_workflows.add_arguments(performance, default_config="release")
    performance.add_argument(
        "--no-build", action="store_true", help="run existing artifacts with unverified source revision"
    )
    performance.add_argument(
        "--samples", type=bazel_workflows.positive_jobs, default=performance_review.DEFAULT_SAMPLES
    )
    performance.add_argument("--warmups", type=_nonnegative, default=performance_review.DEFAULT_WARMUPS)
    performance.add_argument("--filter", default=performance_review.DEFAULT_FILTER, help="Catch2 workload filter")
    performance.add_argument("--library-root", type=Path, help="append an aggregate-only existing-library workload")
    performance.add_argument("--library-locale", default="en-US")
    performance.add_argument("--output", type=Path, help="write the measured JSON report")
    concepts = metrics.add_parser("concepts", help="measure public declarations, edges and native include observations")
    options.add_dependency_arguments(concepts)
    for metric in (coverage, performance, concepts):
        metric.set_defaults(func=run_command)


def coverage_request(args: argparse.Namespace) -> measurements.CoverageRequest:
    return measurements.CoverageRequest(
        path=args.path,
        jobs=args.jobs,
        compiler=args.compiler,
        coverage_suite=args.suite,
        scope=tuple(getattr(args, "scope", ())),
        remote_cache=args.remote_cache,
    )


def dependency_request(args: argparse.Namespace) -> dependency_checks.Request:
    return dependency_checks.Request(config=args.config, path=args.path, vcpkg_prefix=args.vcpkg_prefix, jobs=args.jobs)


def performance_request(args: argparse.Namespace) -> performance_review.Request:
    return performance_review.Request(
        **options.native_values(args),
        no_build=args.no_build,
        samples=args.samples,
        warmups=args.warmups,
        filter=args.filter,
        library_root=args.library_root,
        library_locale=args.library_locale,
        output=args.output,
    )


def requires_build_environment(args: argparse.Namespace) -> bool:
    try:
        if args.measure_action == "coverage":
            measurements.validate_coverage_request(coverage_request(args))
        elif args.measure_action == "performance":
            performance_review.validate_request(performance_request(args))
        else:
            dependency_checks.validate_request(dependency_request(args))
        return True
    except (bazel_native.BazelWorkspaceError, OSError, ValueError) as error:
        raise die(str(error)) from error


def run_command(args: argparse.Namespace) -> int:
    try:
        if args.measure_action == "coverage":
            return measurements.measure_coverage(coverage_request(args))
        if args.measure_action == "performance":
            return performance_review.run(performance_request(args))
        return measurements.measure_concepts(dependency_request(args), json_output=args.json)
    except (bazel_native.BazelWorkspaceError, OSError, ValueError, subprocess.SubprocessError, KeyError) as error:
        raise die(str(error)) from error
