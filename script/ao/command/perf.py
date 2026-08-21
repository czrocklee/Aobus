"""ao perf — build and run the standalone performance review workload."""

import argparse
import json
from pathlib import Path

from ..core import builddir
from ..core.proc import capture, die, run
from . import build

HELP = "Build and run the standalone performance review workload"
NAME = "perf"
REQUIRES_BUILD_ENV = True

TARGET = "ao_perf_baseline"
DEFAULT_FILTER = "[perf][review]"
DEFAULT_SAMPLES = 20
DEFAULT_WARMUPS = 1
REPORT_SCHEMA = "aobus-performance-review/v2"


def _positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _non_negative_integer(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return parsed


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(NAME, help=HELP, description=HELP)
    build.add_build_arguments(parser, default_flavor="release")
    parser.add_argument("--no-build", action="store_true", help="run the existing benchmark executable")
    parser.add_argument(
        "--samples",
        type=_positive_integer,
        default=DEFAULT_SAMPLES,
        metavar="<count>",
        help=f"measured samples per workload (default: {DEFAULT_SAMPLES})",
    )
    parser.add_argument(
        "--warmups",
        type=_non_negative_integer,
        default=DEFAULT_WARMUPS,
        metavar="<count>",
        help=f"unmeasured warm-up samples per workload (default: {DEFAULT_WARMUPS})",
    )
    parser.add_argument(
        "--filter",
        default=DEFAULT_FILTER,
        metavar="<catch-filter>",
        help=f"Catch2 filter to run (default: {DEFAULT_FILTER})",
    )
    parser.add_argument(
        "--library-root",
        type=Path,
        metavar="<directory>",
        help="append an aggregate-only ordering workload from an existing Aobus library",
    )
    parser.add_argument(
        "--library-locale",
        default="en-US",
        metavar="<language-tag>",
        help="collation locale for --library-root (default: en-US)",
    )
    parser.add_argument("--output", type=Path, metavar="<file>", help="write the JSON report to this path")
    parser.set_defaults(func=run_command)


def _revision() -> str:
    revision = capture(["git", "rev-parse", "--verify", "HEAD"]).strip()
    if capture(["git", "status", "--porcelain"], check=False).strip():
        revision += "+dirty"
    return revision


def _print_report(path: Path) -> None:
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise die(f"could not read performance report '{path}': {exc}") from exc

    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        raise die(f"performance report '{path}' has an unsupported schema")

    metadata = report.get("metadata")
    measurements = report.get("measurements")
    if not isinstance(metadata, dict) or not isinstance(measurements, list):
        raise die(f"performance report '{path}' is missing metadata or measurements")

    for key in ("platform", "build_mode", "compiler", "icu_version"):
        if not isinstance(metadata.get(key), str) or not metadata[key]:
            raise die(f"performance report '{path}' has invalid metadata field '{key}'")

    print()
    print(
        f"Performance review: {metadata['platform']} {metadata['build_mode']} "
        f"{metadata['compiler']}, ICU {metadata['icu_version']}"
    )
    for index, measurement in enumerate(measurements):
        context = f"measurement {index}"
        if not isinstance(measurement, dict):
            raise die(f"performance report '{path}' has invalid {context}")

        for key in ("capability", "scenario", "dataset"):
            if not isinstance(measurement.get(key), str) or not measurement[key]:
                raise die(f"performance report '{path}' has invalid {context} field '{key}'")

        for key in ("input_count", "median_ns", "p95_ns"):
            if type(measurement.get(key)) is not int:
                raise die(f"performance report '{path}' has invalid {context} field '{key}'")

        for key in ("policy", "locale"):
            if key in measurement and (not isinstance(measurement[key], str) or not measurement[key]):
                raise die(f"performance report '{path}' has invalid {context} field '{key}'")

        byte_metric = measurement.get("byte_metric")
        if byte_metric is not None and (
            not isinstance(byte_metric, dict)
            or not isinstance(byte_metric.get("kind"), str)
            or not byte_metric["kind"]
            or type(byte_metric.get("count")) is not int
        ):
            raise die(f"performance report '{path}' has invalid {context} field 'byte_metric'")

        median_ms = measurement["median_ns"] / 1_000_000
        p95_ms = measurement["p95_ns"] / 1_000_000
        dimensions = [measurement["capability"]]
        if policy := measurement.get("policy"):
            dimensions.append(policy)
        dimensions.append(measurement["scenario"])
        if locale := measurement.get("locale"):
            dimensions.append(locale)
        dimensions.append(f"{measurement['dataset']}-{measurement['input_count']}")
        summary = f"  {'/'.join(dimensions)}: median {median_ms:.3f} ms, p95 {p95_ms:.3f} ms"
        if byte_metric is not None:
            summary += f", {byte_metric['count']} {byte_metric['kind']}"
        print(summary)
    print(f"  Report: {path}")


def run_command(args: argparse.Namespace) -> int:
    if args.library_root is not None and not args.library_root.is_dir():
        raise die(f"Library root is not a directory: {args.library_root}")

    profile = build.validate_build_options(args)
    result = None
    if not args.no_build:
        result = build.do_build(args, [TARGET])

    build_dir = (
        result.build_dir
        if result is not None
        else Path(args.path)
        if args.path
        else builddir.build_dir(args.flavor, clang=args.clang, asan=args.asan, tsan=args.tsan)
    )
    executable = builddir.executable(build_dir / "test" / TARGET)
    if not executable.exists():
        raise die(f"Performance executable not found at {executable}. Run './ao perf' without --no-build first.")

    output = args.output if args.output is not None else build_dir / "performance-review.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    compiler = result.compiler if result is not None else "clang" if args.clang else profile.compiler
    environment = {
        "AOBUS_PERF_REPORT_JSON": str(output),
        "AOBUS_PERF_SAMPLES": str(args.samples),
        "AOBUS_PERF_WARMUPS": str(args.warmups),
        "AOBUS_PERF_REVISION": _revision(),
        "AOBUS_PERF_COMPILER": compiler,
        "AOBUS_PERF_BUILD_MODE": args.flavor,
        "AOBUS_PERF_PLATFORM": profile.name,
    }
    if args.library_root is not None:
        environment["AOBUS_PERF_LIBRARY_ROOT"] = str(args.library_root)
        environment["AOBUS_PERF_LIBRARY_LOCALE"] = args.library_locale
    if run([str(executable), args.filter], env=environment) != 0:
        raise die("performance review workload failed")
    if not output.is_file():
        raise die(f"performance review workload did not produce '{output}'")

    _print_report(output)
    return 0
