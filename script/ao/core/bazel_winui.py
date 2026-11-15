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

from . import bazel_dependencies as dependencies
from . import bazel_native as native
from . import dependency_policy
from .bazel_native import BazelWorkspaceError
from .paths import PROJECT_ROOT
from .winui import WINDOWS_SDK_VERSION


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
        directory = root / f"{name}.{version}"
        archive = dependency_policy.verify_nuget_package(root, name, version)
        archive.resolve(strict=True)
        packages.append((name, version, directory.resolve()))
    return packages


def _imports(packages: list[tuple[str, str, Path]], extension: str) -> list[Path]:
    result = []
    sequence = packages if extension == "props" else list(reversed(packages))
    for name, _, root in sequence:
        candidates = (root / "build/native" / f"{name}.{extension}", root / "build" / f"{name}.{extension}")
        selected = next((path for path in candidates if native.probe(path).is_file()), None)
        if selected is not None:
            result.append(selected.resolve(strict=True))
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
    evaluator = ET.SubElement(
        project,
        "UsingTask",
        TaskName="AobusEvaluateSdkImports",
        TaskFactory="RoslynCodeTaskFactory",
        AssemblyFile="$(MSBuildToolsPath)\\Microsoft.Build.Tasks.Core.dll",
    )
    parameters = ET.SubElement(evaluator, "ParameterGroup")
    ET.SubElement(parameters, "ProjectPath", ParameterType="System.String", Required="true")
    ET.SubElement(parameters, "GeneratedTaskDirectory", ParameterType="System.String", Required="true")
    for name in ("ImportedProjects", "EvaluatorInputs"):
        ET.SubElement(parameters, name, ParameterType="Microsoft.Build.Framework.ITaskItem[]", Output="true")
    task = ET.SubElement(evaluator, "Task")
    ET.SubElement(task, "Reference", Include="$(MSBuildToolsPath)\\Microsoft.Build.dll")
    # The selected x64 Full-CLR host owns framework references; Roslyn's
    # task factory does not resolve the bare System.Xml name on every install.
    ET.SubElement(task, "Reference", Include="$(MSBuildFrameworkToolsPath64)System.Xml.dll")
    for namespace in ("System", "System.IO", "System.Linq", "System.Collections.Generic", "Microsoft.Build.Evaluation"):
        ET.SubElement(task, "Using", Namespace=namespace)
    ET.SubElement(task, "Code", Type="Fragment", Language="cs").text = """
// The executable query has no /p arguments. Re-evaluate only: no targets,
// InitialTargets, restore or provisioning in this independent collection.
using (var collection = new ProjectCollection())
{
    var evaluated = new Project(ProjectPath, new Dictionary<string, string>(), null, collection);
    ImportedProjects = evaluated.Imports
        .Select(import => new Microsoft.Build.Utilities.TaskItem(import.ImportedProject.FullPath)).ToArray();
    collection.UnloadAllProjects();
}
// Inline-task compiler outputs under our private TEMP are derived from this
// inventoried project, not external dependencies (and may already be deleted).
var generated = Path.GetFullPath(GeneratedTaskDirectory)
    .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
EvaluatorInputs = AppDomain.CurrentDomain.GetAssemblies().Where(assembly => !assembly.IsDynamic)
    .Select(assembly => assembly.Location)
    .Where(path => !String.IsNullOrEmpty(path)
        && !Path.GetFullPath(path).StartsWith(generated, StringComparison.OrdinalIgnoreCase))
    .Concat(new[] { Environment.GetEnvironmentVariable("MSBUILD_EXE_PATH") + ".config" })
    .Distinct(StringComparer.OrdinalIgnoreCase)
    .Select(path => new Microsoft.Build.Utilities.TaskItem(path)).ToArray();
"""
    interface = ET.SubElement(
        project,
        "Target",
        Name="AobusSdkInterface",
        DependsOnTargets="WindowsAppRuntimeAutoInitializer;GenerateBootstrapCpp;WindowsMLAutoInitializer",
    )
    invocation = ET.SubElement(
        interface,
        "AobusEvaluateSdkImports",
        ProjectPath="$(MSBuildProjectFullPath)",
        GeneratedTaskDirectory="$(TEMP)",
    )
    ET.SubElement(invocation, "Output", TaskParameter="ImportedProjects", ItemName="AobusEvaluatedImport")
    ET.SubElement(invocation, "Output", TaskParameter="EvaluatorInputs", ItemName="AobusEvaluatorInput")
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
    msbuild = (visual_studio / "MSBuild/Current/Bin/amd64/MSBuild.exe").resolve(strict=True)
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


def _evaluated_imports(project: Path, msbuild: Path, query: dict[str, Any]) -> tuple[list[Path], dict[str, Any]]:
    """Admit the inline evaluator's facts from the selected MSBuild.exe host."""
    native.probe(project)
    try:
        imports: list[Path] = []
        dependencies: list[Path] = []
        for name, paths in (("AobusEvaluatedImport", imports), ("AobusEvaluatorInput", dependencies)):
            for record in query["Items"][name]:
                # No sparse family/architecture filter on actual imports or
                # loaded dependencies, including selected binding redirects.
                paths.append(native.probe(Path(record["Identity"])).resolve(strict=True))
        # The selected amd64 host can bind its assemblies from parent Bin.
        # Require actual loaded files from that installation, not adjacency.
        bins = {msbuild.parent, msbuild.parent.parent}
        assemblies = ("Microsoft.Build.dll", "Microsoft.Build.Tasks.Core.dll")
        if msbuild.with_name(msbuild.name + ".config") not in dependencies or any(
            not any(path.parent in bins and path.name.casefold() == name.casefold() for path in dependencies)
            for name in assemblies
        ):
            raise ValueError("selected MSBuild assemblies/config were not inventoried")
    except (ValueError, OSError, RuntimeError) as exc:
        raise BazelWorkspaceError(f"WinUI evaluated imports query returned invalid evidence: {exc}") from exc
    sources = [Path(__file__).resolve(), Path(__file__).resolve().parents[3] / "app/windows-winui/msbuild_project.py"]
    # Query project/source identities are provenance, not generation tools.
    # Actual action helpers are declared by their WinUI rules independently.
    inputs = list(dict.fromkeys([*dependencies, *imports]))
    evidence = {
        "project": native.evidence([project])[0],
        "sources": native.evidence(sources),
        "global_properties": {},
        "imports": native.evidence(list(dict.fromkeys(imports))),
        "evaluator": native.evidence(list(dict.fromkeys(dependencies))),
        # Project.Imports reports successful, condition-true imports, not every
        # Exists/property-function read or empty wildcard search. Directory
        # observations below and existing provider traversals supplement it;
        # they do not establish a hermetic installation or syscall inventory.
        "observation_boundary": (
            "resolved imports and observed provider directories; not all missing/wildcard or evaluation-time reads"
        ),
    }
    return inputs, evidence


def resolve_sdk(tree: Path, request: dict[str, Any], environment: Mapping[str, str]) -> dict[str, Any] | None:
    """Admit native SDK observations; no Bazel program or destination names."""
    if request["host"] != "windows":
        return None
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
            "WinUI requires a restored NuGet cache: run `ao.bat deps provision`, "
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
    env.update(
        {
            "TEMP": str(tree / "tmp"),
            "TMP": str(tree / "tmp"),
            "MSBUILDDISABLENODEREUSE": "1",
            # The inline evaluator uses this executable's actual binding policy.
            "MSBUILD_EXE_PATH": str(msbuild),
        }
    )
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
            "/getProperty:VCToolsInstallDir,WindowsSdkDir,WindowsTargetPlatformVersion,MakePriExeFullPath,XamlCompilerExePath,_WindowsAppSDKFoundationPlatform,WindowsAppSdkBootstrapInitialize,WindowsAppSDKMLVerifyTransitiveDependencies,MSBuildRuntimeType,VCTargetsPath,CppWinRTPath,MSBuildProjectFullPath",
            "/getItem:ClCompile,Link,Content,ReferenceCopyLocalPaths,AobusEvaluatedImport,AobusEvaluatorInput",
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
    evaluated_inputs, import_evidence = _evaluated_imports(project, msbuild, query)
    properties = query["Properties"]
    items = query["Items"]
    compile_items = items.get("ClCompile", [])
    exemplar = next(item for item in compile_items if item["Identity"] == "__aobus_sdk_interface.cpp")
    sdk_sources = []
    for item in compile_items:
        if item is exemplar:
            continue
        path = Path(item.get("FullPath", item.get("Identity", "")))
        _provider_path(path, packages)
        sdk_sources.append(path.resolve(strict=True))
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
        path = Path(item.get("FullPath", item.get("Identity", ""))).resolve(strict=True)
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
        (foundation / f"runtimes/win-{platform}/native/Microsoft.WindowsAppRuntime.Bootstrap.dll").resolve(strict=True)
    )
    runtime = list(dict.fromkeys(runtime))
    if len({path.name.casefold() for path in runtime}) != len(runtime):
        raise BazelWorkspaceError("WinUI SDK runtime copy destinations are ambiguous")
    files: dict[str, dict[str, str]] = {}

    def add(path: Path) -> str:
        source = str(path.resolve(strict=True))
        package, relative = _provider_path(path, packages)
        files[source] = {"source": source, "package": package, "path": relative.as_posix()}
        return source

    vc_targets = Path(properties.get("VCTargetsPath", ""))
    tool_roots = []
    for name in ("XamlCompilerExePath", "MakePriExeFullPath", "CppWinRTPath"):
        path = Path(properties[name].strip('"'))
        _provider_path(path, packages)
        directory = path if name == "CppWinRTPath" else path.resolve(strict=True).parent
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
                path.resolve(strict=True)
                destination = add(path)
                if header and suffix != ".idl":
                    headers.append(destination)
                tools.append(destination)
    source_files = [add(path) for path in sdk_sources]
    tools.extend(source_files)
    library_kinds = [dependencies.coff_archive_kind(path, path.name, request["config"]) for path in libraries]
    library_files = [{"file": add(path), "kind": kind} for path, kind in zip(libraries, library_kinds, strict=True)]
    runtime_files = [add(path) for path in runtime]
    # Bound the native input families to the selected VC target/architecture,
    # MIDL's VC preprocessor/runtime/includes, MSBuild host runtime and SDK
    # metadata/includes. Do not mirror all of
    # MSBuild/Microsoft, C++ projections, other tool hosts, or every SDK payload.
    # The original installation remains executable, not a relocated toolchain.
    native_inputs = [msbuild, *evaluated_inputs]
    vc = Path(compiler["VCToolsInstallDir"])
    preprocessor = vc / "bin/Hostx64/x64"
    if preprocessor.resolve() != preprocessor.absolute():
        raise BazelWorkspaceError("WinUI MIDL preprocessor must retain its selected Hostx64/x64 installation")
    native_inputs.append(native.probe(preprocessor / "cl.exe").resolve(strict=True))
    # cl loads its native frontends/CRT from this one host/target directory.
    # Include locale DLLs, but not other tool executables or onecore runtimes.
    for path in sorted(preprocessor.rglob("*")):
        relative = path.relative_to(preprocessor)
        if (
            path.is_file()
            and path.suffix.lower() == ".dll"
            and (len(relative.parts) == 1 or (len(relative.parts) == 2 and relative.parts[0].isdecimal()))
        ):
            native_inputs.append(path.resolve(strict=True))
    for directory in (vc / "include", vc / "atlmfc/include", vc.parents[2] / "Auxiliary/VS/include"):
        # Probe optional roots too: later installation must invalidate the
        # repository even when the initial header family was absent.
        if native.probe(directory).is_dir():
            # VC includes extensionless standard-library headers as well.
            native_inputs.extend(path.resolve(strict=True) for path in sorted(directory.rglob("*")) if path.is_file())
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
            native_inputs.append(path.resolve(strict=True))
    # RoslynCodeTaskFactory compiles only our inline evaluator, not product
    # code. Inventory the selected installation's compiler families, including
    # response files and configs, without selecting another compiler ourselves.
    for directory in (msbuild.parent / "Roslyn", msbuild.parent.parent / "Roslyn"):
        if native.probe(directory).is_dir():
            native_inputs.extend(path.resolve(strict=True) for path in sorted(directory.rglob("*")) if path.is_file())
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
            native_inputs.append(path.resolve(strict=True))
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
        if native.probe(directory).is_dir():
            native_inputs.extend(
                path.resolve(strict=True)
                for path in sorted(directory.rglob("*"))
                if path.is_file() and path.suffix.lower() in suffixes
            )
    for record in import_evidence["imports"]:
        path = Path(record["path"])
        # Observe imported-directory membership (including wildcard additions
        # in these established directories) and adjacent installed task hosts.
        siblings = sorted(path.parent.iterdir())
        if path.is_relative_to(visual_studio.resolve()):
            native_inputs.extend(
                dependency.resolve(strict=True)
                for dependency in siblings
                if dependency.is_file() and dependency.suffix.lower() in runtime_suffixes
            )
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
        "import_evaluation": import_evidence,
    }
    include_roots = []
    for path in includes:
        package_name, relative = _provider_path(path, packages, allow_missing=True)
        include_roots.append({"package": package_name, "path": relative.as_posix()})
    return {
        "sdk_files": list(files.values()),
        "includes": include_roots,
        "defines": definitions,
        "headers": headers,
        "sources": source_files,
        "libraries": library_files,
        "system_libraries": systems,
        "runtime": runtime_files,
        "tools": tools,
        "native_inputs": [str(path.resolve(strict=True)) for path in dict.fromkeys(native_inputs)],
        "execution": execution,
        "diagnostics": diagnostics,
    }
