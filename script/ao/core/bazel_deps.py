"""Governed native interfaces and sparse Bazel repository plans.

Repository rules own invocation, materialization and invalidation. Resolver
tools and provider identity are explicit. Repository plans reject unsatisfied
compile requirements rather than exporting non-transitive cc_library.copts.
Plans do not certify complete ABI/runtime closure or build provenance.
"""

from __future__ import annotations

import hashlib
import os
import re
import shlex
import subprocess
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

GENERATOR = "aobus-bazel-artifact-adapter-v1"
_FIELDS = frozenset({"header_dirs", "defines", "copts", "linkopts", "libraries", "runtime"})
_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
_MODULE = re.compile(r"[A-Za-z0-9_+.-]+\Z")
_COMPILER_FLAGS = frozenset(
    {
        "-pthread",
        "-fPIC",
        "-mfpmath=sse",
        "-msse",
        "-msse2",
        "-fno-strict-aliasing",
        "-fno-strict-overflow",
        "/permissive-",
    }
)


class BazelDependencyError(RuntimeError):
    """Invalid dependency metadata, artifacts, patch guards, or repository output."""


def _require(condition: object, message: str) -> None:
    if not condition:
        raise BazelDependencyError(message)


def _mapping(value: object) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BazelDependencyError("expected JSON object")
    return value


def _keys(
    value: object, allowed: set[str] | frozenset[str], required: set[str] | frozenset[str] = frozenset()
) -> dict[str, Any]:
    result = _mapping(value)
    _require(not (result.keys() - allowed), f"unknown keys: {sorted(result.keys() - allowed)}")
    _require(required <= result.keys(), f"missing keys: {sorted(required - result.keys())}")
    return result


def _array(value: object) -> list[Any]:
    if not isinstance(value, list):
        raise BazelDependencyError("expected JSON array")
    return value


def _text(value: object) -> str:
    if not isinstance(value, str) or not value or any(c in value for c in "\n\r\x00"):
        raise BazelDependencyError("expected nonempty single-line string")
    return value


def _strings(value: object) -> list[str]:
    result = [_text(item) for item in _array(value)]
    _require(len(result) == len(set(result)), "duplicate array value")
    return result


def _absolute(value: object) -> Path:
    path = Path(_text(value))
    _require(path.is_absolute(), f"path must be absolute: {path}")
    return path


def _resolve(path: Path, *, strict: bool = True) -> Path:
    try:
        return path.resolve(strict=strict)
    except (OSError, RuntimeError) as exc:
        raise BazelDependencyError(f"cannot resolve path (missing file or symlink cycle): {path}: {exc}") from exc


def _digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


@dataclass(frozen=True)
class _Library:
    name: str
    kind: str
    file: str | None = None
    runtime: str | None = None
    soname: str | None = None
    # Derived linker hints do not extend the package interface or manifest schema.
    rpath_link_dirs: tuple[str, ...] = ()

    @classmethod
    def parse(cls, raw: object) -> _Library:
        value = _keys(raw, {"name", "kind", "file", "runtime"}, {"name", "kind"})
        name, kind = _text(value["name"]), _text(value["kind"])
        _require(_MODULE.fullmatch(name) and kind in {"system", "static", "shared"}, "invalid library interface")
        for key in ("file", "runtime"):
            if key in value:
                _absolute(value[key])
        return cls(name, kind, value.get("file"), value.get("runtime"))

    def json(self) -> dict[str, Any]:
        result: dict[str, Any] = {"name": self.name, "kind": self.kind}
        for key in ("file", "runtime", "soname"):
            value = getattr(self, key)
            if value is not None:
                result[key] = value
        return result


@dataclass
class _Interface:
    header_dirs: list[str] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)
    copts: list[str] = field(default_factory=list)
    linkopts: list[str] = field(default_factory=list)
    libraries: list[_Library] = field(default_factory=list)
    runtime: list[str] = field(default_factory=list)

    @classmethod
    def parse(cls, raw: object) -> _Interface:
        value = _keys(raw, _FIELDS)
        return cls(
            header_dirs=_strings(value.get("header_dirs", [])),
            defines=_strings(value.get("defines", [])),
            copts=_strings(value.get("copts", [])),
            linkopts=_strings(value.get("linkopts", [])),
            libraries=[_Library.parse(item) for item in _array(value.get("libraries", []))],
            runtime=_strings(value.get("runtime", [])),
        )

    def json(self) -> dict[str, list[Any]]:
        return {
            "header_dirs": self.header_dirs.copy(),
            "defines": self.defines.copy(),
            "copts": self.copts.copy(),
            "linkopts": self.linkopts.copy(),
            "libraries": [item.json() for item in self.libraries],
            "runtime": self.runtime.copy(),
        }


def _append(values: list[str], value: str) -> None:
    if value not in values:
        values.append(value)


def _validate_pc_requirements(path: Path) -> None:
    """Reject path-based metadata lookups, including private shared cflags.

    Some native resolvers fail --print-requires-private on missing static-only
    dependencies even when shared --cflags succeeds. Validate module-only lookup
    syntax directly, without requiring those dependencies to exist. This is a
    bounded syntax guard, not a replacement pkg-config evaluator.
    """
    variables: dict[str, str] = {"pcfiledir": str(path.parent), "pc_sysrootdir": ""}
    requirements: list[str] = []
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeError as exc:
        raise BazelDependencyError(f"invalid pkg-config metadata encoding: {path}") from exc
    for raw in re.sub(r"\\\r?\n", "", content).splitlines():
        line = raw.split("#", maxsplit=1)[0].strip()
        variable = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)", line)
        dependency = re.fullmatch(r"(Requires[^:]*|Conflicts)\s*:(.*)", line)
        if variable:
            _require(variable[1] not in variables, f"duplicate pkg-config metadata variable: {path}: {variable[1]}")
            variables[variable[1]] = variable[2].strip()
        elif dependency:
            _require(
                dependency[1].strip() in {"Requires", "Requires.private", "Conflicts"},
                f"unsupported pkg-config dependency field: {path}: {dependency[1]}",
            )
            requirements.append(dependency[2].strip())

    def expand(value: str, ancestors: tuple[str, ...] = ()) -> str:
        def replace(match: re.Match[str]) -> str:
            name = match[1]
            _require(
                name in variables and name not in ancestors,
                f"unsupported pkg-config requirement variable {name!r}: {path}",
            )
            return expand(variables[name], (*ancestors, name))

        return re.sub(r"\$\{([^}]+)\}", replace, value)

    pattern = re.compile(r"[A-Za-z0-9_+.-]+(?:\s*(?:>=|<=|!=|=|>|<)\s*[A-Za-z0-9_+.~:-]+)?")
    for raw in requirements:
        remaining = expand(raw)
        while remaining:
            match = pattern.match(remaining)
            if match is None:
                raise BazelDependencyError(f"unsupported pkg-config requirement: {raw}: {path}")
            suffix = remaining[match.end() :]
            _require(
                not suffix or suffix[0].isspace() or suffix[0] == ",",
                f"unsupported pkg-config requirement: {raw}: {path}",
            )
            remaining = suffix.lstrip(" ,\t\r\n")


class _Adapter:
    def __init__(self, spec: dict[str, Any], patches: dict[str, Any]) -> None:
        _keys(
            spec,
            {
                "schema",
                "platform",
                "config",
                "linkage",
                "provider",
                "allowed_roots",
                "library_dirs",
                "library_roots",
                "runtime_roots",
                "system_libraries",
                "pkg_config",
                "elf",
                "packages",
            },
            {"schema", "platform", "config", "linkage", "provider", "allowed_roots", "packages"},
        )
        _require(type(spec["schema"]) is int and spec["schema"] == 1, "unsupported spec schema")
        self.platform = _text(spec["platform"])
        self.linkage = _text(spec["linkage"])
        _require(self.platform in {"linux", "macos", "windows"}, "unsupported platform")
        _require(spec["config"] in ("Debug", "Release"), "unsupported configuration")
        _require(self.linkage in {"static", "shared"}, "unsupported linkage")
        provider = _keys(spec["provider"], {"name", "version", "evidence"}, {"name", "version"})
        _text(provider["name"])
        _text(provider["version"])
        self.spec = spec
        self.roots = [_resolve(_absolute(p)) for p in _strings(spec["allowed_roots"])]
        _require(
            self.roots and all(p.is_dir() or p.is_file() for p in self.roots),
            "allowed_roots must be existing files/directories",
        )
        self.inputs: dict[str, dict[str, Any]] = {}
        self.aliases: set[Path] = set()
        self.probes: set[Path] = set()
        self.modules: dict[str, dict[str, str]] = {}
        self.applied: list[str] = []
        self.packages: list[tuple[str, _Interface, str]] = []
        self.default_linkage = self.linkage
        self.package_linkages: dict[str, str] = {}
        self.header_files: dict[str, list[Path]] = {}
        self.library_roots = (
            [self.path(p, directory=True) for p in _strings(spec["library_roots"])] if "library_roots" in spec else None
        )
        self.runtime_roots = (
            [self.path(p, directory=True) for p in _strings(spec["runtime_roots"])] if "runtime_roots" in spec else None
        )
        self.libdirs = [self.artifact_path(p, directory=True) for p in _strings(spec.get("library_dirs", []))]
        self.system_libraries = _strings(spec.get("system_libraries", []))
        for name in self.system_libraries:
            _require(_MODULE.fullmatch(name), f"invalid system name: {name}")
        for raw in _array(provider.get("evidence", [])):
            evidence = _keys(raw, {"path", "sha256"}, {"path", "sha256"})
            path = self.path(evidence["path"])
            _require(_digest(path) == evidence["sha256"], f"provider evidence hash/version identity mismatch: {path}")
            self.record(path, "provider_evidence")
        self.pc: Path | None = None
        self.pcdirs: list[Path] = []
        self.pcfiles: dict[str, list[Path]] = {}
        self.readelf: Path | None = None
        if "pkg_config" in spec:
            pc = _keys(spec["pkg_config"], {"executable", "pc_dirs"}, {"executable", "pc_dirs"})
            self.pc = self.executable(pc["executable"])
            self.pcdirs = [self.path(p, directory=True) for p in _strings(pc["pc_dirs"])]
            _require(self.pcdirs, "pkg_config.pc_dirs cannot be empty")
            # pkg-config searches these directories non-recursively. Record the
            # whole bounded inventory, including private compile dependencies in
            # shared mode and metadata not reachable via --print-requires.
            for directory in self.pcdirs:
                for path in sorted(directory.glob("*.pc")):
                    resolved = self.path(str(path))
                    self.pcfiles.setdefault(path.stem, []).append(resolved)
                    self.record(resolved, "pkg_config_metadata")
                    _validate_pc_requirements(resolved)
        if "elf" in spec:
            elf = _keys(spec["elf"], {"readelf"}, {"readelf"})
            self.readelf = self.executable(elf["readelf"])
        _keys(patches, {"schema", "patches"}, {"schema", "patches"})
        _require(type(patches["schema"]) is int and patches["schema"] == 1, "unsupported patches schema")
        self.patches = [_mapping(p) for p in _array(patches["patches"])]
        ids: set[str] = set()
        for patch in self.patches:
            _keys(
                patch, {"id", "when", "expect", "add", "remove", "satisfy"}, {"id", "when", "expect", "add", "satisfy"}
            )
            patch_id = _text(patch["id"])
            _require(patch_id not in ids, "duplicate patch id")
            ids.add(patch_id)
            predicates = {"package", "platform", "config", "provider", "provider_version"}
            when = _keys(patch["when"], predicates, predicates)
            for value in when.values():
                _text(value)
            expect = _keys(
                patch["expect"],
                {"missing", "present", "unmet_requirements"},
                {"missing", "present", "unmet_requirements"},
            )
            for value in (patch["add"], patch.get("remove", {}), expect["missing"], expect["present"]):
                _Interface.parse(value)
            _strings(expect["unmet_requirements"])
            _strings(patch["satisfy"])

    def path(self, value: object, *, directory: bool = False) -> Path:
        original = _absolute(value)
        path = _resolve(original)
        if original != path:
            self.aliases.add(original)
        _require(any(path.is_relative_to(root) for root in self.roots), f"outside allowed provider roots: {path}")
        _require(
            path.is_dir() if directory else path.is_file(), f"missing {'directory' if directory else 'file'}: {path}"
        )
        return path

    def artifact_path(self, value: object, *, runtime: bool = False, directory: bool = False) -> Path:
        path = self.path(value, directory=directory)
        roots = self.runtime_roots if runtime else self.library_roots
        role = "runtime" if runtime else "library"
        _require(
            roots is None or any(path.is_relative_to(root) for root in roots),
            f"outside configured {role} roots: {path}",
        )
        return path

    def record(self, path: Path, role: str) -> None:
        key = str(path)
        if key not in self.inputs:
            self.inputs[key] = {"path": key, "sha256": _digest(path), "roles": []}
        roles: list[str] = self.inputs[key]["roles"]
        if role not in roles:
            roles.append(role)
            roles.sort()

    def executable(self, value: object) -> Path:
        original = _absolute(value)
        path = _resolve(original)
        _require(path.is_file() and (os.name == "nt" or os.access(path, os.X_OK)), f"not executable: {path}")
        self.record(path, "resolver_tool")
        # Multicall tools can select their behavior from the invocation basename.
        return original

    def run(self, executable: Path, *args: str) -> str:
        env = {
            "PATH": os.defpath,
            "PKG_CONFIG_PATH": "",
            "PKG_CONFIG_LIBDIR": os.pathsep.join(map(str, self.pcdirs)),
            "PKG_CONFIG_DISABLE_UNINSTALLED": "1",
            "PKG_CONFIG_ALLOW_SYSTEM_CFLAGS": "1",
            "PKG_CONFIG_ALLOW_SYSTEM_LIBS": "1",
            "LC_ALL": "C",
        }
        for key in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP"):
            if key in os.environ:
                env[key] = os.environ[key]
        try:
            result = subprocess.run(
                [str(executable), *args],
                env=env,
                text=True,
                encoding="utf-8",
                capture_output=True,
                check=False,
                timeout=60,
            )
        except (OSError, subprocess.TimeoutExpired, UnicodeError) as exc:
            raise BazelDependencyError(f"external query failed ({executable} {' '.join(args)}): {exc}") from exc
        _require(
            result.returncode == 0, f"external query failed ({executable} {' '.join(args)}): {result.stderr.strip()}"
        )
        return result.stdout.strip()

    def prepare(self) -> None:
        names: set[str] = set()
        for raw in _array(self.spec["packages"]):
            package = _keys(
                raw,
                {"name", "modules", "expected_versions", "linkage"},
                {"name", "modules"},
            )
            name = _text(package["name"])
            _require(_NAME.fullmatch(name) and name not in names, f"invalid/duplicate package name: {name}")
            names.add(name)
            self.linkage = _text(package.get("linkage", self.default_linkage))
            _require(self.linkage in {"static", "shared"}, f"unsupported package linkage: {name}: {self.linkage}")
            self.package_linkages[name] = self.linkage
            modules = _strings(package["modules"])
            for module in modules:
                _require(_MODULE.fullmatch(module), f"invalid module: {module}")
            expected = _mapping(package.get("expected_versions", {}))
            _require(expected.keys() <= set(modules), "expected_versions must name requested modules")
            for version in expected.values():
                _text(version)
            interface = _Interface()
            if modules:
                _require(self.pc is not None, "pkg_config required for modules")
                self.from_pc(modules, expected, interface)
            requirements: set[str] = set()
            if not modules:
                requirements.add("manual_interface")
            interface = self.patch(name, interface, requirements)
            _require(not requirements, f"unmet requirements for {name}: {sorted(requirements)}")
            self.packages.append((name, interface, "automatic" if modules else "manual_fallback"))
        _require(len(self.applied) == len(self.patches), "stale/nonapplicable patch predicate or unknown package")

    def from_pc(self, modules: list[str], expected: dict[str, Any], interface: _Interface) -> None:
        if self.pc is None:
            raise BazelDependencyError("pkg_config required for modules")
        pc = self.pc
        seen: set[str] = set()

        def metadata(module: str) -> None:
            if module in seen:
                return
            seen.add(module)
            _require(module in self.pcfiles, f"metadata outside isolated pc_dirs or missing: {module}")
            _require(len(set(self.pcfiles[module])) == 1, f"ambiguous pkg-config metadata: {module}")
            version = self.run(pc, "--modversion", module)
            _require(
                module not in expected or version == expected[module],
                f"module version mismatch: {module}: {version} != {expected.get(module)}",
            )
            directory = self.path(self.run(pc, "--variable=pcfiledir", module), directory=True)
            _require(directory in self.pcdirs, f"metadata outside isolated pc_dirs: {directory}")
            path = self.path(str(directory / (module + ".pc")))
            _require(path in self.pcfiles[module], f"metadata outside isolated pc_dirs: {path}")
            self.modules[module] = {"version": version, "metadata": str(path)}
            queries = ["--print-requires"]
            if self.linkage == "static":
                queries.append("--print-requires-private")
            for query in queries:
                for line in self.run(pc, query, module).splitlines():
                    dep = re.split(r"\s|[<>=]", line.strip(), maxsplit=1)[0]
                    _require(_MODULE.fullmatch(dep), f"unsupported pkg-config requirement: {line}")
                    metadata(dep)

        for module in modules:
            metadata(module)
        static = ["--static"] if self.linkage == "static" else []
        for flag, compile_flags in (("--cflags", True), ("--libs", False)):
            try:
                tokens = shlex.split(self.run(pc, *static, flag, *modules))
            except ValueError as exc:
                raise BazelDependencyError(f"malformed pkg-config flags: {exc}") from exc
            self.flags(tokens, interface, compile_flags=compile_flags)

    def flags(self, tokens: list[str], interface: _Interface, *, compile_flags: bool) -> None:
        index = 0
        while index < len(tokens):
            token = tokens[index]
            index += 1
            if token in ("-I", "-isystem", "-L", "-D", "-l"):
                _require(index < len(tokens), f"missing flag argument: {token}")
                value = tokens[index]
                index += 1
                kind: str | None = token
            else:
                kind = next((p for p in ("-isystem", "-I", "-L", "-D", "-l") if token.startswith(p)), None)
                value = token[len(kind) :] if kind else token
            if kind in ("-I", "-isystem") and compile_flags:
                _append(interface.header_dirs, str(self.path(value, directory=True)))
            elif kind == "-D" and compile_flags:
                _append(interface.defines, _text(value))
            elif kind == "-L" and not compile_flags:
                path = self.artifact_path(value, directory=True)
                if path not in self.libdirs:
                    self.libdirs.append(path)
            elif kind == "-l" and not compile_flags:
                _require(_MODULE.fullmatch(value), f"unsupported library flag: {value}")
                library_kind = "system" if value in self.system_libraries else self.linkage
                _require(
                    not any(item.name == value for item in interface.libraries),
                    f"repeated library order unsupported: {value}",
                )
                interface.libraries.append(_Library(value, library_kind))
            elif token == "-pthread" or (compile_flags and token in _COMPILER_FLAGS):
                _append(interface.copts if compile_flags else interface.linkopts, token)
            else:
                raise BazelDependencyError(f"unsupported {'compile' if compile_flags else 'link'} flag: {token}")

    def patch(self, name: str, interface: _Interface, requirements: set[str]) -> _Interface:
        # Exact JSON comparisons are the public patch guard contract. Re-parse
        # the result into the typed interface before resolving any artifacts.
        values = interface.json()
        for patch in self.patches:
            wanted = {
                "package": name,
                "platform": self.platform,
                "config": self.spec["config"],
                "provider": self.spec["provider"]["name"],
                "provider_version": self.spec["provider"]["version"],
            }
            if patch["when"] != wanted:
                continue
            expect, additions = patch["expect"], patch["add"]
            removals = patch.get("remove", {})
            for fieldset, present in ((expect["missing"], False), (expect["present"], True)):
                for key, items in fieldset.items():
                    for item in items:
                        _require(
                            (item in values[key]) == present,
                            f"stale patch {patch['id']}: expected {'present' if present else 'missing'} {key}: {item}",
                        )
            unmet = set(_strings(expect["unmet_requirements"]))
            satisfy = set(_strings(patch["satisfy"]))
            _require(unmet <= requirements and unmet == satisfy, f"stale/unmet patch requirement: {patch['id']}")
            count = 0
            for key, items in removals.items():
                for item in items:
                    _require(
                        item in values[key] and item in expect["present"].get(key, []),
                        f"unguarded/stale patch removal: {patch['id']}",
                    )
                    _require(item not in additions.get(key, []), f"redundant remove/re-add patch: {patch['id']}")
                    values[key].remove(item)
                    count += 1
            for key, items in additions.items():
                for item in items:
                    _require(item not in values[key], f"redundant patch {patch['id']}: {key}: {item}")
                    _require(
                        unmet or item in expect["missing"].get(key, []), f"unguarded patch addition: {patch['id']}"
                    )
                    values[key].append(item)
                    count += 1
            _require(count, f"empty/redundant patch: {patch['id']}")
            requirements.difference_update(satisfy)
            self.applied.append(patch["id"])
        return _Interface.parse(values)

    def candidates(self, name: str, kind: str) -> list[Path]:
        if self.platform == "windows":
            patterns = [name + ".lib", "lib" + name + ".lib"]
        elif kind == "static":
            patterns = ["lib" + name + ".a"]
        elif self.platform == "macos":
            patterns = ["lib" + name + ".dylib"]
        else:
            patterns = ["lib" + name + ".so"]
        matches: set[Path] = set()
        for directory in self.libdirs:
            for pattern in patterns:
                path = directory / pattern
                self.probes.add(path)
                if path.exists() or path.is_symlink():
                    matches.add(self.artifact_path(str(path)))
            if self.platform == "linux" and kind == "shared" and not (directory / patterns[0]).exists():
                for path in sorted(directory.glob(patterns[0] + ".*")):
                    matches.add(self.artifact_path(str(path)))
        return sorted(matches)

    def library(self, item: _Library) -> _Library:
        name, kind = item.name, item.kind
        if kind == "system":
            _require(
                item.json() == {"name": name, "kind": kind} and name in self.system_libraries,
                f"unapproved system library: {name}",
            )
            _require(not self.candidates(name, self.linkage), f"system/provider library ambiguity: {name}")
            return item
        _require(kind == self.linkage, f"library linkage differs from exact spec configuration: {name}")
        if item.file is not None:
            file = self.artifact_path(item.file)
        else:
            candidates = self.candidates(name, kind)
            _require(
                len(candidates) == 1, f"library resolution missing/ambiguous: {name}: {list(map(str, candidates))}"
            )
            file = candidates[0]
        _require(kind == "shared" or item.runtime is None, "runtime only valid for shared imports")
        self.record(file, "library")
        runtime, soname = item.runtime, None
        rpath_link_dirs: list[str] = []
        if self.platform == "windows":
            _require(file.suffix.lower() == ".lib", f"Windows import must be .lib: {file}")
            if kind == "shared":
                _require(runtime is not None, f"Windows shared requires explicit DLL: {name}")
                dll = self.artifact_path(runtime, runtime=True)
                _require(dll.suffix.lower() == ".dll", f"Windows runtime must be DLL: {dll}")
                self.record(dll, "runtime")
                runtime = str(dll)
        else:
            _require(runtime is None, "runtime only valid for Windows shared imports")
            if kind == "static":
                _require(file.suffix == ".a", f"static library must be .a: {file}")
            elif self.platform == "macos":
                _require(file.suffix == ".dylib", f"macOS shared library must be .dylib: {file}")
            else:
                if self.readelf is None:
                    raise BazelDependencyError("elf.readelf required for Linux shared imports")
                output = self.run(self.readelf, "-d", str(file))
                sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", output)
                _require(
                    len(sonames) == 1 and re.fullmatch(r"[A-Za-z0-9_+.-]+\.so(?:\.[A-Za-z0-9_+.-]+)?", sonames[0]),
                    f"missing/invalid ELF SONAME: {file}",
                )
                soname = sonames[0]
                if self.spec["provider"]["name"] == "nix":
                    # GNU ld searches executable rpath before an input DSO's RUNPATH.
                    # Prefer the immutable provider's declared directories at link time;
                    # these hints are not a complete recursive ELF runtime closure.
                    runpaths = re.findall(r"\(RUNPATH\)([^\n]*)", output)
                    rpaths = re.findall(r"\(RPATH\)([^\n]*)", output)
                    _require(len(runpaths) <= 1 and len(rpaths) <= 1, f"duplicate ELF runtime search path: {file}")
                    store_directory = re.compile(
                        r"/nix/store/[0-9abcdfghijklmnpqrsvwxyz]{32}-[A-Za-z0-9+._?=-]+(?:/[A-Za-z0-9+._?=-]+)*"
                    )
                    # RUNPATH supersedes RPATH when both dynamic tags are present.
                    for entry in runpaths or rpaths:
                        match = re.fullmatch(r"[^\[\]]*\[([^\[\]]+)\]\s*", entry)
                        if match is None:
                            raise BazelDependencyError(f"invalid ELF runtime search path: {file}")
                        for value in match[1].split(":"):
                            _require(
                                store_directory.fullmatch(value) and not {".", ".."}.intersection(value.split("/")),
                                f"invalid Nix ELF runtime search directory: {value}: {file}",
                            )
                            directory = _resolve(Path(value))
                            canonical = directory.as_posix()
                            _require(
                                store_directory.fullmatch(canonical) and directory.is_dir(),
                                f"invalid Nix ELF runtime search directory: {value}: {file}",
                            )
                            _append(rpath_link_dirs, canonical)
        return _Library(name, kind, str(file), runtime, soname, tuple(rpath_link_dirs))

    def headers(self, root: Path, ancestors: tuple[Path, ...] = ()) -> Iterator[Path]:
        resolved = self.path(str(root), directory=True)
        _require(resolved not in ancestors, f"header directory symlink cycle: {root}")
        for path in sorted(root.iterdir()):
            if path.is_dir():
                yield from self.headers(path, (*ancestors, resolved))
            else:
                self.path(str(path))
                yield path

    def resolve(self) -> dict[str, Any]:
        """Validate installed interfaces and report facts, never generate Bazel rules."""
        manifests: list[dict[str, Any]] = []
        packages: list[dict[str, Any]] = []

        for name, interface, origin in self.packages:
            self.linkage = self.package_linkages[name]
            headers: list[dict[str, Any]] = []
            includes: list[dict[str, Any]] = []
            header_trees: dict[Path, int] = {}
            libraries: list[dict[str, Any]] = []
            runtime: list[str] = []
            resolved: list[dict[str, Any]] = []
            roots = [self.path(directory, directory=True) for directory in interface.header_dirs]
            for root in roots:
                # Choose only this package's declared parents. Sharing a different
                # package's parent would widen a child-only header interface.
                # Nested roots retain their tree for wrappers including ../c4.
                anchor = min((parent for parent in roots if root.is_relative_to(parent)), key=lambda p: len(p.parts))
                if anchor not in header_trees:
                    files = (
                        sorted(file for file in self.header_files[name] if file.is_relative_to(anchor))
                        if name in self.header_files
                        else sorted(self.headers(anchor))
                    )
                    _require(files, f"empty header directory: {anchor}")
                    inventory = []
                    for file in files:
                        source = self.path(str(file))
                        self.record(source, "header")
                        inventory.append({"source": str(source), "path": file.relative_to(anchor).as_posix()})
                    header_trees[anchor] = len(headers)
                    headers.append({"root": str(anchor), "index": roots.index(anchor), "files": inventory})
                tree = header_trees[anchor]
                _require(
                    any(Path(file["path"]).is_relative_to(root.relative_to(anchor)) for file in headers[tree]["files"]),
                    f"empty header directory: {root}",
                )
                includes.append({"tree": tree, "path": root.relative_to(anchor).as_posix()})
            for value in interface.defines:
                _require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:=.*)?", value), f"invalid define: {value}")
            for value in interface.copts:
                _require(value in _COMPILER_FLAGS, f"unsupported compile flag: {value}")
            remaining = set(interface.copts)
            if self.platform == "windows":
                remaining.discard("/permissive-")  # Required globally by the selected profile.
            if name == "pipewire" and self.platform == "linux":
                required = {"-fno-strict-aliasing", "-fno-strict-overflow"}
                _require(remaining == required, "unreviewed PipeWire compile requirements")
                remaining.clear()  # Applied by its compiling project consumers.
            if name == "gtkmm" and self.platform == "linux":
                required = {"-mfpmath=sse", "-msse", "-msse2"}
                _require(not remaining or remaining == required, "unreviewed GTKmm compile requirements")
                remaining.clear()  # Applied conditionally by the GTK compiling consumers, not this wrapper.
            _require(not remaining, f"unsatisfied compile requirements for {name}: {sorted(remaining)}")
            linkopts = interface.linkopts.copy()
            for value in linkopts:
                # Named Windows SDK imports are separate from provider archives;
                # admit only caller-declared system libraries, never paths.
                _require(
                    value == "-pthread"
                    or (self.platform == "windows" and value in {name + ".lib" for name in self.system_libraries}),
                    f"unsupported link flag: {value}",
                )
            names = [item.name for item in interface.libraries]
            _require(len(names) == len(set(names)), "repeated library order unsupported")
            # Bazel separates user link flags from imported archives. Do not
            # silently claim to preserve archive/system-library interleaving.
            kinds = {item.kind for item in interface.libraries}
            _require(not {"static", "system"} <= kinds, "static provider/system library link order unsupported")
            saw_system = False
            for index, item in enumerate(interface.libraries):
                _require(not saw_system or item.kind == "system", "system/provider library interleaving unsupported")
                saw_system = saw_system or item.kind == "system"
                library = self.library(item)
                for directory in library.rpath_link_dirs:
                    _append(linkopts, "-Wl,-rpath-link," + directory)
                resolved.append(library.json())
                if library.kind == "system":
                    linkopts.append(library.name + ".lib" if self.platform == "windows" else "-l" + library.name)
                    continue
                if library.file is None:
                    raise BazelDependencyError("resolved library has no file")
                libraries.append(
                    {
                        "index": index,
                        "kind": library.kind,
                        "source": library.file,
                        "basename": library.soname or Path(library.file).name,
                        "runtime": library.runtime,
                    }
                )
            for value in interface.runtime:
                file = self.artifact_path(value, runtime=True)
                self.record(file, "runtime")
                runtime.append(str(file))
            packages.append(
                {
                    "name": name,
                    "headers": headers,
                    "includes": includes,
                    "defines": interface.defines.copy(),
                    "linkopts": linkopts,
                    "libraries": libraries,
                    "runtime": runtime,
                }
            )
            manifests.append(
                {
                    "name": name,
                    "origin": origin,
                    "linkage": self.linkage,
                    "interface": interface.json(),
                    "resolved_libraries": resolved,
                    "declared_headers": sum(len(tree["files"]) for tree in headers),
                }
            )
        return {
            "packages": packages,
            "manifest": {
                "generator": GENERATOR,
                "configuration": self.spec,
                "materialization": "repository",
                "packages": manifests,
                "counts": {
                    "automatic": sum(p[2] == "automatic" for p in self.packages),
                    "manual_fallback": sum(p[2] == "manual_fallback" for p in self.packages),
                },
                "modules": self.modules,
                "applied_patches": self.applied,
                "inputs": [self.inputs[key] for key in sorted(self.inputs)],
            },
        }
