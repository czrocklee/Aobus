"""Portal orchestration for the native C++ stage of ``ao tidy``.

The portal owns scope selection, owner discovery, configured compatibility,
the report build and the public failure gate.  Bazel owns the actual native
compile contexts and report actions; this module never prepares CMake or runs
clang-tidy itself.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urlparse

from . import bazel_lint_reports, bazel_workspace, builddir, tidyengine
from . import bazel_native as native
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT, absolute_path

_ASPECT = "//bazel/lint:lint.bzl%lint_aspect"
_ANALYZER_ASPECT = "//bazel/lint:lint.bzl%analyzer_aspect"
_ANALYZER_TREE = "bazel-analyzer"
_OUTPUT_GROUP = "aobus_lint"
_QUERY_EXPRESSION = 'kind("rule", //...)'
_CQUERY_EXPRESSION = 'str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))'

_SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx", ".mm"))
_HEADER_SUFFIXES = frozenset((".h", ".hh", ".hpp", ".hxx"))
_INCLUDE_FRAGMENT_SUFFIXES = frozenset((".def",))
_LINT_SUFFIXES = tuple(sorted(_SOURCE_SUFFIXES | _HEADER_SUFFIXES | _INCLUDE_FRAGMENT_SUFFIXES))
_COMPILE_OWNER_KINDS = frozenset({"cc_library", "cc_binary", "cc_test", "objc_library"})
_PLATFORM_IMPLEMENTATION_SUFFIXES = ("", "Linux", "Macos", "Posix", "Windows")
_RETAINED_RUNS = 8
_NO_CONSUMER = "no analyzed translation unit includes it"
_LABEL = re.compile(r"^//([A-Za-z0-9._+~/-]*):([A-Za-z0-9._+~/-]+)$")

AttributeValue = tuple[str, ...] | str | bool | int


@dataclass(frozen=True)
class Rule:
    """One rule node from Bazel's query XML."""

    label: str
    kind: str
    attributes: Mapping[str, AttributeValue]


class QueryGraph:
    """The loaded project graph needed for real source/header ownership.

    Only rule ``srcs`` (recursively through filegroups and aliases) establish
    lint ownership.  A dependency closure is never treated as a lint scope.
    """

    def __init__(self, rules: Mapping[str, Rule]) -> None:
        self.rules = dict(rules)
        self._srcs: dict[str, tuple[str, ...]] = {}
        self._headers: dict[str, tuple[str, ...]] = {}
        self._closures: dict[str, frozenset[str]] = {}
        self._declarers: dict[str, frozenset[str]] | None = None

    def source_owners(self, path: str) -> tuple[str, ...]:
        """Return every compile owner whose actual ``srcs`` contain ``path``."""
        if not _is_source(path):
            return ()
        return tuple(
            label
            for label in sorted(self.rules)
            if self.rules[label].kind in _COMPILE_OWNER_KINDS and path in self._rule_srcs(label)
        )

    def header_owners(self, path: str) -> tuple[str, ...]:
        """Return owners with an actual same-component companion source in ``srcs``."""
        if not _is_header(path):
            return ()
        return tuple(
            label
            for label in sorted(self.rules)
            if self.rules[label].kind in _COMPILE_OWNER_KINDS
            and any(_paired(path, source) for source in self._rule_srcs(label) if _is_source(source))
        )

    def consumer_owners(self, path: str) -> tuple[str, ...]:
        """Return owners whose translation units can include ``path``.

        An owner qualifies when it has native sources and declares the path
        or reaches a declaring rule through ``deps``.  Only the includes that
        a report records establish which of them actually consume it.
        """
        if self._declarers is None:
            declarers: dict[str, set[str]] = {}
            for label in self.rules:
                if self.rules[label].kind in _COMPILE_OWNER_KINDS:
                    for header in self._rule_headers(label):
                        declarers.setdefault(header, set()).add(label)
            self._declarers = {header: frozenset(labels) for header, labels in declarers.items()}
        declaring = self._declarers.get(path, frozenset())
        if not declaring:
            return ()
        return tuple(
            label
            for label in sorted(self.rules)
            if self.rules[label].kind in _COMPILE_OWNER_KINDS
            and self.native_sources(label)
            and (label in declaring or not declaring.isdisjoint(self._closure(label, frozenset())))
        )

    def native_sources(self, label: str) -> tuple[str, ...]:
        """Return the native translation units declared by one owner."""
        return tuple(source for source in self._rule_srcs(label) if _is_source(source))

    def _closure(self, label: str, active: frozenset[str]) -> frozenset[str]:
        """Return the project rules ``label`` reaches through its dependencies."""
        if label in self._closures:
            return self._closures[label]
        if label in active or len(active) > 256:
            raise ValueError(f"cyclic or excessive Bazel query dependency edge: {label}")
        rule = self.rules.get(label)
        reached: set[str] = set()
        if rule is not None:
            for attribute in ("deps", "implementation_deps", "actual"):
                for value in _attribute_labels(rule, attribute):
                    try:
                        dependency = _query_label(value)
                    except ValueError:
                        if value.startswith("@") and not value.startswith("@//"):
                            continue
                        raise
                    reached.add(dependency)
                    reached.update(self._closure(dependency, active | {label}))
        self._closures[label] = frozenset(reached)
        return self._closures[label]

    def _rule_srcs(self, label: str) -> tuple[str, ...]:
        if label not in self._srcs:
            rule = self.rules.get(label)
            if rule is None:
                return ()
            self._srcs[label] = self._expand_attribute(rule, "srcs")
        return self._srcs[label]

    def _rule_headers(self, label: str) -> tuple[str, ...]:
        if label not in self._headers:
            rule = self.rules.get(label)
            if rule is None:
                return ()
            paths: list[str] = []
            for attribute in ("hdrs", "textual_hdrs", "additional_compiler_inputs"):
                paths.extend(self._expand_attribute(rule, attribute))
            paths.extend(path for path in self._rule_srcs(label) if _is_header(path))
            self._headers[label] = tuple(dict.fromkeys(paths))
        return self._headers[label]

    def _expand_attribute(self, rule: Rule, attribute: str) -> tuple[str, ...]:
        value = rule.attributes.get(attribute)
        labels: tuple[str, ...]
        if isinstance(value, str):
            labels = (value,)
        elif isinstance(value, tuple):
            labels = value
        else:
            return ()
        return self._expand_labels(labels, frozenset({rule.label}))

    def _expand_labels(self, labels: Sequence[str], active: frozenset[str]) -> tuple[str, ...]:
        if len(active) > 256:
            raise ValueError("excessive Bazel query filegroup/alias nesting")
        paths: list[str] = []
        for value in labels:
            try:
                label = _query_label(value)
            except ValueError:
                # External labels cannot own project source files.  Keep them
                # out of the project ownership graph rather than guessing.
                if value.startswith("@") and not value.startswith("@//"):
                    continue
                raise
            rule = self.rules.get(label)
            if rule is not None:
                if rule.kind == "filegroup":
                    paths.extend(self._expand_labels(_attribute_labels(rule, "srcs"), active | {label}))
                elif rule.kind == "alias":
                    paths.extend(self._expand_labels(_attribute_labels(rule, "actual"), active | {label}))
                continue
            path = _label_path(label)
            if path is not None:
                paths.append(path)
        return tuple(dict.fromkeys(paths))


def _attribute_labels(rule: Rule, name: str) -> tuple[str, ...]:
    value = rule.attributes.get(name)
    if isinstance(value, str):
        return (value,)
    if isinstance(value, tuple):
        return value
    return ()


def _query_label(value: object) -> str:
    """Normalize one bare project label and reject patterns/external repos."""
    if not isinstance(value, str):
        raise ValueError("Bazel label must be a string")
    text = value.strip()
    if text.startswith("@@"):
        text = text[2:]
    elif text.startswith("@//"):
        text = text[1:]
    elif text.startswith("@"):
        raise ValueError(f"external Bazel target is not a project lint owner: {value}")
    match = _LABEL.fullmatch(text)
    if match is None:
        raise ValueError(f"invalid bare Bazel target label: {value}")
    package, name = match.groups()
    if package and any(part in {"", ".", "..", "..."} for part in package.split("/")):
        raise ValueError(f"invalid Bazel package in target label: {value}")
    if any(part in {"", ".", "..", "..."} for part in name.split("/")):
        raise ValueError(f"invalid Bazel target name: {value}")
    return f"//{package}:{name}"


def _label_path(label: str) -> str | None:
    match = _LABEL.fullmatch(label)
    if match is None:
        return None
    package, name = match.groups()
    path = f"{package}/{name}" if package else name
    return path


def _canonical_owner(query_label: str) -> str:
    return bazel_lint_reports.canonical_owner(query_label)


def _is_source(path: str) -> bool:
    return PurePosixPath(path).suffix.lower() in _SOURCE_SUFFIXES


def _is_header(path: str) -> bool:
    return PurePosixPath(path).suffix.lower() in _HEADER_SUFFIXES


def _component(path: str) -> tuple[str, ...] | None:
    """Normalize public include trees to the implementation component they own."""
    return tidyengine.component_parts(PurePosixPath(path).parent.parts)


def _paired(header: str, source: str) -> bool:
    header_component = _component(header)
    source_component = _component(source)
    if header_component is None or source_component is None or header_component != source_component:
        return False
    header_stem = PurePosixPath(header).stem.casefold()
    source_stem = PurePosixPath(source).stem.casefold()
    return source_stem in {header_stem + suffix.casefold() for suffix in _PLATFORM_IMPLEMENTATION_SUFFIXES}


def _attributes(element: ET.Element) -> dict[str, AttributeValue]:
    result: dict[str, AttributeValue] = {}
    for child in element:
        name = child.get("name")
        if not name:
            continue
        if name in result:
            raise ValueError(f"duplicate Bazel query attribute: {name}")
        tag = child.tag
        if tag == "list":
            values: list[str] = []
            for item in child:
                value = item.get("value")
                if item.tag not in {"label", "string", "output"} or value is None:
                    raise ValueError(f"invalid Bazel query list item for {name}")
                values.append(value)
            result[name] = tuple(values)
        elif tag in {"label", "string"}:
            value = child.get("value")
            if value is None:
                raise ValueError(f"missing Bazel query attribute value: {name}")
            result[name] = value
        elif tag == "boolean":
            result[name] = child.get("value") == "true"
        elif tag == "int":
            value = child.get("value")
            if value is None:
                raise ValueError(f"missing Bazel query integer value: {name}")
            result[name] = int(value)
        # licenses, visibility and other non-ownership attributes are irrelevant.
    return result


def parse_query_xml(path: Path) -> QueryGraph:
    """Parse the current ``bazel query --output=xml`` graph."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise ValueError(f"invalid Bazel query XML: {error}") from error
    rules: dict[str, Rule] = {}
    for element in root:
        if element.tag == "rule":
            label = _query_label(element.get("name"))
            # Lint ownership follows only rule ``srcs`` through filegroups and aliases.
            rules[label] = Rule(label, element.get("class", ""), _attributes(element))
    return QueryGraph(rules)


def parse_cquery_rows(text: str) -> dict[str, bool]:
    """Parse one Starlark cquery compatibility row per requested root."""
    rows: dict[str, bool] = {}
    for line in text.splitlines():
        if line.strip():
            label, _, value = line.strip().rpartition("|")
            rows[_query_label(label)] = value.strip() == "True"
    return rows


def _write_json(path: Path, value: object) -> None:
    content = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def _write_json_if_changed(path: Path, value: object) -> bool:
    content = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        if path.read_bytes() == content:
            return False
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return True


def _lint_configuration(
    check: str | None, header_filter: str | None, clang_tidy_args: Sequence[str], *, alpha: bool = False
) -> dict[str, object]:
    return {
        "schema": 1,
        "source_root": native.source_root(),
        "check": check,
        "header_filter": header_filter,
        "clang_tidy_args": list(clang_tidy_args),
        "alpha": bool(alpha),
    }


def _write_lint_config(path: Path, configuration: Mapping[str, object]) -> bool:
    return _write_json_if_changed(path, configuration)


def _new_run_dir(tree: Path, kind: str = "lint") -> Path:
    """Create this invocation's evidence directory and keep only recent runs.

    Callers hold the build-tree lock, so no retired run is still being written.
    """
    root = tree / kind
    root.mkdir(parents=True, exist_ok=True)
    runs = sorted((path for path in root.glob("run-*") if path.is_dir()), key=lambda path: path.stat().st_mtime_ns)
    for retired in runs[: max(0, len(runs) - _RETAINED_RUNS + 1)]:
        shutil.rmtree(retired, ignore_errors=True)
    return Path(tempfile.mkdtemp(prefix="run-", dir=root))


def _run_process(argv: Sequence[str], stdout: Path, stderr: Path) -> int:
    with stdout.open("wb") as out, stderr.open("wb") as err:
        result = subprocess.run(
            list(argv),
            cwd=PROJECT_ROOT,
            env=native.child_environment(),
            stdout=out,
            stderr=err,
            check=False,
        )
    return result.returncode


def _print_failure(what: str, status: int, stderr: Path) -> None:
    print(f"Bazel lint {what} failed (exit {status}); stderr: {stderr}", file=sys.stderr)
    try:
        lines = stderr.read_text(encoding="utf-8", errors="replace").strip().splitlines()
    except OSError:
        return
    if lines:
        print("\n".join(lines[-20:]), file=sys.stderr)


def _query_graph(executable: Path, prepared: Path, run_dir: Path, owners: Sequence[str]) -> QueryGraph:
    # Explicit roots need rule-kind validation, not recursive workspace discovery.
    # Their configured native action/index inventory remains the coverage authority.
    expression = 'kind("rule", set(' + " ".join(owners) + "))" if owners else _QUERY_EXPRESSION
    stdout = run_dir / "query.xml"
    stderr = run_dir / "query.stderr.log"
    argv = [
        str(executable),
        f"--bazelrc={prepared / 'aobus.bazelrc'}",
        "query",
        expression,
        "--output=xml",
    ]
    status = _run_process(argv, stdout, stderr)
    _write_json(
        run_dir / "query.receipt.json",
        {"schema": 1, "command": argv, "status": status, "stdout": str(stdout), "stderr": str(stderr)},
    )
    if status:
        _print_failure("graph query", status, stderr)
        raise ValueError(f"Bazel lint graph query failed with exit {status}")
    return parse_query_xml(stdout)


def _query_compatibility(
    executable: Path, prepared: Path, run_dir: Path, owners: Sequence[str], *, name: str = "cquery"
) -> dict[str, bool]:
    stdout = run_dir / f"{name}.txt"
    stderr = run_dir / f"{name}.stderr.log"
    # An owner that another requested owner also builds as a tool appears in
    # the exec configuration too; only its top-level configuration is linted.
    expression = "config(set(" + " ".join(owners) + "), target)"
    argv = [
        str(executable),
        f"--bazelrc={prepared / 'aobus.bazelrc'}",
        "cquery",
        expression,
        "--output=starlark",
        f"--starlark:expr={_CQUERY_EXPRESSION}",
    ]
    # The native canary (cquery-canary-02) established that this expression
    # returns compatible and incompatible rows with exit 0 and no skip flag.
    # Do not add --skip_incompatible_explicit_targets: it would hide the row
    # and turn the compatibility boundary into a guess.
    status = _run_process(argv, stdout, stderr)
    receipt: dict[str, object] = {
        "schema": 1,
        "command": argv,
        "status": status,
        "stdout": str(stdout),
        "stderr": str(stderr),
    }
    if status:
        _write_json(run_dir / f"{name}.receipt.json", receipt)
        _print_failure("compatibility query", status, stderr)
        raise ValueError(f"Bazel lint compatibility query failed with exit {status}")
    try:
        rows = parse_cquery_rows(stdout.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        receipt["error"] = str(error)
        _write_json(run_dir / f"{name}.receipt.json", receipt)
        raise
    receipt["rows"] = rows
    receipt["incompatible"] = sorted(owner for owner, incompatible in rows.items() if incompatible)
    _write_json(run_dir / f"{name}.receipt.json", receipt)
    return rows


def _diagnostic_arguments(value: object) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, (list, tuple)) or any(
        not isinstance(argument, str) or not argument or "\x00" in argument for argument in value
    ):
        raise ValueError("clang_tidy_arg must be an array of nonempty strings")
    return list(value)


def _validate_lint_configuration(
    check: str | None, header_filter: str | None, clang_tidy_args: Sequence[str], *, alpha: bool = False
) -> None:
    # Reuse the launcher's declared diagnostic-only policy before any workspace,
    # repository or tool lookup.  The launcher remains the action-time authority.
    from . import bazel_lint_action

    bazel_lint_action.validate_configuration(
        {
            "source_root": str(PROJECT_ROOT),
            "check": check,
            "header_filter": header_filter,
            "clang_tidy_args": list(clang_tidy_args),
            "alpha": bool(alpha),
        }
    )


def _optional_text(value: object, name: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"{name} must be a nonempty string")
    return value


def _path_argument(value: object, name: str) -> str | None:
    if isinstance(value, Path):
        value = str(value)
    return _optional_text(value, name)


def _string_list(value: object, name: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, (list, tuple)) or any(
        not isinstance(item, str) or not item or "\x00" in item for item in value
    ):
        raise ValueError(f"{name} must be an array of nonempty strings")
    return list(value)


def _jobs(value: object) -> int:
    if type(value) is not int or value < 1:
        raise ValueError("jobs must be a positive integer")
    return value


def _config(value: object) -> str:
    if value not in ("debug", "release"):
        raise ValueError("config must be debug or release")
    return str(value)


def _remote_cache(value: object) -> str | None:
    text = _optional_text(value, "remote_cache")
    if text is None:
        return None
    if urlparse(text).scheme not in {"http", "https", "grpc", "grpcs"}:
        raise ValueError("remote cache requires an http(s) or grpc(s) URL")
    return text


def _output_path(value: object) -> Path | None:
    if value is None:
        return None
    if isinstance(value, Path):
        return value
    text = _optional_text(value, "output")
    if text is None:
        return None
    return Path(text)


def _owner_targets(value: object) -> list[str]:
    targets: list[str] = []
    for raw in _string_list(value, "owner_targets"):
        target = _query_label(raw)
        if target.endswith(":all"):
            raise ValueError("explicit lint owners must be bare target labels, not package patterns")
        if target in targets:
            raise ValueError(f"duplicate explicit lint owner: {target}")
        targets.append(target)
    return targets


def _validate_explicit_paths(files: Sequence[str]) -> None:
    root = absolute_path(PROJECT_ROOT)
    missing: list[str] = []
    outside: list[str] = []
    for name in files:
        if "\x00" in name:
            raise ValueError("explicit path contains a NUL byte")
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        resolved = absolute_path(path)
        try:
            resolved.relative_to(root)
        except ValueError:
            outside.append(name)
            continue
        if not resolved.is_file():
            missing.append(name)
    if missing or outside:
        details: list[str] = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if outside:
            details.append("outside checkout: " + ", ".join(outside))
        raise ValueError("explicit lint paths are invalid (" + "; ".join(details) + ")")


def _relative_selected(paths: Iterable[Path]) -> list[str]:
    root = absolute_path(PROJECT_ROOT)
    selected: list[str] = []
    seen: set[str] = set()
    for path in paths:
        candidate = path if path.is_absolute() else PROJECT_ROOT / path
        try:
            relative = candidate.relative_to(root)
        except ValueError:
            try:
                relative = candidate.resolve().relative_to(root)
            except ValueError as error:
                raise ValueError(f"selected path is outside the checkout: {path}") from error
        text = relative.as_posix()
        if text not in seen:
            seen.add(text)
            selected.append(text)
    return sorted(selected)


def _validate_selected_suffix(path: str) -> None:
    if PurePosixPath(path).suffix.lower() not in _LINT_SUFFIXES:
        raise ValueError(f"unsupported C++ lint file type: {path}")


def _common_prefix(left: Sequence[str], right: Sequence[str]) -> int:
    count = 0
    for first, second in zip(left, right, strict=False):
        if first != second:
            break
        count += 1
    return count


def _nearest(owners: Sequence[str], path: str) -> tuple[str, ...]:
    """Return the owners whose packages lie closest to the component of ``path``."""
    anchor = _component(path) or PurePosixPath(path).parent.parts

    def score(owner: str) -> int:
        package = owner[2:].partition(":")[0]
        return _common_prefix(anchor, package.split("/") if package else [])

    best = max((score(owner) for owner in owners), default=0)
    return tuple(owner for owner in owners if score(owner) == best)


def _map_selected(graph: QueryGraph, selected: Sequence[str]) -> tuple[list[str], dict[str, tuple[str, ...]]]:
    """Return the first report pass's owners and the consumer owners held back.

    A header needs its companion owners, while a header without one and an
    include fragment need a translation unit that includes them.  Only the
    nearest possible consumers join the first pass; the others build only if
    none of the first pass's reports includes the path.
    """
    owners: set[str] = set()
    held: dict[str, tuple[str, ...]] = {}
    for path in selected:
        _validate_selected_suffix(path)
        if _is_source(path):
            owners.update(graph.source_owners(path))
            continue
        consumers = graph.consumer_owners(path)
        first = graph.header_owners(path) or _nearest(consumers, path)
        owners.update(first)
        held[path] = tuple(owner for owner in consumers if owner not in first)
    return sorted(owners), held


def _unreported(selected: Iterable[str], inventories: Sequence[bazel_lint_reports.OwnerInventory]) -> list[str]:
    """Return the selected headers and fragments that no report analyzes as a main."""
    reported = {report.identity.main for inventory in inventories for report in inventory.reports}
    return sorted(path for path in selected if not _is_source(path) and path not in reported)


def _choose_consumers(
    paths: Sequence[str], inventories: Sequence[bazel_lint_reports.OwnerInventory]
) -> dict[str, tuple[str, str]]:
    """Select, per path, the nearest source report that included it.

    A same-component translation unit comes first, then the longest shared
    directory prefix; owner and source labels break remaining ties.
    """
    wanted = set(paths)
    candidates: dict[str, list[tuple[str, str]]] = {}
    for inventory in inventories:
        for report in inventory.reports:
            if report.identity.main == report.identity.source:
                for path in wanted.intersection(report.includes):
                    candidates.setdefault(path, []).append((inventory.owner, report.identity.source))
    return {
        path: min((_consumer_rank(path, *option), option) for option in options)[1]
        for path, options in candidates.items()
    }


def _consumer_rank(path: str, owner: str, source: str) -> tuple[bool, int, str, str]:
    component = _component(path)
    foreign = component is None or _component(source) != component
    return (foreign, -_common_prefix(PurePosixPath(path).parts, PurePosixPath(source).parts), owner, source)


def _consumer_selection(consumers: Mapping[str, tuple[str, str]]) -> dict[str, list[list[str]]]:
    grouped: dict[str, list[list[str]]] = {}
    for path, (owner, source) in sorted(consumers.items()):
        grouped.setdefault(owner, []).append([source, path])
    return grouped


def _write_consumer_parameters(prepared: Path, consumers: Mapping[str, tuple[str, str]]) -> str:
    """Keep the whole map out of native argv; the caller holds the tree lock.

    The rc is byte-addressed: only ``\n`` separates lines (never the Unicode
    line separators that ``str.splitlines`` would also split on), and the
    rewritten file always keeps the canonical UTF-8 LF encoding that
    ``prepare_workspace`` writes.  An already byte-identical rc is not
    rewritten.
    """
    parameters = json.dumps(_consumer_selection(consumers), sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    prefix = "build:aobus_lint_consumers "
    rc = prepared / "aobus.bazelrc"
    original = rc.read_bytes()
    lines = original.decode("utf-8").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    lines = [line for line in lines if not line.startswith(prefix)]
    lines.append(prefix + json.dumps("--aspects_parameters=aobus_lint_consumers=" + parameters))
    content = ("\n".join(lines) + "\n").encode("utf-8")
    if content != original:
        rc.write_bytes(content)
    return parameters


def _validate_targets(graph: QueryGraph, targets: Sequence[str]) -> None:
    for target in targets:
        rule = graph.rules.get(target)
        if rule is None:
            raise ValueError(f"explicit lint owner is not in the project graph: {target}")
        if rule.kind not in _COMPILE_OWNER_KINDS:
            raise ValueError(f"explicit lint owner is not a native compile owner: {target} ({rule.kind})")
        # A bounded query need not expand nested filegroups or aliases. The
        # aspect checks actual configured srcs, and the gate requires TU reports.


def _covered_paths(inventories: Sequence[bazel_lint_reports.OwnerInventory]) -> set[str]:
    covered: set[str] = set()
    for inventory in inventories:
        covered.update(inventory.sources)
        covered.update(report.identity.main for report in inventory.reports)
        covered.update(uncovered.path for uncovered in inventory.uncovered)
    return covered


def _match_paths(selected: set[str], inventories: Sequence[bazel_lint_reports.OwnerInventory]) -> set[str]:
    if os.name != "nt":
        return set(selected)
    available = _covered_paths(inventories)
    by_casefold: dict[str, list[str]] = {}
    for path in available:
        by_casefold.setdefault(path.casefold(), []).append(path)
    matched: set[str] = set()
    for path in selected:
        if path in available:
            matched.add(path)
            continue
        candidates = by_casefold.get(path.casefold(), [])
        matched.add(candidates[0] if len(candidates) == 1 else path)
    return matched


def _platform_incompatible(path: str) -> bool:
    return tidyengine._is_platform_incompatible(absolute_path(PROJECT_ROOT / path), PROJECT_ROOT)


def _deferred_paths(
    selected: Sequence[str],
    inventories: Sequence[bazel_lint_reports.OwnerInventory],
    *,
    allow: bool,
) -> set[str]:
    if not allow:
        return set()
    reported = {report.identity.main for inventory in inventories for report in inventory.reports}
    deferred: set[str] = set()
    for path in selected:
        # The unconfigured query can over-approximate select() and map a
        # platform-owned file to a compatible owner.  The aspect fails if
        # configured srcs differ from captured actions, so every configured
        # source has a report.  A header or fragment that neither a companion
        # nor any analyzed translation unit reaches is not compiled here, and
        # the native platform policy then makes its deferral defensible.
        if path not in reported and _platform_incompatible(path):
            deferred.add(path)
    return deferred


def _print_deferred(paths: Iterable[str]) -> None:
    print("Deferred files that are incompatible with this platform:", file=sys.stderr)
    for path in sorted(paths):
        print(f"  {path}", file=sys.stderr)


def _print_stale(stale: Sequence[Any]) -> None:
    print("ERROR: stale NOLINT suppressions name checks disabled for their file mode:", file=sys.stderr)
    for issue in stale:
        path = issue.path
        try:
            display = path.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
        except ValueError:
            display = path.as_posix()
        print(f"  {display}:{issue.line}: {issue.check}", file=sys.stderr)
    print("Remove these suppressions; the configured check cannot emit there.", file=sys.stderr)


def _finish_unowned(selected: Sequence[str]) -> int:
    deferred = _deferred_paths(selected, (), allow=True)
    failures = [path for path in selected if path not in deferred]
    if failures:
        raise ValueError("no native lint owner covers selected path(s): " + ", ".join(sorted(failures)))
    if deferred:
        _print_deferred(deferred)
    print("No compatible native lint reports selected.", file=sys.stderr)
    return 0


def _tidy_command() -> Any:
    # Keep the core module importable without initializing the command package.
    from ..command import tidy

    return tidy


def _report_pass(
    executable: Path,
    tree: Path,
    prepared: Path,
    run_dir: Path,
    *,
    name: str,
    aspect: str,
    profile: str,
    config: str,
    owners: Sequence[str],
    consumers: Mapping[str, tuple[str, str]],
    jobs: int,
    remote_cache: str | None,
    debug: bool,
) -> tuple[Path, tuple[bazel_lint_reports.OwnerInventory, ...]]:
    """Build one native report pass and bind its owners' current inventories."""
    parameters = _write_consumer_parameters(prepared, consumers)
    aspect_id = bazel_lint_reports.lint_aspect_id(parameters, profile=profile)
    bep = run_dir / f"{name}.bep.json"
    build_targets = [
        *owners,
        f"--aspects={aspect}",
        "--config=aobus_lint_consumers",
        f"--output_groups={_OUTPUT_GROUP}",
        f"--build_event_json_file={bep.absolute()}",
        "--experimental_build_event_upload_strategy=local",
        "--remote_download_outputs=all",
        "--norun_validations",
    ]
    if debug:
        # Diagnostic evidence only; the gate still inventories TargetComplete
        # output groups, never action-execution events or cached stdout.
        build_targets.append("--build_event_publish_all_actions")
    status = bazel_workspace.execute(
        executable, tree, prepared, "build", config, build_targets, jobs=jobs, remote_cache=remote_cache
    )
    _write_json(
        run_dir / f"{name}.receipt.json",
        {
            "schema": 1,
            "command": [str(executable), "build", *build_targets],
            "status": status,
            "jobs": jobs,
            "remote_cache": remote_cache,
            "bep": str(bep),
            "aspect": aspect_id,
            "aspect_parameters": parameters,
        },
    )
    if status:
        raise ValueError(f"Bazel {profile} report build failed (exit {status}); log: {tree / 'build.log'}")
    return bep, bazel_lint_reports.read_reports(
        bep, [_canonical_owner(owner) for owner in owners], consumers=parameters, profile=profile
    )


def _run(args: argparse.Namespace) -> int:
    # Diagnostic-only validation precedes workspace selection and tool lookup.
    clang_tidy_args = _diagnostic_arguments(getattr(args, "clang_tidy_arg", []))
    check = _optional_text(getattr(args, "check", None), "check")
    header_filter = _optional_text(getattr(args, "header_filter", None), "header_filter")
    _validate_lint_configuration(check, header_filter, clang_tidy_args)
    output = _output_path(getattr(args, "output", None))
    jobs = _jobs(getattr(args, "jobs", 2))
    config = _config(getattr(args, "config", "debug"))
    remote_cache = _remote_cache(getattr(args, "remote_cache", None))
    targets = _owner_targets(getattr(args, "owner_targets", []))
    files = _string_list(getattr(args, "files", []), "files")
    all_sources = bool(getattr(args, "all", False))
    folders = _string_list(getattr(args, "folder", []), "folder")
    commit = _optional_text(getattr(args, "commit", None), "commit")
    debug = bool(getattr(args, "debug", False))
    tree_path = _path_argument(getattr(args, "path", None), "path")
    vcpkg_prefix = _path_argument(getattr(args, "vcpkg_prefix", None), "vcpkg_prefix")
    prepared_sources = getattr(args, "_resolved_sources", None)
    has_scope = bool(files or all_sources or folders or commit or prepared_sources)
    target_only = bool(targets) and not has_scope

    selected_paths: list[Path] = []
    if not target_only:
        tidy = _tidy_command()
        resolved, explicit = tidyengine.resolve_scope(args, tidy.ALL_FOLDERS, "Checking", suffixes=_LINT_SUFFIXES)
        if explicit:
            _validate_explicit_paths(resolved)
        buckets = tidy.classify_existing(resolved, explicit)
        selected_paths = [*buckets["STRICT"], *buckets["RELAXED"]]
        if not selected_paths:
            print("No C++ lint sources selected.", file=sys.stderr)
            return 0
        stale = tidy.find_stale_nolint_suppressions(buckets)
        if stale:
            _print_stale(stale)
            return 1

    selected = _relative_selected(selected_paths) if selected_paths else []
    for path in selected:
        _validate_selected_suffix(path)

    tree = bazel_workspace.workspace_path(tree_path, config)
    executable = bazel_workspace.bazel_executable()
    native.require_supported_source()
    with build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        lint_configuration = tree / "configuration" / "lint.json"
        _write_lint_config(
            lint_configuration,
            _lint_configuration(check, header_filter, clang_tidy_args),
        )
        prepared = bazel_workspace.prepare_workspace(
            tree,
            config,
            prefix=vcpkg_prefix,
            lint_configuration=lint_configuration,
        )
        run_dir = _new_run_dir(tree)
        if debug:
            print(f"Bazel lint tree: {tree}", file=sys.stderr)
            print(f"Bazel lint evidence: {run_dir}", file=sys.stderr)
            print(f"Bazel lint selected: {' '.join(selected) if selected else '<explicit owners>'}", file=sys.stderr)

        graph = _query_graph(executable, prepared, run_dir, targets)
        held: dict[str, tuple[str, ...]] = {}
        if targets:
            _validate_targets(graph, targets)
            requested = targets
        else:
            requested, held = _map_selected(graph, selected)
            if not requested:
                return _finish_unowned(selected)

        compatibility = _query_compatibility(executable, prepared, run_dir, requested)
        incompatible = {owner for owner, value in compatibility.items() if value}
        compatible = [owner for owner in requested if owner not in incompatible]
        if not compatible and not targets:
            # The nearest consumer is only a loading preference. An incompatible
            # first group cannot establish that farther consumers are unavailable.
            held_owners = sorted({owner for owners in held.values() for owner in owners if owner not in incompatible})
            if held_owners:
                rows = _query_compatibility(executable, prepared, run_dir, held_owners, name="cquery-consumers")
                incompatible.update(owner for owner, value in rows.items() if value)
                compatible.extend(owner for owner in held_owners if not rows[owner])
        if incompatible:
            print("Deferred lint owners incompatible with this platform:", file=sys.stderr)
            for owner in sorted(incompatible):
                print(f"  {owner}", file=sys.stderr)
        if not compatible:
            if targets:
                raise ValueError("all explicit lint owners are incompatible with this platform")
            return _finish_unowned(selected)

        def report_pass(
            name: str, owners: Sequence[str], consumers: Mapping[str, tuple[str, str]]
        ) -> tuple[Path, tuple[bazel_lint_reports.OwnerInventory, ...]]:
            return _report_pass(
                executable,
                tree,
                prepared,
                run_dir,
                name=name,
                aspect=_ASPECT,
                profile="lint",
                config=config,
                owners=owners,
                consumers=consumers,
                jobs=jobs,
                remote_cache=remote_cache,
                debug=debug,
            )

        bep, inventories = report_pass("build", compatible, {})
        consumers: dict[str, tuple[str, str]] = {}
        if not target_only:
            selected = sorted(_match_paths(set(selected), inventories))
            consumers = _choose_consumers(_unreported(selected, inventories), inventories)
            further = {
                owner
                for path in _unreported(selected, inventories)
                if path not in consumers
                for owner in held.get(path, ())
                if owner not in compatible and owner not in incompatible
            }
            if further:
                # Only paths still without a consumer widen the build.
                rows = _query_compatibility(executable, prepared, run_dir, sorted(further), name="cquery-consumers")
                incompatible.update(owner for owner, value in rows.items() if value)
                added = [owner for owner in sorted(further) if not rows[owner]]
                if added:
                    compatible.extend(added)
                    bep, inventories = report_pass("consumer-owners", compatible, {})
                    selected = sorted(_match_paths(set(selected), inventories))
                    consumers = _choose_consumers(_unreported(selected, inventories), inventories)
            if consumers:
                bep, inventories = report_pass("consumers", compatible, consumers)

        deferred: set[str] = set()
        if targets:
            for inventory in inventories:
                if not any(report.identity.source == report.identity.main for report in inventory.reports):
                    raise ValueError(
                        "explicit lint owner produced no native translation-unit reports: " + inventory.owner
                    )
        if target_only:
            # Paired header mains replay an owner's own native contexts, so
            # they belong to its scope; unpaired declared headers do not.
            selected = sorted({report.identity.main for inventory in inventories for report in inventory.reports})
        else:
            deferred = _deferred_paths(selected, inventories, allow=not targets)
            selected = [path for path in selected if path not in deferred]
        if deferred:
            _print_deferred(deferred)
        _write_json(
            run_dir / "run.receipt.json",
            {
                "schema": 1,
                "owners": compatible,
                "incompatible": sorted(incompatible),
                "selected": selected,
                "deferred": sorted(deferred),
                "consumers": {path: {"owner": owner, "source": source} for path, (owner, source) in consumers.items()},
                "bep": str(bep),
            },
        )
        if not selected:
            print("No compatible native lint reports selected.", file=sys.stderr)
            return 0
        reasons = dict.fromkeys(_unreported(selected, inventories), _NO_CONSUMER)
        return bazel_lint_reports.gate_reports(inventories, set(selected), output, reasons=reasons)


def run(args: argparse.Namespace) -> int:
    """Run one production lint invocation and return its public gate status."""
    try:
        return _run(args)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Bazel lint failed: {error}", file=sys.stderr)
        return 2


def _finish_unowned_analyzer(selected: Sequence[str], *, explicit: bool) -> int:
    """Fail an analyzer scope no compatible native owner covers, or defer foreign files."""
    deferred = _deferred_paths(selected, (), allow=not explicit)
    failures = [path for path in selected if path not in deferred]
    if failures:
        print(
            "ERROR: no compatible native compile owner covers selected path(s): " + ", ".join(sorted(failures)),
            file=sys.stderr,
        )
        return 1
    if deferred:
        _print_deferred(deferred)
    # An entirely deferred scope never claims a clean analysis.
    return 0


def _run_analyzer(args: argparse.Namespace, files: Sequence[str], *, explicit: bool) -> int:
    """Run the explicit clang-analyzer profile over real Bazel compile contexts."""
    # Diagnostic-only validation precedes workspace selection and tool lookup.
    tidy_args = _diagnostic_arguments(getattr(args, "tidy_arg", []))
    check = _optional_text(getattr(args, "check", None), "check")
    alpha = bool(getattr(args, "alpha", False))
    _validate_lint_configuration(check, None, tidy_args, alpha=alpha)
    output = _output_path(getattr(args, "output", None))
    jobs = _jobs(getattr(args, "jobs", 2))
    debug = bool(getattr(args, "debug", False))
    include_external = bool(getattr(args, "include_external_diagnostics", False))
    fail_on_diagnostics = bool(getattr(args, "fail_on_diagnostics", False))
    if not files:
        print("No analyzable C++ or Objective-C++ sources found.", file=sys.stderr)
        return 0
    selected = _relative_selected([Path(name) for name in files])
    for path in selected:
        _validate_selected_suffix(path)
    tree = bazel_workspace.workspace_path(
        _path_argument(getattr(args, "path", None), "path")
        or str(builddir.platform_profile().build_root / _ANALYZER_TREE),
        "debug",
    )
    executable = bazel_workspace.bazel_executable()
    native.require_supported_source()
    with build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        analyzer_configuration = tree / "configuration" / "analyzer.json"
        _write_lint_config(analyzer_configuration, _lint_configuration(check, None, tidy_args, alpha=alpha))
        prepared = bazel_workspace.prepare_workspace(tree, "debug", lint_configuration=analyzer_configuration)
        run_dir = _new_run_dir(tree, "analyzer")
        if debug:
            print(f"Analyzer tree: {tree}", file=sys.stderr)
            print(f"Analyzer evidence: {run_dir}", file=sys.stderr)
            print(f"Analyzer selected: {' '.join(selected)}", file=sys.stderr)

        graph = _query_graph(executable, prepared, run_dir, [])
        requested, held = _map_selected(graph, selected)
        if not requested:
            return _finish_unowned_analyzer(selected, explicit=explicit)

        def report_pass(
            name: str, owners: Sequence[str], consumers: Mapping[str, tuple[str, str]]
        ) -> tuple[Path, tuple[bazel_lint_reports.OwnerInventory, ...]]:
            return _report_pass(
                executable,
                tree,
                prepared,
                run_dir,
                name=name,
                aspect=_ANALYZER_ASPECT,
                profile="analyzer",
                config="debug",
                owners=owners,
                consumers=consumers,
                jobs=jobs,
                remote_cache=None,
                debug=debug,
            )

        compatibility = _query_compatibility(executable, prepared, run_dir, requested)
        incompatible = {owner for owner, value in compatibility.items() if value}
        compatible = [owner for owner in requested if owner not in incompatible]
        if not compatible:
            # The nearest consumer is only a loading preference. An incompatible
            # first group cannot establish that farther consumers are unavailable.
            held_owners = sorted({owner for owners in held.values() for owner in owners if owner not in incompatible})
            if held_owners:
                rows = _query_compatibility(executable, prepared, run_dir, held_owners, name="cquery-consumers")
                incompatible.update(owner for owner, value in rows.items() if value)
                compatible.extend(owner for owner in held_owners if not rows[owner])
        if incompatible:
            print("Deferred analyzer owners incompatible with this platform:", file=sys.stderr)
            for owner in sorted(incompatible):
                print(f"  {owner}", file=sys.stderr)
        if not compatible:
            return _finish_unowned_analyzer(selected, explicit=explicit)

        bep, inventories = report_pass("build", compatible, {})
        consumers: dict[str, tuple[str, str]] = {}
        selected = sorted(_match_paths(set(selected), inventories))
        consumers = _choose_consumers(_unreported(selected, inventories), inventories)
        further = {
            owner
            for path in _unreported(selected, inventories)
            if path not in consumers
            for owner in held.get(path, ())
            if owner not in compatible and owner not in incompatible
        }
        if further:
            # Only paths still without a consumer widen the build.
            rows = _query_compatibility(executable, prepared, run_dir, sorted(further), name="cquery-consumers")
            incompatible.update(owner for owner, value in rows.items() if value)
            added = [owner for owner in sorted(further) if not rows[owner]]
            if added:
                compatible.extend(added)
                bep, inventories = report_pass("consumer-owners", compatible, {})
                selected = sorted(_match_paths(set(selected), inventories))
                consumers = _choose_consumers(_unreported(selected, inventories), inventories)
        if consumers:
            bep, inventories = report_pass("consumers", compatible, consumers)

        deferred = _deferred_paths(selected, inventories, allow=not explicit)
        selected = [path for path in selected if path not in deferred]
        if deferred:
            _print_deferred(deferred)
        _write_json(
            run_dir / "run.receipt.json",
            {
                "schema": 1,
                "profile": "analyzer",
                "owners": compatible,
                "incompatible": sorted(incompatible),
                "selected": selected,
                "deferred": sorted(deferred),
                "consumers": {path: {"owner": owner, "source": source} for path, (owner, source) in consumers.items()},
                "bep": str(bep),
            },
        )
        if not selected:
            # An entirely deferred scope never claims a clean analysis.
            return 0
        reported = {report.identity.main for inventory in inventories for report in inventory.reports}
        missing_objective = [
            path for path in selected if PurePosixPath(path).suffix.lower() == ".mm" and path not in reported
        ]
        if missing_objective:
            print("ERROR: Objective-C++ analysis requires an exact native compile command:", file=sys.stderr)
            for path in missing_objective:
                print(f"  {path}", file=sys.stderr)
            print("Build these files on their native platform and analyze them there.", file=sys.stderr)
            return 1
        reasons = dict.fromkeys(_unreported(selected, inventories), _NO_CONSUMER)
        return bazel_lint_reports.gate_analyzer_reports(
            inventories,
            set(selected),
            output,
            include_external=include_external,
            fail_on_diagnostics=fail_on_diagnostics,
            reasons=reasons,
        )


def run_analyzer(args: argparse.Namespace, files: Sequence[str], *, explicit: bool) -> int:
    """Run one production analyzer invocation and return its report gate status."""
    try:
        return _run_analyzer(args, files, explicit=explicit)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Analyzer failed: {error}", file=sys.stderr)
        return 2
