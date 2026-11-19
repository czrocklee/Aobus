# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
"""Run WinUI SDK generation, PRI-only generation, or executable-local assembly.

The local Starlark rules supply declared paths, not a copied checkout or a second
SDK registry. Generate publishes project C++ text through --headers, external
C++/WinRT projections through --sdk-headers, XBFs through --xbf, and wrappers
through --cpp ROLE PATH; --private retains the SDK diagnostics and intermediates.
--language-standard forwards the Bzl MSVC policy into the generated project. Resources publishes
--output-pri separately from its private MSBuild tree. Stage consumes that --pri
and the original --xbf/--asset files, selects basenames with --identity (Aobus by
default), and takes --runtime SOURCE DESTINATION for each runtime file.
"""

from __future__ import annotations

import argparse
import json
import ntpath
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from types import ModuleType
from typing import Any, cast

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
_SDK_PROJECTION = re.compile(r"winrt/((?:Microsoft|Windows)(?:\.[A-Za-z_][A-Za-z0-9_]*)+)\.h")
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


def read_sdk(path: Path) -> dict[str, Any]:
    """Read the action protocol; full SDK observations stay in repository diagnostics."""
    return cast(dict[str, Any], json.loads(path.read_text(encoding="utf-8")))


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
                "/getItem:ClCompile,ClInclude",
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
    raise ValueError("WinUI SDK generation returned no postgeneration item metadata")


def _validate_items(private: Path, kind: str, items: list[tuple[Path, str]], query: dict[str, Any]) -> None:
    expected = {_key(path): dependent_upon for path, dependent_upon in items}
    if len(expected) != len(items):
        raise ValueError(f"Duplicate expected WinUI {kind} item")
    for item in query["Items"].get(kind, []):
        value = item.get("FullPath", item.get("Identity"))
        if not isinstance(value, str) or item.get("ExcludedFromBuild", "").lower() == "true":
            raise ValueError(f"Invalid or excluded WinUI {kind} item")
        path = _item_path(value, private)
        key = _key(path)
        if key not in expected:
            raise ValueError(f"Unexpected or duplicate WinUI {kind} item: {path}")
        if item.get("DependentUpon", "") != expected.pop(key):
            raise ValueError(f"WinUI {kind} DependentUpon changed: {path}")
    if expected:
        raise ValueError(f"Missing WinUI {kind} items: {', '.join(sorted(expected))}")


def _sdk_projection_headers(files: set[str]) -> set[str]:
    # Project.idl and SDK metadata share the winrt/ output directory. Only
    # externalize complete SDK namespace families and C++/WinRT's own support
    # headers. Unknown shapes remain normal inputs to compiler diagnostics.
    sdk = files & {"winrt/base.h", "winrt/base_macros.h", "winrt/fast_forward.h"}
    for name in files:
        if match := _SDK_PROJECTION.fullmatch(name):
            layers = {f"winrt/impl/{match[1]}.{level}.h" for level in range(3)}
            if layers <= files:
                sdk.add(name)
                sdk.update(layers)
    return sdk


def generate(options: argparse.Namespace, sdk: dict[str, Any]) -> None:
    private = _required(options, "private")
    payload = _required(options, "headers")
    sdk_headers = _required(options, "sdk_headers")
    # The Bzl policy owns the language standard; the helper only forwards it.
    language_standard = options.language_standard
    if not isinstance(language_standard, str) or not language_standard:
        raise ValueError("WinUI generate requires --language-standard")
    xbf = options.xbf
    _native(private)
    idl_input = _required(options, "idl")
    xaml_inputs = options.xaml
    if sorted(path.name for path in xaml_inputs) != ["App.xaml", "MainWindow.xaml"]:
        raise ValueError("WinUI generation requires the authored App and MainWindow XAML")
    if sorted(path.name for path in options.header) != sorted(page.name + ".h" for page in xaml_inputs):
        raise ValueError("WinUI generation requires the matching XAML code-behind headers as content")
    if idl_input.name != "Project.idl" or any(
        path.parent != idl_input.parent for path in [*xaml_inputs, *options.header]
    ):
        raise ValueError("WinUI generation requires Project.idl beside the XAML and code-behind headers")
    # Keep SDK local lookup off the checkout. XamlTypeInfo.g.cpp names each
    # paired header relative to the project, so stage it next to the project.
    project_inputs = {path: private / path.name for path in [idl_input, *xaml_inputs, *options.header]}
    idl = project_inputs[idl_input]
    xaml = [project_inputs[path] for path in xaml_inputs]
    # The pinned SDK needs the code-behind relationships but does not read
    # the .xaml.cpp bodies. These ClCompile paths have no files in this tree.
    sources = [(page.with_suffix(".xaml.cpp"), page.name) for page in xaml]
    headers = [(page.with_suffix(".xaml.h"), page.name) for page in xaml]
    wrappers: dict[str, Path] = {}
    for role, spelling in options.cpp:
        if role not in _UNITS or role in wrappers:
            raise ValueError(f"Unexpected or duplicate WinUI wrapper role: {role}")
        wrappers[role] = absolute(spelling)
    if set(wrappers) != set(_UNITS):
        raise ValueError("WinUI generation requires four separate wrapper outputs")
    validate_outputs(
        [private, payload, sdk_headers, *wrappers.values(), *xbf],
        [options.sdk, *project_inputs, *_sdk_inputs(sdk)],
    )
    if private.exists() and any(private.iterdir()):
        raise ValueError(f"WinUI generation requires a fresh private project directory: {private}")
    for source, destination in project_inputs.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
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
    _element(compile_options, "LanguageStandard", language_standard)
    _element(compile_options, "PrecompiledHeader", "NotUsing")
    # Clear the VC default: C++/WinRT emits a nonempty filename as an include
    # in XamlMetaDataProvider.cpp even when PrecompiledHeader is NotUsing.
    _element(compile_options, "PrecompiledHeaderFile", "")
    items = _element(project, "ItemGroup")
    item = _element(items, "Midl", Include=_native(idl))
    _element(item, "OutputDirectory", "$(ProjectDir)")
    for path in xaml:
        item = _element(items, "ApplicationDefinition" if path.name == "App.xaml" else "Page", Include=_native(path))
        _element(item, "Link", path.name)
    for kind, pairs in (("ClCompile", sources), ("ClInclude", headers)):
        for path, page_name in pairs:
            item = _element(items, kind, Include=_native(path))
            _element(item, "DependentUpon", page_name)
    _element(items, "ClCompile", Include="$(GeneratedFilesDir)module.g.cpp")
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
    generated = private / "headers"
    compile_items = [
        *sources,
        *((generated / name, "") for name in _UNITS.values()),
        *((absolute(value), "") for value in sdk["initializer_sources"]),
    ]
    _validate_items(private, "ClCompile", compile_items, query)
    _validate_items(private, "ClInclude", headers, query)
    int_dir = query["Properties"].get("IntDir")
    if not isinstance(int_dir, str):
        raise ValueError("WinUI SDK omitted its private IntDir")
    intermediate = Path(int_dir.replace("\\", os.sep))
    if intermediate.is_absolute() or not (private / intermediate).resolve().is_relative_to(private.resolve()):
        raise ValueError("WinUI SDK IntDir must remain private and relative")
    (private / "postgeneration.json").write_text(json.dumps(query, indent=2) + "\n", encoding="utf-8")
    # Only generated C++ text belongs to the compilation context. Keep the
    # SDK's logs, caches, metadata and XBFs out of both include roots. Keep
    # each include spelling and file contents intact, with a single provider.
    sdk_headers.mkdir(parents=True, exist_ok=True)
    files = {
        path.relative_to(generated).as_posix()
        for path in generated.rglob("*")
        if path.suffix in {".h", ".hpp", ".cpp"} and path.is_file()
    }
    sdk_files = _sdk_projection_headers(files)
    copies = [(generated / name, (sdk_headers if name in sdk_files else payload) / name) for name in sorted(files)]
    copies.extend((private / ("headers/" + output.name), output) for output in xbf)
    for source, output in copies:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)
    for role, output in wrappers.items():
        native = payload / _UNITS[role]
        include = os.path.relpath(native, output.parent).replace("\\", "/")
        if '"' in include or "\n" in include:
            raise ValueError(f"Unsafe generated C++ include path: {include}")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            f'// Generated WinUI translation-unit owner. Do not edit.\n#include "{include}"\n',
            encoding="utf-8",
            newline="\n",
        )


def _require_resource_file(path: Path) -> None:
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f"Missing, empty or non-file WinUI payload: {path}")


def _content_aliases(options: argparse.Namespace) -> dict[str, Path]:
    assets = {path.name: path for path in options.asset}
    if len(assets) != len(options.asset) or assets.keys() != _ASSETS.keys():
        raise ValueError("Missing, duplicate or unexpected WinUI content asset")
    xbf = {path.name: path for path in options.xbf}
    if len(xbf) != len(options.xbf) or xbf.keys() != set(_XBF):
        raise ValueError("WinUI content requires exactly App.xbf and MainWindow.xbf")
    return {_ASSETS[name]: assets[name] for name in _ASSETS} | xbf


def catalog_resources(catalog: Path) -> list[tuple[str, Path]]:
    if not catalog.is_dir():
        raise ValueError(f"Missing catalog RESW tree: {catalog}")
    result: list[tuple[str, Path]] = []
    tags: set[str] = set()
    for directory in sorted(catalog.iterdir()):
        tag = directory.name
        if (
            not _LOCALE.fullmatch(tag)
            or ntpath.isreserved(tag)
            or tag.casefold() in tags
            or not directory.is_dir()
            or directory.is_symlink()
            or directory.is_junction()
        ):
            raise ValueError(f"Unsafe or duplicate catalog RESW locale: {tag}")
        path = directory / "Resources.resw"
        if {member.name for member in directory.iterdir()} != {path.name} or path.is_symlink():
            raise ValueError(f"Unexpected catalog RESW layout: {directory}")
        _require_resource_file(path)
        tags.add(tag.casefold())
        result.append((tag, path))
    if not any(tag == "en" for tag, _ in result):
        raise ValueError("WinUI PRI requires its English default RESW")
    return result


def resources(options: argparse.Namespace, sdk: dict[str, Any]) -> None:
    private = _required(options, "private")
    _native(private)
    catalog = _required(options, "catalog")
    pri = _required(options, "output_pri")
    if pri.name != "Aobus.pri":
        raise ValueError("WinUI resources must publish Aobus.pri")
    aliases = _content_aliases(options)
    resw = catalog_resources(catalog)
    validate_outputs([private, pri], [options.sdk, catalog, *aliases.values(), *_sdk_inputs(sdk)])
    for path in aliases.values():
        _require_resource_file(path)
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
    # MakePRI indexes these relative paths in the private OutDir. This Copy
    # does not transform content, so app staging uses the same original files.
    _element(stage, "Copy", SourceFiles="@(Content)", DestinationFiles="@(Content->'$(OutDir)%(TargetPath)')")
    _element(
        project,
        "Target",
        Name="AobusPri",
        DependsOnTargets=("AobusStageResources;_GetSdkToolPaths;_GenerateProjectPriFile"),
    )
    _write_project(project, private)
    _execute(sdk, private, options.configuration, "AobusPri")
    payload = private / "out"
    actual_assets = {path.relative_to(payload).as_posix() for path in (payload / "Assets").rglob("*") if path.is_file()}
    if actual_assets != aliases.keys() - set(_XBF):
        raise ValueError("Unexpected WinUI resource asset layout")
    native_pri = payload / "Aobus.pri"
    for path in [native_pri, *(payload / alias for alias in _XBF)]:
        _require_resource_file(path)
        if path.is_symlink():
            raise ValueError(f"Unexpected WinUI SDK payload link: {path}")
    pri.parent.mkdir(parents=True, exist_ok=True)
    native_pri.replace(pri)


def stage(options: argparse.Namespace) -> None:
    executable = _required(options, "exe")
    pri = _required(options, "pri")
    if pri.name != "Aobus.pri":
        raise ValueError("WinUI stage requires the published Aobus.pri")
    aliases = _content_aliases(options)
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
    identity = options.identity
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_-]*", identity) or ntpath.isreserved(identity):
        raise ValueError(f"Unsupported WinUI executable identity: {identity}")
    if [path.name for path in destinations] != [identity + ".exe", identity + ".pri", *_XBF, "Assets"]:
        raise ValueError("Unexpected WinUI executable-local output names")
    if len({_key(path.parent) for path in destinations}) != 1:
        raise ValueError("WinUI executable and resources must have adjacent output paths")
    assets = destinations[4]
    if assets.exists() and (not assets.is_dir() or any(assets.iterdir())):
        raise ValueError(f"WinUI staging requires a fresh Assets tree: {assets}")
    files = [executable, pri, *(aliases[name] for name in _XBF)]
    copies = list(zip(files, destinations[:4], strict=True))
    names = {path.name.casefold() for path in destinations}
    runtime_outputs = []
    for source_value, output_value in options.runtime:
        source, output = absolute(source_value), absolute(output_value)
        if output.parent != destinations[0].parent or source.name != output.name or output.name.casefold() in names:
            raise ValueError(f"Colliding or misplaced WinUI runtime output: {output}")
        names.add(output.name.casefold())
        runtime_outputs.append(output)
        copies.append((source, output))
    copies.extend((aliases[alias], assets / alias.removeprefix("Assets/")) for alias in _ASSETS.values())
    validate_outputs([*destinations, *runtime_outputs], [source for source, _ in copies])
    for source, _ in copies:
        _require_resource_file(source)
    for source, output in copies:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)
    destinations[0].chmod(0o755)


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
    # Supplied by the Bzl MSVC policy; the helper deliberately has no default.
    parser.add_argument("--language-standard", dest="language_standard")
    for name in (
        "sdk",
        "private",
        "headers",
        "sdk_headers",
        "idl",
        "catalog",
        "exe",
        "pri",
        "output_exe",
        "output_pri",
        "output_app_xbf",
        "output_window_xbf",
        "output_assets",
    ):
        parser.add_argument("--" + name.replace("_", "-"), dest=name, type=absolute)
    for name in ("header", "xaml", "asset", "xbf"):
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
