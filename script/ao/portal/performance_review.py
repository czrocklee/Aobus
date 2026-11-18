"""Standalone performance review with exact artifact and report provenance."""

import json
import os
from dataclasses import dataclass
from pathlib import Path

from ao.core.bazel_native import BazelWorkspaceError
from ao.core.paths import PROJECT_ROOT
from ao.core.proc import capture, die
from ao.core.proc import run as run_process
from ao.portal import bazel_workflows, git_files

TARGET_LABEL = "//test:ao_perf_baseline"
TARGET = "ao_perf_baseline"
DEFAULT_FILTER = "PerformanceReview - ordering and completion-alias workloads"
DEFAULT_SAMPLES = 20
DEFAULT_WARMUPS = 1
REPORT_SCHEMA = "aobus-performance-review/v2"


@dataclass(frozen=True, kw_only=True)
class Request(bazel_workflows.Request):
    config: str = "release"
    no_build: bool = False
    samples: int = DEFAULT_SAMPLES
    warmups: int = DEFAULT_WARMUPS
    filter: str = DEFAULT_FILTER
    library_root: Path | None = None
    library_locale: str = "en-US"
    output: Path | None = None


def validate_request(request: Request) -> tuple[str, str]:
    config, host = bazel_workflows.validate_performance_request(request)
    if type(request.samples) is not int or request.samples < 1:
        raise BazelWorkspaceError("performance samples must be positive")
    if type(request.warmups) is not int or request.warmups < 0:
        raise BazelWorkspaceError("performance warmups must be non-negative")
    if request.library_root is not None and not request.library_root.is_dir():
        raise BazelWorkspaceError(f"Library root is not a directory: {request.library_root}")
    return config, host


def _revision() -> str:
    revision = capture(git_files.git_command("rev-parse", "--verify", "HEAD"), cwd=PROJECT_ROOT).strip()
    # Observational status must not persist stat-only index refreshes or hide
    # untracked/submodule changes through inherited Git configuration.
    if capture(
        git_files.git_command(
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
    # Reject collisions before deleting either artifact.
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


def run(request: Request) -> int:
    config, host = validate_request(request)
    with bazel_workflows.prepare_artifact(request, TARGET_LABEL, no_build=request.no_build) as artifact:
        output = (
            request.output if request.output is not None else artifact.tree / "performance-review.json"
        ).absolute()
        baseline_request = os.environ.get("AOBUS_PERF_BASELINE_JSON", "")
        baseline = Path(baseline_request).absolute() if baseline_request else None
        # Never attribute a fresh revision to an existing --no-build executable.
        revision = "unverified" if request.no_build else _revision()
        _prepare_outputs(output, baseline)
        environment = {
            **artifact.environment(),
            "AOBUS_PERF_REPORT_JSON": str(output),
            "AOBUS_PERF_BASELINE_JSON": str(baseline) if baseline is not None else "",
            "AOBUS_PERF_SAMPLES": str(request.samples),
            "AOBUS_PERF_WARMUPS": str(request.warmups),
            "AOBUS_PERF_REVISION": revision,
            "AOBUS_PERF_COMPILER": artifact.compiler_identity,
            "AOBUS_PERF_BUILD_MODE": config,
            "AOBUS_PERF_PLATFORM": host,
            # Empty values clear inherited dataset requests.
            "AOBUS_PERF_LIBRARY_ROOT": str(request.library_root) if request.library_root is not None else "",
            "AOBUS_PERF_LIBRARY_LOCALE": request.library_locale if request.library_root is not None else "",
        }
        if run_process([str(artifact.executable), request.filter], env=os.environ | environment) != 0:
            raise die("performance review workload failed")
        if baseline is not None:
            _validate_baseline_report(baseline)
        provenance = artifact.provenance() | {"source_attribution": "unverified" if request.no_build else "built"}
        _record_build_provenance(output, environment, provenance)
        _print_report(output)
        return 0
