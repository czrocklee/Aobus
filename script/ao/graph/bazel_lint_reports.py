"""Read only the current Bazel lint output group, then gate complete context reports."""

from __future__ import annotations

import json
import re
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlparse
from urllib.request import url2pathname

from ao.core.paths import PROJECT_ROOT
from ao.graph.bazel_lint_schema import (
    Identity,
    canonical_owner,
    completed_outcome,
    exec_path,
    exec_paths,
    json_object,
    load_json,
    nonempty_text,
    unique_object,
)
from ao.policy import dedup


@dataclass(frozen=True)
class Report:
    identity: Identity
    outcome: str
    tool_exit: int
    report: Path
    log: Path
    # Checkout files the analysis included: consumer evidence for headers and
    # fragments that have no companion translation unit.
    includes: tuple[str, ...] = ()


@dataclass(frozen=True)
class Uncovered:
    path: str
    reason: str


@dataclass(frozen=True)
class OwnerInventory:
    owner: str
    configuration: str
    bep_configuration: str
    reports: tuple[Report, ...]
    sources: tuple[str, ...]
    uncovered: tuple[Uncovered, ...]
    execroot: Path
    _omitted_native_tu: bool = False
    # An upper bound, not observed includes. None cannot rule out a consumer.
    candidate_inputs: frozenset[str] | None = None

    @property
    def has_native_tu(self) -> bool:
        """Whether the complete index, not just the selected groups, has a native TU."""
        return self._omitted_native_tu or any(r.identity.main == r.identity.source for r in self.reports)


def _event_key(value: object) -> str:
    data = json_object(value, "BEP event id")
    if len(data) != 1 or not isinstance(next(iter(data.values())), dict):
        raise ValueError("invalid BEP event identity")
    return json.dumps(data, sort_keys=True)


def _array(value: object, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise ValueError(f"{label}: expected an array")
    return value


def _events(bep: Path) -> list[dict[str, Any]]:
    events = [
        json_object(json.loads(line, object_pairs_hook=unique_object), "BEP event")
        for line in bep.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if not events or events[-1].get("lastMessage") is not True or any(e.get("lastMessage") for e in events[:-1]):
        raise ValueError("BEP stream is incomplete")
    if events[0].get("id") != {"started": {}} or not isinstance(events[0].get("started"), dict):
        raise ValueError("BEP stream has no build start")
    identities: set[str] = set()
    children: set[str] = set()
    finished = []
    for event in events:
        key = _event_key(event.get("id"))
        if key in identities:
            raise ValueError("duplicate BEP event identity")
        identities.add(key)
        children.update(_event_key(child) for child in _array(event.get("children", []), "BEP children"))
        if "aborted" in event:
            raise ValueError("aborted BEP event")
        if "finished" in event:
            if event["id"] != {"buildFinished": {}}:
                raise ValueError("wrong buildFinished identity")
            finished.append(json_object(event["finished"], "finished"))
    if children - identities:
        raise ValueError("missing announced BEP events")
    if len(finished) != 1:
        raise ValueError("missing/duplicate build completion")
    exit_code = json_object(finished[0].get("exitCode"), "build exit code")
    if exit_code.get("name") != "SUCCESS" or type(exit_code.get("code", 0)) is not int or exit_code.get("code", 0) != 0:
        raise ValueError("BEP does not establish a successful report build")
    if finished[0].get("overallSuccess", True) is not True:
        raise ValueError("unsuccessful BEP build")
    return events


def _local_file(value: object) -> tuple[str, Path]:
    data = json_object(value, "output file")
    prefix = _array(data.get("pathPrefix", []), "file pathPrefix")
    name = exec_path(
        "/".join([*(nonempty_text(p, "path prefix") for p in prefix), nonempty_text(data.get("name"), "file name")])
    )
    parsed = urlparse(nonempty_text(data.get("uri"), "local output URI"))
    if parsed.scheme != "file" or parsed.netloc not in ("", "localhost") or parsed.query or parsed.fragment:
        raise ValueError("lint outputs require locally materialized file URIs")
    path = Path(url2pathname(parsed.path))
    if not path.is_absolute() or not path.as_posix().endswith("/" + name) or not path.is_file():
        raise ValueError(f"missing or misidentified local output: {name}")
    return name, path


def _owner_directory(owner: str, configuration: str) -> tuple[str, str]:
    repository, target = owner.split("//", 1)
    package, name = target.split(":")
    external = f"external/{repository[2:]}/" if repository != "@@" else ""
    directory = configuration + "/" + external + (package + "/" if package else "")
    return directory, directory + name + ".aobus-lint.index.json"


_FULL = "aobus_lint"
_INDEX = "aobus_lint_index"
_SOURCES = "aobus_lint_sources"
_MAIN = "aobus_lint_main_"

CHECKER_VERIFICATION = "//tool/lint:verification"


def _checker_binding(
    owner: str, root_configuration: str, groups: dict[str, dict[str, Path]], index_group: str
) -> tuple[str, str]:
    receipt_name = root_configuration + "/tool/lint/verification.json"
    receipt = groups.get(index_group, {}).pop(receipt_name, None)
    if receipt is None:
        raise ValueError(f"missing current checker verification in {index_group}: {receipt_name}")
    if any(name.endswith("/tool/lint/verification.json") for files in groups.values() for name in files):
        raise ValueError("unexpected additional checker verification output")
    data = load_json(receipt)
    if data.get("verified") is not True:
        raise ValueError("checker verification did not verify the native tool")
    sdk = json_object(data.get("sdk"), "checker verification SDK")
    platform = nonempty_text(sdk.get("platform"), "checker SDK platform")
    if platform not in ("linux", "macos", "windows"):
        raise ValueError("unsupported checker SDK platform")
    target = re.fullmatch(r"@@//tool/lint:([^/]+)", owner)
    if target is None:
        raise ValueError("expected checker owner must be a concrete target in //tool/lint")
    if platform == "windows":
        if sdk.get("host_kind") != "standalone" or "plugin" not in data or data["plugin"] is not None:
            raise ValueError("Windows checker verification must use the standalone host")
        # The verifier records str(Path), which uses backslashes on Windows.
        artifact = exec_path(nonempty_text(data.get("tidy"), "standalone checker path").replace("\\", "/"))
    else:
        if sdk.get("host_kind") != "plugin":
            raise ValueError("Unix checker verification must use the native plugin")
        artifact = exec_path(data.get("plugin"))
    # The scheduled verifier sees the declared checker File through an exec
    # dependency. Its path identifies the actual bin directory; the mnemonic
    # need not have any particular spelling or be published as a BEP event.
    binding = re.fullmatch(r"(bazel-out/[^/]+/bin)/tool/lint/([^/]+)", artifact)
    if binding is None or binding[1] == root_configuration:
        raise ValueError("checker verification does not describe a distinct native tool configuration")
    if binding[2] != target[1] + (".exe" if platform == "windows" else ""):
        raise ValueError("checker verification does not match the expected owner")
    return binding[1], platform


def main_output_group(path: str, *, windows: bool) -> str | None:
    """Encode the producer's fixed main group, or decline an unsupported spelling."""
    if not re.fullmatch(r"[a-zA-Z0-9_/.-]+", path):
        return None
    key = path.lower() if windows else path
    return _MAIN + key.replace("_", "__").replace("/", "_s").replace(".", "_d").replace("-", "_h")


def _candidate_inputs(value: object) -> frozenset[str] | None:
    """Read optional checkout inputs, which may name files or directories."""
    if value is None:
        return None
    try:
        paths = exec_paths(value, "candidate inputs")
    except ValueError:
        return None
    if any(path.split("/", 1)[0] in ("bazel-out", "external") for path in paths):
        return None
    return frozenset(paths)


def _inventory(
    owner: str,
    config_id: str,
    configuration: str,
    groups: dict[str, dict[str, Path]],
    requested: frozenset[str],
    execroot: Path,
    allowed_modes: frozenset[str],
    *,
    checker_platform: str | None = None,
    input_index: bool = False,
) -> OwnerInventory:
    directory, index_name = _owner_directory(owner, configuration)
    full = _FULL in requested
    index_group = _FULL if full else _INDEX
    index_files = groups.get(index_group, {})
    if index_name not in index_files or (not full and len(index_files) != 1):
        raise ValueError(f"expected current owner index in {index_group}: {index_name}")
    data = load_json(index_files[index_name])
    if data.get("owner") != owner or data.get("configuration") != configuration:
        raise ValueError("index owner/configuration mismatch")
    sources = exec_paths(data.get("sources"), "eligible owner sources")
    uncovered: list[Uncovered] = []
    for item in _array(data.get("uncovered"), "uncovered"):
        item = json_object(item, "uncovered entry")
        uncovered.append(Uncovered(exec_path(item.get("path")), nonempty_text(item.get("reason"), "coverage reason")))
    uncovered_paths = {u.path for u in uncovered}
    if len(uncovered_paths) != len(uncovered) or not uncovered_paths.issubset(sources):
        raise ValueError("duplicate or foreign uncovered source")
    reports: list[Report] = []
    identities: set[Identity] = set()
    indexed_paths = {index_name}
    accounted = {name: set[str]() for name in requested}
    accounted[index_group].add(index_name)
    omitted_native_tu = False
    for entry in _array(data.get("reports"), "indexed reports"):
        entry = json_object(entry, "report index entry")
        identity = Identity.from_json(entry.get("identity"))
        if identity.mode not in allowed_modes:
            raise ValueError(f"report context mode is foreign to the requested analysis kind: {identity.mode}")
        if (
            identity.owner != owner
            or identity.configuration != configuration
            or (checker_platform is not None and identity.platform != checker_platform)
            or identity.source not in sources
            or identity.main not in sources
            or any(not output.startswith(directory) for output in identity.outputs)
        ):
            raise ValueError("foreign report context")
        if identity in identities or identity.main in uncovered_paths:
            raise ValueError("duplicate or simultaneously uncovered report identity")
        identities.add(identity)
        report_name, log_name = exec_path(entry.get("report")), exec_path(entry.get("log"))
        for name in (report_name, log_name):
            if not name.startswith(directory) or name in indexed_paths:
                raise ValueError(f"duplicate or foreign indexed report/log: {name}")
            indexed_paths.add(name)

        # Require the pair independently in *each* matching group. A flat union
        # could hide a missing main group behind overlapping discovery outputs.
        native_tu = identity.main == identity.source
        selected = [_FULL] if full else []
        if not full:
            main = main_output_group(identity.main, windows=identity.platform == "windows")
            if main is not None and main in requested:
                selected.append(main)
            if native_tu and _SOURCES in requested:
                selected.append(_SOURCES)
        if not selected:
            omitted_native_tu |= native_tu
            continue
        for group in selected:
            files = groups.get(group, {})
            if report_name not in files or log_name not in files:
                raise ValueError(f"missing current report/log in {group}: {report_name}")
            accounted[group].update((report_name, log_name))
        files = groups[selected[0]]
        report_path, log = files[report_name], files[log_name]
        report = load_json(report_path)
        if Identity.from_json(report.get("identity")) != identity:
            raise ValueError("report identity differs from its current index")
        outcome = completed_outcome(log, report.get("tool_exit"))
        if report.get("outcome") != outcome:
            raise ValueError("report outcome differs from its completed log")
        includes = exec_paths(report.get("includes"), "report includes")
        reports.append(Report(identity, outcome, report["tool_exit"], report_path, log, includes))
    if set(sources) != {identity.main for identity in identities} | uncovered_paths:
        raise ValueError("eligible owner inventory has unaccounted sources")
    for group, files in groups.items():
        extras = files.keys() - accounted[group]
        if extras and (not full or any(p.endswith((".report.json", ".log", ".aobus-lint.index.json")) for p in extras)):
            raise ValueError(f"unexpected lint output in {group}")
    return OwnerInventory(
        owner,
        configuration,
        config_id,
        tuple(reports),
        sources,
        tuple(uncovered),
        execroot,
        omitted_native_tu,
        candidate_inputs=_candidate_inputs(data.get("candidate_inputs")) if input_index else None,
    )


def _group_files(group: dict[str, Any], sets: dict[str, dict[str, Any]]) -> tuple[dict[str, Path], Path | None]:
    files: dict[str, Path] = {}
    visited: set[str] = set()
    roots: set[Path] = set()

    def add(entries: object) -> None:
        for item in _array(entries, "output files"):
            name, path = _local_file(item)
            root = path.parents[len(name.split("/")) - 1]
            roots.add(root)
            if len(roots) != 1 or name in files or path in files.values():
                raise ValueError("duplicate lint output file or mixed execroots")
            files[name] = path

    def visit(identifier: str, active: frozenset[str]) -> None:
        if identifier in active or identifier not in sets or len(active) > 256:
            raise ValueError("cyclic/missing or excessive named file set nesting")
        if identifier in visited:
            return  # A depset DAG may share a child; its files are still unique.
        value = sets[identifier]
        add(value.get("files", []))
        for child in _array(value.get("fileSets", []), "child named sets"):
            visit(
                nonempty_text(json_object(child, "named set reference").get("id"), "named set id"),
                active | {identifier},
            )
        visited.add(identifier)

    add(group.get("inlineFiles", []))
    for reference in _array(group.get("fileSets", []), "group named sets"):
        visit(nonempty_text(json_object(reference, "named set reference").get("id"), "named set id"), frozenset())
    return files, next(iter(roots), None)


_LINT_ASPECT_BASE = "//bazel/lint:lint.bzl%lint_aspect"
_ANALYZER_ASPECT_BASE = "//bazel/lint:lint.bzl%analyzer_aspect"
_ANALYSIS_MODES = frozenset({"STRICT", "RELAXED"})
_ANALYZER_MODES = frozenset({"ANALYZER"})


def lint_aspect_id(consumers: str = "{}", *, profile: str = "lint", input_index: bool = False) -> str:
    """Return the exact BEP identity for the requested production aspect."""
    if not isinstance(consumers, str):
        raise ValueError("lint consumers must be a string")
    if not isinstance(input_index, bool):
        raise ValueError("lint input index selection must be a bool")
    parameters = "aobus_lint_consumers=" + json.dumps(consumers)
    if profile == "lint":
        mode = '"on"' if input_index else '"off"'
        return f"{_LINT_ASPECT_BASE}[{parameters},aobus_lint_inputs={mode}]"
    if profile == "analyzer":
        if input_index:
            raise ValueError("analyzer does not publish a scoped lint input index")
        return f"{_ANALYZER_ASPECT_BASE}[{parameters}]"
    raise ValueError("analysis kind must be lint or analyzer")


def read_reports(
    bep: Path,
    owners: list[str],
    *,
    consumers: str = "{}",
    profile: str = "lint",
    output_groups: Sequence[str] = (_FULL,),
    checker_owner: str | None = None,
    input_index: bool = False,
) -> tuple[OwnerInventory, ...]:
    """Bind exactly one current aspect completion to each requested root.

    Only a completion of the exact aspect ID with its requested parameters
    binds an owner; all owners in this BEP must share one execution root,
    including when their reports are used only for donor discovery.
    Unparameterized, foreign, differently parameterized or other-profile
    aspects cannot stand in for the requested one, and every report identity
    must carry exactly the profile's mode set. The consumer payload spelling
    is the producer's authority. The caller must first require the native
    Bazel process to exit successfully. No retained-directory scan or
    action-execution inventory is used here.

    If requested, the concrete checker owner is published by the verification
    root. Its inventory keeps that completion's BEP ID, but its native bin
    directory comes from the verified checker artifact, not the publishing root.
    """
    expected = [canonical_owner(owner) for owner in owners]
    roots = set(expected)
    if not expected or len(roots) != len(expected):
        raise ValueError("expected owners must be nonempty and unique")
    verification = canonical_owner(CHECKER_VERIFICATION)
    checker = canonical_owner(checker_owner) if checker_owner is not None else None
    if checker is not None:
        if checker not in roots or verification in roots:
            raise ValueError("expected owners must contain the concrete checker, not the verification root")
        roots.remove(checker)
        roots.add(verification)
    expected_aspect = lint_aspect_id(consumers, profile=profile, input_index=input_index)
    allowed_modes = _ANALYZER_MODES if profile == "analyzer" else _ANALYSIS_MODES
    requested = frozenset(output_groups)
    if len(requested) != len(output_groups):
        raise ValueError("duplicate requested lint output group")
    if input_index and _FULL in requested:
        raise ValueError("lint input evidence requires scoped output groups")
    if requested != {_FULL} and (
        profile != "lint"
        or _INDEX not in requested
        or any(not re.fullmatch(_MAIN + r"(?:[a-zA-Z0-9]|_[sdh_])+", g) for g in requested - {_INDEX, _SOURCES})
    ):
        raise ValueError("invalid scoped lint output groups")
    events = _events(bep)
    configurations: dict[str, dict[str, Any]] = {}
    sets: dict[str, dict[str, Any]] = {}
    completions: dict[str, tuple[str, dict[str, Any]]] = {}
    for event in events:
        identity = event["id"]
        if "configuration" in identity and "configuration" in event:
            config_id = nonempty_text(
                json_object(identity["configuration"], "configuration id").get("id"), "configuration id"
            )
            if config_id in configurations:
                raise ValueError("duplicate full configuration id")
            configurations[config_id] = json_object(event["configuration"], "configuration")
        if "namedSetOfFiles" in event:
            set_id = nonempty_text(json_object(identity.get("namedSet"), "named set id").get("id"), "named set id")
            if set_id in sets:
                raise ValueError("duplicate named file set id")
            sets[set_id] = json_object(event["namedSetOfFiles"], "named set")
        target = json_object(identity.get("targetCompleted", {}), "targetCompleted id")
        if target.get("aspect") != expected_aspect:
            continue
        root_owner = canonical_owner(target.get("label"))
        if root_owner not in roots or root_owner in completions:
            raise ValueError("unexpected/duplicate configured lint owner completion")
        complete = json_object(event.get("completed"), "target completion")
        if complete.get("success") is not True:
            raise ValueError("owning lint aspect did not complete")
        config_id = nonempty_text(
            json_object(target.get("configuration"), "target configuration").get("id"), "configuration id"
        )
        completions[root_owner] = config_id, complete
    if set(completions) != roots:
        raise ValueError("missing requested lint owner completion")
    result = []
    execroot: Path | None = None
    for owner in expected:
        is_checker = owner == checker
        config_id, complete = completions[verification if is_checker else owner]
        if config_id not in configurations:
            raise ValueError("missing full BEP configuration")
        config = configurations[config_id]
        mnemonic = exec_path(config.get("mnemonic"))
        if "/" in mnemonic:
            raise ValueError("invalid configuration mnemonic")
        configuration = f"bazel-out/{mnemonic}/bin"
        variables = json_object(config.get("makeVariable", {}), "configuration variables")
        if variables.get("BINDIR", configuration) != configuration:
            raise ValueError("BEP configuration bin directory mismatch")
        groups: dict[str, dict[str, Path]] = {}
        for item in _array(complete.get("outputGroup"), "output groups"):
            group = json_object(item, "output group")
            name = nonempty_text(group.get("name"), "output group name")
            if name not in requested:
                if (is_checker or _FULL not in requested) and name.startswith(_FULL):
                    raise ValueError(f"unexpected lint output group: {name}")
                continue
            if name in groups or group.get("incomplete", False) is not False:
                raise ValueError(f"duplicate/incomplete lint output group: {name}")
            groups[name], root = _group_files(group, sets)
            if root is not None:
                if execroot is not None and root != execroot:
                    raise ValueError("lint reports come from multiple execution roots")
                execroot = root
        if execroot is None:
            raise ValueError("lint output groups require a local execution root")
        checker_platform = None
        if is_checker:
            if config.get("isTool", False) is not False:
                raise ValueError("checker verification root must use the target configuration")
            configuration, checker_platform = _checker_binding(
                owner, configuration, groups, _FULL if _FULL in requested else _INDEX
            )
        result.append(
            _inventory(
                owner,
                config_id,
                configuration,
                groups,
                requested,
                execroot,
                allowed_modes,
                checker_platform=checker_platform,
                input_index=input_index,
            )
        )
    return tuple(result)


def _display_mapper(execroot: Path) -> Callable[[Path], Path]:
    """Map execution-relative log paths to checkout sources or retained outputs.

    Reports spell paths relative to the execution root. Source paths therefore
    resolve in the checkout, while generated, virtual-include and external
    paths resolve in the execution root that produced this gate's outputs.
    """

    def mapper(path: Path) -> Path:
        try:
            relative = path.relative_to(PROJECT_ROOT)
        except ValueError:
            return path
        if relative.parts[:1] in (("bazel-out",), ("external",)):
            return execroot / relative
        return path

    return mapper


def gate_reports(
    inventories: Sequence[OwnerInventory],
    selected: set[str],
    output: Path | None = None,
    *,
    reasons: Mapping[str, str] | None = None,
) -> int:
    """Gate whole main/context reports: incomplete=2, findings=1, complete clean=0.

    A selected path is covered when any requested owner reports it as a main.
    Another owner may list the same interface header as uncovered because it
    has no companion there; that listing does not make the path incomplete.
    A caller's reason replaces the index reasons for a path no report covers.
    """
    reasons = reasons or {}
    chosen = [report for inventory in inventories for report in inventory.reports if report.identity.main in selected]
    covered = {report.identity.main for report in chosen}
    uncovered = [
        u
        for inventory in inventories
        for u in inventory.uncovered
        if u.path in selected and u.path not in covered and u.path not in reasons
    ]
    missing = selected - covered - {u.path for u in uncovered}
    messages = [
        f"{path}: {reasons[path]}" if path in reasons else f"Uncovered lint source: {path}" for path in sorted(missing)
    ]
    messages.extend(f"{u.path}: {u.reason}" for u in sorted(set(uncovered), key=lambda u: (u.path, u.reason)))
    try:
        execroots = {inventory.execroot for inventory in inventories}
        if len(execroots) > 1:
            raise ValueError("lint reports come from multiple execution roots")
        mapper = _display_mapper(next(iter(execroots))) if execroots else None
        # Recheck availability if a caller retained the typed inventory beyond read_reports.
        for report in chosen:
            if completed_outcome(report.log, report.tool_exit) != report.outcome:
                raise ValueError("report log changed after inventory read")
        logs = (r.log for r in chosen)
        if output is None:
            dedup.deduplicate(logs, sys.stdout, PROJECT_ROOT, path_mapper=mapper)
            for message in messages:
                print(message, file=sys.stderr)
        else:
            with output.open("w", encoding="utf-8") as sink:
                dedup.deduplicate(logs, sink, PROJECT_ROOT, path_mapper=mapper)
                sink.writelines(message + "\n" for message in messages)
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Cannot gate lint reports: {error}", file=sys.stderr)
        return 2
    return 2 if missing or uncovered else int(any(report.outcome != "clean" for report in chosen))


def gate_analyzer_reports(
    inventories: Sequence[OwnerInventory],
    selected: set[str],
    output: Path | None = None,
    *,
    include_external: bool = True,
    fail_on_diagnostics: bool = False,
    reasons: Mapping[str, str] | None = None,
) -> int:
    """Report analyzer findings: tool failures stay nonzero, findings are gated.

    Keeps the analyze command's report-only contract. A selected path without
    a completed report and a compiler-error report are tool failures and always
    return non-zero. Warning findings are deduplicated, counted and reported,
    and only the --fail-on-diagnostics gate turns them into a failure.
    """
    reasons = reasons or {}
    chosen = [report for inventory in inventories for report in inventory.reports if report.identity.main in selected]
    covered = {report.identity.main for report in chosen}
    uncovered = [
        u
        for inventory in inventories
        for u in inventory.uncovered
        if u.path in selected and u.path not in covered and u.path not in reasons
    ]
    missing = selected - covered - {u.path for u in uncovered}
    messages = [
        f"{path}: {reasons[path]}" if path in reasons else f"Uncovered analyzer source: {path}"
        for path in sorted(missing)
    ]
    messages.extend(f"{u.path}: {u.reason}" for u in sorted(set(uncovered), key=lambda u: (u.path, u.reason)))
    try:
        execroots = {inventory.execroot for inventory in inventories}
        if len(execroots) > 1:
            raise ValueError("analyzer reports come from multiple execution roots")
        mapper = _display_mapper(next(iter(execroots))) if execroots else None
        # Recheck availability if a caller retained the typed inventory beyond read_reports.
        for report in chosen:
            if completed_outcome(report.log, report.tool_exit) != report.outcome:
                raise ValueError("report log changed after inventory read")
        failures = [report for report in chosen if report.outcome == "compiler_error"]
        logs = [report.log for report in chosen if report.outcome != "clean"]
        if output is None:
            count = dedup.deduplicate(
                logs, sys.stdout, PROJECT_ROOT, include_external=include_external, path_mapper=mapper
            )
            for message in messages:
                print(message, file=sys.stderr)
        else:
            with output.open("w", encoding="utf-8") as sink:
                count = dedup.deduplicate(
                    logs, sink, PROJECT_ROOT, include_external=include_external, path_mapper=mapper
                )
                sink.writelines(message + "\n" for message in messages)
            print(f"Output written to {output}")
        if failures:
            print("Analyzer tool failure output:", file=sys.stderr)
            for report in failures:
                text = report.log.read_text(encoding="utf-8", errors="replace") if report.log.is_file() else ""
                if text:
                    print(f"=== {report.identity.main} ===", file=sys.stderr)
                    sys.stderr.write("".join(text.splitlines(keepends=True)[:120]))
        if count == 0:
            print("No analyzer diagnostics found.")
        else:
            print(f"Analyzer diagnostics: {count}")
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Cannot gate analyzer reports: {error}", file=sys.stderr)
        return 2
    if missing or uncovered:
        return 1
    if failures:
        return 1
    return 1 if count and fail_on_diagnostics else 0
