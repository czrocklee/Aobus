"""ao perf — build and run the standalone performance review workload."""

import argparse
import json
import os
from pathlib import Path

from ..core import bazel_workflows, gitfiles
from ..core.bazel_native import BazelWorkspaceError
from ..core.paths import PROJECT_ROOT
from ..core.proc import capture, die, run

HELP = "Build and run the standalone performance review workload"
NAME = "perf"
REQUIRES_BUILD_ENV = True
REQUIRES_PYTHON_TOOLS = False

TARGET_LABEL = "//test:ao_perf_baseline"
TARGET = "ao_perf_baseline"
DEFAULT_FILTER = "PerformanceReview - ordering and completion-alias workloads"
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
    bazel_workflows.add_arguments(parser, default_flavor="release")
    parser.add_argument(
        "--no-build", action="store_true", help="run the existing benchmark executable with unverified source revision"
    )
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
    revision = capture(gitfiles.git_command("rev-parse", "--verify", "HEAD"), cwd=PROJECT_ROOT).strip()
    # Provenance is observational: status must not persist stat-only index refreshes.
    # Do not opt into source-discovery bindings or change generic Git write policy.
    # Command-local status configuration wins over every configuration channel and
    # is inherited by the nested submodule status, so no checkout configuration can
    # hide non-ignored untracked changes or submodule content changes.
    if capture(
        gitfiles.git_command(
            "--no-optional-locks",
            "-c",
            "status.showUntrackedFiles=normal",
            "status",
            "--porcelain",
            "--ignore-submodules=none",
        ),
        cwd=PROJECT_ROOT,
    ).strip():
        revision += "+dirty"
    return revision


def _require_fresh_report(path: Path) -> None:
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise die(f"performance workload did not produce a nonempty regular report at '{path}'")


def _print_report(path: Path) -> None:
    _require_fresh_report(path)
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise die(f"could not read performance report '{path}': {exc}") from exc

    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        raise die(f"performance report '{path}' has an unsupported schema")

    metadata = report.get("metadata")
    measurements = report.get("measurements")
    if not isinstance(metadata, dict) or not isinstance(measurements, list) or not measurements:
        raise die(f"performance report '{path}' is missing metadata or nonempty measurements")

    for key in ("platform", "build_mode", "compiler", "icu_version"):
        if not isinstance(metadata.get(key), str) or not metadata[key]:
            raise die(f"performance report '{path}' has invalid metadata field '{key}'")

    summaries = []
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
        dimensions.append(f"{measurement['dataset']} (input_count={measurement['input_count']})")
        summary = f"  {'/'.join(dimensions)}: median {median_ms:.3f} ms, p95 {p95_ms:.3f} ms"
        if byte_metric is not None:
            summary += f", {byte_metric['count']} {byte_metric['kind']}"
        summaries.append(summary)

    print()
    print(
        f"Performance review: {metadata['platform']} {metadata['build_mode']} "
        f"{metadata['compiler']}, ICU {metadata['icu_version']}"
    )
    print(f"  Source revision: {metadata.get('revision', 'unverified')}")
    for summary in summaries:
        print(summary)
    print(f"  Report: {path}")


def _validate_baseline_report(path: Path) -> None:
    _require_fresh_report(path)
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise die(f"could not read baseline report '{path}': {exc}") from exc
    if not isinstance(report, dict) or report.get("schema") != "aobus-performance-baseline/v1":
        raise die(f"baseline report '{path}' has an unsupported schema")


def _prepare_outputs(output: Path, baseline: Path | None) -> None:
    paths = [output] if baseline is None else [output, baseline]
    # Reject a colliding pair before deleting either artifact.
    if baseline is not None and (
        output.resolve() == baseline.resolve() or (output.exists() and baseline.exists() and output.samefile(baseline))
    ):
        raise die("v1 and v2 performance reports require distinct output paths")
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)
    for path in paths:
        path.unlink(missing_ok=True)


def _record_build_provenance(path: Path, environment: dict[str, str], provenance: dict[str, object]) -> None:
    _require_fresh_report(path)
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise die(f"could not read performance report '{path}': {error}") from error
    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        raise die(f"performance report '{path}' has an unsupported schema")
    metadata = report.get("metadata")
    if not isinstance(metadata, dict):
        raise die(f"performance report '{path}' is missing metadata")
    for field, variable in (
        ("revision", "AOBUS_PERF_REVISION"),
        ("compiler", "AOBUS_PERF_COMPILER"),
        ("platform", "AOBUS_PERF_PLATFORM"),
        ("build_mode", "AOBUS_PERF_BUILD_MODE"),
    ):
        if metadata.get(field) != environment[variable]:
            raise die(f"performance report '{path}' disagrees with launched artifact metadata '{field}'")
    metadata["bazel"] = provenance
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def run_command(args: argparse.Namespace) -> int:
    if args.library_root is not None and not args.library_root.is_dir():
        raise die(f"Library root is not a directory: {args.library_root}")
    try:
        config, host = bazel_workflows.validate_request(args)
        with bazel_workflows.prepare_artifacts(
            args,
            [TARGET_LABEL],
            no_build=args.no_build,
            compiler_identity=True,
        ) as artifacts:
            (artifact,) = artifacts
            output = (args.output if args.output is not None else artifact.tree / "performance-review.json").absolute()
            baseline_request = os.environ.get("AOBUS_PERF_BASELINE_JSON", "")
            baseline = Path(baseline_request).absolute() if baseline_request else None
            # Source attribution follows this invocation's build, never a fresh
            # checkout revision attached to an old --no-build executable.
            revision = "unverified" if args.no_build else _revision()
            _prepare_outputs(output, baseline)
            environment = {
                **artifact.environment(),
                "AOBUS_PERF_REPORT_JSON": str(output),
                "AOBUS_PERF_BASELINE_JSON": str(baseline) if baseline is not None else "",
                "AOBUS_PERF_SAMPLES": str(args.samples),
                "AOBUS_PERF_WARMUPS": str(args.warmups),
                "AOBUS_PERF_REVISION": revision,
                "AOBUS_PERF_COMPILER": artifact.compiler_identity,
                "AOBUS_PERF_BUILD_MODE": config,
                "AOBUS_PERF_PLATFORM": host,
                # Explicit empty values prevent an unrelated inherited library
                # request from silently changing this benchmark's dataset.
                "AOBUS_PERF_LIBRARY_ROOT": str(args.library_root) if args.library_root is not None else "",
                "AOBUS_PERF_LIBRARY_LOCALE": args.library_locale if args.library_root is not None else "",
            }
            if run([str(artifact.executable), args.filter], env=environment) != 0:
                raise die("performance review workload failed")
            if baseline is not None:
                _validate_baseline_report(baseline)
            provenance = artifact.provenance() | {"source_attribution": "unverified" if args.no_build else "built"}
            _record_build_provenance(output, environment, provenance)
            _print_report(output)
            return 0
    except BazelWorkspaceError as error:
        raise die(str(error)) from error
