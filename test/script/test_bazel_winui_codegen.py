"""WinUI action mechanisms; actual SDK compilation/link/startup are native gates.

Each action fixture stays its narrow declared contract: parameter files parse
without an SDK, generation fakes one SDK query over one authored source and the
two XAML pages, resources declare only the two generated XBF fixtures, a small
catalog, and the required shipping assets, and staging consumes only the PRI
payload tree with its executable and runtime files. Selection and validation
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
        self.xbf_root = self.root / "generated/xbf"
        self.wrapper_root = self.root / "generated/src"
        self.source = self.root / "authored"
        self.source.mkdir()
        for name in ("Project.idl", "App.xaml", "MainWindow.xaml", "App.xaml.cpp", "pch.h"):
            (self.source / name).write_text("authored fixture", encoding="utf-8")
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
                "--xbf",
                str(self.xbf_root / "App.xbf"),
                "--xbf",
                str(self.xbf_root / "MainWindow.xbf"),
                "--idl",
                str(self.source / "Project.idl"),
                "--source",
                str(self.source / "App.xaml.cpp"),
                "--header",
                str(self.source / "pch.h"),
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
        self.assertIn("/getItem:ClCompile", arguments)
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
        projection = headers / "winrt/Windows.Foundation.h"
        projection.parent.mkdir(exist_ok=True)
        projection.write_text("SDK platform projection", encoding="utf-8")
        # The echo answers exactly the compile inputs the project declared.
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
                ]
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
        types = (self.wrapper_root / "types.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "pch.h"\n', types)
        self.assertIn('#include "../headers/XamlTypeInfo.g.cpp"', types)
        metadata = (self.wrapper_root / "metadata.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "../headers/XamlMetaDataProvider.cpp"', metadata)
        self.assertNotIn('#include "../headers/XamlMetaDataProvider.g.cpp"', metadata)
        self.assertEqual(
            sorted(path.name for path in self.wrapper_root.iterdir()),
            ["metadata.cpp", "module.cpp", "types.cpp", "types_impl.cpp"],
        )
        self.assertTrue((self.generated / "postgeneration.json").is_file())
        self.assertEqual((self.payload / "winrt/Windows.Foundation.h").read_bytes(), b"SDK platform projection")
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
            for root in (self.payload, self.wrapper_root, self.xbf_root)
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
                for root in (self.payload, self.wrapper_root, self.xbf_root)
                for path in root.rglob("*")
                if path.is_file()
            },
            published,
        )

    def test_xaml_headers_retain_their_partial_class_relationships(self):
        arguments = []
        for name in ("App.xaml.h", "MainWindow.xaml.h"):
            header = self.source / name
            header.write_text("authored XAML header fixture", encoding="utf-8")
            arguments.extend(["--header", str(header)])
        self.generate(*arguments)
        headers = {
            _project_path(item.attrib["Include"], self.generated).name: item
            for item in self.calls[0].findall("m:ItemGroup/m:ClInclude", _NS)
        }
        # This native SDK relationship owns emission of the corresponding
        # partial-class includes, including the generated application entry.
        self.assertEqual(headers["App.xaml.h"].find("m:DependentUpon", _NS).text, "App.xaml")
        self.assertEqual(headers["MainWindow.xaml.h"].find("m:DependentUpon", _NS).text, "MainWindow.xaml")
        self.assertIsNone(headers["pch.h"].find("m:DependentUpon", _NS))

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
        self.resource_tree = self.root / "resources/private"
        self.catalog = self.root / "catalog/private"
        self.generated.mkdir(parents=True)
        for name in ("App.xbf", "MainWindow.xbf"):
            (self.generated / name).write_text("generated " + name, encoding="utf-8")
        for tag in ("en", "fr-CA"):
            path = self.catalog / "resw" / tag / "Resources.resw"
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
        document = ET.parse(arguments[1]).getroot()
        self.calls.append(document)
        # The official PRI tool stages the declared content into its output tree.
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
        resw_links = [item.find("m:Link", _NS).text for item in document.findall("m:ItemGroup/m:PRIResource", _NS)]
        self.assertCountEqual(resw_links, ["Resources\\en\\Resources.resw", "Resources\\fr-CA\\Resources.resw"])
        # The staged copy lands byte-identical from the declared XBF input.
        self.assertEqual((self.resource_tree / "out/App.xbf").read_bytes(), (self.generated / "App.xbf").read_bytes())


class StageActionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus winui stage with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.resource_tree = self.root / "resources/private"
        payload = self.resource_tree / "out"
        payload.mkdir(parents=True)
        (payload / "Aobus.pri").write_bytes(b"official PRI fixture")
        for name in ("App.xbf", "MainWindow.xbf"):
            (payload / name).write_text("payload " + name, encoding="utf-8")
        for alias in _PAYLOAD_ASSETS:
            path = payload / "Assets" / alias
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("payload asset " + alias, encoding="utf-8")
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
            "--resources",
            str(self.resource_tree),
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
        self.assertEqual(options.output_exe.read_bytes(), self.executable.read_bytes())
        self.assertEqual(options.output_pri.read_bytes(), b"official PRI fixture")
        self.assertEqual(
            (options.output_assets / "Brand/SoulMark.svg").read_bytes(),
            (self.resource_tree / "out/Assets/Brand/SoulMark.svg").read_bytes(),
        )
        self.assertEqual((options.output_exe.parent / runtime.name).read_bytes(), runtime.read_bytes())
        self.assertFalse(list(options.output_exe.parent.glob("*.cmd")))

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
        self.assertEqual(default.output_pri.read_bytes(), (self.resource_tree / "out/Aobus.pri").read_bytes())

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
        self.assertEqual(probe.output_pri.read_bytes(), (self.resource_tree / "out/Aobus.pri").read_bytes())
        self.assertEqual(
            (probe.output_exe.parent / "App.xbf").read_bytes(),
            (self.resource_tree / "out/App.xbf").read_bytes(),
        )
        self.assertEqual(
            (probe.output_exe.parent / "MainWindow.xbf").read_bytes(),
            (self.resource_tree / "out/MainWindow.xbf").read_bytes(),
        )
        self.assertEqual(
            (probe.output_assets / "Brand/SoulMark.svg").read_bytes(),
            (self.resource_tree / "out/Assets/Brand/SoulMark.svg").read_bytes(),
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
        options = self.stage_options(directory=self.resource_tree / "out")
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            winui.run(options)
        self.assertFalse(options.output_exe.exists())


if __name__ == "__main__":
    unittest.main()
