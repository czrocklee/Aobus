"""Fail-closed analysis of real native Bazel compile actions.

Linux additionally expands one representative driver and checks declared SPA
inputs. Verification is analysis, not compilation, linking or runtime acceptance.
"""

from __future__ import annotations

import json
import os
import posixpath
import re
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from functools import cache
from pathlib import Path
from typing import Any

from .paths import PROJECT_ROOT

UNIX_WARNINGS = {
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wunused",
    "-Wformat=2",
    "-Woverloaded-virtual",
    "-Wnon-virtual-dtor",
    "-Wcast-align",
    "-Werror",
    "-Wno-null-dereference",
    "-Wno-double-promotion",
    "-Wno-conversion",
    "-Wno-sign-conversion",
    "-Wno-old-style-cast",
    "-Wno-float-conversion",
    "-Wno-c2y-extensions",
}
# Preserve the pinned Linux Bazel toolchain diagnostics independently of CMake.
LINUX_EXTRA_WARNINGS = {
    "-Wthread-safety",
    "-Wself-assign",
    "-Wunused-but-set-parameter",
    "-Wno-free-nonheap-object",
}
SPA_FLAGS = ("-fno-strict-aliasing", "-fno-strict-overflow")
_CLEARED_CHANNELS = {
    "CPLUS_INCLUDE_PATH",
    "C_INCLUDE_PATH",
    "CPATH",
    "LIBRARY_PATH",
    "LD_LIBRARY_PATH",
    "NIX_CFLAGS_COMPILE",
    "NIX_CFLAGS_COMPILE_FOR_TARGET",
    "NIX_LDFLAGS",
    "NIX_LDFLAGS_FOR_TARGET",
}


@dataclass(frozen=True)
class CompileAction:
    label: str
    argv: tuple[str, ...]
    inputs: frozenset[str] = frozenset()
    quote_includes: frozenset[str] = frozenset()
    environment: tuple[tuple[str, str], ...] = ()
    is_tool: bool = False


def _unique_pairs[T](pairs: Iterable[tuple[str, T]]) -> dict[str, T]:
    result: dict[str, T] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key: {key}")
        result[key] = value
    return result


def extract_compile_actions(document: Any, *, require_compile_context: bool = False) -> list[CompileAction]:
    """Resolve aquery IDs and declared argv/context without execution-tree reads.

    Artifact-free jsonproto omits input tables and inputDepSetIds altogether.
    Windows requires the analysis-only aspect's inline FileWrite compile contexts,
    joined by target AND configuration: quote roots, not exemptions inferred from
    /I flags, and the expanded command line of each compile action whose aquery
    entry names only an MSVC param file, joined by its outputs as well.
    Dangling IDs, duplicate contexts and unexpanded params fail closed.
    """
    if not isinstance(document, dict):
        raise ValueError("aquery jsonproto must be an object")
    targets = _unique_pairs((str(item["id"]), item["label"]) for item in document.get("targets", []))
    configurations = _unique_pairs((str(item["id"]), item) for item in document.get("configuration", []))
    fragments = _unique_pairs((str(item["id"]), item) for item in document.get("pathFragments", []))
    artifacts = _unique_pairs((str(item["id"]), item) for item in document.get("artifacts", []))
    depsets = _unique_pairs((str(item["id"]), item) for item in document.get("depSetOfFiles", []))
    active_fragments: set[str] = set()
    active_depsets: set[str] = set()

    @cache
    def fragment_path(identifier: str | int) -> str:
        identifier = str(identifier)
        if identifier in active_fragments:
            raise ValueError(f"recursive path fragment: {identifier}")
        active_fragments.add(identifier)
        try:
            item = fragments[identifier]
            parent = item.get("parentId")
            label: str = item["label"]
            return (fragment_path(parent) + "/" if parent else "") + label
        finally:
            active_fragments.remove(identifier)

    @cache
    def input_paths(identifier: str | int) -> frozenset[str]:
        identifier = str(identifier)
        if identifier in active_depsets:
            raise ValueError(f"recursive input depset: {identifier}")
        active_depsets.add(identifier)
        try:
            item = depsets[identifier]
            direct = frozenset(
                fragment_path(artifacts[str(artifact)]["pathFragmentId"])
                for artifact in item.get("directArtifactIds", [])
            )
            return direct.union(*(input_paths(child) for child in item.get("transitiveDepSetIds", [])))
        finally:
            active_depsets.remove(identifier)

    def expand(
        arguments: Iterable[str], params: Mapping[str, Iterable[str]], active: tuple[str, ...] = ()
    ) -> Iterator[str]:
        for argument in arguments:
            if argument.startswith("@"):
                path = argument[1:]
                if path not in params or path in active:
                    raise ValueError(f"unexpanded or recursive response file: {argument}")
                yield from expand(params[path], params, (*active, path))
            else:
                yield argument

    def strings(value: object) -> bool:
        return isinstance(value, list) and all(isinstance(item, str) for item in value)

    def outputs_of(action: dict[str, Any]) -> tuple[str, ...]:
        return tuple(
            sorted(
                fragment_path(artifacts[str(identifier)]["pathFragmentId"])
                for identifier in action.get("outputIds", [])
            )
        )

    quote_contexts: dict[tuple[str, str], frozenset[str]] = {}
    expanded_argv: dict[tuple[str, str, tuple[str, ...]], tuple[str, ...]] = {}
    if require_compile_context:
        for action in document.get("actions", []):
            if action.get("mnemonic") != "FileWrite":
                continue
            outputs = outputs_of(action)
            if not any(path.endswith(".aobus-compile-context.json") for path in outputs):
                continue
            if len(outputs) != 1:
                raise ValueError("compile context must have exactly one output")
            key = (str(action["targetId"]), str(action["configurationId"]))
            targets[key[0]]
            configurations[key[1]]
            context = json.loads(action["fileContents"])
            if not isinstance(context, dict) or set(context) != {"quote_includes", "compile_argv"}:
                raise ValueError("compile context must hold quote_includes and compile_argv")
            roots = context["quote_includes"]
            if not strings(roots) or any(any(ord(char) < 32 or ord(char) == 127 for char in root) for root in roots):
                raise ValueError("quote context must be a list of include paths")
            if key in quote_contexts:
                raise ValueError("duplicate configured quote context: " + str(key))
            quote_contexts[key] = frozenset(roots)
            if not isinstance(context["compile_argv"], list):
                raise ValueError("compile_argv must be a list")
            for compile in context["compile_argv"]:
                if not isinstance(compile, dict) or set(compile) != {"outputs", "argv"}:
                    raise ValueError("compile_argv entries must hold outputs and argv")
                if not strings(compile["outputs"]) or not compile["outputs"] or not strings(compile["argv"]):
                    raise ValueError("compile_argv outputs and argv must be nonempty string lists")
                identity = (*key, tuple(sorted(compile["outputs"])))
                if identity in expanded_argv:
                    raise ValueError("duplicate expanded compile command line: " + str(identity))
                expanded_argv[identity] = tuple(compile["argv"])

    result: list[CompileAction] = []
    for action in document.get("actions", []):
        if action.get("mnemonic") != "CppCompile":
            continue
        params = _unique_pairs((item["execPath"], item["arguments"]) for item in action.get("paramFiles", []))
        arguments = action.get("arguments", [])
        configuration_id = action.get("configurationId")
        captured = (
            expanded_argv.get((str(action["targetId"]), str(configuration_id), outputs_of(action)))
            if expanded_argv and not params and any(argument.startswith("@") for argument in arguments)
            else None
        )
        if captured is not None:
            if captured[:1] != tuple(arguments[:1]):
                raise ValueError("expanded compile command line names another compiler: " + str(arguments[:1]))
            argv = captured
        else:
            argv = tuple(expand(arguments, params))
        if not argv:
            raise ValueError("CppCompile action has no command line")
        inputs = frozenset[str]().union(*(input_paths(item) for item in action.get("inputDepSetIds", [])))
        environment = _unique_pairs(
            (item["key"], item.get("value", "")) for item in action.get("environmentVariables", [])
        )
        if any(not isinstance(key, str) or not isinstance(value, str) for key, value in environment.items()):
            raise ValueError("action environment keys and values must be strings")
        if configuration_id is None and configurations:
            raise ValueError("CppCompile action has no configuration ID")
        configuration = configurations[str(configuration_id)] if configuration_id is not None else {}
        is_tool = configuration.get("isTool", False)
        if not isinstance(is_tool, bool):
            raise ValueError("configuration isTool must be a boolean")
        compiled = CompileAction(
            targets[str(action["targetId"])],
            argv,
            inputs,
            quote_includes=quote_contexts.get((str(action["targetId"]), str(configuration_id)), frozenset()),
            environment=tuple(environment.items()),
            is_tool=is_tool,
        )
        if require_compile_context and _first_party(compiled):
            if configuration_id is None or (str(action["targetId"]), str(configuration_id)) not in quote_contexts:
                raise ValueError("missing configured quote context: " + compiled.label)
        result.append(compiled)
    if not result:
        raise ValueError("no CppCompile actions matched")
    return result


def _last(arguments: Sequence[str], predicate: Callable[[str], bool], default: str | None = None) -> str | None:
    return next((item for item in reversed(arguments) if predicate(item)), default)


def _defines(arguments: Iterable[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    iterator = iter(arguments)
    for argument in iterator:
        if argument[:2] not in {"-D", "/D", "-U", "/U"}:
            continue
        value = argument[2:] or next(iterator, None)
        if value is None:
            raise ValueError("missing macro argument: " + argument)
        name, _, setting = value.partition("=")
        if argument[1] == "U":
            result.pop(name, None)
        else:
            result[name] = setting or "1"
    return result


def include_paths(arguments: Iterable[str]) -> Iterator[tuple[str, str]]:
    """Return search-path kind/path pairs without shell splitting Windows paths."""
    prefixes = (
        ("/external:I", "system"),
        ("-external:I", "system"),
        ("-isystem", "system"),
        ("-idirafter", "system"),
        ("-iquote", "quote"),
        ("/imsvc", "system"),
        ("-I", "ordinary"),
        ("/I", "ordinary"),
    )
    iterator = iter(arguments)
    for argument in iterator:
        for prefix, kind in prefixes:
            if argument.startswith(prefix):
                value = argument[len(prefix) :] if argument != prefix else next(iterator, None)
                if value is None:
                    raise ValueError("missing include path: " + argument)
                yield kind, posixpath.normpath(value.replace("\\", "/"))
                break


def _first_party(action: CompileAction) -> bool:
    return action.label.startswith(("//", "@//", "@@//"))


def _consumes_spa(action: CompileAction) -> bool:
    return any(re.search(r"(?:^|/)(?:spa|spa-0\.2)/.*\.h$", path) for path in action.inputs)


def compile_contract_errors(
    actions: Iterable[CompileAction],
    platform: str,
    mode: str,
    *,
    compiler_include_roots: Iterable[str] = (),
    require_spa: bool = False,
    expected_environment: Mapping[str, str] | None = None,
    expected_tool_environment: Mapping[str, str] | None = None,
) -> list[str]:
    """Return compile-policy diagnostics, including zero first-party/SPA matches.

    platform is linux/macos/windows; mode is dbg/opt/fastbuild. Explicit Linux
    allowlist roots must name compiler/stdlib/sysroot include directories, never
    the whole Nix store. SPA checks conservatively inspect every declared header
    input, not textual include usage. Windows callers supply declared quote
    includes from the actual compile context, including private dependencies;
    MSVC represents these with /I, not a quote-only search switch. No roots are
    inferred from path prefixes. Callers own platform/tool/source receipts.
    When expected_environment is supplied, target channels must be explicitly
    empty; strict exec actions may omit them. Neither may carry ambient flags.
    Aquery omits inherited-only settings: callers still own strict action/launch
    configuration, not just this check of captured fixed environment values.
    Tool configurations use Bazel's opt execution transition and, on Linux, a
    separately supplied Nix hardening policy, never the target Debug policy.
    Language defaults cover the current C++ graph, not C/ObjC. Windows checks
    interpret known switches and explicit macro definitions, not preprocessor
    output or arbitrary command-line rewrites.
    """
    if platform not in {"linux", "macos", "windows"} or mode not in {"dbg", "opt", "fastbuild"}:
        raise ValueError("unsupported compile-contract platform or mode")
    roots = tuple(posixpath.normpath(root.replace("\\", "/")) for root in compiler_include_roots)
    if any(root in {"/", "/nix", "/nix/store"} for root in roots):
        raise ValueError("compiler include allowlist must not admit application-dependency roots")
    first_party = [action for action in actions if _first_party(action)]
    errors = [] if first_party else ["no first-party CppCompile actions matched"]
    spa_count = 0
    for action in first_party:
        argv = action.argv
        problems = []
        # Bazel's execution transition builds native tools in opt, independently
        # of the requested target mode. Never infer that role from source paths.
        effective_mode = "opt" if action.is_tool else mode
        policy = expected_environment
        if action.is_tool and platform == "linux" and expected_environment is not None:
            if expected_tool_environment is None:
                problems.append("execution configuration requires an explicit Nix hardening policy")
            else:
                policy = expected_tool_environment
        if policy is not None:
            environment = _unique_pairs(action.environment)
            for key, value in policy.items():
                if environment.get(key) != value:
                    problems.append("action environment must set " + key + "=" + value)
            for key in sorted(_CLEARED_CHANNELS):
                # The strict exec transition has no default host_action_env.
                # An absent variable is not a leak or a missing empty marker.
                ambient_value = environment.get(key, "") if action.is_tool else environment.get(key)
                if ambient_value != "":
                    requirement = "absent or empty" if action.is_tool else "explicitly cleared"
                    problems.append("ambient include/link channel must be " + requirement + ": " + key)
            for key, value in environment.items():
                if value and key.startswith(("NIX_CFLAGS_", "NIX_LDFLAGS")) and key not in _CLEARED_CHANNELS:
                    problems.append("ambient include/link channel must be cleared: " + key)
        defines = _defines(argv)
        windows = platform == "windows"
        if windows:
            standard = _last(argv, lambda flag: bool(re.fullmatch(r"[/-]std:.*", flag)))
            if standard not in {"/std:c++latest", "-std:c++latest"}:
                problems.append("C++ language standard requires effective /std:c++latest")
            for switch in ("permissive", "Zc:__cplusplus", "Zc:preprocessor"):
                required = switch + "-" if switch == "permissive" else switch

                def matches_switch(flag: str, switch: str = switch) -> bool:
                    return bool(re.fullmatch(r"[/-]" + switch + r"-?", flag))

                effective = _last(argv, matches_switch)
                if effective not in {"/" + required, "-" + required}:
                    problems.append("Windows C++ defaults require effective /" + required)
            # /utf-8 sets both charsets; later explicit charset options can override
            # either one. This checks known argv switches, not preprocessor output.
            utf8_present = False
            charsets = {}
            for flag in argv:
                if flag in {"/utf-8", "-utf-8"}:
                    utf8_present = True
                    charsets = {"source": "utf-8", "execution": "utf-8"}
                else:
                    match = re.fullmatch(r"[/-](source|execution)-charset:(.*)", flag)
                    if match:
                        charsets[match[1]] = match[2]
            if not utf8_present or any(
                charsets.get(kind) not in {"utf-8", ".65001"} for kind in ("source", "execution")
            ):
                problems.append("Windows C++ defaults require /utf-8 without non-UTF-8 charset overrides")
            for name in ("NOMINMAX", "WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE", "_CRT_SECURE_NO_WARNINGS"):
                if defines.get(name) != "1":
                    problems.append("Windows C++ defaults require effective " + name + "=1")
            for name in ("WINVER", "_WIN32_WINNT"):
                try:
                    version = int(defines.get(name, ""), 0)
                except ValueError:
                    version = None
                if version != 0x0A00:
                    problems.append("Windows SDK defaults require effective " + name + "=0x0A00")
            optimization = _last(argv, lambda flag: bool(re.fullmatch(r"[/-]O[d12x]", flag)))
            debug = _last(argv, lambda flag: bool(re.fullmatch(r"[/-]Z[7iI]-?", flag)))
            debug_enabled = debug is not None and not debug.endswith("-")
            optimized = optimization in {"/O2", "-O2", "/Ox", "-Ox"}
            unoptimized = optimization in {None, "/Od", "-Od"}
        else:
            standard = _last(argv, lambda flag: bool(re.fullmatch(r"--?std=.*", flag)))
            if standard not in {"-std=c++26", "--std=c++26"}:
                problems.append("C++ language standard requires effective -std=c++26")
            optimization = _last(argv, lambda flag: bool(re.fullmatch(r"-O(?:[0-3sgz]|fast)", flag)))
            debug = _last(argv, lambda flag: bool(re.fullmatch(r"-g(?:[0-3]|gdb[0-3]?|line-tables-only)?", flag)))
            debug_enabled = debug is not None and debug not in {"-g0", "-ggdb0"}
            optimized = optimization in {"-O2", "-O3"}
            unoptimized = optimization in {None, "-O0"}
        if effective_mode == "opt" and (not optimized or "NDEBUG" not in defines):
            problems.append("opt requires effective optimization and NDEBUG")
        if effective_mode == "dbg" and (not debug_enabled or not unoptimized or "NDEBUG" in defines):
            problems.append("dbg requires debug information, no optimization and no NDEBUG")
        if effective_mode == "fastbuild" and (not unoptimized or debug_enabled or "NDEBUG" in defines):
            problems.append("fastbuild requires no optimization, debug information or NDEBUG")
        if defines.get("gsl_CONFIG_CONTRACT_CHECKING_ON") != "1" or "gsl_CONFIG_CONTRACT_CHECKING_OFF" in defines:
            problems.append("GSL contract checking must remain on")
        if effective_mode in {"dbg", "opt"}:
            expected = "ASSERTS" if effective_mode == "dbg" else "TERMINATES"
            opposite = "TERMINATES" if effective_mode == "dbg" else "ASSERTS"
            if (
                defines.get("gsl_CONFIG_CONTRACT_VIOLATION_" + expected) != "1"
                or "gsl_CONFIG_CONTRACT_VIOLATION_" + opposite in defines
            ):
                problems.append("GSL contract violation mode must be " + expected)
        if windows:
            if _last(argv, lambda flag: bool(re.fullmatch(r"[/-]W[0-4]", flag))) not in {"/W4", "-W4"}:
                problems.append("first-party warnings require effective /W4")
            if _last(argv, lambda flag: bool(re.fullmatch(r"[/-]WX-?", flag))) not in {"/WX", "-WX"}:
                problems.append("first-party warnings require effective /WX")
            # MSVC ignores /external:I without /external:W; switch case matters.
            if any(flag.startswith(("/external:I", "-external:I")) for flag in argv) and _last(
                argv, lambda flag: bool(re.fullmatch(r"[/-]external:W[0-4]", flag))
            ) not in {"/external:W0", "-external:W0"}:
                problems.append("MSVC external include paths require effective /external:W0")
        else:
            warnings = {}
            for flag in argv:
                if flag.startswith("-W"):
                    warnings[flag.removeprefix("-W").removeprefix("no-")] = not flag.startswith("-Wno-")
            required_warnings = UNIX_WARNINGS | LINUX_EXTRA_WARNINGS if platform == "linux" else UNIX_WARNINGS
            for required in sorted(required_warnings):
                key = required.removeprefix("-W").removeprefix("no-")
                if warnings.get(key) != (not required.startswith("-Wno-")):
                    problems.append("first-party warning policy requires " + required)
        if any(flag in {"-w", "/w", "-Wno-everything"} for flag in argv):
            problems.append("first-party warnings must not be suppressed wholesale")
        quote_includes = {posixpath.normpath(path.replace("\\", "/")) for path in action.quote_includes}
        for kind, path in include_paths(argv):
            declared_quote = windows and kind == "ordinary" and path in quote_includes
            if kind != "quote" and path == "." and not declared_quote:
                problems.append("empty/repository-root include search path")
            if path.startswith("/nix/store/") and not any(
                path == root or path.startswith(root + "/") for root in roots
            ):
                problems.append("ambient Nix dependency include: " + path)
            if (path.startswith("external/") or "/external/" in path) and kind == "ordinary" and not declared_quote:
                problems.append("imported include must be system/external: " + path)
        if _consumes_spa(action):
            spa_count += 1
            for required in SPA_FLAGS:
                opposite = required.replace("-fno-", "-f")

                def matches_spa_flag(flag: str, pair: tuple[str, str] = (required, opposite)) -> bool:
                    return flag in pair

                if _last(argv, matches_spa_flag) != required:
                    problems.append("declared SPA consumer requires effective " + required)
        source = next((arg for arg in argv if arg.endswith((".cpp", ".cc", ".cxx"))), "<unknown source>")
        errors.extend(f"{action.label} ({source}): {problem}" for problem in problems)
    if require_spa and not spa_count:
        errors.append("no declared SPA-consuming first-party CppCompile actions matched")
    return errors


def _driver_file(value: str) -> Path:
    path = Path(value)
    if (
        any(ord(char) < 32 or ord(char) == 127 for char in value)
        or not path.is_absolute()
        or not path.is_file()
        or not os.access(path, os.X_OK)
    ):
        raise ValueError("representative compiler must be an absolute existing executable file: " + value)
    # Do not resolve symlinks: Nix/Clang multicall wrappers depend on argv[0].
    return path


def _compiler_source_label(value: str) -> str:
    match = re.fullmatch(r"external/([A-Za-z0-9._+~-]+)/([A-Za-z0-9._+~/-]+)", value)
    if match is None or any(part in {"", ".", ".."} for part in value.split("/")):
        raise ValueError("unsupported representative compiler source path: " + value)
    return f"@@{match[1]}//:{match[2]}"


def _verify_driver(
    action: CompileAction,
    mode: str,
    expected_environment: Mapping[str, str],
    evidence: dict[str, Any],
    *,
    compiler: Path | None = None,
) -> int:
    """Expand captured argv with only the compiler path resolved, never execute cc1.

    This wrapper diagnostic runs from PROJECT_ROOT for unstaged first-party
    sources, not as a literal Bazel action in an execution root.
    """
    policy_keys = list(expected_environment)
    if len(policy_keys) != 1 or not re.fullmatch(r"NIX_HARDENING_ENABLE_[A-Za-z0-9_]+", policy_keys[0]):
        raise ValueError("representative driver requires one explicit salted Nix hardening policy")
    policy = expected_environment[policy_keys[0]].split()
    evidence["hardening_policy"] = policy
    evidence["captured_argv"] = list(action.argv)
    evidence["argv"] = [
        str(_driver_file(str(compiler) if compiler is not None else action.argv[0])),
        *action.argv[1:],
        "-###",
    ]
    evidence["cwd"] = str(PROJECT_ROOT)
    evidence["status"] = "running"
    with Path(evidence["stdout"]).open("wb") as out, Path(evidence["stderr"]).open("wb") as err:
        result = subprocess.run(
            evidence["argv"], cwd=PROJECT_ROOT, env=dict(action.environment), stdout=out, stderr=err
        )
    evidence["exit"] = result.returncode
    evidence["status"] = "exited"
    if result.returncode:
        evidence["status"] = "driver failed"
        return result.returncode
    text = Path(evidence["stderr"]).read_text(encoding="utf-8", errors="replace")
    commands = [
        shlex.split(line)
        for line in text.splitlines()
        if re.search(r"""(?:^|\s)(?:"-cc1"|'-cc1'|-cc1)(?:\s|$)""", line)
    ]
    if len(commands) != 1 or len(commands[0]) < 2 or commands[0][1] != "-cc1" or commands[0].count("-cc1") != 1:
        raise ValueError("representative Clang driver must emit exactly one -cc1 command")
    cc1 = commands[0]
    evidence["cc1_argv"] = cc1
    optimization = _last(cc1, lambda flag: bool(re.fullmatch(r"-O(?:[0-3sgz]|fast)", flag)))
    protector = None
    arguments = iter(cc1)
    for argument in arguments:
        if argument == "-stack-protector":
            value = next(arguments, "")
            if not value.isdecimal():
                raise ValueError("malformed Clang -stack-protector value")
            protector = int(value)
    defines = _defines(cc1)
    fortify = defines.get("_FORTIFY_SOURCE")
    evidence["effective"] = {"optimization": optimization, "stack_protector": protector, "fortify_source": fortify}
    errors = []
    if mode == "dbg":
        if optimization not in {None, "-O0"}:
            errors.append("representative debug driver must remain unoptimized")
        if fortify not in {None, "0"}:
            errors.append("representative debug driver must not enable _FORTIFY_SOURCE")
        if "NDEBUG" in defines:
            errors.append("representative debug driver must not define NDEBUG")
    elif mode == "opt":
        if optimization not in {"-O2", "-O3"}:
            errors.append("representative optimized driver requires -O2 or -O3")
        if {"fortify", "fortify3"} & set(policy) and fortify not in {"2", "3"}:
            errors.append("Nix fortify policy requires effective _FORTIFY_SOURCE=2 or 3")
    else:
        raise ValueError("representative driver requires dbg or opt mode")
    if "stackprotector" in policy and (protector is None or protector < 2):
        errors.append("Nix stackprotector policy requires effective strong/all protection")
    if defines.get("gsl_CONFIG_CONTRACT_CHECKING_ON") != "1" or "gsl_CONFIG_CONTRACT_CHECKING_OFF" in defines:
        errors.append("representative driver must retain GSL contract checking")
    evidence["errors"] = errors
    evidence["status"] = "contract failed" if errors else "passed"
    return 1 if errors else 0


def _expected_hardening_environment(config: str) -> dict[str, str]:
    """Independent verification oracle, never an input to toolchain construction."""
    from . import bazel_compiler_probe

    if config not in {"debug", "release"}:
        raise ValueError(f"unsupported hardening verification configuration: {config}")
    observed = bazel_compiler_probe.nix_cc_wrapper()
    # Check the established unoptimized/optimized contract independently of the
    # Starlark generation helper, so a wrong generated environment can fail.
    flags = [flag for flag in observed["hardeningEnable"] if config == "release" or flag not in {"fortify", "fortify3"}]
    return {"NIX_HARDENING_ENABLE_" + observed["suffixSalt"]: " ".join(flags)}


def verify(executable: Path, tree: Path, prepared: Path, config: str, jobs: int = 2) -> int:
    """Expand default test suites, then analyze their leaves in a caller-locked tree.

    Retain each invocation's stdout/stderr and actual exits in a unique directory.
    Linux also resolves one captured external compiler and expands its driver
    with -###, then checks declared SPA inputs. Other hosts check native captured
    actions without pretending to run that Nix-specific diagnostic. No stage runs
    after the first failure. This is analysis evidence, not a product build/test.
    """
    from . import bazel_native as native
    from . import bazel_workspace as workspace

    host = native.native_platform()
    if host not in {"linux", "macos", "windows"}:
        print("Bazel compile-contract verify requires native Linux, macOS or Windows", file=sys.stderr)
        return 1
    if config not in {"debug", "release"} or jobs < 1:
        print("Bazel compile-contract verify requires debug/release and positive jobs", file=sys.stderr)
        return 1
    attempts = tree / "contracts" / config
    attempts.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix="verify-", dir=attempts))
    receipt_path = artifacts / "receipt.json"
    queries: list[dict[str, Any]] = [
        {
            "name": name,
            "argv": None,
            "stdout": str(artifacts / f"{name}.json"),
            "stderr": str(artifacts / f"{name}.stderr.log"),
            "status": "unrun",
            "exit": None,
        }
        for name in (("flags", "spa") if host == "linux" else ("flags",))
    ]
    driver: dict[str, Any] = {
        "status": "unrun" if host == "linux" else "not applicable",
        "reason": "Linux Nix wrapper diagnostic",
        "exit": None,
        "stdout": str(artifacts / "driver.stdout.log"),
        "stderr": str(artifacts / "driver.stderr.log"),
        "scope": (
            "one representative first-party wrapper expansion with only compiler path resolution, "
            "not literal action execution or whole-graph compilation/linking"
        ),
    }
    source_location: dict[str, Any] = {
        "argv": None,
        "cwd": str(PROJECT_ROOT),
        "status": "unrun" if host == "linux" else "not applicable",
        "reason": "Linux Nix wrapper diagnostic",
        "exit": None,
        "stdout": str(artifacts / "source-location.stdout.log"),
        "stderr": str(artifacts / "source-location.stderr.log"),
    }
    selection: dict[str, Any] = {
        "argv": None,
        "cwd": str(PROJECT_ROOT),
        "roots": list(workspace.DEFAULT_TEST_TARGETS),
        "status": "unrun",
        "exit": None,
        "stdout": str(artifacts / "selection.stdout.log"),
        "stderr": str(artifacts / "selection.stderr.log"),
    }
    receipt: dict[str, Any] = {
        "config": config,
        "mode": "dbg" if config == "debug" else "opt",
        "platform": host,
        "executable": str(executable),
        "prepared": str(prepared),
        "source": str(PROJECT_ROOT),
        "python": sys.version,
        "jobs": jobs,
        "evidence": (
            "query-expanded default tests and native aquery analysis; Linux additionally checks "
            "one representative driver expansion and declared SPA inputs; "
            "no product compilation, linking or runtime certification"
        ),
        "selection": selection,
        "queries": queries,
        "driver": driver,
        "source_location": source_location,
        "status": "incomplete",
        "exit": None,
    }

    def save() -> None:
        receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def query_label(value: str) -> str:
        match = re.fullmatch(r"(?:@@?[A-Za-z0-9._+~-]+)?//([A-Za-z0-9._+~/-]*):([A-Za-z0-9._+~/-]+)", value)
        if match is None or any(
            part in {"", ".", ".."} for path in (match[1], match[2]) if path for part in path.split("/")
        ):
            raise ValueError("invalid default-test target label: " + value)
        return json.dumps(value)

    save()
    try:
        native.require_supported_source()
        # An empty mapping still checks the common cleared action channels.
        # Only Linux has an additional salted Nix hardening policy.
        expected_environment = {}
        expected_tool_environment = None
        if host == "linux":
            expected_environment = _expected_hardening_environment(config)
            if not expected_environment:
                raise ValueError("empty Nix hardening environment policy")
            expected_tool_environment = _expected_hardening_environment("release")
            if not expected_tool_environment:
                raise ValueError("empty execution-configuration Nix hardening environment policy")
            receipt["expected_tool_environment"] = expected_tool_environment
        receipt["expected_environment"] = expected_environment
        # test_suite nodes disappear from the configured graph. Expand the owned
        # default aggregate with query before requesting any compile actions.
        selection.update(
            argv=[
                str(executable),
                f"--bazelrc={prepared / 'aobus.bazelrc'}",
                "query",
                "tests(set(" + " ".join(query_label(label) for label in workspace.DEFAULT_TEST_TARGETS) + "))",
                "--output=label",
            ],
            status="running",
        )
        save()
        print(f"Bazel default test selection ({config}); receipt: {receipt_path}", file=sys.stderr, flush=True)
        with Path(selection["stdout"]).open("wb") as out, Path(selection["stderr"]).open("wb") as err:
            result = subprocess.run(
                selection["argv"], cwd=PROJECT_ROOT, env=native.child_environment(), stdout=out, stderr=err
            )
        selection["exit"] = result.returncode
        selection["status"] = "exited"
        save()
        if result.returncode:
            receipt["status"] = "selection failed"
            receipt["exit"] = result.returncode
            print(f"Bazel test selection exited {result.returncode}; stderr: {selection['stderr']}", file=sys.stderr)
            return result.returncode
        targets = Path(selection["stdout"]).read_text(encoding="utf-8").splitlines()
        if not targets or len(targets) != len(set(targets)):
            raise ValueError("Bazel default test query must return a nonempty set of unique target labels")
        targets = sorted(targets)
        for label in targets:
            query_label(label)
        selection.update(targets=targets, status="passed")
        scopes = [(queries[0], targets, False)]
        if host == "linux":
            scopes.append((queries[1], ["//test:audio_test", "//test:audio_platform_test"], True))
        for query, scope_targets, spa in scopes:
            scope = "deps(set(" + " ".join(query_label(label) for label in scope_targets) + "))"
            expression = 'mnemonic("CppCompile",' + scope + ")"
            if host == "windows":
                expression += ' union outputs(".*[.]aobus-compile-context[.]json$",' + scope + ")"
            argv = [
                str(executable),
                f"--bazelrc={prepared / 'aobus.bazelrc'}",
                "aquery",
                f"--jobs={jobs}",
                expression,
                # query expands suites without configuration. Preserve Bazel's
                # native compatibility filtering when analyzing those leaves.
                "--skip_incompatible_explicit_targets",
                "--output=jsonproto",
                "--include_artifacts=" + ("true" if spa or host == "windows" else "false"),
                "--include_commandline=true",
                "--include_param_files=true",
            ]
            if spa:
                argv.append("--include_pruned_inputs=true")
            if host == "windows":
                argv.extend(
                    [
                        "--aspects=//bazel:compile_context.bzl%compile_context_aspect",
                        "--include_aspects=true",
                        "--include_file_write_contents=true",
                    ]
                )
            query["argv"] = argv
        save()
        for query in queries:
            query["status"] = "running"
            save()
            print(
                f"Bazel compile-contract {query['name']} ({config}); receipt: {receipt_path}",
                file=sys.stderr,
                flush=True,
            )
            with Path(query["stdout"]).open("wb") as out, Path(query["stderr"]).open("wb") as err:
                result = subprocess.run(
                    query["argv"], cwd=PROJECT_ROOT, env=native.child_environment(), stdout=out, stderr=err
                )
            query["exit"] = result.returncode
            query["status"] = "exited"
            save()
            if result.returncode:
                receipt["status"] = "analysis failed"
                receipt["exit"] = result.returncode
                print(f"Bazel analysis exited {result.returncode}; stderr: {query['stderr']}", file=sys.stderr)
                return result.returncode
            with Path(query["stdout"]).open(encoding="utf-8") as source:
                actions = extract_compile_actions(
                    json.load(source, object_pairs_hook=_unique_pairs), require_compile_context=host == "windows"
                )
            first_party = [action for action in actions if _first_party(action)]
            query["counts"] = {
                "cpp_compile": len(actions),
                "first_party": len(first_party),
                "spa": sum(_consumes_spa(action) for action in first_party),
            }
            query["execution_cpp_compile"] = sum(action.is_tool for action in first_party)
            errors = compile_contract_errors(
                actions,
                host,
                receipt["mode"],
                require_spa=query["name"] == "spa",
                expected_environment=expected_environment,
                expected_tool_environment=expected_tool_environment,
            )
            query["errors"] = errors
            query["status"] = "contract failed" if errors else "passed"
            save()
            if errors:
                receipt["status"] = "contract failed"
                receipt["exit"] = 1
                print("\n".join(errors), file=sys.stderr)
                return 1
            if query["name"] == "flags" and host == "linux":
                # The contract above checks every action, including generated
                # sources and exec tools. This diagnostic separately needs one
                # target-mode source that can be expanded from the checkout.
                for action in first_party:
                    compile_source = next(
                        (arg for arg in action.argv[1:] if arg.endswith((".cpp", ".cc", ".cxx"))), None
                    )
                    if compile_source is None:
                        raise ValueError("CppCompile action has no supported C++ source")
                    if compile_source.startswith(("bazel-out/", "external/")):
                        continue
                    if (
                        Path(compile_source).is_absolute()
                        or ".." in Path(compile_source).parts
                        or not (PROJECT_ROOT / compile_source).is_file()
                    ):
                        raise ValueError("compile action requires an existing first-party source: " + compile_source)
                representative = next(
                    (
                        action
                        for action in first_party
                        if not action.is_tool
                        and any(
                            arg.endswith((".cpp", ".cc", ".cxx"))
                            and not Path(arg).is_absolute()
                            and not arg.startswith(("bazel-out/", "external/"))
                            and ".." not in Path(arg).parts
                            and (PROJECT_ROOT / arg).is_file()
                            for arg in action.argv[1:]
                        )
                    ),
                    None,
                )
                if representative is None:
                    raise ValueError("representative driver requires a target-configuration checkout source")
                driver.update(
                    label=representative.label,
                    captured_argv=list(representative.argv),
                    environment=dict(representative.environment),
                    cwd=str(PROJECT_ROOT),
                    source_query=query["stdout"],
                )
                source_path = next(arg for arg in representative.argv[1:] if arg.endswith((".cpp", ".cc", ".cxx")))
                if (
                    Path(source_path).is_absolute()
                    or source_path.startswith(("bazel-out/", "external/"))
                    or ".." in Path(source_path).parts
                    or not (PROJECT_ROOT / source_path).is_file()
                ):
                    raise ValueError("representative driver requires an existing first-party source: " + source_path)
                captured_compiler = representative.argv[0]
                source_location["captured_compiler"] = captured_compiler
                if Path(captured_compiler).is_absolute():
                    compiler = _driver_file(captured_compiler)
                    source_location["status"] = "not required"
                else:
                    label = _compiler_source_label(captured_compiler)
                    source_location.update(
                        target=label,
                        # Literal query quotes are required for names containing ++.
                        argv=[
                            str(executable),
                            f"--bazelrc={prepared / 'aobus.bazelrc'}",
                            "query",
                            f"'{label}'",
                            "--output=location",
                        ],
                        status="running",
                    )
                    save()
                    print(
                        f"Bazel compiler source location ({config}); receipt: {receipt_path}",
                        file=sys.stderr,
                        flush=True,
                    )
                    with (
                        Path(source_location["stdout"]).open("wb") as out,
                        Path(source_location["stderr"]).open("wb") as err,
                    ):
                        result = subprocess.run(
                            source_location["argv"],
                            cwd=PROJECT_ROOT,
                            env=native.child_environment(),
                            stdout=out,
                            stderr=err,
                        )
                    source_location["exit"] = result.returncode
                    source_location["status"] = "exited"
                    save()
                    if result.returncode:
                        receipt["status"] = "source location failed"
                        receipt["exit"] = result.returncode
                        print(
                            f"Bazel compiler source query exited {result.returncode}; "
                            f"stderr: {source_location['stderr']}",
                            file=sys.stderr,
                        )
                        return result.returncode
                    lines = Path(source_location["stdout"]).read_text(encoding="utf-8").splitlines()
                    location = (
                        re.fullmatch(r"(.+):[0-9]+:[0-9]+: source file \S+", lines[0]) if len(lines) == 1 else None
                    )
                    if location is None:
                        raise ValueError("Bazel compiler query must return exactly one source-file location")
                    source_location["location"] = location[1]
                    compiler = _driver_file(location[1])
                    source_location["status"] = "passed"
                source_location["compiler"] = str(compiler)
                driver["argv"] = [str(compiler), *representative.argv[1:], "-###"]
                driver["status"] = "running"
                save()
                print(
                    f"Bazel representative Clang driver ({config}); receipt: {receipt_path}",
                    file=sys.stderr,
                    flush=True,
                )
                status = _verify_driver(
                    representative, receipt["mode"], expected_environment, driver, compiler=compiler
                )
                save()
                if status:
                    receipt["status"] = "driver failed"
                    receipt["exit"] = status
                    print(f"Representative driver gate failed ({status}); stderr: {driver['stderr']}", file=sys.stderr)
                    if driver.get("errors"):
                        print("\n".join(driver["errors"]), file=sys.stderr)
                    return status
        receipt["status"] = "passed"
        receipt["exit"] = 0
        print(f"Bazel compile contracts passed ({config}); receipt: {receipt_path}")
        return 0
    except (
        native.BazelWorkspaceError,
        OSError,
        subprocess.SubprocessError,
        ValueError,
        KeyError,
        TypeError,
        StopIteration,
        RecursionError,
    ) as exc:
        receipt["status"] = "error"
        receipt["exit"] = 1
        receipt["error"] = str(exc)
        if selection["status"] in {"running", "exited"}:
            selection["status"] = "error"
            selection["error"] = str(exc)
        if source_location["status"] in {"running", "exited"}:
            source_location["status"] = "error"
            source_location["error"] = str(exc)
        if driver["status"] in {"running", "exited"}:
            driver["status"] = "error"
            driver["error"] = str(exc)
        for query in queries:
            if query["status"] in {"running", "exited"}:
                query["status"] = "error"
                query["error"] = str(exc)
        print(f"Bazel compile-contract verify: {exc}; receipt: {receipt_path}", file=sys.stderr)
        return 1
    finally:
        save()
