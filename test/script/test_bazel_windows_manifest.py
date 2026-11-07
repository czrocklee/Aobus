"""Static Bazel graph contracts and native Windows manifest-verifier regressions.

Source inspection does not execute Bazel or prove action-cache behavior.
"""

import ast
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from test_bazel_test_graph import bazel_rule_has_manifest, bazel_rule_target_compatible

from ao.core import bazel_host_tools as host_tools
from ao.core import bazel_native as native
from ao.core.paths import PROJECT_ROOT


def _keywords(call):
    return {keyword.arg: keyword.value for keyword in call.keywords}


def _assigned(module, name):
    return next(
        node.value
        for node in module.body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == name for target in node.targets)
    )


def _named_rules(module):
    return {
        ast.literal_eval(_keywords(node.value)["name"]): node.value
        for node in module.body
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call) and "name" in _keywords(node.value)
    }


class WindowsManifestGraphTests(unittest.TestCase):
    def setUp(self):
        self.aspect = ast.parse((PROJECT_ROOT / "bazel/windows_utf8_manifest.bzl").read_text(encoding="utf-8"))
        self.functions = {node.name: node for node in self.aspect.body if isinstance(node, ast.FunctionDef)}
        self.build = ast.parse((PROJECT_ROOT / "test/BUILD.bazel").read_text(encoding="utf-8"))
        self.rules = _named_rules(self.build)

    def test_aspect_selects_main_repo_cc_executables_and_collects_every_propagated_edge(self):
        implementation = self.functions["_windows_utf8_manifest_impl"]
        conditions = [ast.unparse(node.test) for node in ast.walk(implementation) if isinstance(node, ast.If)]
        self.assertIn("not ctx.label.repo_name and CcInfo in target", conditions)
        self.assertIn(
            "executable and ctx.target_platform_has_constraint(ctx.attr._windows[platform_common.ConstraintValueInfo])",
            conditions,
        )
        executable = next(
            node.value
            for node in ast.walk(implementation)
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "executable" for target in node.targets)
        )
        self.assertEqual(ast.unparse(executable), "target[DefaultInfo].files_to_run.executable")
        recursive = next(node for node in implementation.body if isinstance(node, ast.For))
        edges = ["deps", "implementation_deps", "srcs", "hdrs", "data", "tools"]
        self.assertEqual(ast.literal_eval(recursive.iter), edges)
        self.assertIn("_validation_outputs(dependency)", ast.unparse(recursive))
        outputs = ast.unparse(self.functions["_validation_outputs"])
        self.assertIn("getattr(target[OutputGroupInfo], '_validation', depset())", outputs)
        aspect = _keywords(_assigned(self.aspect, "windows_utf8_manifest_aspect"))
        self.assertEqual(ast.literal_eval(aspect["attr_aspects"]), edges)
        self.assertTrue(ast.literal_eval(aspect["apply_to_generating_rules"]))
        self.assertEqual(ast.literal_eval(aspect["exec_compatible_with"]), ["@platforms//os:windows"])

    def test_validation_action_declares_binary_verifier_sdk_tools_and_both_outputs(self):
        implementation = self.functions["_windows_utf8_manifest_impl"]
        action = next(
            node
            for node in ast.walk(implementation)
            if isinstance(node, ast.Call) and ast.unparse(node.func) == "ctx.actions.run"
        )
        options = {key: ast.unparse(value) for key, value in _keywords(action).items()}
        self.assertEqual(options["executable"], "WINDOWS_MANIFEST_TOOLS['powershell']")
        self.assertEqual(options["inputs"], "[executable, ctx.file._verifier]")
        self.assertEqual(options["tools"], "ctx.files._tools")
        self.assertEqual(options["outputs"], "[manifest, stamp]")
        self.assertEqual(options["mnemonic"], "'WindowsUtf8Manifest'")
        self.assertEqual(ast.literal_eval(_keywords(action)["env"]), {"PATHEXT": ".EXE"})
        arguments = [
            ast.unparse(node)
            for node in ast.walk(implementation)
            if isinstance(node, ast.Call) and ast.unparse(node.func) == "arguments.add"
        ]
        for expression in (
            "arguments.add(ctx.file._verifier)",
            "arguments.add('-Mt', WINDOWS_MANIFEST_TOOLS['mt'])",
            "arguments.add('-Binary', executable)",
            "arguments.add('-Manifest', manifest)",
            "arguments.add('-Stamp', stamp)",
        ):
            self.assertIn(expression, arguments)
        aspect = _keywords(_assigned(self.aspect, "windows_utf8_manifest_aspect"))
        attrs = dict(zip(map(ast.literal_eval, aspect["attrs"].keys), aspect["attrs"].values, strict=True))
        self.assertEqual(ast.literal_eval(_keywords(attrs["_tools"])["cfg"]), "exec")
        self.assertIn("@aobus_native_tools//:windows_manifest_tools", ast.unparse(attrs["_tools"]))
        self.assertIn("//bazel:verify_windows_utf8_manifest.ps1", ast.unparse(attrs["_verifier"]))
        self.assertIn(
            "load('@aobus_native_tools//:windows_manifest_tools.bzl', 'WINDOWS_MANIFEST_TOOLS')",
            [ast.unparse(node.value) for node in self.aspect.body if isinstance(node, ast.Expr)],
        )

    def test_aspect_only_adds_validation_outputs_without_replacing_executables_or_runfiles(self):
        implementation = self.functions["_windows_utf8_manifest_impl"]
        returns = [ast.unparse(node.value) for node in ast.walk(implementation) if isinstance(node, ast.Return)]
        self.assertEqual(returns, ["[OutputGroupInfo(_validation=depset(transitive=validations))]"])
        self.assertIn("validations.append(depset([manifest, stamp]))", ast.unparse(implementation))
        self.assertNotIn("DefaultInfo(", ast.unparse(implementation))
        self.assertNotIn("ctx.actions.symlink", ast.unparse(implementation))

    def test_default_and_explicit_checks_share_one_aspect_and_validation_output_group(self):
        rc = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            [line for line in rc if line.startswith("build:windows --aspects=")],
            ["build:windows --aspects=//bazel:windows_utf8_manifest.bzl%windows_utf8_manifest_aspect"],
        )
        self.assertIn("build:windows --run_validations", rc)
        checks = _keywords(_assigned(self.aspect, "windows_utf8_manifest_checks"))
        attrs = dict(zip(map(ast.literal_eval, checks["attrs"].keys), checks["attrs"].values, strict=True))
        self.assertEqual(ast.unparse(_keywords(attrs["binaries"])["aspects"]), "[windows_utf8_manifest_aspect]")
        implementation = ast.unparse(self.functions["_manifest_checks_impl"])
        self.assertIn("_validation_outputs(binary) for binary in ctx.attr.binaries", implementation)
        self.assertIn("[DefaultInfo(files=outputs), OutputGroupInfo(_validation=outputs)]", implementation)
        self.assertEqual(
            ast.literal_eval(_keywords(self.rules["windows_utf8_manifest_checks"])["target_compatible_with"]),
            ["@platforms//os:windows"],
        )

    def test_explicit_checks_cover_every_windows_manifest_executable(self):
        roots = {
            label.rsplit(":", 1)[1]
            for label in ast.literal_eval(_keywords(self.rules["windows_utf8_manifest_checks"])["binaries"])
        }
        expected = {
            name
            for name, rule in self.rules.items()
            if rule.func.id in {"cc_test", "cc_binary"}
            and bazel_rule_has_manifest(_keywords(rule))
            and not ({"@platforms//os:linux", "@platforms//os:macos"} & bazel_rule_target_compatible(_keywords(rule)))
        }
        self.assertTrue(expected)
        self.assertEqual(roots, expected)
        for name in expected:
            options = _keywords(self.rules[name])
            with self.subTest(binary=name):
                self.assertIn("WINDOWS_UTF8_LINKER_INPUTS", ast.unparse(options["additional_linker_inputs"]))
                self.assertIn("WINDOWS_UTF8_LINKOPTS", ast.unparse(options["linkopts"]))
        inputs = ast.literal_eval(_assigned(self.aspect, "WINDOWS_UTF8_LINKER_INPUTS").args[0])
        linkopts = ast.literal_eval(_assigned(self.aspect, "WINDOWS_UTF8_LINKOPTS").args[0])
        self.assertEqual(inputs["@platforms//os:windows"], ["//app/windows:utf8-process.manifest"])
        self.assertEqual(
            linkopts["@platforms//os:windows"],
            ["/MANIFEST:EMBED", "/MANIFESTINPUT:$(location //app/windows:utf8-process.manifest)"],
        )
        self.assertEqual(inputs["//conditions:default"], [])
        self.assertEqual(linkopts["//conditions:default"], [])

    def test_product_data_and_catalog_exec_tool_keep_cross_package_validation_edges(self):
        # Source edges alone do not count actual configured validation actions.
        for test, directory, binary in (("cli_test", "app/cli", "aobus"), ("tui_test", "app/tui", "aobus-tui")):
            with self.subTest(binary=binary):
                data = _keywords(self.rules[test])["data"]
                self.assertIsInstance(data, ast.List)
                self.assertIn(f"//{directory}:{binary}", ast.literal_eval(data))
                product = _named_rules(
                    ast.parse((PROJECT_ROOT / directory / "BUILD.bazel").read_text(encoding="utf-8"))
                )
                options = _keywords(product[binary])
                self.assertEqual(product[binary].func.id, "cc_binary")
                self.assertEqual(ast.unparse(options["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
                self.assertEqual(ast.unparse(options["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
        catalog = ast.parse((PROJECT_ROOT / "bazel/catalog.bzl").read_text(encoding="utf-8"))
        attrs = _keywords(_assigned(catalog, "catalog"))["attrs"]
        tools = next(
            value for key, value in zip(attrs.keys, attrs.values, strict=True) if ast.literal_eval(key) == "tools"
        )
        self.assertEqual(ast.unparse(tools.func), "attr.label_list")
        self.assertEqual(ast.literal_eval(_keywords(tools)["cfg"]), "exec")
        self.assertEqual(ast.unparse(_keywords(tools)["default"]), "[Label('//tool/catalog:ao_catalog_compiler')]")
        compiler = _named_rules(ast.parse((PROJECT_ROOT / "tool/catalog/BUILD.bazel").read_text(encoding="utf-8")))
        options = _keywords(compiler["ao_catalog_compiler"])
        self.assertEqual(compiler["ao_catalog_compiler"].func.id, "cc_binary")
        self.assertEqual(ast.unparse(options["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
        self.assertEqual(ast.unparse(options["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
        loads = [ast.unparse(node.value) for node in self.build.body if isinstance(node, ast.Expr)]
        self.assertIn(
            "load('//bazel:windows_utf8_manifest.bzl', 'WINDOWS_UTF8_LINKER_INPUTS', "
            "'WINDOWS_UTF8_LINKOPTS', 'windows_utf8_manifest_checks')",
            loads,
        )

    def test_generated_source_and_header_routes_reach_the_catalog_exec_tool(self):
        # Both routes must work independently: srcs reachability cannot mask hdrs.
        i18n = _named_rules(ast.parse((PROJECT_ROOT / "app/i18n/BUILD.bazel").read_text(encoding="utf-8")))
        aspect = _keywords(_assigned(self.aspect, "windows_utf8_manifest_aspect"))
        propagated = ast.literal_eval(aspect["attr_aspects"])
        implementation = self.functions["_windows_utf8_manifest_impl"]
        collected = ast.literal_eval(next(node.iter for node in implementation.body if isinstance(node, ast.For)))
        for library, attribute, group, output in (
            ("catalog_data", "srcs", "catalog_cpp", "cpp"),
            ("catalog_locale_tags", "hdrs", "catalog_tags", "header"),
        ):
            with self.subTest(consumer=library):
                for owner, edge, dependency in (
                    ("i18n", "implementation_deps", library),
                    (library, attribute, group),
                    (group, "srcs", "catalog_codegen"),
                ):
                    self.assertIn(":" + dependency, ast.literal_eval(_keywords(i18n[owner])[edge]))
                    self.assertIn(edge, propagated)
                    self.assertIn(edge, collected)
                self.assertEqual(i18n[group].func.id, "filegroup")
                self.assertEqual(ast.literal_eval(_keywords(i18n[group])["output_group"]), output)
        self.assertEqual(i18n["catalog_codegen"].func.id, "catalog")
        catalog = ast.parse((PROJECT_ROOT / "bazel/catalog.bzl").read_text(encoding="utf-8"))
        attrs = _keywords(_assigned(catalog, "catalog"))["attrs"]
        tools = next(
            value for key, value in zip(attrs.keys, attrs.values, strict=True) if ast.literal_eval(key) == "tools"
        )
        self.assertEqual(ast.literal_eval(_keywords(tools)["cfg"]), "exec")
        self.assertEqual(ast.unparse(_keywords(tools)["default"]), "[Label('//tool/catalog:ao_catalog_compiler')]")
        self.assertIn("tools", propagated)
        self.assertIn("tools", collected)


class WindowsManifestToolTests(unittest.TestCase):
    """Observe SDK inputs and execute the checked-in projection with a recording ctx."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-manifest-tools-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.sdk = self.root / "Åsa 音乐/SDK"
        self.mt = self.sdk / "bin/selected/x64/mt.exe"
        self.system = self.root / "Windows"
        self.powershell = self.system / "System32/WindowsPowerShell/v1.0/powershell.exe"
        self.gperf = self.root / "gperf.exe"
        self.runtime = self.root / "gperf-runtime.dll"
        for path in (self.mt, self.powershell, self.gperf, self.runtime):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"selected SDK input")
        self.environment = {
            "SYSTEMROOT": str(self.system),
            "WindowsSdkDir": str(self.sdk),
            "WindowsSDKVersion": "selected\\",
        }
        self.enterContext(
            patch.object(
                host_tools, "_native_gperf", return_value=({"version": "fixture"}, self.gperf, [self.runtime], [])
            )
        )
        self.catalog = self.enterContext(
            patch.object(
                host_tools,
                "_catalog_sdk",
                return_value=(
                    {
                        "python": str(self.root / "python"),
                        "genrb": str(self.root / "genrb"),
                        "pkgdata": str(self.root / "pkgdata"),
                    },
                    [],
                    [],
                ),
            )
        )

        def query(command, **kwargs):
            self.assertEqual(command, [str(self.gperf), "--version"])
            self.assertTrue(kwargs["check"])
            return subprocess.CompletedProcess(command, 0, "GNU gperf fixture\n", "")

        self.enterContext(patch.object(host_tools.subprocess, "run", side_effect=query))
        self.files = {}
        self.links = []
        namespace = {"json": SimpleNamespace(encode=json.dumps)}
        source = (PROJECT_ROOT / "bazel/host_tools_repository.bzl").read_text(encoding="utf-8")
        exec(compile(source, "host_tools_repository.bzl", "exec"), namespace)
        self.materialize = namespace["materialize_host_tools"]

    def resolve(self, host="windows"):
        with patch.dict(os.environ, self.environment, clear=True):
            return host_tools.resolve_host_tools(self.root, self.root / "provider", host)

    def project(self, data):
        class BazelPath:
            def __init__(self, source: str | Path) -> None:
                self.path = Path(source)

            @property
            def realpath(self) -> "BazelPath":
                return BazelPath(self.path.resolve())

            def __str__(self) -> str:
                return self.path.as_posix()

        context = SimpleNamespace(
            path=BazelPath,
            symlink=lambda source, destination: self.links.append({"source": str(source), "destination": destination}),
            file=lambda name, content, **_: self.files.__setitem__(name, content),
        )
        self.materialize(context, data)
        namespace = {}
        exec(self.files["host_tools_data.bzl"], namespace)
        return namespace["HOST_TOOLS"]

    def test_sdk_tool_payloads_are_declared_inputs_and_both_lexical_and_resolved_paths_are_watched(self):
        if os.name != "nt":
            installed = self.mt.with_name("installed-mt.exe")
            self.mt.rename(installed)
            self.mt.symlink_to(installed)
        observed = []
        with native.capture_probes(observed.append):
            data = self.resolve()
        self.assertIn(self.powershell, observed)
        for tool in (self.mt, self.powershell):
            self.assertLessEqual({str(tool.absolute()), str(tool.resolve())}, set(data["watch_files"]))
        manifest_tools = {"mt": self.mt.resolve().as_posix(), "powershell": self.powershell.resolve().as_posix()}
        self.assertEqual(data["manifest_tools"], manifest_tools)
        projected = self.project(data)
        self.assertEqual(
            projected["windows_manifest_inputs"], ["windows-manifest/mt.exe", "windows-manifest/powershell.exe"]
        )
        for name, tool in manifest_tools.items():
            self.assertIn({"source": tool, "destination": "windows-manifest/" + name + ".exe"}, self.links)
        source = self.files["windows_manifest_tools.bzl"]
        self.assertIn("Åsa", source)
        self.assertIn("音乐", source)
        self.assertNotRegex(source, r"(?<!\\)(?:\\\\)*\\[uU]")
        namespace = {}
        exec(source, namespace)
        self.assertEqual(namespace["WINDOWS_MANIFEST_TOOLS"], manifest_tools)
        groups = {}
        definitions = {
            "native": SimpleNamespace(
                exports_files=lambda *args, **kwargs: None,
                alias=lambda **kwargs: None,
                filegroup=lambda **kwargs: groups.__setitem__(kwargs["name"], kwargs["srcs"]),
                config_setting=lambda **kwargs: None,
            ),
            "select": lambda values, **kwargs: values,
        }
        exec((PROJECT_ROOT / "bazel/host_tools.bzl").read_text(encoding="utf-8"), definitions)
        definitions["host_tools"](projected)
        self.assertEqual(groups["windows_manifest_tools"], projected["windows_manifest_inputs"])

    def test_missing_or_relative_system_powershell_is_rejected_before_sdk_inputs_are_published(self):
        self.powershell.unlink()
        for system in ("", "relative/system", str(self.system)):
            self.environment["SYSTEMROOT"] = system
            with (
                self.subTest(system=system),
                self.assertRaisesRegex(ValueError, "selected Windows PowerShell runtime is missing"),
            ):
                self.resolve()
            self.catalog.assert_not_called()
        self.assertEqual(self.files, {})
        self.assertEqual(self.links, [])

    def test_manifest_tool_requires_the_selected_sdk_without_searching_path(self):
        self.mt.unlink()
        for sdk, version in (
            ("", "selected"),
            ("relative/sdk", "selected"),
            (str(self.sdk), ""),
            (str(self.sdk), "selected"),
        ):
            self.environment.update(WindowsSdkDir=sdk, WindowsSDKVersion=version)
            with (
                self.subTest(sdk=sdk, version=version),
                patch.object(host_tools.native, "executable", side_effect=AssertionError("no ambient mt.exe")),
                self.assertRaisesRegex(native.BazelWorkspaceError, "selected Windows SDK manifest tool is missing"),
            ):
                self.resolve()
            self.catalog.assert_not_called()
        self.assertEqual(self.files, {})
        self.assertEqual(self.links, [])

    def test_manifest_tool_metadata_stays_distinct_from_the_complete_artifact_inventory(self):
        data = self.resolve()
        self.project(data)
        manifest = json.loads(self.files["manifest.json"])
        self.assertEqual(manifest["artifacts"], self.links)
        self.assertEqual(manifest["windows_manifest_tools"], data["manifest_tools"])
        self.assertEqual(len(self.links), 4)
        self.assertIn({"source": self.gperf.as_posix(), "destination": "bin/gperf.exe"}, self.links)
        self.assertIn({"source": self.runtime.as_posix(), "destination": "bin/gperf-runtime.dll"}, self.links)
        non_windows = self.resolve("macos")
        self.assertEqual(non_windows["manifest_tools"], {})
        self.files.clear()
        self.links.clear()
        projected = self.project(non_windows)
        self.assertEqual(projected["windows_manifest_inputs"], [])
        self.assertEqual(json.loads(self.files["manifest.json"])["windows_manifest_tools"], {})
        self.assertTrue(all(link["destination"].startswith("bin/") for link in self.links))


@unittest.skipUnless(sys.platform == "win32", "Requires native Windows PowerShell and cmd.exe")
class WindowsManifestVerifierTests(unittest.TestCase):
    def setUp(self):
        base = Path(tempfile.gettempdir()).resolve()
        if (
            not base.drive
            or str(base).startswith("\\\\")
            or len(str(base)) > 120
            or base.is_relative_to(PROJECT_ROOT.resolve())
        ):
            self.skipTest("Verifier fixtures require a short local ordinary TEMP outside the checkout")
        self.powershell = Path(os.environ["SYSTEMROOT"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        self.assertTrue(self.powershell.is_file(), f"Missing system PowerShell: {self.powershell}")
        temporary = tempfile.TemporaryDirectory(prefix="aobus manifest ", dir=base)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "dummy program.exe"
        self.binary.write_bytes(b"owned dummy executable; never loaded")
        self.mt = self.root / "fake mt.cmd"
        self.fixture = self.root / "fixture.xml"
        self.manifest = self.root / "extracted manifest.xml"
        self.stamp = self.root / "verified manifest.ok"
        self.write_mt()

    def write_mt(self, status=0):
        self.mt.write_text(
            "@echo off\n"
            "setlocal\n"
            'set "output=%~3"\n'
            'echo %* > "%~dp0mt arguments.txt"\n'
            f"if not {status}==0 exit /b {status}\n"
            'copy /b "%~dp0fixture.xml" "%output:~5%" >nul\n'
            "exit /b %errorlevel%\n",
            encoding="ascii",
        )

    def verify(self, xml):
        self.fixture.write_text(xml, encoding="utf-8")
        return subprocess.run(
            [
                str(self.powershell),
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(PROJECT_ROOT / "bazel/verify_windows_utf8_manifest.ps1"),
                "-Mt",
                str(self.mt),
                "-Binary",
                str(self.binary),
                "-Manifest",
                str(self.manifest),
                "-Stamp",
                str(self.stamp),
            ],
            cwd=self.root,
            capture_output=True,
            timeout=30,
            check=False,
        )

    def test_resource_one_accepts_single_utf8_and_writes_deterministic_stamp_with_spaced_paths(self):
        xml = (
            '<assembly><activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">'
            "UTF-8</activeCodePage></assembly>"
        )
        result = self.verify(xml)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.stamp.read_bytes(), b"UTF-8\n")
        self.assertEqual(self.manifest.read_bytes(), self.fixture.read_bytes())
        arguments = (self.root / "mt arguments.txt").read_text()
        self.assertIn(f"-inputresource:{self.binary};#1", arguments)
        self.assertIn(f"-out:{self.manifest}", arguments)
        self.assertEqual(self.binary.read_bytes(), b"owned dummy executable; never loaded")

    def test_invalid_xml_or_nonunique_exact_utf8_value_never_writes_success_stamp(self):
        namespace = "http://schemas.microsoft.com/SMI/2019/WindowsSettings"
        fixtures = {
            "missing": "<assembly/>",
            "wrong_namespace": '<assembly><activeCodePage xmlns="urn:wrong">UTF-8</activeCodePage></assembly>',
            "wrong_element_case": f'<assembly xmlns="{namespace}"><activecodepage>UTF-8</activecodepage></assembly>',
            "wrong_value": f'<assembly xmlns="{namespace}"><activeCodePage>1252</activeCodePage></assembly>',
            "wrong_value_case": f'<assembly xmlns="{namespace}"><activeCodePage>utf-8</activeCodePage></assembly>',
            "duplicate": (
                f'<assembly xmlns="{namespace}"><activeCodePage>UTF-8</activeCodePage>'
                "<activeCodePage>UTF-8</activeCodePage></assembly>"
            ),
            "malformed": "<assembly>",
            "dtd": (
                '<!DOCTYPE assembly [<!ENTITY codepage "UTF-8">]>'
                f'<assembly xmlns="{namespace}"><activeCodePage>&codepage;</activeCodePage></assembly>'
            ),
        }
        for name, xml in fixtures.items():
            with self.subTest(fixture=name):
                result = self.verify(xml)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(self.stamp.exists())

    def test_sdk_failure_is_nonzero_and_never_writes_success_stamp(self):
        self.write_mt(status=7)
        result = self.verify("<assembly/>")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.stamp.exists())
        self.assertFalse(self.manifest.exists())
        self.assertIn(b"mt.exe failed with exit 7", result.stderr)


if __name__ == "__main__":
    unittest.main()
