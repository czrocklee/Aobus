"""Configured target identities and declared test-source ownership for the gate.

The unconfigured query graph says which executables declare each registered
test source; configured queries say which tests the gate runs and which
executables the completion roots reach on this host. Bazel owns everything
else about the build graph.
"""

from __future__ import annotations

import re
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import testaudit
from .paths import absolute_path

if TYPE_CHECKING:
    from .bazel_lint import QueryGraph

_COMPILE_KINDS = {"cc_library", "objc_library", "cc_binary", "cc_test"}
_EXECUTABLE_KINDS = {"cc_binary", "cc_test"}


@dataclass(frozen=True, order=True)
class ConfiguredTarget:
    label: str
    configuration: str


def _label(value: Any) -> str:
    if not isinstance(value, str):
        raise ValueError("ownership label must be a string")
    text: str = value.removeprefix("@@").removeprefix("@") if value.startswith(("@@//", "@//")) else value
    if not re.fullmatch(r"//[A-Za-z0-9._+~/-]*:[A-Za-z0-9._+~/-]+", text):
        raise ValueError(f"invalid first-party ownership label: {text}")
    package, name = text[2:].split(":")
    if any(part in {"", ".", "..", "..."} for path in (package, name) if path for part in path.split("/")):
        raise ValueError(f"unsafe ownership label: {text}")
    return text


def _first_party(value: Any) -> bool:
    return isinstance(value, str) and value.startswith(("//", "@//", "@@//"))


def _path(value: Any) -> str:
    return str(value)


def _configuration(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("configured ownership requires a full configuration checksum")
    return value


def extract_configured_targets(document: Any) -> frozenset[ConfiguredTarget]:
    """Read first-party rule identities from modern cquery --output=jsonproto.

    Collect incompatible rules too, then subtract extract_compatibility's
    positive results for the aspect join. The caller's cquery expression must
    exclude aliases, config settings, test suites and native constraint metadata
    without independent aspect nodes. Aliases resolve to their actual targets.
    Leaked alias/constraint rules fail closed rather than shrinking this
    independent universe by label. Source-file results do not have aspect nodes.
    """
    if not isinstance(document, dict) or not isinstance(document.get("results"), list):
        raise ValueError("cquery ownership evidence must contain results")
    configurations = {str(item["id"]): item for item in document.get("configurations", [])}
    result = set()
    for item in document["results"]:
        target = item["target"]
        if target.get("type") not in {"RULE", "SOURCE_FILE", "GENERATED_FILE", "PACKAGE_GROUP", "ENVIRONMENT_GROUP"}:
            raise ValueError("unknown cquery ownership target type")
        if target["type"] != "RULE":
            continue
        rule = target["rule"]
        if not _first_party(rule["name"]):
            continue
        if rule.get("ruleClass") == "alias":
            raise ValueError("configured ownership universe must exclude transparent alias nodes")
        if rule.get("ruleClass") in {"constraint_setting", "constraint_value"}:
            raise ValueError("configured ownership universe must exclude native constraint metadata")
        configuration = configurations[str(item["configurationId"])]
        identity = ConfiguredTarget(_label(rule["name"]), _configuration(configuration["checksum"]))
        if identity in result:
            raise ValueError(f"duplicate cquery configured target: {identity}")
        result.add(identity)
    if not result:
        raise ValueError("no configured first-party rules in cquery ownership evidence")
    return frozenset(result)


def extract_compatibility(text: str, expected_targets: Iterable[ConfiguredTarget]) -> frozenset[ConfiguredTarget]:
    """Join label|True/False provider rows to the independent configured universe.

    cquery's Starlark target exposes no configuration checksum. Repeated labels
    must therefore agree and have exactly the independently observed number of
    configurations; conflicting statuses cannot be safely joined by label.
    """
    expected = frozenset(expected_targets)
    counts = Counter(target.label for target in expected)
    observed: Counter[str] = Counter()
    statuses: dict[str, bool] = {}
    for row in text.splitlines():
        fields = row.split("|")
        if len(fields) != 2 or fields[1] not in {"True", "False"}:
            raise ValueError(f"malformed configured compatibility row: {row!r}")
        label = _label(fields[0])
        incompatible = fields[1] == "True"
        if label not in counts:
            raise ValueError(f"unexpected configured compatibility label: {label}")
        if label in statuses and statuses[label] != incompatible:
            raise ValueError(f"conflicting configured compatibility statuses: {label}")
        statuses[label] = incompatible
        observed[label] += 1
    if not expected or observed != counts:
        raise ValueError("configured compatibility row counts disagree with cquery universe")
    return frozenset(target for target in expected if statuses[target.label])


def discover_registration_sources(root: Path) -> dict[str, tuple[testaudit.TestCase, ...]]:
    """Reuse the lexical detector, including guarded, manual and Benchmark cases."""
    # Discovery returns paths in the shared audit spelling; relate them to the same root.
    root = absolute_path(root)
    result = {}
    for path in testaudit.discover_test_files(root):
        cases, issues = testaudit.scan_test_file(path)
        issues.extend(testaudit.registration_placement_issues(path, cases))
        if issues:
            raise ValueError("\n".join(issue.format(root) for issue in issues))
        if cases:
            result[path.relative_to(root).as_posix()] = tuple(cases)
    return result


def _attribute_labels(rule: Any, name: str) -> tuple[str, ...]:
    value = rule.attributes.get(name, ())
    return (value,) if isinstance(value, str) else value if isinstance(value, tuple) else ()


def _declared_sources(graph: QueryGraph) -> dict[str, frozenset[str]]:
    result: dict[str, frozenset[str]] = {}

    def collect(label: str, active: frozenset[str]) -> frozenset[str]:
        if label in active or len(active) > 256:
            raise ValueError(f"cyclic or excessive declared ownership dependencies: {label}")
        if label in result:
            return result[label]
        rule = graph.rules[label]
        sources = set(graph.native_sources(label)) if rule.kind in _COMPILE_KINDS else set()
        for name in ("deps", "implementation_deps", "srcs", "actual"):
            for value in _attribute_labels(rule, name):
                if not _first_party(value):
                    continue
                dependency = _label(value)
                if dependency in graph.rules and graph.rules[dependency].kind not in _EXECUTABLE_KINDS:
                    sources.update(collect(dependency, active | {label}))
        result[label] = frozenset(sources)
        return result[label]

    for label in graph.rules:
        collect(label, frozenset())
    return result


def ownership_errors(
    declared_graph: QueryGraph,
    registration_sources: Iterable[str],
    *,
    reached: Iterable[str],
    executed: Iterable[str],
    incompatible: Iterable[str],
) -> list[str]:
    """Require a declared executable owner that the gate runs when this host can build it.

    A ``cc_test`` owner must be in ``executed``, the test selection the gate
    runs: a test that is only built below a completion root never runs. A
    ``cc_binary`` owner holds deliberately run registrations, such as
    benchmarks, and only has to be in ``reached``, the executables below the
    completion roots. ``incompatible`` are declared owners this host cannot
    build. Sources a compatible owner drops through select() are not
    distinguished from compiled ones.
    """
    sources = frozenset(_path(path) for path in registration_sources)
    reached_labels = frozenset(reached)
    executed_labels = frozenset(executed)
    incompatible_labels = frozenset(incompatible)
    declared = _declared_sources(declared_graph)
    owners: dict[str, set[str]] = {}
    for label, rule in declared_graph.rules.items():
        if rule.kind in _EXECUTABLE_KINDS:
            for source in declared[label] & sources:
                owners.setdefault(source, set()).add(label)
    errors = [f"{source}: no declared executable owner" for source in sorted(sources - owners.keys())]
    for label in sorted({label for labels in owners.values() for label in labels} - incompatible_labels):
        if declared_graph.rules[label].kind == "cc_test":
            if label not in executed_labels:
                errors.append(f"{label}: test is not in the executed test selection")
        elif label not in reached_labels:
            errors.append(f"{label}: test executable is absent from the completion roots")
    return errors


def registration_owners(declared_graph: QueryGraph, registration_sources: Iterable[str]) -> list[str]:
    """Return the executables that declare at least one registered test source."""
    sources = frozenset(_path(path) for path in registration_sources)
    declared = _declared_sources(declared_graph)
    return sorted(
        label
        for label, rule in declared_graph.rules.items()
        if rule.kind in _EXECUTABLE_KINDS and declared[label] & sources
    )
