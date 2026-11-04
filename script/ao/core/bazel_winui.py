"""Lazy projection of the existing WinUI NuGet SDK; never restores packages."""

from __future__ import annotations

import json
import re
import subprocess
import xml.etree.ElementTree as ET
from collections.abc import Mapping
from pathlib import Path
from types import ModuleType
from typing import Any

from . import dependency_policy
from .bazel_workspace import BazelWorkspaceError, _probe, _windows_coff_archive_kind
from .paths import PROJECT_ROOT
from .winui import WINDOWS_SDK_VERSION


def _owned(path: Path, root: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
        allowed = root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise BazelWorkspaceError(f"WinUI SDK input is missing or escapes its selected provider: {path}") from exc
    if not resolved.is_relative_to(allowed) or not resolved.is_file():
        raise BazelWorkspaceError(f"WinUI SDK input is missing or escapes its selected provider: {path}")
    return resolved


def _list(value: str) -> list[str]:
    return list(dict.fromkeys(part.strip() for part in value.split(";") if part.strip()))


def _json_output(text: str) -> dict[str, Any]:
    # Native MSBuild may precede its query JSON with normal target messages.
    for match in re.finditer(r"(?m)^\s*\{", text):
        try:
            value, _ = json.JSONDecoder().raw_decode(text[match.start() :].lstrip())
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and "Items" in value and "Properties" in value:
            return value
    raise BazelWorkspaceError("WinUI SDK MSBuild query did not return its declared item/property JSON")


def _locked_packages(root: Path) -> list[tuple[str, str, Path]]:
    lock = PROJECT_ROOT / "app/windows-winui/packages.config"
    locked = dependency_policy._packages_config(lock)
    if not locked:
        raise BazelWorkspaceError(f"WinUI NuGet lock is empty: {lock}")
    contract = dependency_policy.load_contract()
    for name, definition in contract["dependencies"].items():
        nuget = definition.get("nuget")
        if not nuget:
            continue
        package = nuget["package"]
        selected = next((version for key, version in locked.items() if key.casefold() == package.casefold()), None)
        if selected is None or not dependency_policy.effective_policy(contract, name, "windows").accepts(selected):
            raise BazelWorkspaceError(f"WinUI NuGet lock does not satisfy the existing {name} dependency policy")
    packages = []
    for name, version in locked.items():
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or not re.fullmatch(r"[A-Za-z0-9_.+-]+", version):
            raise BazelWorkspaceError(f"invalid WinUI NuGet package identity: {name} {version}")
        directory = root / f"{name}.{version}"
        archive = dependency_policy.verify_nuget_package(root, name, version)
        _owned(archive, root)
        if not directory.resolve().is_relative_to(root):
            raise BazelWorkspaceError(f"WinUI NuGet package escapes the selected SDK: {directory}")
        packages.append((name, version, directory.resolve()))
    return packages


def _imports(packages: list[tuple[str, str, Path]], extension: str) -> list[Path]:
    result = []
    sequence = packages if extension == "props" else list(reversed(packages))
    for name, _, root in sequence:
        candidates = (root / "build/native" / f"{name}.{extension}", root / "build" / f"{name}.{extension}")
        selected = next((path for path in candidates if path.is_file()), None)
        if selected is not None:
            result.append(_owned(selected, root))
    return result


def _interface_project(
    props: list[Path], targets: list[Path], compiler: Mapping[str, str], toolset: str, config: str = "debug"
) -> str:
    # The resolver audits this source read into watches. Use the module's own
    # location, not the independently supplied lock/policy root; load lazily so
    # non-Windows SDK stubs never evaluate project support.
    path = Path(__file__).resolve().parents[3] / "app/windows-winui/msbuild_project.py"
    builder = ModuleType("aobus_msbuild_project")
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), builder.__dict__)
    project: ET.Element = builder.project(
        configuration=config.capitalize(),
        toolset=toolset,
        sdk_version=WINDOWS_SDK_VERSION,
        configuration_type="Application",
        initial_properties={
            "VCToolsVersion": compiler["VCToolsVersion"],
            "WindowsAppSDKMLVerifyTransitiveDependencies": "true",
        },
        global_properties={},
        project_properties={
            "IntDir": "int\\",
            "GeneratedFilesDir": "$(IntDir)Generated Files\\",
            "AppContainerApplication": "false",
        },
        props=[str(path) for path in props],
    )
    project.set("DefaultTargets", "AobusSdkInterface")
    items = ET.SubElement(project, "ItemGroup")
    ET.SubElement(items, "ClCompile", Include="__aobus_sdk_interface.cpp")
    ET.SubElement(items, "Link", Include="metadata-only")
    builder.finish(project, [str(path) for path in targets])
    ET.SubElement(
        project,
        "Target",
        Name="AobusSdkInterface",
        DependsOnTargets="WindowsAppRuntimeAutoInitializer;GenerateBootstrapCpp;WindowsMLAutoInitializer",
    )
    guard = ET.SubElement(
        project,
        "Target",
        Name="AobusNoProductBuild",
        BeforeTargets="ClCompile;Lib;Link;Midl;MarkupCompilePass1;MarkupCompilePass2;CppWinRTMakeProjections",
    )
    ET.SubElement(guard, "Error", Text="WinUI SDK interface query must not generate or compile product code.")
    ET.indent(project)
    serialized: bytes = ET.tostring(project, encoding="utf-8", xml_declaration=True)
    return serialized.decode("utf-8")


def _native_tools(compiler: Mapping[str, str]) -> tuple[Path, str, Path]:
    vc = Path(compiler.get("VCToolsInstallDir", ""))
    version = compiler.get("VCToolsVersion", "")
    if (
        not vc.is_absolute()
        or vc.name != version
        or [part.casefold() for part in vc.parts[-4:-1]] != ["vc", "tools", "msvc"]
    ):
        raise BazelWorkspaceError("WinUI SDK query requires the selected VCToolsInstallDir/VCToolsVersion")
    visual_studio = vc.parents[3]
    msbuild = _owned(visual_studio / "MSBuild/Current/Bin/amd64/MSBuild.exe", visual_studio)
    auxiliary = vc.parents[2] / "Auxiliary/Build"
    matches = []
    for path in auxiliary.glob("Microsoft.VCToolsVersion.v*.default.txt"):
        match = re.fullmatch(r"Microsoft\.VCToolsVersion\.(v[0-9]+)\.default\.txt", path.name)
        if match and path.read_text(encoding="utf-8-sig").strip() == version:
            matches.append(match[1])
    if len(matches) != 1:
        raise BazelWorkspaceError("WinUI SDK query cannot uniquely bind the installed selected VC platform toolset")
    sdk = Path(compiler.get("WindowsSdkDir", ""))
    if not sdk.is_absolute() or compiler.get("WindowsSDKVersion", "").rstrip("\\/") != WINDOWS_SDK_VERSION:
        raise BazelWorkspaceError("WinUI requires the existing governed Windows SDK selection")
    return msbuild, matches[0], visual_studio


def _provider_path(
    path: Path, packages: list[tuple[str, str, Path]], *, allow_missing: bool = False
) -> tuple[str, Path]:
    resolved = path.resolve(strict=not allow_missing)
    for name, _, root in packages:
        if resolved.is_relative_to(root):
            return name, resolved.relative_to(root)
    raise BazelWorkspaceError(f"WinUI SDK interface points outside its locked NuGet providers: {path}")


def _projected(path: Path, packages: list[tuple[str, str, Path]], *, allow_missing: bool = False) -> str:
    name, relative = _provider_path(path, packages, allow_missing=allow_missing)
    return (Path("sdk") / name / relative).as_posix()


def _render_repository(
    defines: list[str],
    includes: list[str],
    headers: list[str],
    sources: list[str],
    libraries: list[dict[str, str]],
    systems: list[str],
    runtime: list[str],
    tools: list[str],
) -> str:
    def quote(value: object) -> str:
        return json.dumps(value, ensure_ascii=False)

    return (
        'load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")\n'
        'package(default_visibility = ["//visibility:public"])\n'
        'exports_files(["sdk.json", "sdk-diagnostics.json"])\n'
        + 'cc_library(name = "headers", hdrs = '
        + quote(headers)
        + ", includes = "
        + quote(includes)
        + ", defines = "
        + quote(defines)
        + ")\n"
        + "\n".join(
            'cc_import(name = "library_'
            + str(index)
            + '", '
            + ("interface_library" if library["kind"] == "interface" else "static_library")
            + " = "
            + quote(library["file"])
            + (", system_provided = True" if library["kind"] == "interface" else "")
            + ")"
            for index, library in enumerate(libraries)
        )
        + '\ncc_library(name = "bootstrap", srcs = '
        + quote(sources)
        + ', alwayslink = True, deps = [":headers"] + '
        + quote([":library_" + str(index) for index in range(len(libraries))])
        + ", linkopts = "
        + quote(systems)
        + ")\n"
        + 'filegroup(name = "runtime", srcs = '
        + quote(runtime)
        + ")\n"
        + 'filegroup(name = "tools", srcs = '
        + quote(tools)
        + ")\n"
    )


def repository_plan(tree: Path, request: dict[str, Any], environment: Mapping[str, str]) -> dict[str, Any]:
    """Return a watched repository plan; Windows SDK evaluation is lazy and local."""
    if request["host"] != "windows":
        return {
            "links": [],
            "files": {
                "BUILD.bazel": (
                    'load("@rules_cc//cc:defs.bzl", "cc_library")\n'
                    'package(default_visibility = ["//visibility:public"])\n'
                    'cc_library(name = "headers")\ncc_library(name = "bootstrap")\n'
                    'filegroup(name = "runtime")\nfilegroup(name = "tools")\n'
                    'exports_files(["sdk.json", "sdk-diagnostics.json"])\n'
                ),
                "sdk.json": "{}\n",
                "sdk-diagnostics.json": "{}\n",
            },
        }
    lookup = {key.upper(): value for key, value in environment.items()}
    selected = lookup.get("AOBUS_NUGET_PACKAGES_DIR", "")
    if not selected:
        from .bazel_provision import restored_nuget_packages

        # Keep the default ready-cache lookup lazy: a TUI build needs no NuGet.
        if restored := restored_nuget_packages(environment):
            selected = str(restored)
    root = Path(selected)
    if not selected or not root.is_absolute() or not root.is_dir():
        raise BazelWorkspaceError(
            "WinUI requires a restored NuGet cache: run `ao.bat bazel provision`, "
            "or set AOBUS_NUGET_PACKAGES_DIR to an already restored one"
        )
    root = root.resolve(strict=True)
    packages = _locked_packages(root)
    props, targets = _imports(packages, "props"), _imports(packages, "targets")
    compiler = request["compiler_environment"]
    msbuild, toolset, visual_studio = _native_tools(compiler)
    tree.mkdir(parents=True, exist_ok=True)
    project = tree / "AobusSdk.vcxproj"
    project.write_text(_interface_project(props, targets, compiler, toolset, request["config"]), encoding="utf-8")
    # Run the SDK's own InitialTargets dependency-presence check once here,
    # against the real locked archive and lock. Product generation can then
    # suppress that repeat check without copying whole nupkg archives into
    # each action. Both original inputs remain repository-rule watch files.
    (tree / "packages.config").write_bytes((PROJECT_ROOT / "app/windows-winui/packages.config").read_bytes())
    (tree / "tmp").mkdir(exist_ok=True)
    compiler_keys = {
        "INCLUDE",
        "LIB",
        "PATH",
        "VCTOOLSINSTALLDIR",
        "VCTOOLSVERSION",
        "WINDOWSSDKDIR",
        "WINDOWSSDKVERSION",
        "UCRTVERSION",
        "NETFXSDKDIR",
    }
    env = {key.upper(): value for key, value in compiler.items() if key.upper() in compiler_keys}
    env.update(
        {
            key: lookup[key]
            # Native VC task hosts use the observed host architecture for tool
            # selection; Tracker/Detours also need the Windows runtime roots.
            for key in (
                "SYSTEMROOT",
                "SYSTEMDRIVE",
                "WINDIR",
                "COMSPEC",
                "PROCESSOR_ARCHITECTURE",
                "PROCESSOR_ARCHITEW6432",
                "USERPROFILE",
                "LOCALAPPDATA",
                "APPDATA",
            )
            if key in lookup
        }
    )
    env.update({"TEMP": str(tree / "tmp"), "TMP": str(tree / "tmp"), "MSBUILDDISABLENODEREUSE": "1"})
    result = subprocess.run(
        [
            str(msbuild),
            str(project),
            "/nologo",
            "/noautoresponse",
            "/v:quiet",
            "/m:1",
            "/nr:false",
            "/t:AobusSdkInterface",
            "/getProperty:VCToolsInstallDir,WindowsSdkDir,WindowsTargetPlatformVersion,MakePriExeFullPath,XamlCompilerExePath,_WindowsAppSDKFoundationPlatform,WindowsAppSdkBootstrapInitialize,WindowsAppSDKMLVerifyTransitiveDependencies,MSBuildRuntimeType,VCTargetsPath,CppWinRTPath,MSBuildAllProjects",
            "/getItem:ClCompile,Link,Content,ReferenceCopyLocalPaths",
        ],
        cwd=tree,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    (tree / "query.stdout").write_text(result.stdout, encoding="utf-8")
    (tree / "query.stderr").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        raise BazelWorkspaceError(
            f"WinUI installed SDK interface query failed ({result.returncode}): {result.stdout}\n{result.stderr}"
        )
    query = _json_output(result.stdout)
    properties = query["Properties"]
    if properties.get("WindowsAppSDKMLVerifyTransitiveDependencies") != "true":
        raise BazelWorkspaceError("WinUI SDK must run its own locked transitive-dependency check before projection")
    items = query["Items"]
    compile_items = items.get("ClCompile", [])
    exemplar = next((item for item in compile_items if item["Identity"] == "__aobus_sdk_interface.cpp"), None)
    if exemplar is None or len(items.get("Link", [])) != 1:
        raise BazelWorkspaceError("WinUI SDK query lacks its independent compile/link interface items")
    sdk_sources = []
    for item in compile_items:
        if item is exemplar:
            continue
        path = Path(item.get("FullPath", item.get("Identity", "")))
        if not path.is_absolute() or path.suffix.lower() != ".cpp":
            raise BazelWorkspaceError("WinUI SDK autoinitializers must be existing provider-owned C++ sources")
        _provider_path(path, packages)
        sdk_sources.append(_owned(path, root))
    if not sdk_sources:
        raise BazelWorkspaceError("WinUI SDK did not supply its required native autoinitializer sources")
    includes = []
    for value in _list(exemplar.get("AdditionalIncludeDirectories", "")):
        path = Path(value)
        if not path.is_absolute() or path.resolve().is_relative_to(tree.resolve()):
            continue  # The private project/generated-header directories are action-owned.
        # Microsoft also advertises optional include subdirectories absent from
        # some restored packages. Preserve that compiler search interface while
        # enforcing provider containment; package traversal watches the parents
        # so later presence changes still invalidate the sparse projection.
        _provider_path(path, packages, allow_missing=True)
        includes.append(path.resolve())
    # ABI/mode macros remain toolchain-owned; the SDK supplies its independent
    # bootstrap/WRL interface, not a second Debug/Release compiler policy.
    definitions = [
        value
        for value in _list(exemplar.get("PreprocessorDefinitions", ""))
        if value.split("=", 1)[0] not in {"_DEBUG", "NDEBUG"}
    ]
    if exemplar.get("ForcedIncludeFiles", "").strip() or any(
        not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:=[A-Za-z0-9_.+-]+)?", value) for value in definitions
    ):
        raise BazelWorkspaceError("unreviewed WinUI SDK forced-include or compiler-definition interface")
    link = items["Link"][0]
    library_dirs = [Path(value).resolve(strict=True) for value in _list(link.get("AdditionalLibraryDirectories", ""))]
    libraries, systems = [], []
    sdk_root = Path(compiler["WindowsSdkDir"]) / "Lib" / WINDOWS_SDK_VERSION / "um/x64"
    for value in _list(link.get("AdditionalDependencies", "")):
        path = Path(value)
        if path.is_absolute():
            _provider_path(path, packages)
            libraries.append(path.resolve(strict=True))
            continue
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\.lib", value, re.IGNORECASE):
            raise BazelWorkspaceError(f"unreviewed WinUI SDK linker token: {value}")
        candidates = list(
            dict.fromkeys(directory / value for directory in library_dirs if (directory / value).is_file())
        )
        if len(candidates) == 1:
            _provider_path(candidates[0], packages)
            libraries.append(candidates[0])
        elif not candidates and (sdk_root / value).is_file():
            systems.append(value)
        else:
            raise BazelWorkspaceError(f"missing or ambiguous WinUI SDK link input: {value}")
    runtime = []
    copy_items = [
        item for item in items.get("Content", []) if item.get("CopyToOutputDirectory") in {"Always", "PreserveNewest"}
    ]
    copy_items.extend(items.get("ReferenceCopyLocalPaths", []))
    for item in copy_items:
        path = Path(item.get("FullPath", item.get("Identity", "")))
        if not path.is_absolute():
            raise BazelWorkspaceError(f"WinUI SDK runtime input must be provider-absolute: {path}")
        path = _owned(path, root)
        _provider_path(path, packages)
        runtime.append(path)
    # Foundation's official resource-copy target owns this executable-local DLL
    # but exposes no Content item. Its observed structural layout uses the
    # evaluated platform and the locked Foundation provider, never a version pin.
    platform = properties.get("_WindowsAppSDKFoundationPlatform")
    foundation = next(
        (path for name, _, path in packages if name.casefold() == "microsoft.windowsappsdk.foundation"), None
    )
    if platform != "x64" or properties.get("WindowsAppSdkBootstrapInitialize") != "true" or foundation is None:
        raise BazelWorkspaceError("WinUI SDK must retain its x64 framework-dependent bootstrap contract")
    runtime.append(
        _owned(foundation / f"runtimes/win-{platform}/native/Microsoft.WindowsAppRuntime.Bootstrap.dll", foundation)
    )
    runtime = list(dict.fromkeys(runtime))
    if len({path.name.casefold() for path in runtime}) != len(runtime):
        raise BazelWorkspaceError("WinUI SDK runtime copy destinations are ambiguous")
    projected: dict[str, Path] = {}

    def add(path: Path) -> str:
        destination = _projected(path, packages)
        previous = projected.setdefault(destination, path.resolve(strict=True))
        if previous != path.resolve(strict=True):
            raise BazelWorkspaceError(f"WinUI SDK projection collision: {destination}")
        return destination

    if properties.get("MSBuildRuntimeType") != "Full":
        raise BazelWorkspaceError("WinUI generation requires its observed native Full-CLR MSBuild task host")
    vc_targets = Path(properties.get("VCTargetsPath", ""))
    if (
        not vc_targets.is_absolute()
        or not vc_targets.is_dir()
        or not vc_targets.resolve().is_relative_to(visual_studio.resolve())
    ):
        raise BazelWorkspaceError("WinUI SDK must select its installed VC targets directory")
    tool_roots = []
    for name in ("XamlCompilerExePath", "MakePriExeFullPath", "CppWinRTPath"):
        spelling = properties.get(name, "").strip('"')
        path = Path(spelling)
        if not spelling or not path.is_absolute():
            raise BazelWorkspaceError(f"WinUI SDK omitted its selected generation tool: {name}")
        _provider_path(path, packages)
        directory = path if name == "CppWinRTPath" else _owned(path, root).parent
        if not directory.is_dir():
            raise BazelWorkspaceError(f"WinUI SDK generation tool directory is missing: {directory}")
        tool_roots.append(directory.resolve())

    def other_architecture(parts: tuple[str, ...]) -> bool:
        return any(
            re.fullmatch(r"(?:win(?:[0-9]+(?:\.[0-9]+)*)?-)?(?:arm(?:64(?:ec)?)?|x86|win32)", part.casefold())
            for part in parts
        )

    headers = []
    tools = []
    metadata_suffixes = {".props", ".targets", ".winmd"}
    build_suffixes = metadata_suffixes | {".xml", ".xsd", ".dll", ".exe", ".config", ".json", ".idl", ".in", ".tt"}
    for _, _, package in packages:
        for path in sorted(package.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(package)
            if other_architecture(relative.parts):
                continue
            suffix = path.suffix.lower()
            header = suffix in {".h", ".hpp", ".inc", ".inl", ".idl"} and any(
                path.is_relative_to(include) for include in includes
            )
            support = (
                suffix in metadata_suffixes
                or (relative.parts[0].casefold() == "build" and suffix in build_suffixes)
                or (
                    any(path.is_relative_to(directory) for directory in tool_roots) and suffix not in {".pdb", ".nupkg"}
                )
            )
            if header or support:
                _owned(path, package)
                destination = add(path)
                if header and suffix != ".idl":
                    headers.append(destination)
                tools.append(destination)
    source_labels = [add(path) for path in sdk_sources]
    tools.extend(source_labels)
    library_kinds = [_windows_coff_archive_kind(path, path.name, request["config"]) for path in libraries]
    library_labels = [{"file": add(path), "kind": kind} for path, kind in zip(libraries, library_kinds, strict=True)]
    runtime_labels = [add(path) for path in runtime]
    # Bound the native input families to the selected VC target/architecture,
    # MIDL's VC preprocessor/runtime/includes, MSBuild host runtime and SDK
    # metadata/includes. Do not mirror all of
    # MSBuild/Microsoft, C++ projections, other tool hosts, or every SDK payload.
    # The original installation remains executable, not a relocated toolchain.
    native_inputs = [msbuild]
    vc = Path(compiler["VCToolsInstallDir"])
    preprocessor = vc / "bin/Hostx64/x64"
    if preprocessor.resolve() != preprocessor.absolute():
        raise BazelWorkspaceError("WinUI MIDL preprocessor must retain its selected Hostx64/x64 installation")
    native_inputs.append(_owned(_probe(preprocessor / "cl.exe"), preprocessor))
    # cl loads its native frontends/CRT from this one host/target directory.
    # Include locale DLLs, but not other tool executables or onecore runtimes.
    for path in sorted(preprocessor.rglob("*")):
        relative = path.relative_to(preprocessor)
        if (
            path.is_file()
            and path.suffix.lower() == ".dll"
            and (len(relative.parts) == 1 or (len(relative.parts) == 2 and relative.parts[0].isdecimal()))
        ):
            native_inputs.append(_owned(path, preprocessor))
    for directory in (vc / "include", vc / "atlmfc/include", vc.parents[2] / "Auxiliary/VS/include"):
        # Probe optional roots too: later installation must invalidate the
        # repository even when the initial header family was absent.
        if _probe(directory).is_dir():
            if not directory.resolve().is_relative_to(visual_studio.resolve()):
                raise BazelWorkspaceError(f"WinUI VC include directory escapes its selected installation: {directory}")
            # VC includes extensionless standard-library headers as well.
            native_inputs.extend(_owned(path, directory) for path in sorted(directory.rglob("*")) if path.is_file())
        elif directory == vc / "include":
            raise BazelWorkspaceError(f"WinUI MIDL requires its selected VC include directory: {directory}")
    runtime_suffixes = {".dll", ".exe", ".config", ".json", ".xml"}
    for path in sorted(msbuild.parent.rglob("*")):
        relative = path.relative_to(msbuild.parent)
        if (
            path.is_file()
            and path.suffix.lower() in runtime_suffixes
            and (len(relative.parts) == 1 or re.fullmatch(r"[A-Za-z]{2}(?:-[A-Za-z0-9]+)?", relative.parts[0]))
        ):
            native_inputs.append(_owned(path, msbuild.parent))
    for path in sorted(vc_targets.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(vc_targets)
        parts = [part.casefold() for part in relative.parts]
        if other_architecture(relative.parts) or (
            "platformtoolsets" in parts and parts[parts.index("platformtoolsets") + 1] != toolset.casefold()
        ):
            continue
        if path.is_file() and path.suffix.lower() in build_suffixes:
            native_inputs.append(_owned(path, vc_targets))
    sdk_directory = Path(compiler["WindowsSdkDir"])
    families = [
        (sdk_directory / name / WINDOWS_SDK_VERSION, {".winmd", ".xml"}) for name in ("References", "UnionMetadata")
    ]
    families.extend(
        (sdk_directory / "Include" / WINDOWS_SDK_VERSION / name, {".h", ".idl", ".acf"})
        for name in ("ucrt", "um", "shared", "winrt")
    )
    families.append((sdk_directory / "bin" / WINDOWS_SDK_VERSION / "x64", runtime_suffixes))
    for directory, suffixes in families:
        if _probe(directory).is_dir():
            native_inputs.extend(
                _owned(path, directory)
                for path in sorted(directory.rglob("*"))
                if path.is_file() and path.suffix.lower() in suffixes
            )
    for value in _list(properties.get("MSBuildAllProjects", "")):
        path = Path(value)
        if path.is_absolute() and path.is_file() and path.resolve().is_relative_to(visual_studio.resolve()):
            native_inputs.append(_owned(path, visual_studio))
            native_inputs.extend(
                _owned(dependency, visual_studio)
                for dependency in sorted(path.parent.iterdir())
                if dependency.is_file() and dependency.suffix.lower() in runtime_suffixes
            )
    links = [{"source": str(path), "destination": destination} for destination, path in sorted(projected.items())]
    for index, path in enumerate(dict.fromkeys(native_inputs)):
        destination = f"native-tools/{index}/{path.name}"
        links.append({"source": str(path.resolve(strict=True)), "destination": destination})
        tools.append(destination)
    execution = {
        "props": [str(path) for path in props],
        "targets": [str(path) for path in targets],
        "msbuild": str(msbuild),
        "platform_toolset": toolset,
        "windows_sdk_version": WINDOWS_SDK_VERSION,
        "environment": {key: value for key, value in env.items() if key not in {"TEMP", "TMP"}},
        "initializer_sources": [str(path) for path in sdk_sources],
        "ml_transitive_dependencies_verified": True,
    }
    diagnostics = {
        **execution,
        "packages_root": str(root),
        "packages": [{"id": name, "version": version, "root": str(path)} for name, version, path in packages],
        "defines": definitions,
        "includes": [str(path) for path in includes],
        "libraries": [{"file": str(path), "kind": kind} for path, kind in zip(libraries, library_kinds, strict=True)],
        "system_libraries": systems,
        "runtime": [str(path) for path in runtime],
        "properties": query["Properties"],
    }
    return {
        "links": links,
        "files": {
            "sdk.json": json.dumps(execution, indent=2, ensure_ascii=False) + "\n",
            "sdk-diagnostics.json": json.dumps(diagnostics, indent=2, ensure_ascii=False) + "\n",
            "BUILD.bazel": _render_repository(
                definitions,
                [_projected(path, packages, allow_missing=True) for path in includes],
                headers,
                source_labels,
                library_labels,
                systems,
                runtime_labels,
                tools,
            ),
        },
    }
