"""WinUI action mechanisms; actual SDK compilation/link/startup are native gates.

Each action fixture stays its narrow declared contract: parameter files parse
without an SDK, generation fakes one SDK query over the two authored code-behind
sources and XAML pages, resources declare only the two generated XBF fixtures, a small
catalog, and the required shipping assets, and staging consumes only the PRI,
original content, executable and runtime files. Selection and validation
stay real; the external MSBuild fake discriminates the SDK executable, action
target and declared project items rather than answering arbitrary commands.
The stage-independence case separately injects a missing source-file read.
"""

import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

from ao.core.paths import PROJECT_ROOT

_SPEC = importlib.util.spec_from_file_location("build_winui", PROJECT_ROOT / "app/windows-winui/build_winui.py")
assert _SPEC is not None and _SPEC.loader is not None
winui = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(winui)
_NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}

# The installed SDK's postgeneration output names, spelled here by their
# standalone consumer instead of imported from the production module.
_GENERATED_UNITS = ("module.g.cpp", "XamlMetaDataProvider.cpp", "XamlTypeInfo.g.cpp", "XamlTypeInfo.Impl.g.cpp")
_GENERATED_HEADERS = (
    "App.xbf",
    "MainWindow.xbf",
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
)
_SDK_NAMESPACES = ("Windows.Foundation", "Microsoft.UI.Xaml")
_SDK_PROJECTIONS = (
    *(f"winrt/{namespace}.h" for namespace in _SDK_NAMESPACES),
    *(f"winrt/impl/{namespace}.{depth}.h" for namespace in _SDK_NAMESPACES for depth in range(3)),
    "winrt/base.h",
    "winrt/base_macros.h",
    "winrt/fast_forward.h",
)
_NORMAL_PROJECTIONS = (
    "winrt/Aobus.h",
    "winrt/impl/Aobus.1.h",
    "winrt/Unknown.Namespace.h",
    "winrt/Windows.Foundation.g.h",
    "winrt/impl/Windows.Foundation.3.h",
    "winrt/Windows.Orphan.h",
    "winrt/impl/Windows.Orphan.0.h",
)
# The executable-local shipping assets the resource and stage actions require;
# external consumer contract inputs, not a test-owned registry.
_SHIPPING_ASSETS = (
    "SoulMark.svg",
    "LICENSE.txt",
    "note.svg",
    "vinyl.svg",
    "equalizer.svg",
    "windows_modern_layout.yaml",
    "windows_classic_layout.yaml",
)
_PAYLOAD_ASSETS = (
    "Brand/SoulMark.svg",
    "Brand/LICENSE.txt",
    "NoCover/note.svg",
    "NoCover/vinyl.svg",
    "NoCover/equalizer.svg",
    "Layout/windows_modern_layout.yaml",
    "Layout/windows_classic_layout.yaml",
)


def _project_path(value, project):
    path = Path(value.replace("\\", os.sep))
    return path if path.is_absolute() else project / path


def _install_sdk(root):
    """A tiny installed SDK: one declared tool pair and one initializer source."""
    provider = root / "installed SDK"
    provider.mkdir()
    msbuild = provider / "MSBuild.exe"
    msbuild.write_bytes(b"external tool fixture")
    for name in ("native.props", "native.targets", "bootstrap.cpp"):
        (provider / name).write_text("SDK fixture", encoding="utf-8")
    sdk = root / "sdk.json"
    data = {
        "msbuild": str(msbuild),
        "props": [str(provider / "native.props")],
        "targets": [str(provider / "native.targets")],
        "platform_toolset": "fixture-toolset",
        "windows_sdk_version": "fixture-sdk-version",
        "environment": {"PATH": "declared VC toolchain", "SYSTEMROOT": "declared Windows root"},
        "initializer_sources": [str(provider / "bootstrap.cpp")],
        "ml_transitive_dependencies_verified": True,
    }
    sdk.write_text(json.dumps(data), encoding="utf-8")
    return sdk, data


class ParameterFileTest(unittest.TestCase):
    def test_multiline_parameter_file_preserves_spaces_and_literal_quotes_per_argument(self):
        with tempfile.TemporaryDirectory(prefix="aobus winui arguments with spaces-") as temporary:
            root = Path(temporary).resolve()
            source = root / "authored folder/source \"literal\" and 'single' 音楽.cpp"
            header = root / "header folder/declared header.h"
            wrapper = root / 'output folder/module "literal".cpp'
            runtime_source = root / 'SDK folder/runtime "literal".dll'
            runtime_output = root / 'output folder/runtime "literal".dll'
            parameters = root / "helper arguments.params"
            parameters.write_text(
                "\n".join(
                    [
                        "--action",
                        "generate",
                        "--source",
                        str(source),
                        "--header",
                        str(header),
                        "--header-name=-Leading hyphen.h",
                        "--cpp",
                        "module",
                        str(wrapper),
                        "--runtime",
                        str(runtime_source),
                        str(runtime_output),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            options = winui.parse_args(["@" + str(parameters)])

            self.assertEqual(options.action, "generate")
            self.assertEqual(options.source, [source])
            self.assertEqual(options.header, [header])
            self.assertEqual(options.header_name, ["-Leading hyphen.h"])
            self.assertEqual(options.cpp, [["module", str(wrapper)]])
            self.assertEqual(options.runtime, [[str(runtime_source), str(runtime_output)]])


class GenerationActionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus winui generate with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.sdk, self.sdk_data = _install_sdk(self.root)
        self.generated = self.root / "generated/private"
        self.payload = self.root / "generated/headers"
        self.sdk_headers = self.root / "generated/sdk_headers"
        self.xbf_root = self.root / "generated/xbf"
        self.wrapper_root = self.root / "generated/src"
        self.source = self.root / "authored"
        self.source.mkdir()
        for name in (
            "Project.idl",
            "App.xaml",
            "MainWindow.xaml",
            "App.xaml.cpp",
            "MainWindow.xaml.cpp",
            "App.xaml.h",
            "MainWindow.xaml.h",
            "Control.h",
        ):
            (self.source / name).write_text("authored " + name, encoding="utf-8")
        self.calls = []
        self.mutate_query = lambda query: None

    def generation_options(self, *extra):
        return winui.parse_args(
            [
                "--action",
                "generate",
                "--sdk",
                str(self.sdk),
                "--private",
                str(self.generated),
                "--headers",
                str(self.payload),
                "--sdk-headers",
                str(self.sdk_headers),
                "--xbf",
                str(self.xbf_root / "App.xbf"),
                "--xbf",
                str(self.xbf_root / "MainWindow.xbf"),
                "--language-standard",
                "stdcpplatest",
                "--idl",
                str(self.source / "Project.idl"),
                "--source",
                str(self.source / "App.xaml.cpp"),
                "--source",
                str(self.source / "MainWindow.xaml.cpp"),
                "--header",
                str(self.source / "App.xaml.h"),
                "--header",
                str(self.source / "MainWindow.xaml.h"),
                "--header-name=Control.h",
                "--xaml",
                str(self.source / "App.xaml"),
                "--xaml",
                str(self.source / "MainWindow.xaml"),
                *[
                    argument
                    for role in ("module", "metadata", "types", "types_impl")
                    for argument in ("--cpp", role, str(self.wrapper_root / (role + ".cpp")))
                ],
                *extra,
            ]
        )

    def fake_msbuild(self, arguments, **kwargs):
        """Only the SDK generate query answers, never arbitrary argv."""
        self.assertEqual(arguments[0], self.sdk_data["msbuild"])
        self.assertEqual(Path(arguments[1]).name, "Aobus.vcxproj")
        self.assertEqual(next(argument for argument in arguments if argument.startswith("/t:")), "/t:AobusGenerate")
        self.assertIn("/getItem:ClCompile,ClInclude", arguments)
        self.assertIs(kwargs["check"], False)
        self.assertIs(kwargs["capture_output"], True)
        self.assertNotIn("shell", kwargs)
        private = Path(kwargs["cwd"])
        # The declared toolchain environment replaces the ambient one.
        self.assertEqual(kwargs["env"]["PATH"], self.sdk_data["environment"]["PATH"])
        temp = Path(kwargs["env"]["TEMP"])
        self.assertTrue(temp.is_relative_to(private))
        self.assertNotEqual(temp, private)
        self.assertTrue(temp.is_dir())
        for name in ("TMP", "TMPDIR"):
            self.assertEqual(Path(kwargs["env"][name]), temp)
        document = ET.parse(arguments[1]).getroot()
        self.calls.append(document)
        headers = private / "headers"
        headers.mkdir(parents=True, exist_ok=True)
        for name in (*_GENERATED_UNITS, *_GENERATED_HEADERS):
            (headers / name).write_text("SDK-generated " + name, encoding="utf-8")
        for name in (*_SDK_PROJECTIONS, *_NORMAL_PROJECTIONS):
            projection = headers / name
            projection.parent.mkdir(parents=True, exist_ok=True)
            projection.write_text("projection " + name, encoding="utf-8")
        # Echo the declared inventory and the SDK's standalone compile units.
        source_paths = [
            _project_path(item.attrib["Include"], private)
            for item in document.findall("m:ItemGroup/m:ClCompile", _NS)
            if not item.attrib["Include"].startswith("$(GeneratedFilesDir)")
        ]
        query = {
            "Properties": {
                "GeneratedFilesDir": str(headers),
                "OutDir": str(private / "out"),
                "IntDir": document.find("m:PropertyGroup/m:IntDir", _NS).text,
            },
            "Items": {
                "ClCompile": [
                    {"FullPath": str(path)}
                    for path in (
                        *source_paths,
                        *self.sdk_data["initializer_sources"],
                        *(headers / name for name in _GENERATED_UNITS),
                    )
                ],
                "ClInclude": [
                    {
                        "FullPath": str(_project_path(item.attrib["Include"], private)),
                        "DependentUpon": item.findtext("m:DependentUpon", default="", namespaces=_NS),
                    }
                    for item in document.findall("m:ItemGroup/m:ClInclude", _NS)
                ],
            },
        }
        self.mutate_query(query)
        return subprocess.CompletedProcess(arguments, 0, json.dumps(query).encode(), b"")

    def generate(self, *extra):
        with mock.patch.object(winui.subprocess, "run", side_effect=self.fake_msbuild):
            winui.run(self.generation_options(*extra))

    def test_generation_publishes_four_wrappers_without_compiling_include_fragments(self):
        with mock.patch.dict(os.environ, {"PATH": "ambient must not leak", "TEMP": "ambient temp"}):
            self.generate()
        # A missing override would restore the VC default in the SDK's metadata TU.
        self.assertEqual(
            self.calls[0].findtext("m:ItemDefinitionGroup/m:ClCompile/m:PrecompiledHeaderFile", namespaces=_NS), ""
        )
        includes = {
            path.name: [line for line in path.read_text(encoding="utf-8").splitlines() if line.startswith("#include")]
            for path in self.wrapper_root.iterdir()
        }
        self.assertEqual(
            includes,
            {
                "module.cpp": ['#include "../headers/module.g.cpp"'],
                "metadata.cpp": ['#include "../headers/XamlMetaDataProvider.cpp"'],
                "types.cpp": ['#include "../headers/XamlTypeInfo.g.cpp"'],
                "types_impl.cpp": ['#include "../headers/XamlTypeInfo.Impl.g.cpp"'],
            },
        )
        self.assertTrue((self.generated / "postgeneration.json").is_file())
        for names, destination, other in (
            (_SDK_PROJECTIONS, self.sdk_headers, self.payload),
            (_NORMAL_PROJECTIONS, self.payload, self.sdk_headers),
            (("App.xaml.g.h", "module.g.cpp"), self.payload, self.sdk_headers),
        ):
            for name in names:
                self.assertEqual((destination / name).read_bytes(), (self.generated / "headers" / name).read_bytes())
                self.assertFalse((other / name).exists(), name)
        for name in ("App.xbf", "MainWindow.xbf"):
            self.assertEqual((self.xbf_root / name).read_bytes(), (self.generated / "headers" / name).read_bytes())
            self.assertFalse((self.payload / name).exists())
        self.assertEqual(len(self.calls), 1)

    def test_diagnostic_changes_leave_published_cpp_and_xbf_bytes_unchanged(self):
        stamp = "first execution"

        def diagnostics(query):
            for item in query["Items"]["ClCompile"]:
                item["CreatedTime"] = stamp
            for relative in (
                "msbuild.log",
                "int/build.tlog",
                "t/leftover.txt",
                "headers/compiler-cache.json",
                "headers/Project.winmd",
            ):
                path = self.generated / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(stamp, encoding="utf-8")

        self.mutate_query = diagnostics
        self.generate()
        receipt = (self.generated / "postgeneration.json").read_bytes()
        published = {
            path.relative_to(self.generated.parent): path.read_bytes()
            for root in (self.payload, self.sdk_headers, self.wrapper_root, self.xbf_root)
            for path in root.rglob("*")
            if path.is_file()
        }
        self.assertFalse((self.payload / "compiler-cache.json").exists())
        self.assertFalse((self.payload / "Project.winmd").exists())
        self.assertFalse((self.payload / "postgeneration.json").exists())
        shutil.rmtree(self.generated.parent)
        stamp = "second execution"

        self.generate()

        self.assertNotEqual((self.generated / "postgeneration.json").read_bytes(), receipt)
        self.assertIn(stamp.encode(), (self.generated / "msbuild.stdout.txt").read_bytes())
        self.assertEqual((self.generated / "msbuild.log").read_text(encoding="utf-8"), stamp)
        self.assertEqual(
            {
                path.relative_to(self.generated.parent): path.read_bytes()
                for root in (self.payload, self.sdk_headers, self.wrapper_root, self.xbf_root)
                for path in root.rglob("*")
                if path.is_file()
            },
            published,
        )

    def test_sdk_headers_output_cannot_overlap_normal_headers(self):
        with mock.patch.object(winui.subprocess, "run") as execute:
            with self.assertRaises(ValueError):
                winui.run(self.generation_options("--sdk-headers", str(self.payload)))
        execute.assert_not_called()

    def test_ordinary_source_identities_preserve_directories_and_missing_sdk_item_fails(self):
        names = ("track/Feature.cpp", "library/Feature.cpp", "-Feature.cpp")
        arguments = [f"--source-name={name}" for name in names]
        self.generate(*arguments)
        items = {
            _project_path(item.attrib["Include"], self.generated): item
            for item in self.calls[0].findall("m:ItemGroup/m:ClCompile", _NS)
            if not item.attrib["Include"].startswith("$(GeneratedFilesDir)")
        }
        for name in names:
            identity = self.generated / "i" / name
            self.assertIn(identity, items)
            self.assertFalse(identity.exists())
            self.assertNotIn(self.source / name, items)
        for name in ("App.xaml.cpp", "MainWindow.xaml.cpp"):
            item = items[self.generated / name]
            self.assertEqual(item.find("m:DependentUpon", _NS).text, name.removesuffix(".cpp"))

        def drop_one(query):
            omitted = self.generated / "i" / names[0]
            query["Items"]["ClCompile"] = [
                item for item in query["Items"]["ClCompile"] if Path(item["FullPath"]) != omitted
            ]

        self.mutate_query = drop_one
        shutil.rmtree(self.generated.parent)
        with self.assertRaisesRegex(ValueError, "compile membership changed"):
            self.generate(*arguments)

    def test_ordinary_headers_preserve_same_basenames_without_staging_bytes(self):
        names = ("track/Control.h", "layout/Control.h", "nested/App.xaml.h", "-Extra header.h")
        self.generate(*(f"--header-name={name}" for name in names))
        document = self.calls[0]
        headers = {
            _project_path(item.attrib["Include"], self.generated): item
            for item in document.findall("m:ItemGroup/m:ClInclude", _NS)
        }
        authored = self.generated  # XAML emits these project-relative header names in generated C++.
        inventory = self.generated / "i"
        self.assertEqual(
            set(headers),
            {authored / "App.xaml.h", authored / "MainWindow.xaml.h"}
            | {inventory / name for name in ("Control.h", *names)},
        )
        self.assertFalse(inventory.exists())
        for name in ("Control.h", *names):
            self.assertIsNone(headers[inventory / name].find("m:DependentUpon", _NS))
        # Only this relationship owns the generated partial-class includes.
        for page in ("App.xaml", "MainWindow.xaml"):
            self.assertEqual(headers[authored / (page + ".h")].findtext("m:DependentUpon", namespaces=_NS), page)
        for source in self.source.iterdir():
            destination = authored / source.name
            if source.name == "Control.h":
                self.assertFalse(destination.exists())
            else:
                self.assertEqual(destination.read_bytes(), source.read_bytes())
        for kind in ("Midl", "ApplicationDefinition", "Page"):
            item = document.find(f"m:ItemGroup/m:{kind}", _NS)
            self.assertEqual(_project_path(item.attrib["Include"], self.generated).parent, authored)
        includes = document.findtext("m:ItemDefinitionGroup/m:ClCompile/m:AdditionalIncludeDirectories", namespaces=_NS)
        self.assertEqual(
            [_project_path(value, self.generated) for value in includes.split(";")[:-1]],
            [authored, authored / "include"],
        )

    def test_unsafe_or_case_colliding_header_names_fail_before_msbuild(self):
        for name in (
            "",
            "/Control.h",
            "C:/Control.h",
            "dir\\Control.h",
            "dir//Control.h",
            "dir/./Control.h",
            "../Control.h",
            "NUL.h",
            "COM1/Control.h",
            "dir./Control.h",
            "dir /Control.h",
            "$(Root)/Control.h",
            "dir;other/Control.h",
            "dir%2fControl.h",
            "*.h",
            "not-a-header.hpp",
            "control.h",
            "app.xaml.h",
        ):
            with self.subTest(name=name), mock.patch.object(winui.subprocess, "run") as execute:
                with self.assertRaisesRegex(ValueError, "header inventory name"):
                    winui.run(self.generation_options(f"--header-name={name}"))
                execute.assert_not_called()

    def test_codebehind_header_cannot_be_supplied_as_a_name_only(self):
        options = self.generation_options("--header-name=App.xaml.h")
        options.header.remove(self.source / "App.xaml.h")
        with mock.patch.object(winui.subprocess, "run") as execute:
            with self.assertRaisesRegex(ValueError, "code-behind headers as content"):
                winui.run(options)
        execute.assert_not_called()

    def test_sdk_header_query_rejects_missing_duplicate_repointed_or_changed_items(self):
        for failure in ("missing ordinary", "missing selected", "duplicate", "checkout", "dependent", "excluded"):
            with self.subTest(failure=failure):

                def alter(query, failure=failure):
                    headers = query["Items"]["ClInclude"]
                    selected = next(item for item in headers if Path(item["FullPath"]).name == "App.xaml.h")
                    ordinary = next(item for item in headers if Path(item["FullPath"]).name == "Control.h")
                    if failure == "missing ordinary":
                        headers.remove(ordinary)
                    elif failure == "missing selected":
                        headers.remove(selected)
                    elif failure == "duplicate":
                        headers.append({**ordinary, "FullPath": str(Path(ordinary["FullPath"]).with_name("control.h"))})
                    elif failure == "checkout":
                        ordinary["FullPath"] = str(self.source / "Control.h")
                    elif failure == "dependent":
                        selected.pop("DependentUpon")
                    else:
                        ordinary["ExcludedFromBuild"] = "true"

                self.mutate_query = alter
                with self.assertRaisesRegex(ValueError, "WinUI .*header"):
                    self.generate()
                self.assertFalse(self.payload.exists())
                # A separate Bazel action execution starts with fresh outputs.
                shutil.rmtree(self.generated.parent)

    def test_template_restores_native_xaml_wiring_before_default_props_and_guards_product_targets(self):
        self.generate()
        document = self.calls[0]
        # Canonical native VC import order: Globals precede Default.props.
        nodes = list(document)
        globals_index = next(index for index, node in enumerate(nodes) if node.attrib.get("Label") == "Globals")
        default_index = next(
            index
            for index, node in enumerate(nodes)
            if node.attrib.get("Project", "").endswith("Microsoft.Cpp.Default.props")
        )
        self.assertLess(globals_index, default_index)
        self.assertEqual(nodes[globals_index].find("m:ApplicationType", _NS).text, "Windows Store")
        # The installed SDK selection maps into the native project.
        self.assertEqual(document.find("m:PropertyGroup/m:PlatformToolset", _NS).text, "fixture-toolset")
        self.assertEqual(
            document.find("m:PropertyGroup/m:WindowsTargetPlatformVersion", _NS).text, "fixture-sdk-version"
        )
        # The authored XAML pages are declared to the SDK compiler.
        self.assertTrue(
            any(
                item.attrib["Include"].endswith("App.xaml")
                for item in document.findall("m:ItemGroup/m:ApplicationDefinition", _NS)
            )
        )
        self.assertTrue(
            any(
                item.attrib["Include"].endswith("MainWindow.xaml")
                for item in document.findall("m:ItemGroup/m:Page", _NS)
            )
        )
        # No product compile/link/build target may run from this project.
        guard = document.find("m:Target[@Name='AobusNoProductBuild']", _NS)
        self.assertTrue({"ClCompile", "Link", "Build"} <= set(guard.attrib["BeforeTargets"].split(";")))
        self.assertIsNotNone(guard.find("m:Error", _NS))

    def test_language_standard_forwards_the_caller_argument_without_a_helper_default(self):
        self.generate("--language-standard", "stdcpp17")
        standard = self.calls[0].find("m:ItemDefinitionGroup/m:ClCompile/m:LanguageStandard", _NS)
        self.assertEqual(standard.text, "stdcpp17")

    def test_release_changes_private_intermediates_not_the_exported_headers_path(self):
        self.generate("--configuration", "Release")
        document = self.calls[0]
        self.assertEqual(document.find("m:PropertyGroup/m:IntDir", _NS).text, "int\\Release\\")
        self.assertEqual(document.find("m:PropertyGroup/m:UseDebugLibraries", _NS).text, "false")
        self.assertIn("../headers/module.g.cpp", (self.wrapper_root / "module.cpp").read_text(encoding="utf-8"))

    def test_sdk_failure_retains_raw_status_and_never_publishes_cpp_outputs(self):
        with mock.patch.object(
            winui.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                [self.sdk_data["msbuild"]],
                19,
                b"generation failed",
                b"diagnostic detail",
            ),
        ) as execute:
            with self.assertRaises(subprocess.CalledProcessError) as error:
                winui.run(self.generation_options())
        self.assertEqual(error.exception.returncode, 19)
        self.assertEqual(execute.call_count, 1)
        self.assertEqual((self.generated / "msbuild.exit.txt").read_text(encoding="utf-8"), "19\n")
        self.assertEqual((self.generated / "msbuild.stderr.txt").read_bytes(), b"diagnostic detail")
        self.assertFalse(self.wrapper_root.exists())


class ResourcesActionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus winui resources with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.sdk, self.sdk_data = _install_sdk(self.root)
        self.generated = self.root / "generated/xbf"
        self.resource_tree = self.root / "resources/p"
        self.pri = self.resource_tree.parent / "Aobus.pri"
        self.catalog = self.root / "catalog/resw"
        self.generated.mkdir(parents=True)
        for name in ("App.xbf", "MainWindow.xbf"):
            (self.generated / name).write_text("generated " + name, encoding="utf-8")
        for tag in ("en", "fr-CA"):
            path = self.catalog / tag / "Resources.resw"
            path.parent.mkdir(parents=True)
            path.write_text("catalog " + tag, encoding="utf-8")
        self.assets = []
        (self.root / "assets").mkdir()
        for name in _SHIPPING_ASSETS:
            path = self.root / "assets" / name
            path.write_text("asset " + name, encoding="utf-8")
            self.assets.append(path)
        self.calls = []

    def resource_options(self):
        return winui.parse_args(
            [
                "--action",
                "resources",
                "--sdk",
                str(self.sdk),
                "--private",
                str(self.resource_tree),
                "--output-pri",
                str(self.pri),
                "--xbf",
                str(self.generated / "App.xbf"),
                "--xbf",
                str(self.generated / "MainWindow.xbf"),
                "--catalog",
                str(self.catalog),
                *[argument for path in self.assets for argument in ("--asset", str(path))],
            ]
        )

    def fake_pri(self, arguments, **kwargs):
        """Only the SDK PRI action answers, never arbitrary argv."""
        self.assertEqual(arguments[0], self.sdk_data["msbuild"])
        self.assertEqual(Path(arguments[1]).name, "Aobus.vcxproj")
        self.assertEqual(next(argument for argument in arguments if argument.startswith("/t:")), "/t:AobusPri")
        self.assertFalse(any(argument.startswith("/getItem:") for argument in arguments))
        self.assertIs(kwargs["check"], False)
        private = Path(kwargs["cwd"])
        self.assertEqual(kwargs["env"]["PATH"], self.sdk_data["environment"]["PATH"])
        for name in ("TEMP", "TMP", "TMPDIR"):
            self.assertEqual(Path(kwargs["env"][name]), private / "t")
        document = ET.parse(arguments[1]).getroot()
        self.calls.append(document)
        copy = document.find("m:Target[@Name='AobusStageResources']/m:Copy", _NS)
        self.assertEqual(
            copy.attrib, {"SourceFiles": "@(Content)", "DestinationFiles": "@(Content->'$(OutDir)%(TargetPath)')"}
        )
        # The project stages its original Content verbatim for MakePRI.
        for item in document.findall("m:ItemGroup/m:Content", _NS):
            if "Include" not in item.attrib:
                continue
            source = _project_path(item.attrib["Include"], private)
            alias = item.find("m:TargetPath", _NS).text.replace("\\", "/")
            destination = private / "out" / alias
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        (private / "out/Aobus.pri").write_bytes(b"official PRI fixture")
        return subprocess.CompletedProcess(arguments, 0, b"", b"")

    def test_pri_declares_generated_xbf_and_catalog_locales_as_executable_relative_content(self):
        with mock.patch.object(winui.subprocess, "run", side_effect=self.fake_pri):
            winui.run(self.resource_options())
        document = self.calls[0]
        contents = {
            item.find("m:TargetPath", _NS).text: _project_path(item.attrib["Include"], self.resource_tree)
            for item in document.findall("m:ItemGroup/m:Content", _NS)
            if "Include" in item.attrib
        }
        self.assertEqual(contents["App.xbf"], self.generated / "App.xbf")
        self.assertEqual(contents["MainWindow.xbf"], self.generated / "MainWindow.xbf")
        self.assertEqual(contents["Assets\\Brand\\SoulMark.svg"], self.root / "assets/SoulMark.svg")
        self.assertIn("Assets\\Layout\\windows_classic_layout.yaml", contents)
        self.assertEqual(
            {
                item.find("m:Link", _NS).text: _project_path(item.attrib["Include"], self.resource_tree)
                for item in document.findall("m:ItemGroup/m:PRIResource", _NS)
            },
            {f"Resources\\{tag}\\Resources.resw": self.catalog / tag / "Resources.resw" for tag in ("en", "fr-CA")},
        )
        self.assertEqual(self.pri.read_bytes(), b"official PRI fixture")
        self.assertFalse((self.resource_tree / "out/Aobus.pri").exists())
        self.assertTrue((self.resource_tree / "msbuild.stdout.txt").is_file())

    def test_pri_rejects_duplicate_assets_and_malformed_resw_before_msbuild(self):
        options = self.resource_options()
        options.asset.append(self.root / "other/SoulMark.svg")
        with mock.patch.object(winui.subprocess, "run") as execute:
            with self.assertRaisesRegex(ValueError, "duplicate"):
                winui.run(options)
            (self.catalog / "fr-CA/Resources.resw").unlink()
            with self.assertRaisesRegex(ValueError, "RESW layout"):
                winui.run(self.resource_options())
        execute.assert_not_called()

    def test_pri_does_not_publish_an_empty_sdk_output(self):
        def empty_pri(arguments, **kwargs):
            result = self.fake_pri(arguments, **kwargs)
            (self.resource_tree / "out/Aobus.pri").write_bytes(b"")
            return result

        with mock.patch.object(winui.subprocess, "run", side_effect=empty_pri):
            with self.assertRaisesRegex(ValueError, "empty.*Aobus.pri"):
                winui.run(self.resource_options())
        self.assertFalse(self.pri.exists())


class StageActionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus winui stage with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.pri = self.root / "resources/Aobus.pri"
        self.pri.parent.mkdir()
        self.pri.write_bytes(b"official PRI fixture")
        xbf = self.root / "generated/xbf"
        xbf.mkdir(parents=True)
        self.xbf = [xbf / name for name in ("App.xbf", "MainWindow.xbf")]
        for path in self.xbf:
            path.write_text("payload " + path.name, encoding="utf-8")
        assets = self.root / "authored"
        assets.mkdir()
        self.assets = [assets / name for name in _SHIPPING_ASSETS]
        for path in self.assets:
            path.write_text("payload asset " + path.name, encoding="utf-8")
        self.executable = self.root / "linked/Aobus.exe"
        self.executable.parent.mkdir()
        self.executable.write_bytes(b"real linked executable fixture")

    def stage_options(self, directory=None, runtime=None, identity=None, executable=None):
        directory = directory or self.root / "shipping app"
        executable = executable or self.executable
        name = "Aobus" if identity is None else identity
        arguments = [
            "--action",
            "stage",
            "--exe",
            str(executable),
            "--pri",
            str(self.pri),
            *[argument for path in self.xbf for argument in ("--xbf", str(path))],
            *[argument for path in self.assets for argument in ("--asset", str(path))],
            "--output-exe",
            str(directory / (name + ".exe")),
            "--output-pri",
            str(directory / (name + ".pri")),
            "--output-app-xbf",
            str(directory / "App.xbf"),
            "--output-window-xbf",
            str(directory / "MainWindow.xbf"),
            "--output-assets",
            str(directory / "Assets"),
        ]
        if identity is not None:
            arguments.extend(["--identity", identity])
        for path in runtime or []:
            arguments.extend(["--runtime", str(path), str(directory / path.name)])
        return winui.parse_args(arguments)

    def test_stage_copies_declared_executable_bytes_and_adjacent_payload_without_launching_tools(self):
        runtime = self.root / "installed SDK/framework.bootstrap.dll"
        runtime.parent.mkdir(parents=True, exist_ok=True)
        runtime.write_bytes(b"declared runtime fixture")
        options = self.stage_options(runtime=[runtime])
        with mock.patch.object(winui.subprocess, "run") as execute:
            winui.run(options)
        execute.assert_not_called()
        app = options.output_exe.parent
        self.assertEqual(
            {path.relative_to(app).as_posix(): path.read_bytes() for path in app.rglob("*") if path.is_file()},
            {
                "Aobus.exe": self.executable.read_bytes(),
                "Aobus.pri": self.pri.read_bytes(),
                runtime.name: runtime.read_bytes(),
                **{path.name: path.read_bytes() for path in self.xbf},
                **{
                    f"Assets/{alias}": (self.root / "authored" / Path(alias).name).read_bytes()
                    for alias in _PAYLOAD_ASSETS
                },
            },
        )

    def test_stage_copies_linked_indirect_and_sdk_runtime_pairs_without_sdk_lookup(self):
        runtime = [
            self.root / "native/debug/bin/codec.dll",
            self.root / "native/debug/bin/container.dll",
            self.root / "WinUI SDK/bootstrap.dll",
        ]
        for index, source in enumerate(runtime):
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(f"declared runtime {index}".encode())
        options = self.stage_options(runtime=runtime)

        with (
            mock.patch.object(winui, "read_sdk", side_effect=AssertionError("stage must not discover an SDK")),
            mock.patch.object(winui.subprocess, "run") as execute,
        ):
            winui.run(options)

        execute.assert_not_called()
        for source in runtime:
            self.assertEqual((options.output_exe.parent / source.name).read_bytes(), source.read_bytes())
        self.assertEqual(options.output_exe.read_bytes(), self.executable.read_bytes())

    def test_stage_rejects_distinct_case_colliding_runtime_origins_even_with_equal_bytes(self):
        first = self.root / "native/debug/bin/codec.dll"
        second = self.root / "another SDK/bin/CODEC.dll"
        for source in (first, second):
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"same bytes do not establish the same runtime origin")
        options = self.stage_options(runtime=[first, second])

        with self.assertRaisesRegex(ValueError, "Colliding or misplaced WinUI runtime output"):
            winui.run(options)

        self.assertFalse(options.output_exe.exists())
        self.assertFalse((options.output_exe.parent / first.name).exists())

    def test_stage_defaults_to_shipping_identity_and_accepts_probe_identity_outputs(self):
        default = self.stage_options(directory=self.root / "default app")
        self.assertEqual(default.identity, "Aobus")
        with mock.patch.object(winui.subprocess, "run") as execute:
            winui.run(default)
        execute.assert_not_called()
        self.assertEqual(default.output_exe.name, "Aobus.exe")
        self.assertEqual(default.output_pri.name, "Aobus.pri")
        self.assertEqual(default.output_pri.read_bytes(), self.pri.read_bytes())

        identity = "ao_winui_soul_probe"
        executable = self.root / "linked" / (identity + ".exe")
        executable.write_bytes(b"probe executable fixture")
        runtime = self.root / "installed SDK/framework.bootstrap.dll"
        runtime.parent.mkdir(parents=True, exist_ok=True)
        runtime.write_bytes(b"declared probe runtime fixture")
        probe = self.stage_options(
            directory=self.root / "probe app",
            identity=identity,
            executable=executable,
            runtime=[runtime],
        )
        with mock.patch.object(winui.subprocess, "run") as execute:
            winui.run(probe)
        execute.assert_not_called()
        self.assertEqual(probe.identity, identity)
        self.assertEqual(probe.output_exe.name, identity + ".exe")
        self.assertEqual(probe.output_pri.name, identity + ".pri")
        self.assertEqual(probe.output_exe.read_bytes(), executable.read_bytes())
        self.assertEqual(probe.output_pri.read_bytes(), self.pri.read_bytes())
        for source in self.xbf:
            self.assertEqual((probe.output_exe.parent / source.name).read_bytes(), source.read_bytes())
        self.assertEqual(
            (probe.output_assets / "Brand/SoulMark.svg").read_bytes(),
            (self.root / "authored/SoulMark.svg").read_bytes(),
        )
        self.assertEqual((probe.output_exe.parent / runtime.name).read_bytes(), runtime.read_bytes())

    def test_stage_assembles_without_loading_the_shared_project_sdk_or_diagnostics(self):
        options = self.stage_options()
        read_text = Path.read_text

        def without_undeclared_support(path, *args, **kwargs):
            if path.name in {"msbuild_project.py", "sdk.json", "sdk-diagnostics.json"}:
                raise FileNotFoundError("undeclared stage support is not an input")
            return read_text(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", new=without_undeclared_support):
            winui.run(options)
        self.assertTrue(options.output_exe.exists())

    def test_stage_rejects_runtime_basename_collisions_and_output_input_overlap_before_copying(self):
        collision = self.root / "installed SDK/Aobus.PRI"
        collision.parent.mkdir(parents=True, exist_ok=True)
        collision.write_bytes(b"wrong runtime alias")
        options = self.stage_options(runtime=[collision])
        with self.assertRaisesRegex(ValueError, "Colliding"):
            winui.run(options)
        self.assertFalse(options.output_exe.exists())
        options = self.stage_options(directory=self.pri.parent)
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            winui.run(options)
        self.assertFalse(options.output_exe.exists())

    def test_stage_rejects_wrong_output_names_and_missing_content_before_copying(self):
        options = self.stage_options()
        options.output_pri = options.output_pri.with_name("Wrong.pri")
        with self.assertRaisesRegex(ValueError, "executable-local output names"):
            winui.run(options)
        self.assets[0].unlink()
        options = self.stage_options()
        with self.assertRaisesRegex(ValueError, "Missing.*SoulMark.svg"):
            winui.run(options)
        self.assertFalse(options.output_exe.parent.exists())


if __name__ == "__main__":
    unittest.main()
