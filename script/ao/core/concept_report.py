"""Clang-AST concept metrics for `./ao deps report --concepts`."""

from __future__ import annotations

import json
import math
import os
import re
import shlex
import shutil
import subprocess
from collections import Counter
from collections.abc import Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from . import buildlock, concept_scope, tidyengine
from .paths import PROJECT_ROOT, absolute_path
from .proc import die

IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_:]*")
CPP_COMMENT_OR_LITERAL_RE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.DOTALL,
)
# The inventory tracks the collaborator-aggregate pattern, not one spelling: renaming a
# "*Dependencies" bag to "*Collaborators" must not remove it from the measurement.
AGGREGATE_DEFINITION_RE = re.compile(
    r"\bstruct\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*(?:Context|Dependencies|Collaborators))\b[^;{]*\{"
)
TYPE_KEYWORDS = frozenset(
    {
        "auto",
        "bool",
        "char",
        "class",
        "const",
        "consteval",
        "constexpr",
        "decltype",
        "double",
        "enum",
        "explicit",
        "export",
        "extern",
        "float",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "noexcept",
        "private",
        "protected",
        "public",
        "register",
        "short",
        "signed",
        "sizeof",
        "static",
        "struct",
        "template",
        "typename",
        "union",
        "unsigned",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
    }
)
CXX_METHOD_KINDS = frozenset({"CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl", "CXXConversionDecl"})
TYPE_DECL_KINDS = frozenset({"CXXRecordDecl", "ClassTemplateDecl", "EnumDecl", "TypeAliasDecl", "TypedefDecl"})
FUNCTION_DECL_KINDS = frozenset({"FunctionDecl", "FunctionTemplateDecl"})
DEPENDENCY_TARGET_KINDS = frozenset({"alias", "class", "enum"})
KEEP_FLAG_PREFIXES = (
    "-isystem",
    "-idirafter",
    "-iframework",
    "-isysroot",
    "--sysroot",
    "-include",
    "-stdlib",
    "-std=",
    "-pthread",
    "-I",
    "-D",
    "-U",
    "-f",
    "-m",
    "/std",
    "/I",
    "/D",
    "/U",
)
DROP_EXACT_FLAGS = frozenset(
    {
        "-c",
        "/c",
        "-S",
        "-E",
        "-fsyntax-only",
        "-pipe",
        "-Werror",
        "-fdiagnostics-color",
        "-fdiagnostics-color=always",
        "-fdiagnostics-color=auto",
        "-fcolor-diagnostics",
        "--",
    }
)
DROP_FLAG_PREFIXES = (
    "-Werror=",
    "-o",
    "/Fo",
    "/Fe",
    "/Fd",
    "-MF",
    "-MT",
    "-MQ",
    "-MD",
    "-MMD",
    "-l",
    "-L",
    "-Wl,",
    "-rdynamic",
)
TAKES_VALUE = frozenset(
    {
        "-I",
        "-D",
        "-U",
        "-isystem",
        "-iquote",
        "-idirafter",
        "-include",
        "-iframework",
        "-isysroot",
        "--sysroot",
        "-stdlib",
        "-o",
        "-MF",
        "-MT",
        "-MQ",
        "-Xclang",
        "/I",
        "/D",
        "/U",
        "/std",
        "/Fo",
        "/Fe",
        "/Fd",
    }
)


@dataclass(frozen=True)
class PublicDeclaration:
    kind: str
    name: str
    header: str
    area: str
    line: int
    signature: str = ""

    @property
    def identity(self) -> tuple[str, str, str]:
        return (self.kind, self.name, self.signature if self.kind in {"function", "method"} else "")


@dataclass(frozen=True)
class DependencyEdge:
    source: str
    target: str
    reason: str
    source_signature: str = ""


@dataclass(frozen=True)
class PendingEdge:
    source: str
    type_text: str
    reason: str
    source_signature: str = ""


@dataclass
class HeaderParse:
    header: str
    area: str
    declarations: list[PublicDeclaration] = field(default_factory=list)
    pending_edges: list[PendingEdge] = field(default_factory=list)
    project_headers: list[str] = field(default_factory=list)
    include_bytes: int = 0
    bind_unbind: list[str] = field(default_factory=list)
    aggregates: list[dict[str, object]] = field(default_factory=list)
    error: str | None = None


def report_path(build_dir: Path) -> Path:
    return Path(build_dir) / concept_scope.REPORT_NAME


def write_report(
    build_dir: Path,
    *,
    root: Path = PROJECT_ROOT,
    jobs: int | None = None,
    clang: str | None = None,
    compile_flags: Sequence[str] | None = None,
    chains: Sequence[concept_scope.ConstructionChain] | None = None,
) -> dict[str, object]:
    """Build the concept report, write it next to the compile database, and return it."""
    report = generate_report(
        build_dir,
        root=root,
        jobs=jobs,
        clang=clang,
        compile_flags=compile_flags,
        chains=chains,
    )
    path = report_path(build_dir)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def generate_report(
    build_dir: Path,
    *,
    root: Path = PROJECT_ROOT,
    jobs: int | None = None,
    clang: str | None = None,
    compile_flags: Sequence[str] | None = None,
    chains: Sequence[concept_scope.ConstructionChain] | None = None,
) -> dict[str, object]:
    catalog = concept_scope.CONSTRUCTION_CHAINS if chains is None else tuple(chains)
    if chains is None:
        missing_chains = concept_scope.validate_construction_chains(root)
        if missing_chains:
            listed = ", ".join(missing_chains)
            raise die(f"construction-chain catalog paths are missing: {listed}")

    headers = concept_scope.public_headers(root)
    flags = list(compile_flags) if compile_flags is not None else compile_flag_union(build_dir, root)
    clang_bin = clang if clang is not None else resolve_clang(build_dir)
    workers = jobs if jobs is not None else tidyengine.default_jobs()

    parses = parse_headers(headers, clang_bin, flags, root, workers)
    errors = [parsed.error for parsed in parses if parsed.error]
    if errors:
        preview = "\n".join(errors[:8])
        more = "" if len(errors) <= 8 else f"\n... {len(errors) - 8} more"
        raise die(f"concept report failed to parse {len(errors)} public header(s):\n{preview}{more}")

    declarations = _dedup_declarations(parses)
    edges = resolve_edges(parses, declarations)
    include_includes = _include_edges(parses)
    public_headers = {area: sorted({_relative(path, root) for path in paths}) for area, paths in headers.items()}
    all_public_headers = [header for area_headers in public_headers.values() for header in area_headers]
    fan_out = translation_unit_fan_out(build_dir, root, all_public_headers)

    by_area = _area_totals(declarations, edges)
    include_stats = _include_stats(parses)
    fan_out_stats = _fan_out_stats(fan_out, public_headers["application"])
    core_fan_out = _fan_out_stats(fan_out, public_headers["core"])

    secondary = _secondary_inventory(parses, declarations, headers, root, build_dir)
    construction = [
        {
            "leaf": chain.leaf,
            "path": chain.path,
            "hops": chain.hops,
            "steps": list(chain.steps),
        }
        for chain in catalog
    ]

    return {
        "schemaVersion": concept_scope.SCHEMA_VERSION,
        "generatedAt": datetime.now(UTC).isoformat(),
        "buildDir": str(absolute_path(build_dir)),
        "scope": concept_scope.scope_manifest(),
        "primary": {
            "core": {
                **by_area["core"],
                "includeWeight": include_stats["core"],
                "rebuildFanOut": core_fan_out,
            },
            "application": {
                **by_area["application"],
                "includeWeight": include_stats["application"],
                "rebuildFanOut": fan_out_stats,
            },
            "constructionHops": construction,
        },
        "secondary": secondary,
        "declarations": [asdict(item) for item in declarations],
        "dependencyEdges": [asdict(edge) for edge in edges],
        "headerIncludes": include_includes,
        "headerFanOut": _fan_out_rows(fan_out, public_headers),
    }


def format_summary(report: Mapping[str, Any]) -> str:
    primary = report["primary"]
    core = primary["core"]
    application = primary["application"]
    hops = primary["constructionHops"]
    hop_values = ", ".join(str(item["hops"]) for item in hops)
    return "\n".join(
        [
            "Concept report",
            _area_line("Core", core),
            _area_line("Application", application),
            (f"  Construction hops:        {len(hops)} leaves [{hop_values}]"),
            f"  Wrote {report['buildDir']}/{concept_scope.REPORT_NAME}",
        ]
    )


def resolve_clang(build_dir: Path | None = None) -> str:
    if os.name == "nt":
        if build_dir is None:
            raise die("Windows concept report requires a configured build directory to locate clang")
        return tidyengine.clang_tool(build_dir, "clang")
    found = shutil.which("clang")
    if found is None:
        raise die("clang is required for ./ao deps report --concepts")
    return found


def compile_flag_union(build_dir: Path, root: Path = PROJECT_ROOT) -> list[str]:
    database = Path(build_dir) / "compile_commands.json"
    if not database.is_file():
        raise die(f"compile_commands.json not found in {build_dir}; run ./ao build first")
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise die(f"cannot read {database}: {error}") from error
    if not isinstance(entries, list):
        raise die(f"invalid compilation database in {database}: expected a JSON array")

    flags: list[str] = []
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        for flag in extract_compile_flags(entry, root, build_dir=Path(build_dir)):
            if flag not in seen:
                seen.add(flag)
                flags.append(flag)
    if not any(flag == "-std=c++26" or flag.startswith("-std=") or flag.startswith("/std") for flag in flags):
        flags.insert(0, "-std=c++26")
    return flags


def extract_compile_flags(
    entry: Mapping[str, Any],
    root: Path = PROJECT_ROOT,
    build_dir: Path | None = None,
) -> list[str]:
    raw = entry.get("arguments")
    if isinstance(raw, list) and all(isinstance(item, str) for item in raw):
        arguments = list(raw)
    elif isinstance(command := entry.get("command"), str):
        arguments = shlex.split(command, posix=os.name != "nt")
    else:
        return []

    source = entry.get("file")
    source_names = {str(source)} if isinstance(source, str) else set()
    flags: list[str] = []
    index = 1 if arguments else 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in source_names or _looks_like_source(argument):
            index += 1
            continue
        if argument == "-Xclang" or argument in DROP_EXACT_FLAGS or argument.startswith(DROP_FLAG_PREFIXES):
            if argument in TAKES_VALUE and index + 1 < len(arguments):
                index += 2
                continue
            index += 1
            continue
        if argument in TAKES_VALUE:
            if index + 1 < len(arguments):
                flags.extend(
                    _project_flags(
                        _normalize_flag_pair(argument, arguments[index + 1], root, entry),
                        root,
                        build_dir,
                        entry,
                    )
                )
                index += 2
                continue
        if argument.startswith(KEEP_FLAG_PREFIXES):
            flags.extend(_project_flags([_normalize_flag(argument, root, entry)], root, build_dir, entry))
        index += 1
    return flags


def parse_headers(
    headers: Mapping[str, Sequence[Path]],
    clang: str,
    flags: Sequence[str],
    root: Path,
    jobs: int,
) -> list[HeaderParse]:
    work: list[tuple[str, Path]] = []
    for area, paths in headers.items():
        work.extend((area, path) for path in paths)
    if not work:
        return []

    workers = max(1, min(jobs, len(work)))
    results: list[HeaderParse] = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(parse_header, clang, flags, path, area, root): (area, path) for area, path in work}
        for future in as_completed(futures):
            results.append(future.result())
    results.sort(key=lambda item: (item.area, item.header))
    return results


def parse_header(
    clang: str,
    flags: Sequence[str],
    path: Path,
    area: str,
    root: Path,
) -> HeaderParse:
    relative = _relative(path, root)
    try:
        ast = dump_ast(clang, flags, path)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, ValueError) as error:
        return HeaderParse(header=relative, area=area, error=f"{relative}: {error}")
    return extract_header(ast, path, area, root)


def dump_ast(clang: str, flags: Sequence[str], path: Path) -> dict[str, Any]:
    command = [
        clang,
        "-x",
        "c++",
        "-fsyntax-only",
        "-Xclang",
        "-ast-dump=json",
        "-Xclang",
        "-ast-dump-filter=ao",
        "-fno-color-diagnostics",
        "-Wno-unused-command-line-argument",
        "-Wno-pragma-once-outside-header",
        "-Qunused-arguments",
        *flags,
        str(path),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip() or f"exit {result.returncode}"
        raise ValueError(detail)
    payload = result.stdout.lstrip()
    brace = payload.find("{")
    if brace < 0:
        return {"kind": "TranslationUnitDecl", "inner": []}
    documents = _load_json_documents(payload[brace:])
    if not documents:
        return {"kind": "TranslationUnitDecl", "inner": []}
    if len(documents) == 1 and documents[0].get("kind") == "TranslationUnitDecl":
        return documents[0]
    return {"kind": "TranslationUnitDecl", "inner": documents}


def extract_header(ast: Mapping[str, Any], path: Path, area: str, root: Path) -> HeaderParse:
    main = _normalize_path(path)
    collector = _Collector(main=main, area=area, root=root)
    collector.walk(ast, _WalkFrame())
    project_headers = sorted(collector.project_headers)
    include_bytes = 0
    for header in project_headers:
        candidate = root / header
        try:
            include_bytes += candidate.stat().st_size
        except OSError:
            continue
    return HeaderParse(
        header=_relative(path, root),
        area=area,
        declarations=collector.declarations,
        pending_edges=collector.pending_edges,
        project_headers=project_headers,
        include_bytes=include_bytes,
        bind_unbind=sorted(collector.bind_unbind),
        aggregates=collector.aggregates,
    )


def translation_unit_fan_out(build_dir: Path, root: Path, headers: Sequence[str]) -> dict[str, int]:
    """Count ninja consumers for each public header path (repo-relative posix)."""
    ninja = shutil.which("ninja")
    if ninja is None or not (Path(build_dir) / "build.ninja").is_file():
        return {header: 0 for header in headers}

    try:
        with buildlock.build_tree_lock(build_dir):
            result = subprocess.run(
                [ninja, "-t", "deps"],
                cwd=build_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
    except OSError:
        return {header: 0 for header in headers}
    if result.returncode != 0:
        return {header: 0 for header in headers}

    live_outputs = _compile_outputs(build_dir)
    return _fan_out_from_ninja_deps(result.stdout, build_dir, root, headers, live_outputs)


def _compile_outputs(build_dir: Path) -> set[str]:
    """Return outputs in the current compile graph, excluding stale Ninja deps-log records."""
    database = Path(build_dir) / "compile_commands.json"
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return set()
    if not isinstance(entries, list):
        return set()

    outputs: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(output := entry.get("output"), str):
            continue
        outputs.add(_normalize_path(_ninja_path(output, build_dir)))
    return outputs


def _fan_out_from_ninja_deps(
    output: str,
    build_dir: Path,
    root: Path,
    headers: Sequence[str],
    live_outputs: set[str],
) -> dict[str, int]:
    wanted = {_normalize_path(root / header): header for header in headers}
    consumers: dict[str, set[str]] = {header: set() for header in headers}
    current_output: str | None = None
    for raw_line in output.splitlines():
        if not raw_line:
            current_output = None
            continue
        if raw_line[0].isspace():
            if current_output is None:
                continue
            dependency = raw_line.strip()
            if not dependency:
                continue
            key = _normalize_path(_ninja_path(dependency, build_dir))
            if header := wanted.get(key):
                consumers[header].add(current_output)
            continue
        target, marker, _ = raw_line.partition(": #deps ")
        target_path = _normalize_path(_ninja_path(target.strip(), build_dir))
        current_output = (
            target_path if marker and raw_line.rstrip().endswith("(VALID)") and target_path in live_outputs else None
        )
    return {header: len(names) for header, names in consumers.items()}


def percentile(values: Sequence[int], percent: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    if percent <= 0:
        return ordered[0]
    if percent >= 100:
        return ordered[-1]
    rank = math.ceil(percent / 100.0 * len(ordered))
    return ordered[min(len(ordered), max(1, rank)) - 1]


@dataclass
class _WalkFrame:
    namespace: tuple[str, ...] = ()
    access: str | None = None
    file: str | None = None
    parent_kind: str = ""
    in_function: bool = False
    in_anonymous: bool = False
    type_name: str = ""
    default_constructible: bool = False
    has_bind: bool = False
    has_unbind: bool = False
    public_field_count: int = 0


class _Collector:
    def __init__(self, *, main: str, area: str, root: Path) -> None:
        self.main = main
        self.area = area
        self.root = root
        self.declarations: list[PublicDeclaration] = []
        self.pending_edges: list[PendingEdge] = []
        self.project_headers: set[str] = set()
        self.bind_unbind: set[str] = set()
        self.aggregates: list[dict[str, object]] = []

    def walk(self, node: Mapping[str, Any], frame: _WalkFrame) -> None:
        loc = node.get("loc") if isinstance(node.get("loc"), dict) else {}
        file = loc.get("file", frame.file) if isinstance(loc, dict) else frame.file
        if isinstance(file, str):
            self._note_project_file(file)
        kind = str(node.get("kind") or "")
        name = _string_field(node, "name")
        implicit = bool(node.get("isImplicit"))
        in_main = _normalize_path(file) == self.main if isinstance(file, str) else False
        in_anonymous = frame.in_anonymous or (kind == "NamespaceDecl" and not name)
        access = _child_access(kind, node, frame)
        namespace = frame.namespace
        if kind == "NamespaceDecl" and name and not in_anonymous:
            namespace = (*frame.namespace, name)

        current = _WalkFrame(
            namespace=namespace,
            access=access,
            file=file if isinstance(file, str) else frame.file,
            parent_kind=kind,
            in_function=frame.in_function or kind in FUNCTION_DECL_KINDS or kind in CXX_METHOD_KINDS,
            in_anonymous=in_anonymous,
            type_name=frame.type_name,
            default_constructible=frame.default_constructible,
            has_bind=frame.has_bind,
            has_unbind=frame.has_unbind,
            public_field_count=frame.public_field_count,
        )

        counted = False
        if in_main and not implicit and not in_anonymous and not frame.in_function:
            counted = self._maybe_record(node, kind, name, current, frame)

        entering_record = kind == "CXXRecordDecl" and (counted or frame.parent_kind == "ClassTemplateDecl")
        if entering_record and name:
            current.type_name = _qualified(namespace, name)
            current.namespace = (*namespace, name)
            current.default_constructible = False
            current.has_bind = False
            current.has_unbind = False
            current.public_field_count = 0
            tag = node.get("tagUsed")
            current.access = "public" if tag == "struct" else "private"
            self._collect_edges(node, kind, name, current, in_main)

        for child in node.get("inner") or []:
            if not isinstance(child, dict):
                continue
            child_kind = str(child.get("kind") or "")
            if child_kind == "AccessSpecDecl" and current.type_name:
                current.access = str(child.get("access") or current.access)
                continue
            if current.type_name:
                child_loc = child.get("loc") if isinstance(child.get("loc"), dict) else {}
                child_file = child_loc.get("file", current.file) if isinstance(child_loc, dict) else current.file
                child_main = _normalize_path(child_file) == self.main if isinstance(child_file, str) else in_main
                child_name = _string_field(child, "name")
                self._collect_edges(child, child_kind, child_name, current, child_main)
            if child_kind in {
                "NamespaceDecl",
                "CXXRecordDecl",
                "ClassTemplateDecl",
                "EnumDecl",
                "TypeAliasDecl",
                "TypedefDecl",
                "FunctionDecl",
                "FunctionTemplateDecl",
            }:
                self.walk(child, current)

        if entering_record and current.type_name:
            self._finish_type(current)

    def _maybe_record(
        self,
        node: Mapping[str, Any],
        kind: str,
        name: str,
        current: _WalkFrame,
        parent: _WalkFrame,
    ) -> bool:
        if not name:
            return False
        if parent.parent_kind in {"ClassTemplateDecl", "FunctionTemplateDecl"} and kind in {
            "CXXRecordDecl",
            "FunctionDecl",
        }:
            return False
        if current.access in {"private", "protected"}:
            return False

        qualified = _qualified(current.namespace, name)
        line = _line(node)
        header = self._relative_file(current.file)
        if kind in {"CXXRecordDecl", "ClassTemplateDecl"}:
            if kind == "CXXRecordDecl" and not node.get("completeDefinition") and not node.get("bases"):
                # Keep incomplete standalone forward declarations; they still name a public type.
                pass
            self._add(PublicDeclaration("class", qualified, header, self.area, line))
            return True
        if kind == "EnumDecl":
            self._add(PublicDeclaration("enum", qualified, header, self.area, line))
            return True
        if kind in {"TypeAliasDecl", "TypedefDecl"}:
            signature = _qual_type(node)
            self._add(PublicDeclaration("alias", qualified, header, self.area, line, signature))
            self._pending(qualified, signature, "alias")
            return True
        if kind in FUNCTION_DECL_KINDS:
            if current.type_name:
                # A FunctionTemplateDecl nested in a record wraps the actual
                # CXXMethodDecl. The child is recorded once by _collect_edges.
                return False
            signature = _qual_type(node)
            self._add(PublicDeclaration("function", qualified, header, self.area, line, signature))
            self._function_edges(qualified, node, signature)
            return True
        return False

    def _collect_edges(
        self,
        node: Mapping[str, Any],
        kind: str,
        name: str,
        current: _WalkFrame,
        in_main: bool,
    ) -> None:
        if not in_main:
            return
        if kind == "CXXConstructorDecl" and current.type_name and _is_default_constructor(node):
            current.default_constructible = True
        if not current.type_name:
            return
        owner = current.type_name
        if kind == "CXXRecordDecl":
            for base in node.get("bases") or []:
                if not isinstance(base, dict):
                    continue
                if base.get("access") != "public" and base.get("writtenAccess") != "public":
                    continue
                self._pending(owner, _qual_type(base), "base")
        if kind == "FieldDecl" and current.access == "public":
            current.public_field_count += 1
            self._pending(owner, _qual_type(node), "field")
        if kind in CXX_METHOD_KINDS and current.access == "public" and not node.get("isImplicit"):
            method_name = f"{owner}::{name}"
            signature = _qual_type(node)
            self._add(
                PublicDeclaration(
                    "method", method_name, self._relative_file(current.file), self.area, _line(node), signature
                )
            )
            if name == "bind":
                current.has_bind = True
            if name == "unbind":
                current.has_unbind = True
            self._pending(method_name, owner, "owner", signature)
            self._function_edges(method_name, node, signature)

    def _finish_type(self, current: _WalkFrame) -> None:
        simple = current.type_name.rsplit("::", 1)[-1]
        if current.has_bind and current.has_unbind and current.default_constructible:
            self.bind_unbind.add(current.type_name)
        if simple.endswith("Context") or simple.endswith("Dependencies"):
            self.aggregates.append(
                {
                    "name": current.type_name,
                    "kind": "Context" if simple.endswith("Context") else "Dependencies",
                    "fields": current.public_field_count,
                    "header": self._relative_file(current.file),
                }
            )

    def _function_edges(self, owner: str, node: Mapping[str, Any], signature: str) -> None:
        self._pending(owner, _return_type(signature), "return", signature)
        for child in node.get("inner") or []:
            if isinstance(child, dict) and child.get("kind") == "ParmVarDecl":
                self._pending(owner, _qual_type(child), "parameter", signature)

    def _add(self, declaration: PublicDeclaration) -> None:
        self.declarations.append(declaration)

    def _pending(self, source: str, type_text: str, reason: str, source_signature: str = "") -> None:
        if source and type_text:
            self.pending_edges.append(PendingEdge(source, type_text, reason, source_signature))

    def _note_project_file(self, file: str) -> None:
        path = Path(file)
        if path.suffix not in concept_scope.HEADER_SUFFIXES:
            return
        try:
            relative = _relative(path, self.root)
        except ValueError:
            return
        if relative.startswith(".."):
            return
        self.project_headers.add(relative)

    def _relative_file(self, file: str | None) -> str:
        if not file:
            return ""
        try:
            return _relative(Path(file), self.root)
        except ValueError:
            return file


def _child_access(kind: str, node: Mapping[str, Any], frame: _WalkFrame) -> str | None:
    if kind == "AccessSpecDecl":
        access = node.get("access")
        return str(access) if isinstance(access, str) else frame.access
    if frame.access is None:
        return None
    declared = node.get("access")
    if isinstance(declared, str):
        return declared
    return frame.access


def _is_default_constructor(node: Mapping[str, Any]) -> bool:
    if node.get("isImplicit"):
        return True
    params = [
        child for child in node.get("inner") or [] if isinstance(child, dict) and child.get("kind") == "ParmVarDecl"
    ]
    return not params


def _load_json_documents(payload: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    documents: list[dict[str, Any]] = []
    index = 0
    length = len(payload)
    while index < length:
        while index < length and payload[index].isspace():
            index += 1
        if index >= length:
            break
        try:
            value, end = decoder.raw_decode(payload, index)
        except json.JSONDecodeError as error:
            raise ValueError(f"clang JSON AST was truncated: {error}") from error
        if isinstance(value, dict):
            documents.append(value)
        index = end
    return documents


def _qual_type(node: Mapping[str, Any]) -> str:
    type_node = node.get("type")
    if isinstance(type_node, dict):
        qual = type_node.get("qualType")
        if isinstance(qual, str):
            return qual
    return ""


def _return_type(signature: str) -> str:
    if "(" not in signature:
        return signature
    return signature.split("(", 1)[0].strip()


def _type_tokens(type_text: str) -> list[str]:
    tokens: list[str] = []
    for match in IDENTIFIER_RE.finditer(type_text):
        raw = match.group(0)
        if not raw or raw in TYPE_KEYWORDS:
            continue
        tokens.append(raw)
    return tokens


def _qualified(namespace: tuple[str, ...], name: str) -> str:
    return "::".join((*namespace, name))


def _string_field(node: Mapping[str, Any], key: str) -> str:
    value = node.get(key)
    return value if isinstance(value, str) else ""


def _line(node: Mapping[str, Any]) -> int:
    loc = node.get("loc")
    if not isinstance(loc, dict):
        return 0
    line = loc.get("line")
    return line if isinstance(line, int) else 0


def _dedup_declarations(parses: Sequence[HeaderParse]) -> list[PublicDeclaration]:
    complete: dict[tuple[str, str, str], PublicDeclaration] = {}
    order: list[tuple[str, str, str]] = []
    for parsed in parses:
        for declaration in parsed.declarations:
            key = declaration.identity
            existing = complete.get(key)
            if existing is None:
                complete[key] = declaration
                order.append(key)
                continue
            if declaration.kind in {"class", "enum"} and declaration.header == existing.header:
                # Prefer the later complete definition when both live in the same header.
                complete[key] = declaration
    return [complete[key] for key in order]


def resolve_edges(parses: Sequence[HeaderParse], declarations: Sequence[PublicDeclaration]) -> list[DependencyEdge]:
    """Resolve type strings against the scanned public names after every header is known."""
    type_declarations = [item for item in declarations if item.kind in DEPENDENCY_TARGET_KINDS]
    by_qualified = {item.name: item.name for item in type_declarations}
    by_simple: dict[str, set[str]] = {}
    by_suffix: dict[str, set[str]] = {}
    for item in type_declarations:
        by_simple.setdefault(item.name.rsplit("::", 1)[-1], set()).add(item.name)
        parts = item.name.split("::")
        for index in range(1, len(parts)):
            by_suffix.setdefault("::".join(parts[index:]), set()).add(item.name)

    seen: set[tuple[str, str, str]] = set()
    edges: list[DependencyEdge] = []
    for parsed in parses:
        for pending in parsed.pending_edges:
            for token in _type_tokens(pending.type_text):
                target = by_qualified.get(token)
                if target is None:
                    candidates = by_suffix.get(token, set()) if "::" in token else by_simple.get(token, set())
                    if len(candidates) == 1:
                        target = next(iter(candidates))
                if not target or target == pending.source:
                    continue
                key = (pending.source, pending.source_signature, target)
                if key in seen:
                    continue
                seen.add(key)
                edges.append(DependencyEdge(pending.source, target, pending.reason, pending.source_signature))
    edges.sort(key=lambda item: (item.source, item.source_signature, item.target, item.reason))
    return edges


def _include_edges(parses: Sequence[HeaderParse]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for parsed in parses:
        reachable = [header for header in parsed.project_headers if header != parsed.header]
        rows.append(
            {
                "header": parsed.header,
                "area": parsed.area,
                "headers": len(parsed.project_headers),
                "bytes": parsed.include_bytes,
                "includes": reachable,
            }
        )
    return rows


def _area_totals(
    declarations: Sequence[PublicDeclaration],
    edges: Sequence[DependencyEdge],
) -> dict[str, dict[str, int]]:
    totals: dict[str, dict[str, int]] = {}
    identities_by_area: dict[str, set[tuple[str, str]]] = {"core": set(), "application": set()}
    for area in ("core", "application"):
        kinds = Counter(item.kind for item in declarations if item.area == area)
        identities_by_area[area] = {
            (item.name, item.signature if item.kind in {"function", "method"} else "")
            for item in declarations
            if item.area == area
        }
        totals[area] = {
            "declarations": sum(kinds.values()),
            "class": kinds.get("class", 0),
            "enum": kinds.get("enum", 0),
            "alias": kinds.get("alias", 0),
            "function": kinds.get("function", 0),
            "method": kinds.get("method", 0),
            "dependencyEdges": 0,
        }
    for edge in edges:
        for area, identities in identities_by_area.items():
            if (edge.source, edge.source_signature) in identities:
                totals[area]["dependencyEdges"] += 1
                break
    return totals


def _include_stats(parses: Sequence[HeaderParse]) -> dict[str, dict[str, int]]:
    stats: dict[str, dict[str, int]] = {}
    for area in ("core", "application"):
        header_counts = [len(parsed.project_headers) for parsed in parses if parsed.area == area]
        byte_counts = [parsed.include_bytes for parsed in parses if parsed.area == area]
        stats[area] = {
            "medianHeaders": percentile(header_counts, 50),
            "p95Headers": percentile(header_counts, 95),
            "medianBytes": percentile(byte_counts, 50),
            "p95Bytes": percentile(byte_counts, 95),
        }
    return stats


def _fan_out_stats(fan_out: Mapping[str, int], headers: Sequence[str]) -> dict[str, int]:
    unique_headers = sorted(set(headers))
    values = [fan_out.get(header, 0) for header in unique_headers]
    return {
        "medianTranslationUnits": percentile(values, 50),
        "p95TranslationUnits": percentile(values, 95),
        "headers": len(unique_headers),
    }


def _fan_out_rows(fan_out: Mapping[str, int], public_headers: Mapping[str, Sequence[str]]) -> list[dict[str, object]]:
    return [
        {
            "header": header,
            "area": area,
            "translationUnits": fan_out.get(header, 0),
        }
        for area in ("core", "application")
        for header in sorted(set(public_headers.get(area, ())))
    ]


def _secondary_inventory(
    parses: Sequence[HeaderParse],
    declarations: Sequence[PublicDeclaration],
    headers: Mapping[str, Sequence[Path]],
    root: Path,
    build_dir: Path,
) -> dict[str, object]:
    suffixes: Counter[str] = Counter()
    for declaration in declarations:
        if declaration.kind != "class":
            continue
        simple = declaration.name.rsplit("::", 1)[-1]
        for suffix in sorted(concept_scope.ROLE_SUFFIXES, key=len, reverse=True):
            if simple.endswith(suffix) and simple != suffix:
                suffixes[suffix] += 1
                break
    bind_unbind: list[str] = []
    for parsed in parses:
        bind_unbind.extend(parsed.bind_unbind)
    guardrail_targets = _guardrail_targets(build_dir)
    return {
        "headers": {area: len(paths) for area, paths in headers.items()},
        "roleSuffixes": dict(sorted(suffixes.items())),
        "aggregates": _source_aggregate_inventory(root),
        "bindUnbind": sorted(set(bind_unbind)),
        "guardrailTargets": {
            "count": len(guardrail_targets),
            "targets": guardrail_targets,
        },
    }


def _source_aggregate_inventory(root: Path) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    candidates: list[Path] = []
    for relative_root in ("include", "app"):
        base = root / relative_root
        if not base.is_dir():
            continue
        candidates.extend(
            path for path in base.rglob("*") if path.is_file() and path.suffix in concept_scope.HEADER_SUFFIXES
        )

    for path in sorted(candidates):
        try:
            source = path.read_text(encoding="utf-8")
        except OSError:
            continue
        masked = CPP_COMMENT_OR_LITERAL_RE.sub(lambda match: "\n" * match.group(0).count("\n"), source)
        for match in AGGREGATE_DEFINITION_RE.finditer(masked):
            body_start = match.end() - 1
            body_end = _matching_brace(masked, body_start)
            if body_end is None:
                continue
            name = match.group("name")
            entries.append(
                {
                    "name": name,
                    "kind": _aggregate_kind(name),
                    "fields": _aggregate_field_count(masked[body_start + 1 : body_end]),
                    "header": _relative(path, root),
                }
            )
    return sorted(entries, key=lambda item: (str(item["name"]), str(item["header"])))


def _aggregate_kind(name: str) -> str:
    for suffix in ("Context", "Collaborators"):
        if name.endswith(suffix):
            return suffix
    return "Dependencies"


def _matching_brace(text: str, opening: int) -> int | None:
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def _aggregate_field_count(body: str) -> int:
    fields = 0
    statement: list[str] = []
    brace_depth = 0
    for character in body:
        if character == "{":
            brace_depth += 1
        elif character == "}" and brace_depth > 0:
            brace_depth -= 1
        if character == ";" and brace_depth == 0:
            if _looks_like_data_member("".join(statement)):
                fields += 1
            statement.clear()
            continue
        statement.append(character)
    return fields


def _looks_like_data_member(statement: str) -> bool:
    candidate = statement.strip()
    if not candidate:
        return False
    candidate = re.sub(r"^(?:public|private|protected)\s*:\s*", "", candidate).strip()
    if not candidate or re.match(r"^(?:using|typedef|friend|static_assert|class|struct|enum|template)\b", candidate):
        return False

    angle_depth = 0
    assignment = len(candidate)
    for index, character in enumerate(candidate):
        if character == "<":
            angle_depth += 1
        elif character == ">" and angle_depth > 0:
            angle_depth -= 1
        elif angle_depth == 0 and character in "={":
            assignment = index
            break
        elif angle_depth == 0 and character == "(" and index < assignment:
            return False
    return True


def _guardrail_targets(build_dir: Path) -> list[str]:
    ninja = shutil.which("ninja")
    if ninja is None or not (Path(build_dir) / "build.ninja").is_file():
        return []
    try:
        with buildlock.build_tree_lock(build_dir):
            result = subprocess.run(
                [ninja, "-t", "targets", "all"],
                cwd=build_dir,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
    except OSError:
        return []
    if result.returncode != 0:
        return []
    return _guardrail_targets_from_ninja(result.stdout)


def _guardrail_targets_from_ninja(output: str) -> list[str]:
    suffixes = ("_check", "_guardrail", "_boundary_report")
    targets = {
        raw_line.partition(":")[0].strip()
        for raw_line in output.splitlines()
        if raw_line.startswith("ao_") and raw_line.partition(":")[0].strip().endswith(suffixes)
    }
    return sorted(targets)


def _area_line(label: str, area: Mapping[str, Any]) -> str:
    include = area["includeWeight"]
    fan = area["rebuildFanOut"]
    return (
        f"  {label + ' declarations:':<26}"
        f"{area['declarations']}  "
        f"(class {area['class']}, enum {area['enum']}, "
        f"alias {area['alias']}, function {area['function']}, method {area['method']}; "
        f"edges {area['dependencyEdges']}; "
        f"include p50 {include['medianHeaders']} hdr/{include['medianBytes']} B, "
        f"p95 {include['p95Headers']} hdr/{include['p95Bytes']} B; "
        f"fan-out p50 {fan['medianTranslationUnits']} TU, "
        f"p95 {fan['p95TranslationUnits']} TU)"
    )


def _looks_like_source(argument: str) -> bool:
    suffix = Path(argument).suffix.lower()
    return suffix in {".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx"}


def _normalize_flag(argument: str, root: Path, entry: Mapping[str, Any]) -> str:
    if argument.startswith("/I"):
        return "-I" + _resolve_include(argument[2:], root, entry)
    if argument.startswith("/D"):
        return "-D" + argument[2:]
    if argument.startswith("/U"):
        return "-U" + argument[2:]
    if argument.startswith("/std:"):
        return "-std=" + argument.split(":", 1)[1].replace("c++latest", "c++26")
    for prefix in ("-isystem", "-iquote", "-idirafter", "-I"):
        if argument.startswith(prefix) and len(argument) > len(prefix) and not argument[len(prefix) :].startswith("-"):
            value = argument[len(prefix) :]
            return prefix + _resolve_include(value, root, entry)
    return argument


def _normalize_flag_pair(flag: str, value: str, root: Path, entry: Mapping[str, Any]) -> list[str]:
    mapped = {"/I": "-I", "/D": "-D", "/U": "-U"}
    flag = mapped.get(flag, flag)
    if flag in {"-I", "-isystem", "-iquote", "-idirafter"}:
        return [flag + _resolve_include(value, root, entry)]
    if flag in {"-include", "-iframework"}:
        return [flag, _resolve_include(value, root, entry)]
    if flag == "/std":
        return ["-std=" + value.replace("c++latest", "c++26")]
    return [flag, value]


def _project_flags(
    flags: Sequence[str],
    root: Path,
    build_dir: Path | None,
    entry: Mapping[str, Any],
) -> list[str]:
    """Keep language/defines and project include paths; drop host SDK -isystem directories."""
    kept: list[str] = []
    skip_next = False
    for flag in flags:
        if skip_next:
            skip_next = False
            if _is_project_include_path(flag, root, build_dir, entry):
                kept.append(flag)
            continue
        if flag in {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-iframework"}:
            skip_next = True
            kept.append(flag)
            continue
        prefix = next(
            (
                item
                for item in ("-isystem", "-iquote", "-idirafter", "-I")
                if flag.startswith(item) and len(flag) > len(item)
            ),
            None,
        )
        if prefix is not None:
            if _is_project_include_path(flag[len(prefix) :], root, build_dir, entry):
                kept.append(flag)
            continue
        kept.append(flag)
    if kept and kept[-1] in {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-iframework"}:
        kept.pop()
    return kept


def _is_project_include_path(value: str, root: Path, build_dir: Path | None, entry: Mapping[str, Any]) -> bool:
    path = Path(value)
    bases = [absolute_path(root)]
    if build_dir is not None:
        bases.append(absolute_path(build_dir))
    directory = entry.get("directory")
    if isinstance(directory, str):
        bases.append(absolute_path(Path(directory)))
    for base in bases:
        try:
            absolute_path(path).relative_to(base)
            return True
        except ValueError:
            continue
    return False


def _resolve_include(value: str, root: Path, entry: Mapping[str, Any]) -> str:
    path = Path(value)
    if path.is_absolute():
        return str(path)
    directory = entry.get("directory")
    base = Path(directory) if isinstance(directory, str) else root
    return str((base / path).resolve())


def _relative(path: Path, root: Path) -> str:
    return absolute_path(path).relative_to(absolute_path(root)).as_posix()


def _normalize_path(path: Path | str) -> str:
    return os.path.normcase(str(absolute_path(path)))


def _ninja_path(value: str, build_dir: Path) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path
    return build_dir / path
