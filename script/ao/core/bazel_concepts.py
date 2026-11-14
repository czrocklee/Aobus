"""Fresh native include observations and public Clang ASTs for the concept report.

Bazel supplies configured actions, requested DefaultInfo artifacts and generated
prerequisites. Declared inputs select/bind contexts, never stand in for observed
includes. Normal returns and cancellation cleanup hold the caller's reentrant
lock through child reaping; fatal wait failures are explicitly unconfirmed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from collections.abc import Iterable, Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from functools import cache
from pathlib import Path, PurePosixPath
from threading import Lock
from typing import Any

from . import bazel_contract, bazel_workspace, concept_report, concept_scope
from . import bazel_native as native
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT

_ROOTS = ("//:check_products", "//:check_native_tests", "//:check_runs")
_TESTS = "//:check_tests"
_AST_SDK = "@aobus_lint_sdk//:metadata.json"
_MANIFESTS = frozenset(("@aobus_native_deps//:manifest.json", "@aobus_test_deps//:manifest.json"))
_ASPECT = "//bazel:compile_context.bzl%compile_context_aspect"
_SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx", ".c++", ".C", ".m", ".mm"))
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_RETENTION = (
    "successful-context .ii/.i bodies are consumed after transport/closure/input/output verification, "
    "with path/SHA256/bytes and retained=false receipts; failed-context bodies "
    "and all streams/dependencies/AST/identity evidence remain; inventory AST parses without JSON dumping; "
    "later cohort failure does not restore consumed bodies; "
    "no original-output/SDK/tree/global cleanup or size budget"
)
_PROVIDER = """json.encode({
 'label': str(target.label),
 'incompatible': 'IncompatiblePlatformProvider' in providers(target),
 'files': [] if 'IncompatiblePlatformProvider' in providers(target) else [
  {'path': f.path, 'root': f.root.path, 'source': f.is_source,
   'directory': f.is_directory, 'owner': str(f.owner)}
  for f in providers(target)['DefaultInfo'].files.to_list()],
})"""


def _pairs(pairs: Iterable[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _json(text: str) -> Any:
    return json.loads(text, object_pairs_hook=_pairs)


def _strings(value: object) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item or "\x00" in item for item in value):
        raise ValueError("expected an array of nonempty strings without NUL")
    return value


def _relative(value: object) -> str:
    if (
        not isinstance(value, str)
        or not value
        or value.startswith("/")
        or any(character in value for character in "\\:\x00")
        or any(part in {"", ".", ".."} for part in value.split("/"))
    ):
        raise ValueError(f"invalid exec-relative artifact: {value!r}")
    return value


def _label(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(
        r"(?:@@?[A-Za-z0-9._+~-]*)?//[A-Za-z0-9._+~/-]*:[A-Za-z0-9._+~/-]+", value
    ):
        raise ValueError(f"invalid configured label: {value!r}")
    if any(part in {".", ".."} for part in value.split("/")):
        raise ValueError(f"unsafe configured label: {value}")
    return value.removeprefix("@@").removeprefix("@") if value.startswith(("@@//", "@//")) else value


def _checksum(value: object) -> str:
    if not isinstance(value, str) or not _SHA.fullmatch(value):
        raise ValueError("configured action requires a full configuration checksum")
    return value


def _key(path: Path) -> str:
    return os.path.normcase(str(path.resolve()))


def _same_source(candidate: Path, original: Path) -> bool:
    # Native controls did not establish an inode-only identity oracle on Windows.
    # Bind canonical native names and content; POSIX hard links also bind bytes.
    identity = _key(candidate) == _key(original)
    if not identity and os.name != "nt":
        identity = candidate.samefile(original)
    return identity and _digest(candidate) == _digest(original)


def _digest(path: Path) -> str:
    with path.open("rb") as stream:
        result = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def _environment(value: object, *, windows: bool) -> tuple[tuple[str, str], ...]:
    if not isinstance(value, list):
        raise ValueError("action environment must be an array")
    result = _pairs((entry["key"], entry.get("value", "")) for entry in value)
    if any(
        not isinstance(key, str)
        or not key
        or "=" in key
        or "\x00" in key
        or not isinstance(setting, str)
        or "\x00" in setting
        for key, setting in result.items()
    ) or (windows and len({key.casefold() for key in result}) != len(result)):
        raise ValueError("invalid or ambiguous declared action environment")
    return tuple(sorted(result.items()))


@dataclass(frozen=True)
class CompileAction:
    owner: str
    configuration: str
    is_tool: bool
    mnemonic: str
    source: str
    source_argument: str
    argv: tuple[str, ...]
    environment: tuple[tuple[str, str], ...]
    inputs: frozenset[str]
    outputs: tuple[str, ...]

    @property
    def identity(self) -> tuple[str, str, bool, tuple[str, ...]]:
        return self.owner, self.configuration, self.is_tool, self.outputs

    def provenance(self) -> dict[str, Any]:
        return {
            **asdict(self),
            "argv": list(self.argv),
            "outputs": list(self.outputs),
            "inputs": sorted(self.inputs),
            "environment": dict(self.environment),
        }


class ActionGraph:
    """One exact aquery graph; output reachability, not target attachment, selects TUs."""

    def __init__(self, document: object, platform: str):
        if not isinstance(document, dict):
            raise ValueError("aquery must return an object")
        tables = {}
        for name in ("targets", "configuration", "pathFragments", "artifacts", "depSetOfFiles"):
            rows = document.get(name, [])
            if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
                raise ValueError(f"invalid aquery {name} table")
            tables[name] = _pairs((str(row["id"]), row) for row in rows)
        fragments, artifacts, depsets = (tables[name] for name in ("pathFragments", "artifacts", "depSetOfFiles"))

        @cache
        def fragment(identifier: str, active: tuple[str, ...] = ()) -> str:
            if identifier in active:
                raise ValueError("recursive aquery path fragment")
            row = fragments[identifier]
            name = row["label"]
            if (
                not isinstance(name, str)
                or not name
                or any(character in name for character in "/\\:\x00")
                or name in {".", ".."}
            ):
                raise ValueError("invalid aquery path component")
            parent = row.get("parentId")
            return (fragment(str(parent), (*active, identifier)) + "/" if parent not in (None, 0, "0") else "") + name

        paths = {identifier: _relative(fragment(str(row["pathFragmentId"]))) for identifier, row in artifacts.items()}

        @cache
        def inputs(identifier: str, active: tuple[str, ...] = ()) -> frozenset[str]:
            if identifier in active:
                raise ValueError("recursive aquery input depset")
            row = depsets[identifier]
            direct, children = row.get("directArtifactIds", []), row.get("transitiveDepSetIds", [])
            if len(set(map(str, direct))) != len(direct) or len(set(map(str, children))) != len(children):
                raise ValueError("duplicate aquery depset reference")
            return frozenset(paths[str(key)] for key in direct).union(
                *(inputs(str(key), (*active, identifier)) for key in children)
            )

        records = document.get("actions")
        if not isinstance(records, list) or not records:
            raise ValueError("aquery returned no actions")
        cpp = iter(bazel_contract.extract_compile_actions(document, require_compile_context=platform == "windows"))
        self.compiles: dict[int, CompileAction] = {}
        self.records: dict[int, tuple[str, str, bool, str, frozenset[str], tuple[str, ...]]] = {}
        self.producers: dict[str, int] = {}
        self.aliases: dict[str, str] = {}
        self.details: dict[int, str] = {}
        identities = set()
        for index, row in enumerate(records):
            owner = _label(tables["targets"][str(row["targetId"])]["label"])
            configuration = tables["configuration"][str(row["configurationId"])]
            checksum = _checksum(configuration.get("checksum"))
            is_tool = configuration.get("isTool", False)
            if type(is_tool) is not bool:
                raise ValueError("configuration isTool must be boolean")
            mnemonic = row["mnemonic"]
            bound = frozenset().union(*(inputs(str(key)) for key in row.get("inputDepSetIds", [])))
            outputs = tuple(sorted(paths[str(key)] for key in row.get("outputIds", [])))
            if not outputs or len(set(outputs)) != len(outputs):
                raise ValueError("action requires distinct declared outputs")
            identity = owner, checksum, is_tool, outputs
            if identity in identities:
                raise ValueError("duplicate configured action identity")
            identities.add(identity)
            self.records[index] = owner, checksum, is_tool, mnemonic, bound, outputs
            self.details[index] = json.dumps(
                {
                    name: row[name]
                    for name in (
                        "actionKey",
                        "arguments",
                        "environmentVariables",
                        "paramFiles",
                        "fileContents",
                        "executionInfo",
                    )
                    if name in row
                },
                sort_keys=True,
            )
            for output in outputs:
                if output in self.producers:
                    raise ValueError(f"multiple native producers for {output}")
                self.producers[output] = index
            if mnemonic == "Symlink" and any("_virtual_includes" in PurePosixPath(path).parts for path in outputs):
                if len(outputs) != 1 or len(bound) != 1:
                    raise ValueError("virtual header requires exactly one Symlink input and output")
                self.aliases[outputs[0]] = next(iter(bound))
            if mnemonic not in {"CppCompile", "ObjcCompile"}:
                continue
            params = _pairs((entry["execPath"], _strings(entry["arguments"])) for entry in row.get("paramFiles", []))

            def expand(
                arguments: list[str], active: tuple[str, ...] = (), params: Mapping[str, list[str]] = params
            ) -> list[str]:
                expanded = []
                for argument in arguments:
                    if argument.startswith("@"):
                        path = argument[1:]
                        if path not in params or path in active:
                            raise ValueError("opaque or recursive compile response file")
                        expanded.extend(expand(params[path], (*active, path)))
                    else:
                        expanded.append(argument)
                return expanded

            argv = next(cpp).argv if mnemonic == "CppCompile" else tuple(expand(_strings(row.get("arguments"))))
            _strings(list(argv))
            sources = [
                argument
                for argument in argv[1:]
                if argument.replace("\\", "/") in bound
                and PurePosixPath(argument.replace("\\", "/")).suffix in _SOURCE_SUFFIXES
            ]
            if len(sources) != 1:
                raise ValueError(f"native action lacks one exact source argument: {owner}")
            self.compiles[index] = CompileAction(
                owner,
                checksum,
                is_tool,
                mnemonic,
                sources[0].replace("\\", "/"),
                sources[0],
                tuple(argv),
                _environment(row.get("environmentVariables", []), windows=platform == "windows"),
                bound,
                outputs,
            )

    def reachable(self, outputs: Sequence[str]) -> tuple[CompileAction, ...]:
        visited: set[int] = set()

        def visit(path: str, active: frozenset[int]) -> None:
            producer = self.producers.get(path)
            if producer is None:
                parents = [parent for parent in self.producers if path.startswith(parent + "/")]
                producer = self.producers[max(parents, key=len)] if parents else None
            if producer is None:
                if path.startswith("bazel-out/"):
                    raise ValueError(f"generated input has no declared producer: {path}")
                return
            if producer in active:
                raise ValueError("cyclic action output-to-input graph")
            if producer in visited:
                return
            visited.add(producer)
            for dependency in self.records[producer][4]:
                visit(dependency, active | {producer})

        for output in outputs:
            visit(_relative(output), frozenset())
        self.required_inputs = frozenset().union(*(self.records[index][4] for index in visited))
        return tuple(
            sorted(
                (action for index, action in self.compiles.items() if index in visited), key=lambda item: item.identity
            )
        )

    def proof(self) -> list[Any]:
        return sorted(
            (
                owner,
                checksum,
                tool,
                mnemonic,
                sorted(inputs),
                outputs,
                self.compiles[index].argv if index in self.compiles else (),
                self.compiles[index].environment if index in self.compiles else (),
                self.details[index],
            )
            for index, (owner, checksum, tool, mnemonic, inputs, outputs) in self.records.items()
        )


class _Guard:
    """Bounded identities of named files and declared generated-tree members."""

    def __init__(self) -> None:
        self.files: dict[str, tuple[Path, Any]] = {}
        self.trees: dict[str, tuple[Path, tuple[str, ...]]] = {}
        self.lock = Lock()

    @staticmethod
    def state(path: Path) -> Any:
        try:
            information = path.stat()
        except FileNotFoundError:
            return None
        if not path.is_file():
            raise ValueError(f"expected a regular read/input/output file: {path}")
        return (
            str(path.resolve()),
            os.readlink(path) if path.is_symlink() else None,
            information.st_size,
            information.st_mode,
            information.st_mtime_ns,
            information.st_ino if os.name != "nt" else None,
            _digest(path),
        )

    def add(self, path: Path, *, missing: bool = False, digest: str | None = None) -> None:
        name = str(path.absolute())
        with self.lock:
            if name in self.files and digest is None:
                return
        value = self.state(path)
        if value is None and not missing:
            raise ValueError(f"missing observation input: {path}")
        if digest is not None and (not _SHA.fullmatch(digest) or value is None or value[-1] != digest):
            raise ValueError(f"admitted native input digest changed: {path}")
        with self.lock:
            if name in self.files and self.files[name][1] != value:
                raise ValueError(f"input changed during concept acquisition: {path}")
            self.files[name] = path, value

    def generated_tree(self, path: Path) -> None:
        def members(directory: Path) -> list[Path]:
            result = []
            for item in sorted(directory.iterdir()):
                if item.is_dir():
                    if item.is_symlink():
                        raise ValueError(f"linked directory in declared generated tree: {item}")
                    result.extend(members(item))
                elif item.is_file():
                    result.append(item)
                else:
                    raise ValueError(f"unsupported generated-tree member: {item}")
            return result

        files = members(path)
        names = tuple(str(item.relative_to(path)) for item in files)
        if str(path) in self.trees and self.trees[str(path)][1] != names:
            raise ValueError(f"generated-tree membership changed: {path}")
        self.trees[str(path)] = path, names
        for item in files:
            self.add(item)

    def verify_paths(self, paths: Iterable[Path]) -> None:
        for path in paths:
            name = str(path.absolute())
            with self.lock:
                if name not in self.files:
                    raise ValueError(f"unbound context read input: {path}")
                original, expected = self.files[name]
            if self.state(original) != expected:
                raise ValueError(f"context read input changed: {path}")

    def verify(self) -> None:
        for path, expected in tuple(self.files.values()):
            if self.state(path) != expected:
                raise ValueError(f"concept observation input/output changed: {path}")
        for path, _names in tuple(self.trees.values()):
            self.generated_tree(path)

    def provenance(self) -> dict[str, Any]:
        rows = [
            {"path": str(path), "state": value}
            for path, value in sorted(self.files.values(), key=lambda item: str(item[0]))
        ]
        return {
            "files": len(rows),
            "sha256": hashlib.sha256(json.dumps(rows, sort_keys=True).encode()).hexdigest(),
            "records": rows,
        }


def _process(argv: Sequence[str], *, cwd: Path, environment: Mapping[str, str], stdout: Path, stderr: Path) -> int:
    """Do not release the tree lock on an interrupt until the foreground child exits."""
    with stdout.open("xb") as out, stderr.open("xb") as err:
        child = subprocess.Popen(list(argv), cwd=cwd, env=dict(environment), stdout=out, stderr=err)
        try:
            return child.wait()
        except BaseException as initial:
            errors = [initial]
            try:
                child.kill()
            except BaseException as error:
                errors.append(error)
            # A failed kill must never skip wait. Repeated interrupts defer lock
            # release; fatal OS wait failure is explicitly reaping-unconfirmed.
            while True:
                try:
                    child.wait()
                    break
                except KeyboardInterrupt as error:
                    errors.append(error)
                except BaseException as error:
                    raise BaseExceptionGroup("concept child reaping unconfirmed", [*errors, error]) from None
            if len(errors) > 1:
                raise BaseExceptionGroup("concept child reaped; cancellation cleanup failed", errors) from None
            raise


class _Attempt:
    def __init__(self, args: argparse.Namespace, root: Path, tree: Path):
        self.root, self.tree, self.config, self.jobs = root, tree, args.config, args.jobs
        self.client_environment = native.child_environment(isolate_rc=True)
        self.client_fields = {
            key.upper(): value
            for key, value in self.client_environment.items()
            if key.casefold() in {"systemroot", "systemdrive", "tmp", "temp"}
        }
        directory = tree / "concepts"
        directory.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="report-", dir=directory))
        self.executable = bazel_workspace.bazel_executable()
        base = bazel_workspace.prepare_workspace(tree, args.config, prefix=args.vcpkg_prefix)
        self.prepared = self.directory / "configuration"
        self.prepared.mkdir()
        (self.prepared / "aobus.bazelrc").write_bytes(
            (base / "aobus.bazelrc").read_bytes() + b"\ncommon --lockfile_mode=error\n"
        )
        self.guard = _Guard()
        self.guard.add(self.prepared / "aobus.bazelrc")
        self.guard.add(base / "request.json")
        self.guard.add(self.executable)
        request = _json((base / "request.json").read_text(encoding="utf-8"))
        if request.get("host") != native.native_platform() or Path(request.get("source", "")).resolve() != root:
            raise ValueError("prepared request does not identify the selected source/host")
        for name in ("MODULE.bazel", ".bazelrc", "REPO.bazel", ".bazelversion", "script/ao/bazel-toolchain.json"):
            self.guard.add(root / name)
        if (
            native.native_platform() == "windows"
            and (root / ".bazelversion").read_text(encoding="utf-8").strip() != "9.2.0"
        ):
            raise ValueError("Windows native environment completion is admitted only for pinned Bazel 9.2.0")
        self.sequence = 0
        self.source_declarations: dict[str, dict[str, Any]] = {}
        for path in (
            root / "MODULE.bazel.lock",
            Path(__file__),
            Path(concept_report.__file__),
            Path(concept_scope.__file__),
        ):
            self.guard.add(path, missing=True)

    def invoke(self, action: str, arguments: Sequence[str]) -> str:
        self.sequence += 1
        argv = [*bazel_workspace.invocation(self.executable, self.prepared, isolate_rc=True), action]
        if action not in {"query", "info"}:
            argv.append(f"--jobs={self.jobs}")
        argv.extend(arguments)
        stem = self.directory / f"{self.sequence:03}-{action}"
        receipt: dict[str, Any] = {
            "argv": argv,
            "status": "incomplete",
            "cwd": str(self.root),
            "clientFields": self.client_fields,
        }
        try:
            status = _process(
                argv,
                cwd=self.root,
                environment=self.client_environment,
                stdout=stem.with_suffix(".stdout.log"),
                stderr=stem.with_suffix(".stderr.log"),
            )
            if action == "build":
                # Keep the selected tree's conventional log as well as separate
                # immutable attempt streams, using the same captured client map.
                with (self.tree / "build.log").open("wb") as output:
                    for name in ("stdout", "stderr"):
                        output.write(f"=== concepts build {name} ===\n".encode())
                        with stem.with_suffix(f".{name}.log").open("rb") as source:
                            shutil.copyfileobj(source, output)
            receipt.update(status="exited", exit=status)
            if status:
                raise ValueError(f"concept Bazel {action} failed ({status}); see {stem}.stderr.log")
            return "" if action == "build" else stem.with_suffix(".stdout.log").read_text(encoding="utf-8")
        except BaseException as error:
            receipt.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "error", error=str(error))
            raise
        finally:
            native.write_json(stem.with_suffix(".receipt.json"), receipt)

    def providers(self, expression: str) -> list[dict[str, Any]]:
        text = self.invoke("cquery", [expression, "--output=starlark", "--starlark:expr=" + _PROVIDER])
        rows = [_json(line) for line in text.split("\n") if line.strip()]
        if not rows or any(not isinstance(row, dict) for row in rows):
            raise ValueError("missing/malformed configured DefaultInfo rows")
        return rows

    def source_file(self, record: Mapping[str, Any], requested_owner: str) -> Path:
        """Bind repository source owner/path to stock query's actual location.

        File.root.path may be empty and execution_root/external may not exist.
        Metadata remains selected-output-contained; there is no cache-layout
        escape or guessed external directory fallback.
        """
        owner = _label(record.get("owner"))
        if not owner.startswith("@@"):
            raise ValueError("metadata File requires its actual canonical repository owner")
        canonical, _, file_label = owner[2:].partition("//")
        requested = _label(requested_owner)
        apparent = requested.lstrip("@").partition("//")[0]
        if not apparent or not canonical.endswith("+" + apparent):
            raise ValueError("metadata File owner differs from the selected repository")
        package, _, filename = file_label.partition(":")
        expected = "external/" + canonical + "/" + (package + "/" if package else "") + filename
        if _relative(record.get("path")) != expected:
            raise ValueError("metadata File owner and exec-relative path disagree")
        text = self.invoke("query", [json.dumps(owner), "--output=location"])
        lines = [line for line in text.split("\n") if line.strip()]
        match = re.fullmatch(r"(.+):[0-9]+:[0-9]+: source file (\S+)", lines[0]) if len(lines) == 1 else None
        if match is None or match[2] not in {owner, "@" + apparent + "//" + file_label}:
            raise ValueError("metadata owner query did not return its exact source-file location")
        path = Path(match[1])
        if (
            not path.is_absolute()
            or not path.is_file()
            or not path.resolve().is_relative_to((self.tree / "state/output").resolve())
        ):
            raise ValueError("unsupported metadata source layout outside the selected output base")
        self.guard.add(path)
        return path

    def one_source(self, target: str) -> tuple[Path, dict[str, Any]]:
        rows = self.providers(target)
        if len(rows) != 1 or rows[0].get("incompatible") is not False:
            raise ValueError(f"metadata target unavailable or ambiguous: {target}")
        files = rows[0].get("files")
        if (
            not isinstance(files, list)
            or len(files) != 1
            or not isinstance(files[0], dict)
            or files[0].get("source") is not True
            or files[0].get("directory") is not False
        ):
            raise ValueError(f"metadata must be one actual repository source File: {target}")
        configured_label = _label(rows[0].get("label"))
        file_owner = _label(files[0].get("owner"))
        if configured_label.partition("//")[0] not in {file_owner.partition("//")[0], target.partition("//")[0]}:
            raise ValueError("configured metadata target and File owner disagree")
        path = self.source_file(files[0], target)
        self.source_declarations[target] = {name: files[0][name] for name in ("owner", "path", "source")}
        facts = _json(path.read_text(encoding="utf-8"))
        if not isinstance(facts, dict):
            raise ValueError(f"invalid metadata object: {target}")
        return path, facts

    def graph(self, labels: Sequence[str], platform: str) -> ActionGraph:
        expression = "deps(set(" + " ".join(json.dumps(label) for label in labels) + "))"
        text = self.invoke(
            "aquery",
            [
                expression,
                "--output=jsonproto",
                "--include_artifacts",
                "--include_commandline",
                "--include_param_files",
                "--include_pruned_inputs",
                "--include_aspects",
                "--include_file_write_contents",
                "--aspects=" + _ASPECT,
            ],
        )
        return ActionGraph(_json(text), platform)


def _provider_outputs(rows: Sequence[Mapping[str, Any]]) -> tuple[list[str], list[str]]:
    outputs, incompatible = [], []
    labels = set()
    for row in rows:
        label = _label(row.get("label"))
        if label in labels or type(row.get("incompatible")) is not bool:
            raise ValueError("duplicate/malformed requested configured provider")
        labels.add(label)
        if row["incompatible"]:
            incompatible.append(label)
            continue
        files = row.get("files")
        if not isinstance(files, list):
            raise ValueError("requested root lacks DefaultInfo files")
        for file in files:
            if not isinstance(file, dict) or file.get("source") is not False or type(file.get("directory")) is not bool:
                raise ValueError("completion root must supply declared generated DefaultInfo artifacts")
            outputs.append(_relative(file.get("path")))
    return sorted(set(outputs)), sorted(incompatible)


def _root_identities(document: object, labels: set[str]) -> dict[str, str]:
    from .bazel_test_ownership import extract_configured_targets

    result = {}
    for target in extract_configured_targets(document):
        label = _label(target.label)
        if label in result:
            raise ValueError("requested root has multiple configurations")
        result[label] = _checksum(target.configuration)
    if set(result) != labels:
        raise ValueError("configured root identities disagree with DefaultInfo selection")
    return result


def _validate_build(path: Path, roots: Mapping[str, str]) -> None:
    events = [_json(line) for line in path.read_text(encoding="utf-8").split("\n") if line.strip()]
    if not events or not all(isinstance(event, dict) for event in events) or events[-1].get("lastMessage") is not True:
        raise ValueError("prerequisite build BEP is incomplete")
    finished = [event["finished"] for event in events if "finished" in event]
    if (
        len(finished) != 1
        or finished[0].get("exitCode", {}).get("name") != "SUCCESS"
        or finished[0].get("exitCode", {}).get("code", 0) != 0
    ):
        raise ValueError("prerequisite build has no successful finished event")
    observed = set()
    for event in events:
        identity = event.get("id", {}).get("targetCompleted")
        if not isinstance(identity, dict) or identity.get("aspect"):
            continue
        label = _label(identity.get("label"))
        if label not in roots:
            continue
        if (
            label in observed
            or identity.get("configuration", {}).get("id") != roots[label]
            or event.get("completed", {}).get("success") is not True
        ):
            raise ValueError("prerequisite root completion differs from the configured selection")
        observed.add(label)
    if observed != set(roots):
        raise ValueError("prerequisite build omitted a requested configured root")


def _bind_sdk(attempt: _Attempt, supplied: Mapping[str, Any], platform: str) -> list[dict[str, Any]]:
    if (
        not isinstance(supplied, Mapping)
        or type(supplied.get("schema")) is not int
        or supplied["schema"] != 1
        or supplied.get("platform") != platform
        or supplied.get("config") != attempt.config
        or Path(supplied.get("tree", "")).resolve() != attempt.tree.resolve()
    ):
        raise ValueError("SDK facts do not describe the current concept request")
    manifests = supplied.get("manifests")
    if (
        not isinstance(manifests, list)
        or any(not isinstance(item, dict) for item in manifests)
        or {item.get("target") for item in manifests} != _MANIFESTS
        or len(manifests) != len(_MANIFESTS)
    ):
        raise ValueError("SDK facts must contain the exact native/test manifest selection")
    observations = []
    for item in manifests:
        path, facts = attempt.one_source(item["target"])
        if (
            Path(item["path"]).resolve() != path.resolve()
            or _digest(path) != _checksum(item.get("sha256"))
            or facts != item.get("facts")
            or item.get("declared_source") != attempt.source_declarations[item["target"]]
        ):
            raise ValueError("SDK report no longer matches its configured manifest File")
        if not attempt.source_declarations[item["target"]]["owner"].endswith("//" + attempt.config + ":manifest.json"):
            raise ValueError("SDK alias did not select the requested configuration's manifest")
        configuration = facts.get("configuration")
        if (
            facts.get("generator") != "aobus-bazel-artifact-adapter-v1"
            or not isinstance(configuration, dict)
            or configuration.get("platform") != platform
            or configuration.get("config") != attempt.config.title()
        ):
            raise ValueError("SDK manifest configuration/producer mismatch")
        inputs = facts.get("inputs")
        if not isinstance(inputs, list) or not inputs:
            raise ValueError("SDK manifest lacks recorded native inputs")
        seen = set()
        for entry in inputs:
            source = Path(entry["path"])
            if not source.is_absolute() or str(source) in seen:
                raise ValueError("ambiguous SDK recorded input")
            seen.add(str(source))
            attempt.guard.add(source, digest=_checksum(entry.get("sha256")))
        observations.append(
            {"target": item["target"], "path": str(path), "sha256": _digest(path), "inputs": len(inputs)}
        )
    return observations


def _one_flag(argv: Sequence[str], flag: str) -> int:
    matches = [index for index, argument in enumerate(argv) if argument == flag]
    if len(matches) != 1:
        raise ValueError(f"expected one exact native {flag} argument")
    return matches[0]


def _validate_native(action: CompileAction, platform: str) -> None:
    if any(argument.startswith("@") or "\x00" in argument for argument in action.argv):
        raise ValueError("opaque native response/context")
    if platform == "windows":
        _one_flag(action.argv, "/c")
        _one_flag(action.argv, "/showIncludes")
        environment = {key.casefold(): value for key, value in action.environment}
        if environment.get("vslang") != "1033" or environment.get("cl") or environment.get("_cl_"):
            raise ValueError("MSVC observations need the declared English producer environment without hidden options")
        rejected: tuple[str, ...] = (
            "/y",
            "/fr",
            "/doc",
            "/analyze",
            "/ifc",
            "/reference",
            "/headerunit",
            "/interface",
            "/exportheader",
            "/translateinclude",
            "/sourcedependencies",
            "/scandependencies",
            "/clang:",
        )
        for argument in action.argv[1:]:
            unknown_output = argument.startswith(("/F", "-F")) and not argument.startswith(
                ("/Fo", "/Fd", "/Fa", "/Fm", "/Fi", "/FI", "/FA", "/FS")
            )
            if (
                argument in {"/P", "/E", "/EP", "/Zs", "-P", "-E", "-EP", "-Zs", "/w", "/W0", "/w0", "/link"}
                or unknown_output
                or argument.startswith(("/Fp", "/FA", "/B1", "/B2", "/d1", "/d2"))
                or argument.casefold().startswith(rejected)
            ):
                raise ValueError(f"unsupported native MSVC phase/output/module/PCH option: {argument}")
    else:
        for flag in ("-c", "-MD", "-MF", "-o"):
            _one_flag(action.argv, flag)
        rejected = (
            "-fmodule",
            "-include-pch",
            "-include-pth",
            "-emit-",
            "-save-temps",
            "-serialize-diagnostics",
            "--serialize-diagnostics",
            "-ftime-trace",
            "-foptimization-record",
            "-fsave-optimization-record",
            "-fplugin",
            "-fpass-plugin",
            "-ivfsoverlay",
            "-vfsoverlay",
            "--config",
            "--driver-mode",
            "-fcrash-diagnostics-dir",
            "-fdiagnostics-serialization-file",
            "-dependency-file",
        )
        for argument in action.argv[1:]:
            unknown_output = (
                (argument.startswith("-o") and argument != "-o")
                or (argument.startswith("-M") and argument not in {"-MD", "-MF"})
                or (argument.startswith("-MF") and argument != "-MF")
            )
            if (
                argument
                in {
                    "-E",
                    "-S",
                    "-M",
                    "-MM",
                    "-MMD",
                    "-MP",
                    "-MG",
                    "-MV",
                    "-MJ",
                    "-MT",
                    "-MQ",
                    "-w",
                    "-fsyntax-only",
                    "-Xclang",
                    "-Xpreprocessor",
                    "-mllvm",
                    "-cc1",
                    "-cc1as",
                    "-###",
                    "-working-directory",
                    "--working-directory",
                }
                or unknown_output
                or argument.startswith(rejected)
            ):
                raise ValueError(f"unsupported native Clang phase/output/module/PCH option: {argument}")
        for flag in ("-o", "-MF"):
            index = _one_flag(action.argv, flag)
            if index + 1 >= len(action.argv) or action.argv[index + 1].startswith("-"):
                raise ValueError(f"missing native {flag} output value")


def observation_environment(
    action: CompileAction, platform: str, client: Mapping[str, str] | None = None
) -> dict[str, str]:
    """Bazel 9.2 local Windows completion, never idle-server/full ambient inheritance."""
    declared = dict(action.environment)
    if platform != "windows":
        return declared
    if client is None:
        raise ValueError("Windows native environment completion requires the captured current Bazel client")
    if len({key.casefold() for key in declared}) != len(declared):
        raise ValueError("ambiguous case-insensitive declared action environment")
    lookup = {}
    for key, value in client.items():
        folded = key.casefold()
        if folded in lookup:
            raise ValueError("ambiguous case-insensitive Bazel client environment")
        lookup[folded] = value
    # WindowsLocalEnvProvider removes exact uppercase TMP/TEMP. Other casing
    # could collide later in the native TreeMap; admit the pinned producer form.
    if any(key.casefold() in {"tmp", "temp"} and key not in {"TMP", "TEMP"} for key in declared):
        raise ValueError("unsupported declared Windows temporary-variable casing")
    temporary = lookup.get("tmp") if "tmp" in lookup else lookup.get("temp")
    if not temporary:
        raise ValueError("Windows client TMP/TEMP is unavailable; action-local random fallback is not admitted")
    declared.pop("TMP", None)
    declared.pop("TEMP", None)
    declared["TMP"] = declared["TEMP"] = temporary.replace("/", "\\")
    present = {key.casefold() for key in declared}
    for key in ("SYSTEMROOT", "SYSTEMDRIVE"):
        folded = key.casefold()
        if folded not in present:
            client_value = lookup.get(folded)
            if not client_value:
                raise ValueError(f"Windows client {key} required for native completion is unavailable")
            declared[key] = client_value
        elif not next(value for name, value in declared.items() if name.casefold() == folded):
            raise ValueError(f"declared Windows {key} is empty")
    return declared


def _msvc_outputs(argv: Sequence[str], values: Mapping[str, str] | None = None) -> tuple[list[str], list[str]]:
    rewritten, originals, seen = [], [], set()
    index = 0
    prefixes = ("/Fo", "/Fd", "/Fa", "/Fm", "/Fi")
    while index < len(argv):
        argument = argv[index]
        prefix = next((flag for flag in prefixes if argument.startswith(flag)), None)
        if prefix is None:
            rewritten.append(argument)
            index += 1
            continue
        if prefix in seen:
            raise ValueError(f"duplicate MSVC output switch: {prefix}")
        seen.add(prefix)
        if argument == prefix:
            if index + 1 >= len(argv) or argv[index + 1].startswith("/"):
                raise ValueError(f"missing MSVC output value: {prefix}")
            originals.append(argv[index + 1])
            if values is not None:
                rewritten.extend([prefix, values[prefix]])
            index += 2
        else:
            originals.append(argument[len(prefix) :])
            if values is not None:
                rewritten.append(prefix + values[prefix])
            index += 1
    if "/Fo" not in seen:
        raise ValueError("MSVC context lacks explicit /Fo")
    return rewritten, originals


def dependency_command(action: CompileAction, platform: str, directory: Path) -> tuple[list[str], list[str]]:
    """Known phase/output-only rewrite; preserves native diagnostics and compiler."""
    _validate_native(action, platform)
    if platform != "windows":
        argv = list(action.argv)
        argv[_one_flag(argv, "-c")] = "-E"
        originals = []
        for flag, destination in (("-o", directory / "preprocessed.ii"), ("-MF", directory / "dependencies.d")):
            index = _one_flag(argv, flag) + 1
            originals.append(argv[index])
            argv[index] = str(destination)
        return argv, originals
    values = {
        "/Fo": str(directory / "forbidden.obj"),
        "/Fd": str(directory / "compiler.pdb"),
        "/Fa": str(directory / "assembly.asm"),
        "/Fm": str(directory / "compiler.map"),
        "/Fi": str(directory / "preprocessed.i"),
    }
    argv, originals = _msvc_outputs(action.argv, values)
    if not any(argument.startswith("/Fd") for argument in argv):
        argv.append("/Fd" + values["/Fd"])
    argv.extend(["/P", "/Fi" + values["/Fi"]] if not any(argument.startswith("/Fi") for argument in argv) else ["/P"])
    argv.extend(["/sourceDependencies", str(directory / "dependencies.json")])
    return argv, originals


def _bind_native_outputs(action: CompileAction, platform: str, execution_root: Path) -> None:
    if platform == "windows":
        index = next(index for index, argument in enumerate(action.argv) if argument.startswith("/Fo"))
        argument = action.argv[index]
        names = [action.argv[index + 1] if argument == "/Fo" else argument[3:]]
    else:
        names = [action.argv[_one_flag(action.argv, flag) + 1] for flag in ("-o", "-MF")]
    declared = {_key(execution_root / name) for name in action.outputs}
    for name in names:
        path = Path(name)
        path = path if path.is_absolute() else execution_root / path
        if _key(path) not in declared:
            raise ValueError("native compiler output switches do not bind to its declared action outputs")


def parse_dependencies(text: str) -> tuple[list[str], list[str]]:
    """One GNU-Make dependency rule; not shell quoting or a directive scan."""
    text = re.sub(r"\\\r?\n", "", text)
    colon = None
    escaped = False
    for index, character in enumerate(text):
        if escaped:
            escaped = False
            continue
        if character == "\\":
            escaped = True
        elif character == ":" and not (index == 1 and text[0].isalpha() and text[index + 1 : index + 2] in {"/", "\\"}):
            colon = index
            break
    if colon is None:
        raise ValueError("dependency output has no target separator")

    def words(value: str) -> list[str]:
        result: list[str] = []
        current: list[str] = []
        index = 0
        while index < len(value):
            character = value[index]
            if character in " \t\r\n":
                if current:
                    result.append("".join(current))
                    current = []
            elif character == "\\" and index + 1 < len(value) and value[index + 1] in " \t#:\\":
                index += 1
                current.append(value[index])
            elif character == "$":
                if value[index + 1 : index + 2] != "$":
                    raise ValueError("unexpanded dependency-file variable")
                current.append("$")
                index += 1
            elif character == "#":
                raise ValueError("unescaped dependency-file comment")
            elif character == ":" and not (len(current) == 1 and current[0].isalpha()):
                raise ValueError("multiple dependency rules are not supported")
            else:
                current.append(character)
            index += 1
        if current:
            result.append("".join(current))
        return result

    targets, dependencies = words(text[:colon]), words(text[colon + 1 :])
    if not targets or not dependencies:
        raise ValueError("empty dependency rule is not a complete observation")
    return targets, dependencies


def parse_msvc_dependencies(payload: str) -> tuple[str, list[str]]:
    """Only the native validated ordinary-source Version 1.2 producer layout."""
    document = _json(payload)
    if not isinstance(document, dict) or set(document) != {"Version", "Data"} or document["Version"] != "1.2":
        raise ValueError("unsupported MSVC source-dependency version/layout")
    data = document["Data"]
    fields = {"Source", "ProvidedModule", "Includes", "ImportedModules", "ImportedHeaderUnits"}
    if not isinstance(data, dict) or set(data) != fields:
        raise ValueError("incomplete MSVC source-dependency data")
    source = data["Source"]
    if not isinstance(source, str) or not source or "\x00" in source:
        raise ValueError("MSVC dependency data lacks its native source identity")
    if data["ProvidedModule"] != "" or data["ImportedModules"] != [] or data["ImportedHeaderUnits"] != []:
        raise ValueError("MSVC module/header-unit dependency context is unsupported")
    return source, _strings(data["Includes"])


class _Locations:
    def __init__(
        self,
        root: Path,
        execution_root: Path,
        aliases: Mapping[str, str],
        source_files: Iterable[str] = (),
        *,
        allowed_roots: Sequence[Path] = (),
        allowed_files: Iterable[Path] = (),
    ):
        self.root, self.execution_root = root, execution_root
        self.aliases = {self.wire(name): original for name, original in aliases.items()}
        names = tuple(source_files)
        self.source_files = {self.wire(name): name for name in names}
        if len(self.source_files) != len(set(names)):
            raise ValueError("case-colliding declared source paths")
        if len(self.aliases) != len(aliases):
            raise ValueError("case-colliding virtual-header producer paths")
        self.allowed_roots = tuple(path.resolve() for path in (root, *allowed_roots))
        self.allowed_files = frozenset(_key(path) for path in allowed_files)
        self._physical = cache(self._resolve_physical)

    @staticmethod
    def wire(name: str) -> str:
        return name.replace("\\", "/").casefold() if os.name == "nt" else name

    def admitted(self, path: Path) -> Path:
        resolved = path.resolve(strict=True)
        if (
            not any(resolved.is_relative_to(root) for root in self.allowed_roots)
            and _key(resolved) not in self.allowed_files
        ):
            raise ValueError(f"dependency file is outside the bound source/SDK context: {path}")
        return resolved

    def physical(self, name: str) -> Path:
        return self._physical(name)

    def _resolve_physical(self, name: str) -> Path:
        spelled = name.replace("\\", "/") if os.name == "nt" else name
        path = Path(spelled)
        if path.is_absolute():
            try:
                spelled = path.relative_to(self.execution_root).as_posix()
            except ValueError:
                try:
                    relative = path.relative_to(self.root).as_posix()
                except ValueError:
                    return self.admitted(path)
                original_name = self.source_files.get(self.wire(relative))
                if original_name is None:
                    return self.admitted(path)
                original = self.root / original_name
                if not _same_source(path, original):
                    raise ValueError("absolute source spelling does not bind to its declared original") from None
                return self.admitted(original)
        active = set()
        while self.wire(spelled) in self.aliases:
            key = self.wire(spelled)
            if key in active:
                raise ValueError("cyclic virtual-header origins")
            active.add(key)
            spelled = self.aliases[key]
        candidate = self.execution_root / spelled
        if self.wire(spelled) in self.source_files:
            original = self.root / self.source_files[self.wire(spelled)]
            if not _same_source(candidate, original):
                raise ValueError(f"declared source alias is foreign: {spelled}")
            return self.admitted(original)
        return self.admitted(candidate)

    def project(self, name: str) -> str | None:
        physical = self.physical(name)
        try:
            return physical.relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return None

    def ast(self, value: Any) -> None:
        if isinstance(value, dict):
            for name, child in value.items():
                if name == "file" and isinstance(child, str) and not (child.startswith("<") and child.endswith(">")):
                    project = self.project(child)
                    value[name] = str(self.root / project) if project is not None else str(self.physical(child))
                else:
                    self.ast(child)
        elif isinstance(value, list):
            for child in value:
                self.ast(child)


def _consume_preprocessed(directory: Path, platform: str) -> None:
    """Only a completed context's own unused bodies are consumed; evidence stays."""
    records = []
    for phase in ("read-inventory", "measurement"):
        work = directory / phase
        path = work / ("preprocessed.i" if platform == "windows" else "preprocessed.ii")
        if path.is_symlink() or not path.is_file():
            raise ValueError("successful context temporary is not its owned regular output")
        receipt_path = work / "receipt.json"
        receipt = _json(receipt_path.read_text(encoding="utf-8"))
        receipt["preprocessed"] = {
            "path": str(path),
            "sha256": _digest(path),
            "bytes": path.stat().st_size,
            "retained": True,
            "reason": "completed context; pending temporary consumption",
        }
        native.write_json(receipt_path, receipt)
        records.append((path, receipt_path, receipt))
    for path, receipt_path, receipt in records:
        receipt["preprocessed"].update(
            retained=False,
            reason=(
                "consumed-temporary after compiler exit, complete transport, closure join, "
                "and context input/output guards"
            ),
        )
        native.write_json(receipt_path, receipt)
        try:
            path.unlink()
        except BaseException as error:
            receipt["preprocessed"].update(
                retained=path.exists(), reason="temporary consumption interrupted", error=str(error)
            )
            native.write_json(receipt_path, receipt)
            raise


def _native_observation(
    action: CompileAction,
    platform: str,
    directory: Path,
    execution_root: Path,
    locations: _Locations,
    guard: _Guard,
    client_environment: Mapping[str, str] | None = None,
) -> set[str]:
    environment = observation_environment(action, platform, client_environment)
    observations = []
    originals = _Guard()
    for phase in ("read-inventory", "measurement"):
        work = directory / phase
        work.mkdir(parents=True)
        argv, output_names = dependency_command(action, platform, work)
        for name in (*action.outputs, *output_names):
            path = Path(name)
            originals.add(path if path.is_absolute() else execution_root / path, missing=True)
        receipt: dict[str, Any] = {
            "owner": action.owner,
            "configuration": action.configuration,
            "source": action.source,
            "outputs": action.outputs,
            "argv": argv,
            "cwd": str(execution_root),
            "environment": dict(action.environment),
            "launchEnvironment": environment,
            "clientFields": {
                key: value
                for key, value in (client_environment or {}).items()
                if key.casefold() in {"systemroot", "systemdrive", "tmp", "temp"}
            },
            "environmentContract": "declared environment plus pinned Bazel 9.2 Windows local-client completion only",
            "startedAt": datetime.now(UTC).isoformat(),
            "status": "incomplete",
        }
        try:
            status = _process(
                argv,
                cwd=execution_root,
                environment=environment,
                stdout=work / "stdout.bin",
                stderr=work / "stderr.bin",
            )
            receipt.update(exit=status, status="exited")
            if status:
                raise ValueError(f"native dependency pass failed ({status}): {action.owner}; see {work}")
            suffix = "i" if platform == "windows" else "ii"
            if not (work / f"preprocessed.{suffix}").is_file():
                raise ValueError("native dependency pass omitted its fresh preprocessed output")
            if platform == "windows":
                if any(
                    (work / name).exists() for name in ("forbidden.obj", "compiler.pdb", "assembly.asm", "compiler.map")
                ):
                    raise ValueError("MSVC preprocessing unexpectedly emitted an object/PDB/assembly/map")
                if any(
                    re.search(rb"\b(?:D9002|D9007|D9024)\b", (work / name).read_bytes())
                    for name in ("stdout.bin", "stderr.bin")
                ):
                    raise ValueError("MSVC reported an ignored/unknown dependency context option")
                source, names = parse_msvc_dependencies((work / "dependencies.json").read_text(encoding="utf-8-sig"))
                if not _same_source(locations.physical(source), locations.physical(action.source)):
                    raise ValueError("MSVC JSON source differs from the exact captured native source")
            else:
                targets, names = parse_dependencies((work / "dependencies.d").read_text(encoding="utf-8"))
                if targets != [str(work / "preprocessed.ii")] or _key(locations.physical(action.source)) not in {
                    _key(locations.physical(name)) for name in names
                }:
                    raise ValueError("dependency file does not bind its fresh output and exact native source")
            paths = {locations.physical(name) for name in names}
            for path in paths:
                guard.add(path)
            observations.append(paths)
            if len(observations) == 2 and observations[0] != observations[1]:
                raise ValueError("native include closure changed between inventory and measured pass")
            compiler = Path(action.argv[0])
            compiler = compiler if compiler.is_absolute() else execution_root / compiler
            guard.verify_paths([*paths, locations.physical(action.source), compiler])
            originals.verify()
            receipt["originalOutputGuards"] = originals.provenance()
            receipt["dependencies"] = sorted(str(path) for path in paths)
        except BaseException as error:
            receipt.update(status="error", error=str(error))
            raise
        finally:
            receipt["finishedAt"] = datetime.now(UTC).isoformat()
            native.write_json(work / "receipt.json", receipt)
    included = {
        header_name
        for path in observations[-1]
        if (header_name := locations.project(str(path))) is not None
        and PurePosixPath(header_name).suffix in concept_scope.HEADER_SUFFIXES
    }
    _consume_preprocessed(directory, platform)
    return included


def _ast_command(
    action: CompileAction,
    platform: str,
    metadata: Mapping[str, Any],
    header: Path,
    dependency: Path,
    *,
    dump: bool = True,
) -> list[str]:
    _validate_native(action, platform)
    compiler = metadata["clang"]
    extra = _strings(metadata["system_arguments"]) + _strings(metadata["analysis_arguments"])
    if platform == "windows":
        argv, _originals = _msvc_outputs(action.argv)
        flags = [argument for argument in argv[1:] if argument not in {"/c", "/showIncludes", "/GL", "/GL-"}]
        flags = [argument for argument in flags if argument != action.source_argument]
        analysis = ["-fsyntax-only", "-x", "c++"]
        if dump:
            analysis.extend(["-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=ao"])
        analysis.extend(["-Wno-pragma-once-outside-header", "-MD", "-MF", str(dependency)])
        return [compiler, *flags, *("/clang:" + argument for argument in [*extra, *analysis]), str(header)]
    skip = {_one_flag(action.argv, flag) for flag in ("-c", "-MD", "-MF", "-o")}
    skip |= {index + 1 for index in (_one_flag(action.argv, "-MF"), _one_flag(action.argv, "-o"))}
    flags = [
        argument
        for index, argument in enumerate(action.argv[1:], 1)
        if index not in skip and argument != action.source_argument
    ]
    analysis = ["-x", "c++", "-fsyntax-only"]
    if dump:
        analysis.extend(["-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=ao"])
    return [
        compiler,
        *extra,
        *flags,
        *analysis,
        "-Wno-pragma-once-outside-header",
        "-Wno-unused-command-line-argument",
        "-Qunused-arguments",
        "-MD",
        "-MF",
        str(dependency),
        str(header),
    ]


def _parse_public_header(
    action: CompileAction,
    platform: str,
    metadata: Mapping[str, Any],
    header: Path,
    area: str,
    directory: Path,
    locations: _Locations,
    guard: _Guard,
    client_environment: Mapping[str, str] | None = None,
) -> concept_report.HeaderParse:
    environment = observation_environment(action, platform, client_environment)
    included = []
    ast = None
    for phase in ("read-inventory", "measurement"):
        work = directory / phase
        work.mkdir(parents=True)
        dependency = work / "ast.d"
        measured = phase == "measurement"
        argv = _ast_command(action, platform, metadata, header, dependency, dump=measured)
        receipt = {
            "argv": argv,
            "cwd": str(locations.execution_root),
            "environment": dict(action.environment),
            "launchEnvironment": environment,
            "stage": phase,
            "astDump": measured,
            "status": "incomplete",
        }
        try:
            status = _process(
                argv,
                cwd=locations.execution_root,
                environment=environment,
                stdout=work / "ast.json",
                stderr=work / "stderr.bin",
            )
            receipt.update(exit=status, status="exited")
        except BaseException as error:
            receipt.update(status="error", error=str(error))
            raise
        finally:
            if (work / "ast.json").is_file():
                path = work / "ast.json"
                receipt["rawAST"] = {
                    "path": str(path),
                    "bytes": path.stat().st_size,
                    "sha256": _digest(path),
                    "retained": True,
                }
            native.write_json(work / "receipt.json", receipt)
        if status:
            raise ValueError(f"public-header AST failed ({status}): {header}; see {work}")
        _targets, names = parse_dependencies(dependency.read_text(encoding="utf-8"))
        paths = {locations.physical(name) for name in names}
        if header.resolve() not in paths:
            raise ValueError("AST dependency observation omits its exact public main")
        for path in paths:
            guard.add(path)
        included.append(paths)
        if len(included) == 2 and included[0] != included[1]:
            raise ValueError("public-header AST include closure changed")
        if measured:
            ast = concept_report.ast_from_output((work / "ast.json").read_text(encoding="utf-8"))
    if ast is None:
        raise ValueError("public-header measured AST is unavailable")
    locations.ast(ast)
    return concept_report.extract_header(ast, header, area, locations.root)


def write_bazel_report(
    args: argparse.Namespace, *, sdk_facts: Mapping[str, Any], root: Path = PROJECT_ROOT
) -> dict[str, object]:
    """Write one complete schema-4 report under the selected, caller-reentrant tree lock."""
    if args.config not in {"debug", "release"} or type(args.jobs) is not int or args.jobs < 1:
        raise native.BazelWorkspaceError("concept reports require Debug/Release and positive jobs")
    platform = native.native_platform()
    root = Path(root).resolve()
    tree = bazel_workspace.workspace_path(args.path, args.config)
    with build_tree_lock(tree):
        native.require_supported_source()
        attempt = _Attempt(args, root, tree)
        receipt: dict[str, Any] = {
            "schema": 4,
            "status": "incomplete",
            "platform": platform,
            "config": args.config,
            "retention": _RETENTION,
        }
        try:
            sdk = _bind_sdk(attempt, sdk_facts, platform)
            metadata_path, metadata = attempt.one_source(_AST_SDK)
            if (
                metadata.get("schema") != 1
                or metadata.get("platform") != platform
                or not isinstance(metadata.get("version"), str)
            ):
                raise ValueError("AST SDK metadata does not admit this native platform")
            clang = Path(metadata.get("clang", ""))
            if not clang.is_absolute() or not clang.is_file() or not Path(metadata.get("resource_dir", "")).is_dir():
                raise ValueError("admitted AST compiler/resource directory is unavailable")
            attempt.guard.add(clang)
            _strings(metadata.get("system_arguments"))
            _strings(metadata.get("analysis_arguments"))
            builtin_roots = []
            if platform != "windows":
                identity_path, identity = attempt.one_source("@aobus_toolchain//:identity.json")
                if platform == "linux":
                    if identity.get("schema") != 1 or identity.get("context", {}).get("platform") != platform:
                        raise ValueError("native toolchain identity does not describe the selected host")
                    files = identity.get("files")
                    if not isinstance(files, list) or not files:
                        raise ValueError("native toolchain identity has no corroborating inputs")
                    for item in files:
                        path = Path(item["path"])
                        if not path.is_absolute():
                            raise ValueError("native toolchain identity input must be absolute")
                        attempt.guard.add(path, digest=_checksum(item.get("sha256")))
                    builtin_roots = [Path(name) for name in _strings(_json(identity["context"]["builtin_roots"]))]
                    if any(not path.is_absolute() or not path.is_dir() for path in builtin_roots):
                        raise ValueError("invalid corroborating compiler built-in roots")
                else:
                    # macOS trusts the same installed SDK boundary as Windows; the
                    # small schema-2 selection metadata replaces the recursive byte
                    # ledger, so only the metadata file itself is bound and guarded.
                    if identity.get("schema") != 2 or identity.get("platform") != platform:
                        raise ValueError("native toolchain identity does not describe the selected host")
                    selection = identity.get("sdk")
                    if (
                        not isinstance(selection, Mapping)
                        or not isinstance(selection.get("sdk"), str)
                        or not Path(selection["sdk"]).is_absolute()
                        or not isinstance(selection.get("sdk_version"), str)
                        or not selection["sdk_version"]
                        or not isinstance(selection.get("sdk_build_version"), str)
                        or not selection["sdk_build_version"]
                    ):
                        raise ValueError("macOS toolchain identity lacks its selected SDK facts")
                receipt["nativeToolchain"] = {"path": str(identity_path), "sha256": _digest(identity_path)}
            test_rows = attempt.providers(f"config(tests({_TESTS}), target)")
            tests = [_label(row.get("label")) for row in test_rows]
            if len(set(tests)) != len(tests):
                raise ValueError("test aggregate produced duplicate configured leaves")
            requested = [*_ROOTS, *tests]
            expression = "config(set(" + " ".join(json.dumps(label) for label in requested) + "), target)"
            rows = attempt.providers(expression)
            if {_label(row.get("label")) for row in rows} != set(requested):
                raise ValueError("DefaultInfo selection omitted a requested completion root")
            outputs, incompatible = _provider_outputs(rows)
            compatible = sorted(set(requested) - set(incompatible))
            checksums = _root_identities(
                _json(attempt.invoke("cquery", [expression, "--output=jsonproto"])), set(requested)
            )
            before_graph = attempt.graph(compatible, platform)
            selected = before_graph.reachable(outputs)
            target = [action for action in selected if action.owner.startswith("//") and not action.is_tool]
            if not target:
                raise ValueError("requested DefaultInfo outputs reach no first-party target compiler actions")
            for action in target:
                dependency_command(action, platform, attempt.directory / "uncreated-template-check")
                launch = observation_environment(action, platform, attempt.client_environment)
                if platform == "windows" and not Path(launch["TMP"]).is_dir():
                    raise ValueError(
                        "captured Windows client TMP is unavailable; no creation/cleanup/fallback permitted"
                    )
            headers = concept_scope.public_headers(root)
            public = [path for paths in headers.values() for path in paths]
            if any(not paths for paths in headers.values()):
                raise ValueError("both frozen public-header roots must be nonempty")
            aggregate_headers = concept_report.aggregate_headers(root)
            for path in (*public, *aggregate_headers):
                attempt.guard.add(path)
            missing = concept_scope.validate_construction_chains(root)
            if missing:
                raise ValueError("construction-chain catalog paths are missing: " + ", ".join(missing))
            for chain in concept_scope.CONSTRUCTION_CHAINS:
                attempt.guard.add(root / chain.path)
            buildfiles = ET.fromstring(
                attempt.invoke("query", ["buildfiles(deps(set(" + " ".join(compatible) + ")))", "--output=xml"])
            )
            for entry in buildfiles:
                if entry.tag == "source-file" and _label(entry.get("name")).startswith("//"):
                    name = _label(entry.get("name"))
                    package, file = name[2:].split(":", 1)
                    attempt.guard.add(root / package / file)
            for name in before_graph.required_inputs:
                if not name.startswith(("external/", "bazel-out/")):
                    attempt.guard.add(root / name)
            attempt.invoke(
                "build",
                [
                    *compatible,
                    "--remote_download_outputs=all",
                    "--aspects=" + _ASPECT,
                    "--build_event_json_file=" + str(attempt.directory / "build.bep.json"),
                ],
            )
            _validate_build(attempt.directory / "build.bep.json", {label: checksums[label] for label in compatible})
            attempt.guard.verify()
            execution_root = Path(attempt.invoke("info", ["execution_root"]).strip())
            if not execution_root.is_absolute() or not execution_root.resolve().is_relative_to(
                (tree / "state/output").resolve()
            ):
                raise ValueError("execution_root escapes the selected Bazel output base")
            for row in rows:
                for declaration in row["files"]:
                    path = execution_root / _relative(declaration["path"])
                    if (
                        not path.exists()
                        or declaration["directory"] != path.is_dir()
                        or not path.resolve().is_relative_to((tree / "state/output").resolve())
                    ):
                        raise ValueError(
                            "requested generated DefaultInfo output is absent, has wrong type, "
                            "or escapes the selected tree"
                        )
            graph = attempt.graph(compatible, platform)
            if graph.proof() != before_graph.proof() or attempt.providers(expression) != rows:
                raise ValueError("configured graph/DefaultInfo changed while preparing prerequisites")
            graph.reachable(outputs)
            for name in graph.required_inputs:
                path = execution_root / name
                if path.is_dir():
                    if not name.startswith("bazel-out/") or not path.resolve().is_relative_to(
                        (tree / "state/output").resolve()
                    ):
                        raise ValueError("only explicit generated input tree artifacts may be enumerated")
                    attempt.guard.generated_tree(path)
                else:
                    attempt.guard.add(path)
            for action in target:
                _bind_native_outputs(action, platform, execution_root)
                compiler = Path(action.argv[0])
                attempt.guard.add(compiler if compiler.is_absolute() else execution_root / compiler)
                if not action.source.startswith("bazel-out/") and not _same_source(
                    execution_root / action.source, root / action.source
                ):
                    raise ValueError("native source is not the selected first-party source identity")
            source_files = {name for name in graph.required_inputs if not name.startswith(("external/", "bazel-out/"))}
            source_files.update(path.relative_to(root).as_posix() for path in aggregate_headers)
            sdk_roots = [*builtin_roots, Path(metadata["resource_dir"])]
            for action in target:
                for _kind, name in bazel_contract.include_paths(action.argv):
                    path = Path(name)
                    path = path if path.is_absolute() else execution_root / path
                    if path.is_dir() and not path.resolve().is_relative_to((tree / "state/output").resolve()):
                        sdk_roots.append(path)
                if platform == "windows":
                    for key, value in action.environment:
                        if key.casefold() == "include":
                            sdk_roots.extend(Path(name) for name in value.split(";") if name)
                for flag in ("-isysroot", "--sysroot"):
                    if flag in action.argv:
                        index = _one_flag(action.argv, flag)
                        sdk_roots.append(Path(action.argv[index + 1]))
            if any(not path.is_absolute() or not path.is_dir() for path in sdk_roots):
                raise ValueError("declared native/AST SDK include root is unavailable")
            locations = _Locations(
                root,
                execution_root,
                graph.aliases,
                source_files,
                allowed_roots=sdk_roots,
                allowed_files=(path for path, state in attempt.guard.files.values() if state is not None),
            )

            def observe(item: tuple[int, CompileAction]) -> tuple[Any, set[str]]:
                index, action = item
                return action.identity, _native_observation(
                    action,
                    platform,
                    attempt.directory / f"native-{index:05}",
                    execution_root,
                    locations,
                    attempt.guard,
                    attempt.client_environment,
                )

            with ThreadPoolExecutor(max_workers=min(args.jobs, len(target))) as pool:
                observations = dict(pool.map(observe, enumerate(target)))
            candidates: dict[str, list[tuple[Any, ...]]] = {}
            public_names = {path.relative_to(root).as_posix() for path in public}
            for action in target:
                declared = {
                    locations.project(name)
                    for name in action.inputs
                    if PurePosixPath(name).suffix in concept_scope.HEADER_SUFFIXES
                }
                actual = observations[action.identity]
                for name in public_names.intersection(declared | actual):
                    candidates.setdefault(name, []).append(
                        (name not in actual, action.source.startswith("test/"), action.identity, action)
                    )
            work, selections = [], []
            for index, (area, header) in enumerate((area, path) for area, paths in headers.items() for path in paths):
                relative = header.relative_to(root).as_posix()
                if relative not in candidates:
                    raise ValueError(f"public header has no admitted target compile context: {relative}")
                chosen = min(candidates[relative], key=lambda item: item[:3])[3]
                # Validate every selected AST template before any AST child starts.
                _ast_command(chosen, platform, metadata, header, attempt.directory / "unused-validation.d")
                work.append((index, area, header, chosen))
                selections.append(
                    {
                        "header": relative,
                        "owner": chosen.owner,
                        "configuration": chosen.configuration,
                        "outputs": list(chosen.outputs),
                    }
                )

            def parse(item: tuple[int, str, Path, CompileAction]) -> concept_report.HeaderParse:
                index, area, header, chosen = item
                return _parse_public_header(
                    chosen,
                    platform,
                    metadata,
                    header,
                    area,
                    attempt.directory / f"header-{index:05}",
                    locations,
                    attempt.guard,
                    attempt.client_environment,
                )

            with ThreadPoolExecutor(max_workers=min(args.jobs, len(work))) as pool:
                parses = list(pool.map(parse, work))
            parses.sort(key=lambda parsed: (parsed.area, parsed.header))
            counts = {
                path.relative_to(root).as_posix(): sum(
                    path.relative_to(root).as_posix() in included for included in observations.values()
                )
                for path in public
            }
            if (
                concept_scope.public_headers(root) != headers
                or concept_report.aggregate_headers(root) != aggregate_headers
            ):
                raise ValueError("public/aggregate source-header membership changed")
            if _bind_sdk(attempt, sdk_facts, platform) != sdk:
                raise ValueError("configured SDK manifest selection changed")
            final_metadata, final_facts = attempt.one_source(_AST_SDK)
            if final_metadata.resolve() != metadata_path.resolve() or final_facts != metadata:
                raise ValueError("AST SDK selection changed")
            if attempt.graph(compatible, platform).proof() != graph.proof() or attempt.providers(expression) != rows:
                raise ValueError("configured graph/DefaultInfo changed during observation")
            final_checksums = _root_identities(
                _json(attempt.invoke("cquery", [expression, "--output=jsonproto"])), set(requested)
            )
            if final_checksums != checksums:
                raise ValueError("requested root configuration changed during observation")
            attempt.guard.verify()
            snapshot = attempt.guard.provenance()
            native.write_json(attempt.directory / "input-snapshot.json", snapshot)
            native_compilers = []
            for name in sorted({action.argv[0] for action in target}):
                path = Path(name)
                path = path if path.is_absolute() else execution_root / path
                native_compilers.append(
                    {"argv0": name, "path": str(path), "realpath": str(path.resolve()), "sha256": _digest(path)}
                )
            provenance = {
                "backend": "bazel",
                "config": args.config,
                "platform": platform,
                "roots": checksums,
                "bazelVersion": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
                "executionRoot": str(execution_root),
                "nativeCompilers": native_compilers,
                "incompatibleRoots": incompatible,
                "requestedDefaultOutputs": outputs,
                "requestedDefaultInfo": rows,
                "actionGraphSha256": hashlib.sha256(json.dumps(graph.proof(), sort_keys=True).encode()).hexdigest(),
                "cohort": [
                    {
                        **action.provenance(),
                        "launchEnvironment": observation_environment(action, platform, attempt.client_environment),
                    }
                    for action in target
                ],
                "windowsLocalCompletion": {
                    "bazel": "9.2.0",
                    "clientFields": attempt.client_fields,
                    "claim": (
                        "WindowsLocalEnvProvider TMP/TEMP plus WindowsSubprocessFactory missing "
                        "SYSTEMROOT/SYSTEMDRIVE; no other ambient merge"
                    ),
                }
                if platform == "windows"
                else None,
                "executionActions": [action.provenance() for action in selected if action.is_tool],
                "nativeMethod": (
                    "fresh native preprocessing; inventory then measured pass with unchanged closure; "
                    "Clang -E/-MD or MSVC /P plus sourceDependencies Version 1.2 JSON (no stdout fallback)"
                ),
                "environmentContract": (
                    "declared aquery environment; Windows adds only recorded Bazel 9.2 local completion "
                    "from this invocation's captured client; not a sandbox/remote protocol claim"
                ),
                "astCompiler": {
                    "metadata": str(metadata_path),
                    "sha256": _digest(metadata_path),
                    "clang": str(clang),
                    "version": metadata["version"],
                },
                "headerContexts": selections,
                "sdkManifests": sdk,
                "readInputSnapshot": snapshot["sha256"],
                "retention": _RETENTION,
                "rawAstFootprint": {
                    "dumps": len(work),
                    "retainedBytes": sum(
                        (attempt.directory / f"header-{index:05}" / "measurement/ast.json").stat().st_size
                        for index in range(len(work))
                    ),
                },
                "evidence": str(attempt.directory),
            }
            report = concept_report.assemble_report(
                tree, headers=headers, parses=parses, fan_out=counts, provenance=provenance, root=root
            )
            destination = concept_report.report_path(tree)
            staged = attempt.directory / concept_scope.REPORT_NAME
            staged.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            attempt.guard.verify()
            os.replace(staged, destination)
            receipt.update(
                status="complete", report=str(destination), targetActions=len(target), inputSnapshot=snapshot["sha256"]
            )
            return report
        except (OSError, ValueError, KeyError, TypeError, RecursionError, ET.ParseError) as error:
            receipt.update(status="failed", error=str(error))
            raise native.BazelWorkspaceError(
                f"concept report failed; evidence: {attempt.directory}: {error}"
            ) from error
        except BaseException as error:
            receipt.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "failed", error=str(error))
            raise
        finally:
            if receipt["status"] != "complete":
                native.write_json(
                    attempt.directory / "failed-input-snapshot.json",
                    {
                        "status": "partial before-state; acquisition or verification incomplete",
                        **attempt.guard.provenance(),
                    },
                )
            native.write_json(attempt.directory / "receipt.json", receipt)
