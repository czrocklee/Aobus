"""Portal orchestration for the scoped native C++ stage of ``ao check lint``.

The portal owns scope selection, owner discovery, configured compatibility,
the report build and the public failure gate.  Bazel owns the actual native
compile contexts and report actions; this module never prepares CMake or runs
clang-tidy itself.  ``run`` keeps the caller's file scope; the complete graph
gate is the declared Bazel test, not a portal route.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections.abc import Collection, Iterable, Iterator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any
from urllib.parse import urlparse

from ao.core import bazel_native as native
from ao.core.build_lock import build_tree_lock
from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.graph import bazel_lint_reports
from ao.policy.tidy_policy import common_prefix, component_parts, consumer_rank, find_stale_nolint_suppressions
from ao.portal import bazel_workflows, bazel_workspace, build_dir, tidy_engine

if TYPE_CHECKING:
    from ao.portal.source_analyzer import AnalyzerRequest
    from ao.portal.source_scope import SourceScope

_ASPECT = "//bazel/lint:lint.bzl%lint_aspect"
_ANALYZER_ASPECT = "//bazel/lint:lint.bzl%analyzer_aspect"
_ANALYZER_TREE = "bazel-analyzer"
# The analyzer's true ordinary compile context. The launcher's --debug flag
# stays verbose evidence output, never a build mode.
_ANALYZER_CONFIG = "fastbuild"
_OUTPUT_GROUP = "aobus_lint"
_QUERY_EXPRESSION = 'kind("rule", //...)'
_QUERY_ATTRIBUTES = frozenset(
    {"srcs", "hdrs", "textual_hdrs", "additional_compiler_inputs", "deps", "implementation_deps", "actual"}
)
_CQUERY_EXPRESSION = 'str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))'

_SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx", ".mm"))
_HEADER_SUFFIXES = frozenset((".h", ".hh", ".hpp", ".hxx"))
_INCLUDE_FRAGMENT_SUFFIXES = frozenset((".def",))
LINT_SUFFIXES = tuple(sorted(_SOURCE_SUFFIXES | _HEADER_SUFFIXES | _INCLUDE_FRAGMENT_SUFFIXES))
_COMPILE_OWNER_KINDS = frozenset({"cc_library", "cc_binary", "cc_test", "objc_library"})
_PLATFORM_IMPLEMENTATION_SUFFIXES = ("", "Linux", "Macos", "Posix", "Windows")
_FEATURE_PATH_PREFIXES = {
    "gtk": ("app/linux-gtk/", "test/linux-gtk/"),
    "system-media": ("app/systemmedia/linux/", "app/include/ao/systemmedia/linux/", "test/systemmedia/linux/"),
}
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

    def declared_paths(self, label: str) -> tuple[str, ...]:
        """Return every srcs/hdrs/textual/additional input one owner declares."""
        if label not in self.rules:
            return ()
        paths = list(self._rule_srcs(label))
        paths.extend(self._rule_headers(label))
        return tuple(dict.fromkeys(paths))

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
                    dependency = _normalize_target(value)
                    if dependency is None:
                        continue
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
            label = _normalize_target(value)
            if label is None:
                # External labels cannot own project source files.  Keep them
                # out of the project ownership graph rather than guessing.
                continue
            rule = self.rules.get(label)
            if rule is not None:
                if rule.kind == "filegroup":
                    paths.extend(self._expand_labels(_attribute_labels(rule, "srcs"), active | {label}))
                elif rule.kind == "alias":
                    paths.extend(self._expand_labels(_attribute_labels(rule, "actual"), active | {label}))
                continue
            paths.append(_target_path(label))
        return tuple(dict.fromkeys(paths))


def _attribute_labels(rule: Rule, name: str) -> tuple[str, ...]:
    value = rule.attributes.get(name)
    if isinstance(value, str):
        return (value,)
    if isinstance(value, tuple):
        return value
    return ()


def _query_label(value: object) -> str:
    """Validate one explicit CLI owner label with the strict owner pattern."""
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


def _normalize_target(value: object) -> str | None:
    """Canonicalize one trusted main-repository target label from query data.

    Bazel target names admit any documented punctuation, including quotes,
    parentheses and spaces, so labels from Bazel's own output are validated
    structurally only; external repository labels are not authored project
    inputs and return None.  File labels only ever become checkout paths,
    and owner labels are quoted before they join a query expression.
    """
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"invalid Bazel target label: {value!r}")
    text = value
    if text.startswith("@@//"):
        text = text[2:]
    elif text.startswith("@//"):
        text = text[1:]
    elif text.startswith("@"):
        # Any remaining repository-qualified spelling names an external target.
        return None
    if not text.startswith("//"):
        raise ValueError(f"invalid bare Bazel target label: {value}")
    package, separator, name = text[2:].partition(":")
    if not separator or not name or ":" in name:
        raise ValueError(f"invalid Bazel target label: {value}")
    for segment in (*(package.split("/") if package else ()), *name.split("/")):
        if segment in {"", ".", "..", "..."}:
            raise ValueError(f"unsafe Bazel target label: {value}")
    return f"//{package}:{name}"


def _query_word(label: str) -> str:
    """Quote an owner literally: Bazel's query lexer has no string escapes."""
    if '"' not in label:
        return f'"{label}"'
    if "'" not in label:
        return f"'{label}'"
    raise ValueError(f"Bazel query cannot quote an owner containing both quote characters: {label}")


def _target_path(label: str) -> str:
    """Return the checkout path of one canonical main-repository target label."""
    package, _, name = label[2:].partition(":")
    return f"{package}/{name}" if package else name


def _canonical_owner(query_label: str) -> str:
    return bazel_lint_reports.canonical_owner(query_label)


def _is_source(path: str) -> bool:
    return PurePosixPath(path).suffix.lower() in _SOURCE_SUFFIXES


def _is_header(path: str) -> bool:
    return PurePosixPath(path).suffix.lower() in _HEADER_SUFFIXES


def _component(path: str) -> tuple[str, ...] | None:
    """Normalize public include trees to the implementation component they own."""
    return component_parts(PurePosixPath(path).parent.parts)


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
        # Query XML also contains unrelated optional label attributes without a
        # value (for example a native context wrapper's gtk_enabled setting).
        # Only these ownership/dependency fields participate in this graph.
        if not name or name not in _QUERY_ATTRIBUTES:
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
            label = _normalize_target(element.get("name"))
            if label is not None:
                # Lint ownership follows only rule ``srcs`` through filegroups and aliases.
                rules[label] = Rule(label, element.get("class", ""), _attributes(element))
    return QueryGraph(rules)


def parse_cquery_rows(text: str) -> dict[str, bool]:
    """Parse one Starlark cquery compatibility row per requested root."""
    rows: dict[str, bool] = {}
    for line in text.splitlines():
        if line.strip():
            label, _, value = line.strip().rpartition("|")
            owner = _normalize_target(label)
            if owner is not None:
                rows[owner] = value.strip() == "True"
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
    # Queries share the governed rc isolation: ambient rc selection never
    # reaches the Bazel client the lint portal launches.
    with stdout.open("wb") as out, stderr.open("wb") as err:
        result = subprocess.run(
            list(argv),
            cwd=PROJECT_ROOT,
            env=bazel_workspace.client_environment(isolate_rc=True),
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


def _query_graph(
    executable: Path,
    prepared: Path,
    run_dir: Path,
    owners: Sequence[str],
    *,
    universe: str | None = None,
) -> QueryGraph:
    # Explicit roots need rule-kind validation, not recursive workspace discovery.
    # Their configured native action/index inventory remains the coverage authority.
    if owners:
        expression = 'kind("rule", set(' + " ".join(_query_word(owner) for owner in owners) + "))"
    else:
        expression = universe or _QUERY_EXPRESSION
    stdout = run_dir / "query.xml"
    stderr = run_dir / "query.stderr.log"
    argv = [
        *bazel_workspace.invocation(executable, prepared, isolate_rc=True),
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
    expression = "config(set(" + " ".join(_query_word(owner) for owner in owners) + "), target)"
    argv = [
        *bazel_workspace.invocation(executable, prepared, isolate_rc=True),
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
    from ao.graph import bazel_lint_action

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
    if value not in ("debug", "fastbuild", "release"):
        raise ValueError("config must be debug, fastbuild or release")
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
    if PurePosixPath(path).suffix.lower() not in LINT_SUFFIXES:
        raise ValueError(f"unsupported C++ lint file type: {path}")


def _nearest(owners: Sequence[str], path: str) -> tuple[str, ...]:
    """Return the owners whose packages lie closest to the component of ``path``."""
    anchor = _component(path) or PurePosixPath(path).parent.parts

    def score(owner: str) -> int:
        package = owner[2:].partition(":")[0]
        return common_prefix(anchor, package.split("/") if package else [])

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
        path: min((consumer_rank(path, *option), option) for option in options)[1]
        for path, options in candidates.items()
    }


def _consumer_selection(consumers: Mapping[str, tuple[str, str]]) -> dict[str, list[list[str]]]:
    grouped: dict[str, list[list[str]]] = {}
    for path, (owner, source) in sorted(consumers.items()):
        grouped.setdefault(owner, []).append([source, path])
    return grouped


def _write_consumer_parameters(
    prepared: Path, consumers: Mapping[str, tuple[str, str]], *, output_groups: Sequence[str] = (_OUTPUT_GROUP,)
) -> str:
    """Keep long consumer and output selections out of native argv under the tree lock.

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
    scope_prefix = "build:aobus_lint_scope "
    lines = [line for line in lines if not line.startswith((prefix, scope_prefix))]
    lines.append(prefix + json.dumps("--aspects_parameters=aobus_lint_consumers=" + parameters))
    if tuple(output_groups) != (_OUTPUT_GROUP,):
        lines.append(scope_prefix + "--output_groups=" + ",".join(output_groups))
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


def _scope_key(path: str) -> str:
    """Compare checkout-relative scope spellings with native case semantics."""
    return path.casefold() if os.name == "nt" else path


def _platform_incompatible(path: str) -> bool:
    return tidy_engine._is_platform_incompatible(absolute_path(PROJECT_ROOT / path), PROJECT_ROOT)


def _disabled_feature_paths(
    graph: QueryGraph, selected: Sequence[str], incompatible: set[str], args: bazel_workflows.NativeRequest
) -> dict[str, str]:
    """Map configured-out paths to disabled features using positive owner evidence."""
    disabled: dict[str, str] = {}
    for feature, prefixes in _FEATURE_PATH_PREFIXES.items():
        if bazel_workflows.feature_enabled(getattr(args, feature.replace("-", "_"), None), feature):
            continue
        for path in selected:
            if not path.startswith(prefixes):
                continue
            owners = (
                graph.source_owners(path)
                if _is_source(path)
                else graph.header_owners(path) + graph.consumer_owners(path)
            )
            # Feature directories can contain compatible helpers and adapters.
            # Every possible owner must have an incompatible cquery row.
            if owners and incompatible.issuperset(owners):
                disabled[path] = feature
    return disabled


def _deferred_paths(
    selected: Sequence[str],
    inventories: Sequence[bazel_lint_reports.OwnerInventory],
    *,
    allow: bool,
    explicit_files: Collection[str] = (),
    feature_disabled: Collection[str] = (),
) -> set[str]:
    if not allow:
        return set()
    explicit = {_scope_key(path) for path in _match_paths(set(explicit_files), inventories)}
    reported = {report.identity.main for inventory in inventories for report in inventory.reports}
    deferred: set[str] = set()
    for path in selected:
        # The unconfigured query can over-approximate select() and map a
        # platform-owned file to a compatible owner.  The aspect fails if
        # configured srcs differ from captured actions, so every configured
        # source has a report.  A header or fragment that neither a companion
        # nor any analyzed translation unit reaches is not compiled here, and
        # the native platform policy then makes its deferral defensible.
        if _scope_key(path) in explicit or path in reported:
            continue
        if path in feature_disabled or _platform_incompatible(path):
            deferred.add(path)
    return deferred


def _print_deferred(paths: set[str], *, feature_disabled: Mapping[str, str] | None = None) -> None:
    groups: dict[str, list[str]] = {}
    for path in sorted(paths):
        feature = feature_disabled.get(path) if feature_disabled else None
        label = "GTK" if feature == "gtk" else feature
        heading = (
            f"Deferred {label} files whose owners are incompatible with --{feature} off:"
            if feature
            else "Deferred files that are incompatible with this platform:"
        )
        groups.setdefault(heading, []).append(path)
    for heading, group in groups.items():
        print(heading, file=sys.stderr)
        for path in group:
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


def _finish_unowned(
    selected: Sequence[str], *, explicit_files: Collection[str], feature_disabled: Mapping[str, str] | None = None
) -> int:
    deferred = _deferred_paths(
        selected, (), allow=True, explicit_files=explicit_files, feature_disabled=feature_disabled or ()
    )
    _print_deferred(deferred, feature_disabled=feature_disabled)
    failures = [path for path in selected if path not in deferred]
    if failures:
        raise ValueError("no native lint owner covers selected path(s): " + ", ".join(sorted(failures)))
    print("No compatible native lint reports selected.", file=sys.stderr)
    return 0


def _lint_output_groups(selected: Sequence[str]) -> tuple[str, ...]:
    """Select artifacts, not analysis inputs: a report is reusable across scopes."""
    if not selected:
        return (_OUTPUT_GROUP,)
    groups = {"aobus_lint_index"}
    for path in selected:
        group = bazel_lint_reports.main_output_group(path, windows=os.name == "nt")
        if group is None:
            # Conservatively use the complete inventory for non-portable group names.
            return (_OUTPUT_GROUP,)
        groups.add(group)
    return tuple(sorted(groups))


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
    output_groups: tuple[str, ...] = (_OUTPUT_GROUP,),
) -> tuple[Path, tuple[bazel_lint_reports.OwnerInventory, ...]]:
    """Build one native report pass and bind its owners' current inventories."""
    parameters = _write_consumer_parameters(prepared, consumers, output_groups=output_groups)
    aspect_id = bazel_lint_reports.lint_aspect_id(parameters, profile=profile)
    bep = run_dir / f"{name}.bep.json"
    log = run_dir / f"{name}.log"
    build_targets = [
        *owners,
        f"--aspects={aspect}",
        "--config=aobus_lint_consumers",
        f"--output_groups={_OUTPUT_GROUP}" if output_groups == (_OUTPUT_GROUP,) else "--config=aobus_lint_scope",
        f"--build_event_json_file={bep.absolute()}",
        "--experimental_build_event_upload_strategy=local",
        "--remote_download_outputs=all",
        "--norun_validations",
    ]
    if debug:
        # Diagnostic evidence only; the gate still inventories TargetComplete
        # output groups, never action-execution events or cached stdout.
        build_targets.append("--build_event_publish_all_actions")
    try:
        status = bazel_workspace.execute(
            executable,
            tree,
            prepared,
            "build",
            config,
            build_targets,
            jobs=jobs,
            remote_cache=remote_cache,
            isolate_rc=True,
            log=log,
        )
    finally:
        if log.is_file():
            # Other portal commands truncate build.log in place; a link would
            # let the next invocation overwrite retained pass evidence.
            try:
                shutil.copyfile(log, tree / "build.log")
            except OSError as error:
                print(f"Cannot refresh build.log from {log}: {error}", file=sys.stderr)
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
            "output_groups": output_groups,
        },
    )
    if status:
        raise ValueError(f"Bazel {profile} report build failed (exit {status}); log: {log}")
    return bep, bazel_lint_reports.read_reports(
        bep,
        [_canonical_owner(owner) for owner in owners],
        consumers=parameters,
        profile=profile,
        output_groups=output_groups,
    )


@dataclass(frozen=True)
class _LintRequest:
    """The validated diagnostic and workspace options the lint portal uses."""

    check: str | None
    header_filter: str | None
    clang_tidy_args: tuple[str, ...]
    output: Path | None
    jobs: int
    config: str
    remote_cache: str | None
    debug: bool
    tree_path: str | None
    vcpkg_prefix: str | None


def _common_lint_request(args: bazel_workflows.NativeRequest) -> _LintRequest:
    """Validate the options the lint portal and its analyzer profile share."""
    # Diagnostic-only validation precedes workspace selection and tool lookup.
    clang_tidy_args = _diagnostic_arguments(getattr(args, "clang_tidy_arg", []))
    check = _optional_text(getattr(args, "check", None), "check")
    header_filter = _optional_text(getattr(args, "header_filter", None), "header_filter")
    _validate_lint_configuration(check, header_filter, clang_tidy_args)
    return _LintRequest(
        check=check,
        header_filter=header_filter,
        clang_tidy_args=tuple(clang_tidy_args),
        output=_output_path(getattr(args, "output", None)),
        jobs=_jobs(getattr(args, "jobs", 2)),
        config=_config(getattr(args, "config", "fastbuild")),
        remote_cache=_remote_cache(getattr(args, "remote_cache", None)),
        debug=bool(getattr(args, "debug", False)),
        tree_path=_path_argument(getattr(args, "path", None), "path"),
        vcpkg_prefix=_path_argument(getattr(args, "vcpkg_prefix", None), "vcpkg_prefix"),
    )


@contextmanager
def _lint_workspace(
    request: _LintRequest, args: bazel_workflows.NativeRequest, *, lockfile_readonly: bool = False
) -> Iterator[tuple[Path, Path, Path, Path]]:
    """Prepare one governed lint tree and hold its lock through the gate.

    Tree, configuration and workspace preparation stay in one place; the
    caller's scope, ownership and deferral decisions stay explicit. Explicit
    feature selections travel with the caller's request, so a prepared tree
    never depends on ambient rc inheritance.
    """
    tree = bazel_workspace.workspace_path(request.tree_path, request.config)
    executable = bazel_workspace.bazel_executable()
    native.require_supported_source()
    with build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        # Keep the repository's default policy selected across both completion phases.
        lint_configuration = None
        if request.check is not None or request.header_filter is not None or request.clang_tidy_args:
            lint_configuration = tree / "configuration" / "lint.json"
            _write_lint_config(
                lint_configuration, _lint_configuration(request.check, request.header_filter, request.clang_tidy_args)
            )
        prepared = bazel_workspace.prepare_workspace(
            tree,
            request.config,
            prefix=request.vcpkg_prefix,
            graph_options=bazel_workflows.feature_options(args),
            lint_configuration=lint_configuration,
            lockfile_readonly=lockfile_readonly,
        )
        run_dir = _new_run_dir(tree)
        if request.debug:
            print(f"Bazel lint tree: {tree}", file=sys.stderr)
        if request.debug or lockfile_readonly:
            print(f"Bazel lint evidence: {run_dir}", file=sys.stderr, flush=True)
        yield executable, tree, prepared, run_dir


def _run(args: bazel_workflows.NativeRequest, scope: SourceScope, *, lockfile_readonly: bool) -> int:
    request = _common_lint_request(args)
    output, jobs, config, remote_cache = request.output, request.jobs, request.config, request.remote_cache
    debug = request.debug
    targets = _owner_targets(getattr(args, "owner_targets", []))
    target_only = bool(targets) and not scope.files

    selected_paths: list[Path] = []
    explicit_selected = _relative_selected([absolute_path(PROJECT_ROOT / name) for name in scope.explicit_files])
    stale_status = 0
    if not target_only:
        explicit_keys = {_scope_key(path) for path in explicit_selected}
        resolved = _relative_selected([absolute_path(PROJECT_ROOT / name) for name in scope.files])
        explicit_sources = [name for name in resolved if _scope_key(name) in explicit_keys]
        scanned_sources = [name for name in resolved if _scope_key(name) not in explicit_keys]
        buckets = tidy_engine.classify_existing(scanned_sources, False, require_checkout=True)
        if explicit_sources:
            explicit_buckets = tidy_engine.classify_existing(explicit_sources, True, require_checkout=True)
            for mode, paths in explicit_buckets.items():
                buckets[mode].extend(paths)
        selected_paths = [*buckets["STRICT"], *buckets["RELAXED"]]
        if not selected_paths:
            print("No C++ lint sources selected.", file=sys.stderr)
            return 0
        if request.check is None:
            stale = find_stale_nolint_suppressions(buckets)
            if stale:
                _print_stale(stale)
                stale_status = 1
                # Source findings cannot bypass native tool/context admission;
                # explicit missing compile contexts must still fail with 2.

    selected = _relative_selected(selected_paths) if selected_paths else []
    for path in selected:
        _validate_selected_suffix(path)

    with _lint_workspace(request, args, lockfile_readonly=lockfile_readonly) as (
        executable,
        tree,
        prepared,
        run_dir,
    ):
        if debug:
            print(f"Bazel lint selected: {' '.join(selected) if selected else '<explicit owners>'}", file=sys.stderr)

        graph = _query_graph(executable, prepared, run_dir, targets)
        held: dict[str, tuple[str, ...]] = {}
        if targets:
            _validate_targets(graph, targets)
            requested = targets
        else:
            requested, held = _map_selected(graph, selected)
            if not requested:
                return max(stale_status, _finish_unowned(selected, explicit_files=explicit_selected))

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
            print("Deferred lint owners incompatible with the selected configuration:", file=sys.stderr)
            for owner in sorted(incompatible):
                print(f"  {owner}", file=sys.stderr)
        if not compatible:
            if targets:
                raise ValueError("all explicit lint owners are incompatible with this platform")
            feature_disabled = _disabled_feature_paths(graph, selected, incompatible, args)
            return max(
                stale_status,
                _finish_unowned(selected, explicit_files=explicit_selected, feature_disabled=feature_disabled),
            )

        direct_groups = _lint_output_groups(selected)
        discovery_groups = (
            direct_groups
            if direct_groups == (_OUTPUT_GROUP,)
            else tuple(sorted((*direct_groups, "aobus_lint_sources")))
        )

        def report_pass(
            name: str, owners: Sequence[str], consumers: Mapping[str, tuple[str, str]], groups: tuple[str, ...]
        ) -> tuple[Path, tuple[bazel_lint_reports.OwnerInventory, ...]]:
            return _report_pass(
                executable,
                tree,
                prepared,
                run_dir,
                name=name,
                aspect=_ASPECT,
                output_groups=groups,
                profile="lint",
                config=config,
                owners=owners,
                consumers=consumers,
                jobs=jobs,
                remote_cache=remote_cache,
                debug=debug,
            )

        # Fragments never have a direct companion. For ordinary headers let the
        # configured index decide; the loading query alone cannot prove pairing.
        needs_donor = any(PurePosixPath(path).suffix.lower() in _INCLUDE_FRAGMENT_SUFFIXES for path in selected)
        initial_groups = discovery_groups if needs_donor else direct_groups
        bep, inventories = report_pass("build", compatible, {}, initial_groups)
        consumers: dict[str, tuple[str, str]] = {}
        if not target_only:
            selected = sorted(_match_paths(set(selected), inventories))
            missing = _unreported(selected, inventories)
            if missing and initial_groups != discovery_groups:
                bep, inventories = report_pass("discovery", compatible, {}, discovery_groups)
            consumers = _choose_consumers(missing, inventories)
            further = {
                owner
                for path in missing
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
                    bep, inventories = report_pass("consumer-owners", compatible, {}, discovery_groups)
                    selected = sorted(_match_paths(set(selected), inventories))
                    consumers = _choose_consumers(_unreported(selected, inventories), inventories)
            if consumers:
                bep, inventories = report_pass("consumers", compatible, consumers, direct_groups)

        deferred: set[str] = set()
        if targets:
            for inventory in inventories:
                if not inventory.has_native_tu:
                    raise ValueError(
                        "explicit lint owner produced no native translation-unit reports: " + inventory.owner
                    )
        if target_only:
            # Paired header mains replay an owner's own native contexts, so
            # they belong to its scope; unpaired declared headers do not.
            selected = sorted({report.identity.main for inventory in inventories for report in inventory.reports})
        else:
            feature_disabled = _disabled_feature_paths(graph, selected, incompatible, args) if not targets else {}
            deferred = _deferred_paths(
                selected,
                inventories,
                allow=not targets,
                explicit_files=explicit_selected,
                feature_disabled=feature_disabled,
            )
            selected = [path for path in selected if path not in deferred]
            _print_deferred(deferred, feature_disabled=feature_disabled)
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
            return stale_status
        reasons = dict.fromkeys(_unreported(selected, inventories), _NO_CONSUMER)
        status = bazel_lint_reports.gate_reports(inventories, set(selected), output, reasons=reasons)
        if _context_failed(inventories, set(selected)):
            return 2
        return max(stale_status, status)


def _context_failed(inventories: Sequence[bazel_lint_reports.OwnerInventory], selected: set[str]) -> bool:
    """Classify validated report fields without changing cached graph helpers."""
    chosen = [report for inventory in inventories for report in inventory.reports if report.identity.main in selected]
    return not selected.issubset({report.identity.main for report in chosen}) or any(
        report.outcome == "compiler_error" for report in chosen
    )


def run(args: bazel_workflows.NativeRequest, scope: SourceScope, *, lockfile_readonly: bool = False) -> int:
    """Return 0 for success, 1 for findings, 2 for tool/admission/context failure.

    Report-only requests suppress only ordinary findings, never failures.
    """
    try:
        status = _run(args, scope, lockfile_readonly=lockfile_readonly)
        return 0 if status == 1 and getattr(args, "report_only", False) else status
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Bazel lint failed: {error}", file=sys.stderr)
        return 2


def _finish_unowned_analyzer(selected: Sequence[str], *, explicit_files: Collection[str]) -> int:
    """Fail uncovered explicit/native files; defer only discovered foreign files."""
    deferred = _deferred_paths(selected, (), allow=True, explicit_files=explicit_files)
    failures = [path for path in selected if path not in deferred]
    if failures:
        print(
            "ERROR: no compatible native compile owner covers selected path(s): " + ", ".join(sorted(failures)),
            file=sys.stderr,
        )
        return 2
    if deferred:
        _print_deferred(deferred)
    # An entirely deferred scope never claims a clean analysis.
    return 0


def _run_analyzer(args: AnalyzerRequest, files: Sequence[str], *, explicit_files: Sequence[str]) -> int:
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
    selected = _relative_selected([absolute_path(PROJECT_ROOT / name) for name in files])
    explicit_selected = set(_relative_selected([absolute_path(PROJECT_ROOT / name) for name in explicit_files]))
    for path in selected:
        _validate_selected_suffix(path)
    tree = bazel_workspace.workspace_path(
        _path_argument(getattr(args, "path", None), "path")
        or str(build_dir.platform_profile().build_root / _ANALYZER_TREE),
        _ANALYZER_CONFIG,
    )
    executable = bazel_workspace.bazel_executable()
    native.require_supported_source()
    with build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        analyzer_configuration = tree / "configuration" / "analyzer.json"
        _write_lint_config(analyzer_configuration, _lint_configuration(check, None, tidy_args, alpha=alpha))
        prepared = bazel_workspace.prepare_workspace(tree, _ANALYZER_CONFIG, lint_configuration=analyzer_configuration)
        run_dir = _new_run_dir(tree, "analyzer")
        if debug:
            print(f"Analyzer tree: {tree}", file=sys.stderr)
            print(f"Analyzer evidence: {run_dir}", file=sys.stderr)
            print(f"Analyzer selected: {' '.join(selected)}", file=sys.stderr)

        graph = _query_graph(executable, prepared, run_dir, [])
        requested, held = _map_selected(graph, selected)
        if not requested:
            return _finish_unowned_analyzer(selected, explicit_files=explicit_selected)

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
                config=_ANALYZER_CONFIG,
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
            return _finish_unowned_analyzer(selected, explicit_files=explicit_selected)

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

        deferred = _deferred_paths(selected, inventories, allow=True, explicit_files=explicit_selected)
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
            return 2
        reasons = dict.fromkeys(_unreported(selected, inventories), _NO_CONSUMER)
        status = bazel_lint_reports.gate_analyzer_reports(
            inventories,
            set(selected),
            output,
            include_external=include_external,
            fail_on_diagnostics=fail_on_diagnostics,
            reasons=reasons,
        )
        return 2 if _context_failed(inventories, set(selected)) else status


def run_analyzer(args: AnalyzerRequest, files: Sequence[str], *, explicit_files: Sequence[str]) -> int:
    """Return 0/1/2, retaining per-file explicit provenance through deferral."""
    try:
        return _run_analyzer(args, files, explicit_files=explicit_files)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Analyzer failed: {error}", file=sys.stderr)
        return 2
