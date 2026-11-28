"""Identity, JSON validation and diagnostic outcomes shared by native lint report producers and readers."""

from __future__ import annotations

import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from ao.policy.tidy_diagnostics import has_fatal_compiler_error, log_has_compile_command_error, parse_diagnostic_header


def json_object(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label}: expected an object")
    return value


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def load_json(path: Path) -> dict[str, Any]:
    """Read a versioned object, rejecting ambiguous keys and unknown schemas."""
    value = json_object(json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object), str(path))
    if type(value.get("schema")) is not int or value["schema"] != 1:
        raise ValueError(f"{path}: unsupported schema")
    return value


def nonempty_text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"{label}: expected a nonempty string")
    return value


def exec_path(value: object) -> str:
    """Validate the exact portable exec-relative spelling emitted by native Files."""
    path = nonempty_text(value, "exec path")
    if "\\" in path or ":" in path or path.startswith("/") or any(p in ("", ".", "..") for p in path.split("/")):
        raise ValueError(f"invalid exec-relative path: {path}")
    return path


def exec_paths(value: object, label: str) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise ValueError(f"{label}: expected an array")
    result = tuple(exec_path(item) for item in value)
    if len(set(result)) != len(result):
        raise ValueError(f"{label}: duplicate paths")
    return result


def canonical_owner(value: object) -> str:
    owner = nonempty_text(value, "owner")
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
        data = json_object(value, "identity")
        keys = {"owner", "configuration", "source", "main", "outputs", "mode", "platform"}
        if set(data) != keys:
            raise ValueError("identity must contain exactly the context identity fields")
        owner = canonical_owner(data["owner"])
        if data["owner"] != owner:
            raise ValueError("identity owner must be canonical")
        configuration = exec_path(data["configuration"])
        if re.fullmatch(r"bazel-out/[^/]+/bin", configuration) is None:
            raise ValueError("invalid native bin directory")
        outputs = exec_paths(data["outputs"], "native outputs")
        if not outputs or any(not path.startswith(configuration + "/") for path in outputs):
            raise ValueError("native outputs do not belong to the context configuration")
        if data["mode"] not in ("STRICT", "RELAXED", "ANALYZER") or data["platform"] not in (
            "linux",
            "macos",
            "windows",
        ):
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
