"""Project an existing native LLVM SDK for the Bazel-owned lint checker.

This is a ready-only import, not a resolver or installer. Installed providers have
exactly the same identity/trust boundary as the native product toolchain.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Supports both the portal package and the repository rule's script execution.
# The lexical path keeps imports inside the rule's watched module closure.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parents[2]))

from ao.core import windows_llvm_sdk


class LintSdkError(RuntimeError):
    """An existing SDK cannot satisfy the checker import contract."""


def _absolute(value: str, label: str) -> Path:
    path = Path(value)
    if not value or not path.is_absolute():
        raise LintSdkError(f"{label} must select an absolute existing native path")
    return path


def _file(path: Path) -> Path:
    if not path.is_file():
        raise LintSdkError(f"Existing LLVM SDK file is missing: {path}; provision it through its native owner")
    return path


def _run(executable: Path, *arguments: str, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(_file(executable)), *arguments],
        input=input_text,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode:
        raise LintSdkError(f"LLVM SDK query failed: {executable} {' '.join(arguments)}\n{result.stderr}")
    return result


def _version(text: str) -> str:
    match = re.search(r"(?:LLVM|clang) version ([0-9]+\.[0-9]+\.[0-9]+)", text)
    if match is None:
        raise LintSdkError(f"LLVM tool did not report its native version: {text.strip()}")
    return match[1]


def _tool(environment: Mapping[str, str], variable: str, name: str, root: Path | None = None) -> Path:
    if selected := environment.get(variable):
        return _file(_absolute(selected, variable))
    if root is not None:
        return _file(root / "bin" / name)
    selected = shutil.which(name, path=environment.get("PATH", ""))
    if selected is None:
        raise LintSdkError(f"Missing governed {name}; enter the pinned native environment or set {variable}")
    return _file(_absolute(selected, name))


# The Windows checker consumes Clang's frontend/analyzer and the tidy modules
# referenced by its installed clangTidyMain archive. llvm-config resolves only
# the LLVM side; POSIX uses the provider's clang-cpp shared interface instead.
_WINDOWS_CLANG_LIBRARIES = (
    "clangTidyMain",
    "clangTidyUtils",
    "clangTidy",
    "clangStaticAnalyzerFrontend",
    "clangStaticAnalyzerCheckers",
    "clangStaticAnalyzerCore",
    "clangCrossTU",
    "clangIndex",
    "clangTooling",
    "clangToolingCore",
    "clangDependencyScanning",
    "clangToolingRefactoring",
    "clangToolingSyntax",
    "clangToolingInclusions",
    "clangToolingInclusionsStdlib",
    "clangTransformer",
    "clangFormat",
    "clangRewrite",
    "clangFrontend",
    "clangDriver",
    "clangSerialization",
    "clangParse",
    "clangSema",
    "clangAnalysis",
    "clangAnalysisLifetimeSafety",
    "clangAnalysisFlowSensitive",
    "clangAnalysisFlowSensitiveModels",
    "clangASTMatchers",
    "clangDynamicASTMatchers",
    "clangQuery",
    "clangIncludeCleaner",
    "clangAST",
    "clangEdit",
    "clangLex",
    "clangAPINotes",
    "clangBasic",
    "clangOptions",
    "clangSupport",
)
_WINDOWS_LLVM_COMPONENTS = (
    "support",
    "option",
    "frontendopenmp",
    "frontendhlsl",
    "windowsdriver",
    "targetparser",
    "bitreader",
    "bitwriter",
    "profiledata",
    "debuginfopdb",
    "plugins",
)


@dataclass
class LintSdkProjection:
    host: str
    version: str
    resource_dir: Path
    sdk_root: Path
    clang: Path
    clang_tidy: Path
    includes: list[Path]
    libraries: list[Path]
    copts: list[str]
    linkopts: list[str]
    system_arguments: list[str] = field(default_factory=list)
    analysis_arguments: list[str] = field(default_factory=list)
    fixture_arguments: list[str] = field(default_factory=list)
    watch_files: list[Path] = field(default_factory=list)
    watch_directories: list[Path] = field(default_factory=list)
    runtime: list[Path] = field(default_factory=list)


def _clang_roots(environment: Mapping[str, str], candidates: Sequence[Path]) -> list[Path]:
    if explicit := environment.get("AOBUS_LINT_CLANG_ROOTS"):
        values = json.loads(explicit)
        if not isinstance(values, list) or not values or any(not isinstance(value, str) for value in values):
            raise LintSdkError("AOBUS_LINT_CLANG_ROOTS must be a nonempty JSON array of absolute LLVM/Clang roots")
        roots = [_absolute(value, "AOBUS_LINT_CLANG_ROOTS") for value in values]
    else:
        roots = list(candidates)
        # The pinned Nix shell explicitly publishes its split dev/lib/bin roots.
        # Inspect ownership markers only; never search the store or parse CMake exports.
        for value in environment.get("NIXPKGS_CMAKE_PREFIX_PATH", "").split(os.pathsep):
            if value and re.search(r"(?:^|-)(clang|llvm)-[0-9]", Path(value).name):
                roots.append(Path(value))
    return list(dict.fromkeys(roots))


def _one_library(roots: Sequence[Path], patterns: Sequence[str], name: str) -> Path:
    for pattern in patterns:
        matches = sorted({path for root in roots for path in (root / "lib").glob(pattern) if path.is_file()})
        # Aliases resolving to the same installed payload are not two providers.
        selected = {path.resolve(): path for path in reversed(matches)}
        if len(selected) == 1:
            return next(iter(selected.values()))
        if len(selected) > 1:
            raise LintSdkError(f"Multiple governed providers for {name}: {matches}")
    raise LintSdkError(f"Missing {name} in selected LLVM/Clang library roots: {', '.join(map(str, roots))}")


def _llvm_target_components(symbols: str) -> list[str]:
    # ClangTidyMain initializes target infos, MC descriptions and assembly
    # parsers. Select the referenced roles, not complete target/codegen groups.
    roles = {"TargetInfo": "info", "TargetMC": "desc", "AsmParser": "asmparser"}
    components = set()
    for reference in re.findall(r"(?m)^\s*U\s+([^\n]+)$", symbols):
        # Native C symbol prefixes and COFF import pointers are not target names.
        name = reference.split()[-1].removeprefix("__imp_").removeprefix("_")
        if not name.startswith("LLVMInitialize"):
            continue
        match = re.fullmatch(r"LLVMInitialize([A-Za-z][A-Za-z0-9_]*?)(TargetInfo|TargetMC|AsmParser)", name)
        if match is None:
            raise LintSdkError(f"Unsupported clangTidyMain LLVM initializer reference: {name}")
        components.add(match[1].lower() + roles[match[2]])
    return sorted(components)


def _tidy_main_requirements(
    roots: Sequence[Path], force_linker: Path, main: Path, llvm_nm: Path
) -> tuple[list[Path], list[str]]:
    # clang-tidy/tool/ClangTidyMain.cpp includes ClangTidyForceLinker.h, and the
    # upstream tool CMake links ALL_CLANG_TIDY_CHECKS. Use the installed archive's
    # actual undefined anchors: optional modules follow its build configuration,
    # even when the SDK omits the generated clang-tidy-config.h header.
    declared = set(
        re.findall(
            r"\bextern\s+volatile\s+int\s+([A-Za-z_]\w*Module)AnchorSource\s*;",
            _file(force_linker).read_text(encoding="utf-8"),
        )
    )
    symbols = _run(llvm_nm, "--undefined-only", "--demangle", str(_file(main))).stdout
    # llvm-nm demangles COFF variables with their type, unlike ELF variables.
    required = set(re.findall(r"(?m)^\s*U\s+.*\bclang::tidy::([A-Za-z_]\w*Module)AnchorSource\b", symbols))
    if not declared or not required or required - declared:
        raise LintSdkError(
            f"Cannot match clangTidyMain module anchors to {force_linker}: "
            f"{', '.join(sorted(required - declared)) or 'missing module declarations or references'}"
        )
    # Ordinary archive references retain each module's registration object;
    # neither the module archives nor LLVM require whole-archive linking.
    modules = [_one_library(roots, [f"clangTidy{module}.lib"], f"clangTidy{module}") for module in sorted(required)]
    return modules, _llvm_target_components(symbols)


def _tokens(text: str, host: str) -> list[str]:
    lexer = shlex.shlex(text, posix=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    if host == "windows":
        lexer.escape = ""  # Native paths use backslashes, not shell escapes.
    return list(lexer)


def _compile_flags(text: str, rtti: str, assertions: str, host: str) -> list[str]:
    flags = _tokens(text, host)
    # Include ownership is projected separately; C++23 belongs to the checker.
    # LLVM's assertion, EH, RTTI, ABI and standard-library flags remain SDK-owned.
    result = []
    skip = False
    for flag in flags:
        if skip:
            skip = False
            continue
        if flag in ("-I", "-isystem"):
            skip = True
        elif flag.startswith(("-I", "-std=", "/I", "/std:")):
            continue
        elif re.fullmatch(r"/M[DT]d?", flag):
            continue  # The narrow opt/static_link_msvcrt transition owns the CRT.
        elif host == "windows" and flag.startswith(("-f", "-W", "/EH", "/GR")):
            continue  # llvm-config reports clang driver flags, not cl flags.
        elif host == "windows" and flag.startswith("-D"):
            result.append("/D" + flag[2:])
        else:
            result.append(flag)
    if rtti not in ("YES", "NO"):
        raise LintSdkError(f"Unsupported LLVM RTTI metadata: {rtti}")
    if assertions not in ("ON", "OFF"):
        raise LintSdkError(f"Unsupported LLVM assertion metadata: {assertions}")
    if host == "windows":
        if assertions != "OFF":
            raise LintSdkError("The Windows lint host requires the governed release LLVM SDK without assertions")
        if "-fexceptions" in flags:
            raise LintSdkError("The Windows lint host requires the governed no-exceptions LLVM SDK")
        result.extend(
            [
                "/GR" if rtti == "YES" else "/GR-",
                "/EHs-c-",
                "/D_HAS_EXCEPTIONS=0",
                "/DCLANG_BUILD_STATIC",
                "/DLLVM_BUILD_STATIC",
                "/wd4702",
            ]
        )
    else:
        result.append("-frtti" if rtti == "YES" else "-fno-rtti")
        if assertions == "OFF" and "-DNDEBUG" not in result:
            result.append("-DNDEBUG")
    return list(dict.fromkeys(result))


def _llvm_link_interface(
    llvm_config: Path, host: str, target_components: Sequence[str]
) -> tuple[list[Path], list[str]]:
    mode = "--link-static" if host == "windows" else "--link-shared"
    components = _WINDOWS_LLVM_COMPONENTS if host == "windows" else ("support",)
    if host == "windows":
        available = set(_run(llvm_config, "--components").stdout.split())
        if missing := set(target_components) - available:
            raise LintSdkError(
                f"clangTidyMain requires unavailable LLVM target components: {', '.join(sorted(missing))}"
            )
        components = (*components, *target_components)
    libraries = [
        _file(_absolute(value, "llvm-config --libfiles"))
        for value in _tokens(_run(llvm_config, mode, "--libfiles", *components).stdout, host)
    ]
    if not libraries:
        raise LintSdkError("llvm-config returned no LLVM link closure")
    systems = _tokens(_run(llvm_config, mode, "--system-libs", *components).stdout, host)
    return libraries, systems


def _windows_system_libraries(
    systems: Sequence[str], roots: Sequence[Path], libraries: Sequence[Path], llvm_nm: Path
) -> tuple[list[str], list[Path]]:
    if not any(value.casefold() == "xml2s.lib" for value in systems):
        return list(systems), []
    # This SDK's llvm-config reports a global xml2s.lib even for Support alone.
    # LLVMWindowsManifest is not part of clang-tidy's target initializer closure.
    # Repair that token only with evidence from the actual selected archives.
    needs_xml = any(path.stem.casefold().removeprefix("lib") == "llvmwindowsmanifest" for path in libraries)
    for library in libraries:
        if needs_xml:
            break
        symbols = _run(llvm_nm, "--undefined-only", "--format=posix", str(_file(library))).stdout
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) < 2 or fields[1] != "U":
                continue
            name = fields[0].removeprefix("__imp_").lstrip("_")
            if (
                name.startswith(("xml", "html", "docb"))
                or name in ("initGenericErrorDefaultFunc", "isolat1ToUTF8", "UTF8Toisolat1", "UTF8ToHtml")
                or "windowsmanifest" in name.casefold()
                or "windows_manifest" in name.casefold()
            ):
                needs_xml = True
                break
    # A required non-OS dependency must have real governed bytes, not a bare
    # linker token justified by the MSVC/Windows system-library trust boundary.
    extra = (
        [_one_library(roots, ["xml2s.lib"], "xml2s.lib required by the selected Windows LLVM archives")]
        if needs_xml
        else []
    )
    return [value for value in systems if value.casefold() != "xml2s.lib"], extra


def discover_sdk(
    host: str, environment: Mapping[str, str], windows_manifest: Path, macos_contract: Path
) -> LintSdkProjection:
    """Query selected tools and validate the actual import, without preparing it."""
    if host not in ("linux", "macos", "windows"):
        raise LintSdkError(f"Unsupported lint execution host: {host}")
    root = None
    policy = []
    if host == "windows":
        manifest = windows_llvm_sdk.read_manifest(windows_manifest)
        root, cached = windows_llvm_sdk.ready_root(environment, manifest)
        policy = [windows_manifest, *(root / entry for entry in manifest.required_files)]
        if cached:
            policy.append(root / windows_llvm_sdk.MARKER)
    elif host == "macos":
        root = _absolute(environment.get("AOBUS_LLVM_ROOT", ""), "AOBUS_LLVM_ROOT")
        policy = [macos_contract]
    suffix = ".exe" if host == "windows" else ""
    llvm_config = _tool(environment, "AOBUS_LINT_LLVM_CONFIG", "llvm-config" + suffix, root)
    tidy = _tool(environment, "AOBUS_LINT_CLANG_TIDY", "clang-tidy" + suffix, root)
    clang = _tool(environment, "AOBUS_LINT_CLANG", "clang-cl.exe" if host == "windows" else "clang", root)
    fixture_compiler = clang if host == "windows" else _tool(environment, "AOBUS_LINT_CLANGXX", "clang++", root)
    version = _run(llvm_config, "--version").stdout.strip()
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise LintSdkError(f"Invalid llvm-config version: {version}")
    for tool in dict.fromkeys((tidy, clang, fixture_compiler)):
        if _version(_run(tool, "--version").stdout) != version:
            raise LintSdkError(f"LLVM SDK tools disagree on version: {llvm_config} versus {tool}")
    if host == "windows" and version != manifest.version:
        raise LintSdkError(f"Windows LLVM SDK {version} does not match the shared manifest {manifest.version}")
    if host == "macos":
        contract = json.loads(macos_contract.read_text(encoding="utf-8"))
        if version.split(".")[0] != contract["homebrew"]["llvmMajorVersion"]:
            raise LintSdkError("LLVM SDK does not match the governed macOS compiler major")
    prefix = _absolute(_run(llvm_config, "--prefix").stdout.strip(), "llvm-config --prefix")
    llvm_include = _absolute(_run(llvm_config, "--includedir").stdout.strip(), "llvm-config --includedir")
    llvm_lib = _absolute(_run(llvm_config, "--libdir").stdout.strip(), "llvm-config --libdir")
    roots = _clang_roots(environment, [prefix, tidy.resolve().parent.parent, llvm_lib.parent])
    clang_includes = [
        candidate / "include" for candidate in roots if (candidate / "include/clang-tidy/ClangTidy.h").is_file()
    ]
    if len(set(path.resolve() for path in clang_includes)) != 1:
        raise LintSdkError("Select exactly one governed Clang/Tidy header provider (including Nix's split dev output)")
    includes = list(dict.fromkeys([llvm_include, clang_includes[0]]))
    llvm_header = _file(llvm_include / "llvm/Config/llvm-config.h")
    header_version = re.search(
        r'^#define LLVM_VERSION_STRING "([^"]+)"', llvm_header.read_text(encoding="utf-8"), re.MULTILINE
    )
    if header_version is None or header_version[1] != version:
        raise LintSdkError("LLVM headers disagree with the selected llvm-config version")
    clang_header = _file(clang_includes[0] / "clang/Basic/Version.inc")
    clang_version = re.search(
        r'^#define CLANG_VERSION_STRING "([^"]+)"', clang_header.read_text(encoding="utf-8"), re.MULTILINE
    )
    if clang_version is None or clang_version[1] != version:
        raise LintSdkError("Clang headers disagree with the selected native tools")
    resource_query = "-print-resource-dir" if host != "windows" else "/clang:-print-resource-dir"
    resource_dir = _absolute(_run(clang, resource_query).stdout.strip(), "Clang resource directory")
    _file(resource_dir / "include/stddef.h")
    copts = _compile_flags(
        _run(llvm_config, "--cxxflags").stdout,
        _run(llvm_config, "--has-rtti").stdout.strip(),
        _run(llvm_config, "--assertion-mode").stdout.strip(),
        host,
    )
    target_components: list[str] = []
    if host == "windows":
        clang_libraries = {name: _one_library(roots, [name + ".lib"], name) for name in _WINDOWS_CLANG_LIBRARIES}
        force_linker = clang_includes[0] / "clang-tidy/ClangTidyForceLinker.h"
        llvm_nm = _file((root or prefix) / "bin/llvm-nm.exe")
        modules, target_components = _tidy_main_requirements(
            roots, force_linker, clang_libraries["clangTidyMain"], llvm_nm
        )
        libraries = [*clang_libraries.values(), *modules]
        policy.extend([force_linker, llvm_nm])
    llvm_libraries, systems = _llvm_link_interface(llvm_config, host, target_components)
    system_arguments: list[str] = []
    if host == "windows":
        systems = [
            value.removeprefix("-l") + ".lib" if value.startswith("-l") else value.strip('"') for value in systems
        ]
        systems, xml_libraries = _windows_system_libraries(systems, roots, [*libraries, *llvm_libraries], llvm_nm)
        # clangDriver's MSVC discovery uses the Windows version APIs; llvm-config
        # describes LLVM's system interface, not this Clang dependency.
        systems.append("version.lib")
        libraries.extend(xml_libraries)
        if any("diaguids" in value.lower() for value in systems):
            systems = [value for value in systems if "diaguids" not in value.lower()]
        if "#define LLVM_ENABLE_DIA_SDK 1" in llvm_header.read_text(encoding="utf-8"):
            dia = _file(
                _absolute(environment.get("VSINSTALLDIR", ""), "VSINSTALLDIR") / "DIA SDK/lib/amd64/diaguids.lib"
            )
            libraries.append(dia)
        analysis_arguments = [f"-resource-dir={resource_dir}", "-D_USE_STD_VECTOR_ALGORITHMS=0"]
        system_arguments = []
        runtime = sorted((root / "bin").glob("*.dll")) if root else []
    else:
        libraries = [
            _one_library(roots, [f"lib{name}.a"], name)
            for name in ("clangTidyReadabilityModule", "clangTidyUtils", "clangTidy")
        ]
        cpp = _one_library(
            roots, ["libclang-cpp.so", "libclang-cpp.so.*", "libclang-cpp.dylib", "libclang-cpp.*.dylib"], "clang-cpp"
        )
        libraries.append(cpp)
        runtime = [cpp, *llvm_libraries]
        analysis_arguments = [f"-resource-dir={resource_dir}"]
        system_arguments = []
        if shim := environment.get("AOBUS_LIBCXX_EXPECTED_SHIM"):
            if Path(shim).is_dir():
                system_arguments.append("-isystem" + shim)
        probe = _run(clang, "-E", "-x", "c++", "-", "-v", input_text="")
        for line in probe.stderr.splitlines():
            if line.startswith(" /nix") and Path(line.strip()).is_dir():
                argument = "-isystem" + line.strip()
                if argument not in system_arguments:
                    system_arguments.append(argument)
        # llvm-config's standard-library selection is a link contract as well.
        systems.extend(flag for flag in copts if flag.startswith("-stdlib="))
    fixture_arguments = list(system_arguments)
    if host == "macos":
        xcrun = _tool(environment, "AOBUS_LINT_XCRUN", "xcrun")
        macos_sdk = _absolute(_run(xcrun, "--show-sdk-path").stdout.strip(), "selected macOS SDK")
        if not macos_sdk.is_dir():
            raise LintSdkError(f"Selected native macOS SDK is missing: {macos_sdk}")
        shim_path = _absolute(environment.get("AOBUS_LIBCXX_EXPECTED_SHIM", ""), "AOBUS_LIBCXX_EXPECTED_SHIM")
        if not shim_path.is_dir():
            raise LintSdkError(f"Governed libc++ expected shim is missing: {shim_path}")
        fixture_arguments = [
            "--no-default-config",
            "-stdlib=libc++",
            "-isysroot",
            str(macos_sdk),
            "-mmacosx-version-min=" + contract["deploymentTarget"],
            *system_arguments,
        ]
        policy.extend(
            [
                xcrun,
                *(path for path in (macos_sdk / "SDKSettings.json", macos_sdk / "SDKSettings.plist") if path.is_file()),
            ]
        )
    return LintSdkProjection(
        host=host,
        version=version,
        resource_dir=resource_dir,
        sdk_root=root or prefix,
        clang=fixture_compiler,
        clang_tidy=tidy,
        includes=includes,
        libraries=[*libraries, *llvm_libraries],
        copts=copts,
        linkopts=systems,
        system_arguments=system_arguments,
        analysis_arguments=analysis_arguments,
        fixture_arguments=fixture_arguments,
        watch_files=[
            *policy,
            llvm_config,
            tidy,
            clang,
            fixture_compiler,
            llvm_header,
            clang_header,
            llvm_include / "llvm/Config/abi-breaking.h",
        ],
        watch_directories=[*(candidate / "lib" for candidate in roots), *includes],
        runtime=runtime,
    )


def _projection_facts(sdk: LintSdkProjection) -> dict[str, Any]:
    """Serialize only admitted native facts; the repository rule owns the graph.

    Standalone paths use pathlib's portable posix wire form: POSIX names keep
    literal backslashes, Windows canonicalizes to forward separators. Embedded
    SDK argument arrays stay verbatim.
    """
    return {
        "host": sdk.host,
        "version": sdk.version,
        "sdk_root": sdk.sdk_root.as_posix(),
        "clang": sdk.clang.as_posix(),
        "clang_tidy": sdk.clang_tidy.as_posix(),
        "resource_dir": sdk.resource_dir.as_posix(),
        "includes": [path.as_posix() for path in sdk.includes],
        "libraries": [path.as_posix() for path in sdk.libraries],
        "runtime": [path.as_posix() for path in sdk.runtime],
        "copts": sdk.copts,
        "linkopts": sdk.linkopts,
        "system_arguments": sdk.system_arguments,
        "analysis_arguments": sdk.analysis_arguments,
        "fixture_arguments": sdk.fixture_arguments,
        "watch_files": [path.as_posix() for path in sdk.watch_files],
        "watch_directories": [path.as_posix() for path in sdk.watch_directories],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, choices=("linux", "macos", "windows"))
    parser.add_argument("--windows-manifest", required=True, type=Path)
    parser.add_argument("--macos-contract", required=True, type=Path)
    parser.add_argument("--environment-json", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        environment = json.loads(args.environment_json)
        if not isinstance(environment, dict) or any(
            not isinstance(key, str) or not isinstance(value, str) for key, value in environment.items()
        ):
            raise LintSdkError("Expected explicit native environment strings")
        facts = _projection_facts(discover_sdk(args.host, environment, args.windows_manifest, args.macos_contract))
        args.output.write_text(json.dumps(facts, ensure_ascii=False) + "\n", encoding="utf-8")
        return 0
    except (LintSdkError, OSError, ValueError) as error:
        print(f"Cannot import existing lint LLVM SDK: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
