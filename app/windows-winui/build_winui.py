# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
"""Run WinUI SDK generation, PRI-only generation, or executable-local assembly.

The local Starlark rules supply declared paths, not a copied checkout or a second
SDK registry. Generate publishes C++ text through --headers, XBFs through --xbf,
and --cpp ROLE PATH wrappers for module, metadata, types and types_impl; --private
retains the SDK diagnostics and intermediates. Stage selects executable/PRI
basenames with --identity (Aobus by default) and takes --runtime SOURCE
DESTINATION for each SDK payload file.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from types import ModuleType
from typing import Any

_UNITS = {
    "module": "module.g.cpp",
    "metadata": "XamlMetaDataProvider.cpp",
    "types": "XamlTypeInfo.g.cpp",
    "types_impl": "XamlTypeInfo.Impl.g.cpp",
}
_ASSETS = {
    "SoulMark.svg": "Assets/Brand/SoulMark.svg",
    "LICENSE.txt": "Assets/Brand/LICENSE.txt",
    "note.svg": "Assets/NoCover/note.svg",
    "vinyl.svg": "Assets/NoCover/vinyl.svg",
    "equalizer.svg": "Assets/NoCover/equalizer.svg",
    "windows_modern_layout.yaml": "Assets/Layout/windows_modern_layout.yaml",
    "windows_classic_layout.yaml": "Assets/Layout/windows_classic_layout.yaml",
}
_XBF = ("App.xbf", "MainWindow.xbf")
_LOCALE = re.compile(r"[A-Za-z]{2,8}(?:-[A-Za-z0-9]{2,8})*\Z", re.ASCII)
_FORBIDDEN_TARGETS = "ClCompile;_ClCompile;Lib;_Lib;Link;_Link;Build;Restore;Rebuild;Clean;XamlPreCompile"


def absolute(value: str) -> Path:
    return Path(os.path.abspath(value))


def _native(path: Path) -> str:
    value = str(path)
    if any(character in value for character in ('"', "$", "@", "%", ";", "\r", "\n")):
        raise ValueError(f"Unsupported MSBuild path spelling: {path}")
    return value.replace("/", "\\")


def _item_path(value: str, project: Path) -> Path:
    path = Path(value.replace("\\", os.sep))
    return path if path.is_absolute() else project / path


def _file(path: Path) -> Path:
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f"Missing or empty declared WinUI input: {path}")
    return path


def _owned_file(root: Path, relative: str) -> Path:
    path = root / relative
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"WinUI file escapes its declared tree: {path}")
    return _file(path)


def _key(path: Path) -> str:
    return str(path.resolve()).casefold()


def validate_outputs(outputs: list[Path], inputs: list[Path]) -> None:
    """Reject tree/file overlap and input aliases before creating any outputs."""
    for index, output in enumerate(outputs):
        if output.is_symlink():
            raise ValueError(f"Unsafe WinUI output symlink: {output}")
        resolved = Path(_key(output))
        for other in outputs[index + 1 :] + inputs:
            candidate = Path(_key(other))
            if resolved.is_relative_to(candidate) or candidate.is_relative_to(resolved):
                raise ValueError(f"Overlapping WinUI paths: {output}, {other}")


def _strings(document: dict[str, Any], name: str) -> list[str]:
    value = document.get(name)
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise ValueError(f"WinUI SDK metadata requires {name}")
    return value


def read_sdk(path: Path) -> dict[str, Any]:
    """Read the action protocol; full SDK observations stay in repository diagnostics."""
    document = json.loads(_file(path).read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("ml_transitive_dependencies_verified") is not True:
        raise ValueError("WinUI generation requires the SDK's verified transitive-dependency receipt")
    for name in ("msbuild", "platform_toolset", "windows_sdk_version"):
        if not isinstance(document.get(name), str) or not document[name]:
            raise ValueError(f"WinUI SDK metadata requires {name}")
    for name in ("props", "targets", "initializer_sources"):
        values = _strings(document, name)
        if not values:
            raise ValueError(f"WinUI SDK metadata has no {name}")
        for value in values:
            if not Path(value).is_absolute():
                raise ValueError(f"WinUI SDK input must be an installed absolute path: {value}")
            _file(Path(value))
    if not Path(document["msbuild"]).is_absolute():
        raise ValueError("WinUI SDK MSBuild must be an installed absolute path")
    _file(Path(document["msbuild"]))
    environment = document.get("environment")
    if not isinstance(environment, dict) or any(
        not isinstance(key, str) or not isinstance(value, str) for key, value in environment.items()
    ):
        raise ValueError("WinUI SDK metadata requires its selected toolchain environment")
    return document


def _sdk_inputs(sdk: dict[str, Any]) -> list[Path]:
    return [
        absolute(sdk["msbuild"]),
        *[absolute(value) for name in ("props", "targets", "initializer_sources") for value in sdk[name]],
    ]


def _element(parent: ET.Element, name: str, value: str | None = None, **attributes: str) -> ET.Element:
    child = ET.SubElement(parent, name, attributes)
    child.text = value
    return child


def _properties(parent: ET.Element, values: dict[str, str], **attributes: str) -> None:
    group = _element(parent, "PropertyGroup", **attributes)
    for name, value in values.items():
        _element(group, name, value)


def _project_builder() -> ModuleType:
    # Keep -I/-S isolation: load the sibling declared input, not an ambient module.
    path = Path(__file__).with_name("msbuild_project.py")
    module = ModuleType("aobus_msbuild_project")
    # Reading the source explicitly also avoids undeclared bytecode-cache inputs.
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), module.__dict__)
    return module


def project_template(sdk: dict[str, Any], configuration: str, *, generate: bool) -> ET.Element:
    """Keep action-only policy outside the shared canonical native VC spine."""
    project: ET.Element = _project_builder().project(
        configuration=configuration,
        toolset=sdk["platform_toolset"],
        sdk_version=sdk["windows_sdk_version"],
        configuration_type="StaticLibrary" if generate else "Application",
        initial_properties={
            "BuildProjectReferences": "false",
            # Only read_sdk's verified SDK receipt permits omitting the archive scan.
            "WindowsAppSDKMLVerifyTransitiveDependencies": "false",
        },
        global_properties={"AppContainerApplication": "false", "DesktopCompatible": "true"},
        project_properties={
            "IntDir": "int\\" + configuration + "\\",
            "GeneratedFilesDir": "$(ProjectDir)headers\\",
            **({} if generate else {"CppWinRTAddXamlMetaDataProviderIdl": "false"}),
        },
        props=[_native(absolute(path)) for path in sdk["props"]],
    )
    return project


def _finish_project(project: ET.Element, sdk: dict[str, Any]) -> None:
    _project_builder().finish(project, [_native(absolute(path)) for path in sdk["targets"]])
    _properties(
        project,
        {
            "WindowsAppContainer": "false",
            "AppxPackage": "false",
            "WindowsPackageType": "None",
        },
    )
    guard = _element(project, "Target", Name="AobusNoProductBuild", BeforeTargets=_FORBIDDEN_TARGETS)
    _element(guard, "Error", Text="WinUI SDK action forbids product compile/link/build/restore/cleanup.")


def _write_project(project: ET.Element, private: Path) -> Path:
    private.mkdir(parents=True, exist_ok=True)
    path = private / "Aobus.vcxproj"
    ET.indent(project)
    ET.ElementTree(project).write(path, encoding="utf-8", xml_declaration=True)
    return path


def _execute(sdk: dict[str, Any], private: Path, configuration: str, target: str, *, query: bool = False) -> str:
    # Full-CLR MSBuild Exec appends GUID directories and a batch filename.
    # Keep its private TEMP short enough for the native legacy path limit.
    temp = private / "t"
    temp.mkdir(parents=True, exist_ok=True)
    environment = {key.upper(): value for key, value in sdk["environment"].items()}
    environment.update(
        {
            "TEMP": str(temp),
            "TMP": str(temp),
            "TMPDIR": str(temp),
            "PYTHONDONTWRITEBYTECODE": "1",
            "MSBUILDDISABLENODEREUSE": "1",
        }
    )
    arguments = [
        sdk["msbuild"],
        str(private / "Aobus.vcxproj"),
        "/nologo",
        "/noautoresponse",
        "/v:quiet",
        "/m:1",
        "/nr:false",
        "/p:Configuration=" + configuration,
        "/p:Platform=x64",
        "/p:BuildProjectReferences=false",
        "/t:" + target,
        "/fl",
        "/flp:logfile=" + str(private / "msbuild.log") + ";verbosity=normal;encoding=UTF-8",
    ]
    if query:
        arguments.extend(
            [
                "/getItem:ClCompile",
                "/getProperty:GeneratedFilesDir,IntDir,OutDir",
            ]
        )
    result = subprocess.run(arguments, cwd=private, env=environment, capture_output=True, check=False)
    (private / "msbuild.stdout.txt").write_bytes(result.stdout)
    (private / "msbuild.stderr.txt").write_bytes(result.stderr)
    (private / "msbuild.exit.txt").write_text(str(result.returncode) + "\n", encoding="utf-8")
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, arguments, result.stdout, result.stderr)
    return result.stdout.decode("utf-8-sig")


def _query_json(text: str) -> dict[str, Any]:
    for match in re.finditer(r"(?m)^\s*\{", text):
        try:
            document, _ = json.JSONDecoder().raw_decode(text[match.start() :].lstrip())
        except json.JSONDecodeError:
            continue
        if isinstance(document, dict) and "Items" in document and "Properties" in document:
            return document
    raise ValueError("WinUI SDK generation returned no postgeneration compile metadata")


def validate_generated(private: Path, sources: list[Path], sdk: dict[str, Any], query: dict[str, Any]) -> None:
    expected = [*sources, *(absolute(value) for value in sdk["initializer_sources"])]
    expected.extend(_owned_file(private, "headers/" + name) for name in _UNITS.values())
    expected_keys = {_key(path) for path in expected}
    if len(expected_keys) != len(expected):
        raise ValueError("Duplicate authored, SDK initializer or generated compile input")
    actual: set[str] = set()
    for item in query.get("Items", {}).get("ClCompile", []):
        value = item.get("FullPath", item.get("Identity"))
        if not isinstance(value, str) or item.get("ExcludedFromBuild", "").lower() == "true":
            raise ValueError("Invalid or excluded WinUI postgeneration compile input")
        path = _file(_item_path(value, private))
        key = _key(path)
        if key in actual or key not in expected_keys:
            raise ValueError(f"Unexpected or duplicate standalone WinUI compile input: {path}")
        actual.add(key)
    if actual != expected_keys:
        raise ValueError("WinUI standalone/SDK initializer compile membership changed")
    for name, relative in (("GeneratedFilesDir", "headers"), ("OutDir", "out")):
        value = query.get("Properties", {}).get(name)
        if not isinstance(value, str) or _key(_item_path(value, private)) != _key(private / relative):
            raise ValueError(f"WinUI SDK output escaped its private {name}")
    int_dir = query.get("Properties", {}).get("IntDir")
    if not isinstance(int_dir, str):
        raise ValueError("WinUI SDK omitted its private IntDir")
    intermediate = Path(int_dir.replace("\\", os.sep))
    if intermediate.is_absolute() or not (private / intermediate).resolve().is_relative_to(private.resolve()):
        raise ValueError("WinUI SDK IntDir must remain private and relative")
    for name in (
        *_XBF,
        "App.xaml.g.h",
        "App.xaml.g.hpp",
        "MainWindow.xaml.g.h",
        "MainWindow.xaml.g.hpp",
        "XamlTypeInfo.xaml.g.h",
        "XamlMetaDataProvider.h",
        "XamlMetaDataProvider.g.h",
        "XamlMetaDataProvider.g.cpp",
        "XamlBindingInfo.xaml.g.h",
        "XamlBindingInfo.xaml.g.hpp",
    ):
        _owned_file(private, "headers/" + name)


def generate(options: argparse.Namespace, sdk: dict[str, Any]) -> None:
    private = _required(options, "private")
    payload = _required(options, "headers")
    xbf = options.xbf
    if sorted(path.name for path in xbf) != sorted(_XBF):
        raise ValueError("WinUI generation requires separate App.xbf and MainWindow.xbf outputs")
    _native(private)
    idl = _file(_required(options, "idl"))
    sources = [_file(path) for path in options.source]
    headers = [_file(path) for path in options.header]
    xaml = [_file(path) for path in options.xaml]
    if not sources or len({_key(path) for path in sources}) != len(sources):
        raise ValueError("WinUI generation requires unique authored sources")
    if sorted(path.name for path in xaml) != ["App.xaml", "MainWindow.xaml"]:
        raise ValueError("WinUI generation requires the authored App and MainWindow XAML")
    wrappers: dict[str, Path] = {}
    for role, spelling in options.cpp:
        if role not in _UNITS or role in wrappers:
            raise ValueError(f"Unexpected or duplicate WinUI wrapper role: {role}")
        wrappers[role] = absolute(spelling)
    if set(wrappers) != set(_UNITS):
        raise ValueError("WinUI generation requires four separate wrapper outputs")
    validate_outputs(
        [private, payload, *wrappers.values(), *xbf],
        [options.sdk, idl, *sources, *headers, *xaml, *_sdk_inputs(sdk)],
    )
    project = project_template(sdk, options.configuration, generate=True)
    definitions = _element(project, "ItemDefinitionGroup")
    midl = _element(definitions, "Midl")
    _element(midl, "OutputDirectory", "$(ProjectDir)")
    compile_options = _element(definitions, "ClCompile")
    _element(
        compile_options,
        "AdditionalIncludeDirectories",
        ";".join(
            [
                _native(idl.parent),
                _native(idl.parent / "include"),
                "%(AdditionalIncludeDirectories)",
            ]
        ),
    )
    _element(compile_options, "LanguageStandard", "stdcpplatest")
    _element(compile_options, "PrecompiledHeader", "NotUsing")
    _element(compile_options, "PrecompiledHeaderFile", "pch.h")
    items = _element(project, "ItemGroup")
    item = _element(items, "Midl", Include=_native(idl))
    _element(item, "OutputDirectory", "$(ProjectDir)")
    for path in xaml:
        item = _element(items, "ApplicationDefinition" if path.name == "App.xaml" else "Page", Include=_native(path))
        _element(item, "Link", path.name)
    for path in sources:
        item = _element(items, "ClCompile", Include=_native(path))
        if path.name in {"App.xaml.cpp", "MainWindow.xaml.cpp"}:
            _element(item, "DependentUpon", path.name.removesuffix(".cpp"))
    _element(items, "ClCompile", Include="$(GeneratedFilesDir)module.g.cpp")
    for path in headers:
        item = _element(items, "ClInclude", Include=_native(path))
        # The XAML compiler uses the header relationship, not just the C++
        # source relationship, to emit partial-class and application includes.
        if path.name in {"App.xaml.h", "MainWindow.xaml.h"}:
            _element(item, "DependentUpon", path.name.removesuffix(".h"))
    _finish_project(project, sdk)
    normalize = _element(project, "Target", Name="AobusNormalizeMidl", BeforeTargets="Midl;GetCppWinRTMdMergeInputs")
    update = _element(_element(normalize, "ItemGroup"), "Midl", Update="@(Midl)")
    _element(update, "OutputDirectory", "$(ProjectDir)")
    _element(
        project,
        "Target",
        Name="AobusGenerate",
        DependsOnTargets=(
            "_BuildGenerateSourcesAction;MarkupCompilePass1;$(BeforeClCompileTargets);"
            "$(ComputeCompileInputsTargets);MarkupCompilePass2"
        ),
    )
    _write_project(project, private)
    query = _query_json(_execute(sdk, private, options.configuration, "AobusGenerate", query=True))
    validate_generated(private, sources, sdk, query)
    (private / "postgeneration.json").write_text(json.dumps(query, indent=2) + "\n", encoding="utf-8")
    # Only generated C++ text belongs to the compilation context. Keep the
    # SDK's logs, caches, metadata and XBFs out of that consumer tree.
    generated = private / "headers"
    copies = []
    for path in sorted(generated.rglob("*")):
        if path.is_file() and path.suffix in {".h", ".hpp", ".cpp"}:
            if not path.resolve().is_relative_to(generated.resolve()):
                raise ValueError(f"WinUI generated C++ input escapes its private header tree: {path}")
            output = payload / path.relative_to(generated)
            if output.is_symlink() or not output.resolve().is_relative_to(payload.resolve()):
                raise ValueError(f"WinUI generated C++ output escapes its declared header tree: {output}")
            copies.append((path, output))
    copies.extend((_owned_file(private, "headers/" + output.name), output) for output in xbf)
    for source, output in copies:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)
    for role, output in wrappers.items():
        native = _owned_file(payload, _UNITS[role])
        include = os.path.relpath(native, output.parent).replace("\\", "/")
        if '"' in include or "\n" in include:
            raise ValueError(f"Unsafe generated C++ include path: {include}")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            f'// Generated WinUI translation-unit owner. Do not edit.\n#include "pch.h"\n#include "{include}"\n',
            encoding="utf-8",
            newline="\n",
        )


def resource_aliases(paths: list[Path]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for path in paths:
        _file(path)
        if path.name not in _ASSETS or path.name in result:
            raise ValueError(f"Unexpected or duplicate WinUI content alias: {path}")
        result[path.name] = path
    if set(result) != set(_ASSETS):
        raise ValueError("Missing declared WinUI content asset")
    return {_ASSETS[name]: result[name] for name in _ASSETS}


def catalog_resources(catalog: Path) -> list[tuple[str, Path]]:
    """The catalog action's intermediates tree already is its private directory."""
    root = catalog / "resw"
    if not root.is_dir() or not root.resolve().is_relative_to(catalog.resolve()):
        raise ValueError(f"Missing or escaped catalog RESW tree: {root}")
    result: list[tuple[str, Path]] = []
    tags: set[str] = set()
    for directory in sorted(root.iterdir()):
        tag = directory.name
        if not directory.is_dir() or not _LOCALE.fullmatch(tag) or tag.casefold() in tags:
            raise ValueError(f"Unsafe or duplicate catalog RESW locale: {tag}")
        tags.add(tag.casefold())
        result.append((tag, _owned_file(catalog, f"resw/{tag}/Resources.resw")))
    if "en" not in tags:
        raise ValueError("WinUI PRI requires its English default RESW")
    return result


def resources(options: argparse.Namespace, sdk: dict[str, Any]) -> None:
    private = _required(options, "private")
    _native(private)
    xbf = options.xbf
    if sorted(path.name for path in xbf) != sorted(_XBF):
        raise ValueError("WinUI resources require separate App.xbf and MainWindow.xbf inputs")
    catalog = _required(options, "catalog")
    aliases = resource_aliases(options.asset)
    aliases.update({path.name: _file(path) for path in xbf})
    resw = catalog_resources(catalog)
    validate_outputs([private], [options.sdk, catalog, *xbf, *options.asset, *_sdk_inputs(sdk)])
    project = project_template(sdk, options.configuration, generate=False)
    _finish_project(project, sdk)
    items = _element(project, "ItemGroup")
    # Runtime deployment is owned by the SDK repository, not the application's PRI.
    _element(items, "Content", Remove="@(Content)")
    for alias, path in aliases.items():
        item = _element(items, "Content", Include=_native(path))
        _element(item, "Link", alias.replace("/", "\\"))
        _element(item, "TargetPath", alias.replace("/", "\\"))
    for tag, path in resw:
        item = _element(items, "PRIResource", Include=_native(path))
        _element(item, "Link", f"Resources\\{tag}\\Resources.resw")
    _properties(
        project,
        {
            "DefaultResourceLanguage": "en",
            "DisableEmbeddedXbf": "true",
            "InsertReverseMap": "false",
        },
    )
    stage = _element(project, "Target", Name="AobusStageResources")
    _element(stage, "MakeDir", Directories="$(IntDir);$(OutDir)")
    item = _element(_element(stage, "ItemGroup"), "PackagingOutputs", Include="@(Content)")
    _element(item, "TargetPath", "%(Content.TargetPath)")
    _element(item, "OutputGroup", "ContentFilesProjectOutputGroup")
    _element(item, "ProjectName", "$(ProjectName)")
    _element(stage, "Copy", SourceFiles="@(Content)", DestinationFiles="@(Content->'$(OutDir)%(TargetPath)')")
    _element(
        project,
        "Target",
        Name="AobusPri",
        DependsOnTargets=("AobusStageResources;_GetSdkToolPaths;_GenerateProjectPriFile"),
    )
    _write_project(project, private)
    _execute(sdk, private, options.configuration, "AobusPri")
    for name in ("Aobus.pri", *_XBF, *_ASSETS.values()):
        _owned_file(private, "out/" + name)


def stage(options: argparse.Namespace) -> None:
    identity = options.identity
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_-]*", identity, flags=re.ASCII):
        raise ValueError(f"Unsupported WinUI executable identity: {identity}")
    executable = _file(_required(options, "exe"))
    resource_tree = _required(options, "resources")
    destinations = [
        _required(options, name)
        for name in (
            "output_exe",
            "output_pri",
            "output_app_xbf",
            "output_window_xbf",
            "output_assets",
        )
    ]
    if [path.name for path in destinations] != [identity + ".exe", identity + ".pri", *_XBF, "Assets"]:
        raise ValueError("Unexpected executable-local WinUI output names")
    if len({_key(path.parent) for path in destinations}) != 1:
        raise ValueError("WinUI executable and resources must have adjacent output paths")
    payload = resource_tree / "out"
    files = [executable, *[_owned_file(resource_tree, "out/" + name) for name in ("Aobus.pri", *_XBF)]]
    assets = {alias.removeprefix("Assets/"): _owned_file(resource_tree, "out/" + alias) for alias in _ASSETS.values()}
    actual_assets = {
        path.relative_to(payload / "Assets").as_posix() for path in (payload / "Assets").rglob("*") if path.is_file()
    }
    if actual_assets != set(assets):
        raise ValueError("Unexpected WinUI resource asset layout")
    for relative in assets:
        output = destinations[4] / relative
        if output.is_symlink() or not output.resolve().is_relative_to(destinations[4].resolve()):
            raise ValueError(f"WinUI asset output escapes its declared tree: {output}")
    copies = list(zip(files, destinations[:4], strict=True))
    names = {path.name.casefold() for path in destinations}
    for source_value, output_value in options.runtime:
        source, output = _file(absolute(source_value)), absolute(output_value)
        if output.parent != destinations[0].parent or source.name != output.name or output.name.casefold() in names:
            raise ValueError(f"Colliding or misplaced WinUI runtime output: {output}")
        names.add(output.name.casefold())
        copies.append((source, output))
    validate_outputs(
        [*destinations, *[output for _, output in copies[4:]]],
        [resource_tree, *[source for source, _ in copies]],
    )
    for source, output in copies:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)
    destinations[0].chmod(0o755)
    for relative, source in assets.items():
        output = destinations[4] / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)


def _required(options: argparse.Namespace, name: str) -> Path:
    value = getattr(options, name)
    if not isinstance(value, Path):
        raise ValueError(f"WinUI {options.action} requires --{name.replace('_', '-')}")
    return value


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    # Bazel's multiline parameter file carries one literal argument per line;
    # argparse's default conversion deliberately performs no shell tokenization.
    parser = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars="@")
    parser.add_argument("--action", required=True, choices=("generate", "resources", "stage"))
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--identity", default="Aobus")
    for name in (
        "sdk",
        "private",
        "headers",
        "idl",
        "catalog",
        "exe",
        "resources",
        "output_exe",
        "output_pri",
        "output_app_xbf",
        "output_window_xbf",
        "output_assets",
    ):
        parser.add_argument("--" + name.replace("_", "-"), dest=name, type=absolute)
    for name in ("source", "header", "xaml", "asset", "xbf"):
        parser.add_argument("--" + name, action="append", default=[], type=absolute)
    parser.add_argument("--cpp", nargs=2, action="append", default=[], metavar=("ROLE", "OUTPUT"))
    parser.add_argument("--runtime", nargs=2, action="append", default=[], metavar=("SOURCE", "OUTPUT"))
    return parser.parse_args(argv)


def run(options: argparse.Namespace) -> None:
    if options.action == "stage":
        stage(options)
    else:
        sdk = read_sdk(_required(options, "sdk"))
        if options.action == "generate":
            generate(options, sdk)
        else:
            resources(options, sdk)


def main() -> int:
    try:
        run(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"WinUI action failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
