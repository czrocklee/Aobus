"""Read only the current Bazel lint output group, then gate complete context reports."""

from __future__ import annotations

import json
import re
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlparse
from urllib.request import url2pathname

from . import dedup
from .paths import PROJECT_ROOT
from .tidydiagnostics import has_fatal_compiler_error, log_has_compile_command_error, parse_diagnostic_header


def _object(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label}: expected an object")
    return value


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def load_json(path: Path) -> dict[str, Any]:
    """Read a versioned object, rejecting ambiguous keys and unknown schemas."""
    value = _object(json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object), str(path))
    if type(value.get("schema")) is not int or value["schema"] != 1:
        raise ValueError(f"{path}: unsupported schema")
    return value


def _text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"{label}: expected a nonempty string")
    return value


def exec_path(value: object) -> str:
    """Validate the exact portable exec-relative spelling emitted by native Files."""
    path = _text(value, "exec path")
    if "\\" in path or ":" in path or path.startswith("/") or any(p in ("", ".", "..") for p in path.split("/")):
        raise ValueError(f"invalid exec-relative path: {path}")
    return path


def _strings(value: object, label: str) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise ValueError(f"{label}: expected an array")
    result = tuple(exec_path(item) for item in value)
    if len(set(result)) != len(result):
        raise ValueError(f"{label}: duplicate paths")
    return result


def canonical_owner(value: object) -> str:
    owner = _text(value, "owner")
    if owner.startswith("//"):
        owner = "@@" + owner
    elif owner.startswith("@//"):
        owner = "@" + owner
    if re.fullmatch(r"@@[^/]*//[^:]*:[^:]+", owner) is None:
        raise ValueError(f"invalid configured owner: {owner}")
    package, name = owner.split("//", 1)[1].split(":")
    if package:
        exec_path(package)
    exec_path(name)
    return owner


@dataclass(frozen=True)
class Identity:
    owner: str
    configuration: str
    source: str
    main: str
    outputs: tuple[str, ...]
    mode: str
    platform: str

    @classmethod
    def from_json(cls, value: object) -> Identity:
        data = _object(value, "identity")
        keys = {"owner", "configuration", "source", "main", "outputs", "mode", "platform"}
        if set(data) != keys:
            raise ValueError("identity must contain exactly the context identity fields")
        owner = canonical_owner(data["owner"])
        if data["owner"] != owner:
            raise ValueError("identity owner must be canonical")
        configuration = exec_path(data["configuration"])
        if re.fullmatch(r"bazel-out/[^/]+/bin", configuration) is None:
            raise ValueError("invalid native bin directory")
        outputs = _strings(data["outputs"], "native outputs")
        if not outputs or any(not path.startswith(configuration + "/") for path in outputs):
            raise ValueError("native outputs do not belong to the context configuration")
        if data["mode"] not in ("STRICT", "RELAXED") or data["platform"] not in ("linux", "macos", "windows"):
            raise ValueError("unsupported context mode/platform")
        return cls(
            owner,
            configuration,
            exec_path(data["source"]),
            exec_path(data["main"]),
            outputs,
            data["mode"],
            data["platform"],
        )

    def as_json(self) -> dict[str, Any]:
        return {**asdict(self), "outputs": list(self.outputs)}


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


# These are infrastructure failures even when clang-tidy exits zero or prints a
# compiler diagnostic before failing. Ordinary source errors remain cacheable.
_INCOMPLETE = re.compile(
    r"^(?:Skipping\b|Skipped\b|No (?:input files|checks enabled|compile commands)|"
    r"Unable to (?:handle compilation|load)|Unable to find compiler|"
    r"LLVM ERROR:|PLEASE submit a bug report|Stack dump:|"
    r"(?:clang-tidy(?:\.exe)?:\s*)?(?:error:\s*)?(?:"
    r"unknown (?:command line )?argument|unknown (?:command line )?option|no checks enabled|"
    r"invalid configuration|error parsing (?:-config|configuration)|could not process))",
    re.IGNORECASE | re.MULTILINE,
)


def completed_outcome(log: Path, tool_exit: object) -> str:
    """Classify a completed attempt; never convert an incomplete attempt to clean."""
    text = log.read_text(encoding="utf-8")
    # clang-tidy prints this summary for ordinary source errors too.
    without_summary = re.sub(r"^Error while processing .+\.\s*$", "", text, flags=re.MULTILINE)
    if (
        log_has_compile_command_error(log)
        or _INCOMPLETE.search(without_summary)
        or re.search(r"^.*: error: clang frontend command failed", without_summary, re.MULTILINE)
    ):
        raise ValueError(f"incomplete analyzer attempt: {log}")
    compiler_error = has_fatal_compiler_error(text)
    if without_summary != text and not compiler_error:
        raise ValueError("processing failed without an ordinary compiler diagnostic")
    if type(tool_exit) is not int or tool_exit not in (0, 1) or (tool_exit == 1 and not compiler_error):
        raise ValueError(f"unexpected analyzer exit: {tool_exit}")
    if compiler_error:
        # Tool/config/driver failures must not masquerade as source compilation.
        if not has_fatal_compiler_error(text, tag="[clang-diagnostic-error]"):
            raise ValueError("error without an ordinary clang compiler diagnostic")
        return "compiler_error"
    if any((header := parse_diagnostic_header(line)) and header.severity == "warning" for line in text.splitlines()):
        return "warning"
    return "clean"


def _event_key(value: object) -> str:
    data = _object(value, "BEP event id")
    if len(data) != 1 or not isinstance(next(iter(data.values())), dict):
        raise ValueError("invalid BEP event identity")
    return json.dumps(data, sort_keys=True)


def _array(value: object, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise ValueError(f"{label}: expected an array")
    return value


def _events(bep: Path) -> list[dict[str, Any]]:
    events = [
        _object(json.loads(line, object_pairs_hook=_unique_object), "BEP event")
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
            finished.append(_object(event["finished"], "finished"))
    if children - identities:
        raise ValueError("missing announced BEP events")
    if len(finished) != 1:
        raise ValueError("missing/duplicate build completion")
    exit_code = _object(finished[0].get("exitCode"), "build exit code")
    if exit_code.get("name") != "SUCCESS" or type(exit_code.get("code", 0)) is not int or exit_code.get("code", 0) != 0:
        raise ValueError("BEP does not establish a successful report build")
    if finished[0].get("overallSuccess", True) is not True:
        raise ValueError("unsuccessful BEP build")
    return events


def _local_file(value: object) -> tuple[str, Path]:
    data = _object(value, "output file")
    prefix = _array(data.get("pathPrefix", []), "file pathPrefix")
    name = exec_path("/".join([*(_text(p, "path prefix") for p in prefix), _text(data.get("name"), "file name")]))
    parsed = urlparse(_text(data.get("uri"), "local output URI"))
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


def _inventory(
    owner: str, config_id: str, configuration: str, files: dict[str, Path], execroot: Path
) -> OwnerInventory:
    directory, index_name = _owner_directory(owner, configuration)
    if index_name not in files:
        raise ValueError(f"missing current owner index: {index_name}")
    data = load_json(files[index_name])
    if data.get("owner") != owner or data.get("configuration") != configuration:
        raise ValueError("index owner/configuration mismatch")
    sources = _strings(data.get("sources"), "eligible owner sources")
    uncovered: list[Uncovered] = []
    for item in _array(data.get("uncovered"), "uncovered"):
        item = _object(item, "uncovered entry")
        uncovered.append(Uncovered(exec_path(item.get("path")), _text(item.get("reason"), "coverage reason")))
    if len({u.path for u in uncovered}) != len(uncovered) or any(u.path not in sources for u in uncovered):
        raise ValueError("duplicate or foreign uncovered source")
    reports: list[Report] = []
    identities: set[Identity] = set()
    used = {index_name}
    for entry in _array(data.get("reports"), "indexed reports"):
        entry = _object(entry, "report index entry")
        identity = Identity.from_json(entry.get("identity"))
        if (
            identity.owner != owner
            or identity.configuration != configuration
            or identity.source not in sources
            or identity.main not in sources
            or any(not output.startswith(directory) for output in identity.outputs)
        ):
            raise ValueError("foreign report context")
        if identity in identities or identity.main in {u.path for u in uncovered}:
            raise ValueError("duplicate or simultaneously uncovered report identity")
        identities.add(identity)
        paths = []
        for key in ("report", "log"):
            name = exec_path(entry.get(key))
            if not name.startswith(directory) or name not in files or name in used:
                raise ValueError(f"missing, duplicate or foreign current {key}: {name}")
            used.add(name)
            paths.append(files[name])
        report_path, log = paths
        report = load_json(report_path)
        if Identity.from_json(report.get("identity")) != identity:
            raise ValueError("report identity differs from its current index")
        outcome = completed_outcome(log, report.get("tool_exit"))
        if report.get("outcome") != outcome:
            raise ValueError("report outcome differs from its completed log")
        includes = _strings(report.get("includes"), "report includes")
        reports.append(Report(identity, outcome, report["tool_exit"], report_path, log, includes))
    if set(sources) != {r.identity.main for r in reports} | {u.path for u in uncovered}:
        raise ValueError("eligible owner inventory has unaccounted sources")
    if any(name.endswith((".report.json", ".log", ".aobus-lint.index.json")) and name not in used for name in files):
        raise ValueError("unindexed lint output")
    return OwnerInventory(owner, configuration, config_id, tuple(reports), sources, tuple(uncovered), execroot)


def _group_files(group: dict[str, Any], sets: dict[str, dict[str, Any]]) -> tuple[dict[str, Path], Path]:
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
            visit(_text(_object(child, "named set reference").get("id"), "named set id"), active | {identifier})
        visited.add(identifier)

    add(group.get("inlineFiles", []))
    for reference in _array(group.get("fileSets", []), "group named sets"):
        visit(_text(_object(reference, "named set reference").get("id"), "named set id"), frozenset())
    if len(roots) != 1:
        raise ValueError("lint output group has no local execution root")
    return files, next(iter(roots))


def read_reports(bep: Path, owners: list[str]) -> tuple[OwnerInventory, ...]:
    """Bind exactly one current lint aspect completion to each requested root.

    The caller must first require the native Bazel process to exit successfully.
    No retained-directory scan or action-execution inventory is used here.
    """
    expected = [canonical_owner(owner) for owner in owners]
    if not expected or len(set(expected)) != len(expected):
        raise ValueError("expected owners must be nonempty and unique")
    events = _events(bep)
    configurations: dict[str, dict[str, Any]] = {}
    sets: dict[str, dict[str, Any]] = {}
    completions: dict[str, tuple[str, dict[str, Any]]] = {}
    for event in events:
        identity = event["id"]
        if "configuration" in identity and "configuration" in event:
            config_id = _text(_object(identity["configuration"], "configuration id").get("id"), "configuration id")
            if config_id in configurations:
                raise ValueError("duplicate full configuration id")
            configurations[config_id] = _object(event["configuration"], "configuration")
        if "namedSetOfFiles" in event:
            set_id = _text(_object(identity.get("namedSet"), "named set id").get("id"), "named set id")
            if set_id in sets:
                raise ValueError("duplicate named file set id")
            sets[set_id] = _object(event["namedSetOfFiles"], "named set")
        target = _object(identity.get("targetCompleted", {}), "targetCompleted id")
        aspect = target.get("aspect", "")
        if not isinstance(aspect, str) or not aspect.endswith("%lint_aspect"):
            continue
        owner = canonical_owner(target.get("label"))
        if owner not in expected or owner in completions:
            raise ValueError("unexpected/duplicate configured lint owner completion")
        complete = _object(event.get("completed"), "target completion")
        if complete.get("success") is not True:
            raise ValueError("owning lint aspect did not complete")
        config_id = _text(_object(target.get("configuration"), "target configuration").get("id"), "configuration id")
        completions[owner] = config_id, complete
    if set(completions) != set(expected):
        raise ValueError("missing requested lint owner completion")
    result = []
    for owner in expected:
        config_id, complete = completions[owner]
        if config_id not in configurations:
            raise ValueError("missing full BEP configuration")
        config = configurations[config_id]
        mnemonic = exec_path(config.get("mnemonic"))
        if "/" in mnemonic:
            raise ValueError("invalid configuration mnemonic")
        configuration = f"bazel-out/{mnemonic}/bin"
        variables = _object(config.get("makeVariable", {}), "configuration variables")
        if variables.get("BINDIR", configuration) != configuration:
            raise ValueError("BEP configuration bin directory mismatch")
        groups = [_object(g, "output group") for g in _array(complete.get("outputGroup"), "output groups")]
        groups = [g for g in groups if g.get("name") == "aobus_lint"]
        if len(groups) != 1 or groups[0].get("incomplete", False) is not False:
            raise ValueError("missing/incomplete lint output group")
        files, execroot = _group_files(groups[0], sets)
        result.append(_inventory(owner, config_id, configuration, files, execroot))
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
