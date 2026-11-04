"""Static Bazel graph contracts and native Windows manifest-verifier regressions.

Source inspection does not execute Bazel or prove action-cache behavior.
"""

import ast
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_bazel_test_graph import bazel_rule_has_manifest, bazel_rule_target_compatible

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


class WindowsManifestToolSourceTests(unittest.TestCase):
    """Inspect resolver declarations without installing a process-global audit hook."""

    def setUp(self):
        resolver = ast.parse((PROJECT_ROOT / "script/ao/core/bazel_resolver.py").read_text(encoding="utf-8"))
        self.tools = next(
            node
            for node in ast.walk(resolver)
            if isinstance(node, ast.If) and ast.unparse(node.test) == "args.group == 'tools'"
        )
        self.windows = next(
            node
            for node in self.tools.body
            if isinstance(node, ast.If) and ast.unparse(node.test) == "host == 'windows'"
        )
        self.outputs = {
            ast.literal_eval(target.slice): node.value
            for node in self.tools.body
            if isinstance(node, ast.Assign)
            for target in node.targets
            if isinstance(target, ast.Subscript) and ast.unparse(target.value) == "result['files']"
        }

    def test_sdk_tool_payloads_are_declared_inputs_and_both_lexical_and_resolved_paths_are_watched(self):
        tools = next(node for node in self.windows.body if isinstance(node, ast.For))
        self.assertEqual(ast.unparse(tools.iter), "{'mt': workspace._windows_mt(), 'powershell': powershell}.items()")
        statements = [ast.unparse(node) for node in tools.body]
        self.assertIn("manifest_tools[name] = tool.resolve().as_posix()", statements)
        self.assertIn("destination = 'windows-manifest/' + name + '.exe'", statements)
        self.assertIn("manifest_inputs.append(destination)", statements)
        self.assertIn("result['links'].append({'source': str(tool.resolve()), 'destination': destination})", statements)
        self.assertIn("files.update({str(tool.absolute()), str(tool.resolve())})", statements)
        constants = [
            node.value
            for node in ast.walk(self.outputs["windows_manifest_tools.bzl"])
            if isinstance(node, ast.Constant) and isinstance(node.value, str)
        ]
        self.assertIn("WINDOWS_MANIFEST_TOOLS = ", "".join(constants))
        self.assertIn(
            "json.dumps(manifest_tools, sort_keys=True, ensure_ascii=False)",
            ast.unparse(self.outputs["windows_manifest_tools.bzl"]),
        )
        build_constants = "".join(
            node.value
            for node in ast.walk(self.outputs["BUILD.bazel"])
            if isinstance(node, ast.Constant) and isinstance(node.value, str)
        )
        self.assertIn('filegroup(name = "windows_manifest_tools", srcs = ', build_constants)
        self.assertIn("json.dumps(manifest_inputs, ensure_ascii=False)", ast.unparse(self.outputs["BUILD.bazel"]))

    def test_missing_or_relative_system_powershell_is_rejected_before_sdk_inputs_are_published(self):
        self.assertEqual(ast.unparse(_assigned(self.windows, "system")), "Path(os.environ.get('SYSTEMROOT', ''))")
        self.assertEqual(
            ast.unparse(_assigned(self.windows, "powershell")),
            "workspace._probe(system / 'System32/WindowsPowerShell/v1.0/powershell.exe')",
        )
        guard = next(node for node in self.windows.body if isinstance(node, ast.If))
        self.assertEqual(ast.unparse(guard.test), "not system.is_absolute() or not powershell.is_file()")
        self.assertEqual(len(guard.body), 1)
        self.assertIsInstance(guard.body[0], ast.Raise)
        self.assertEqual(ast.unparse(guard.body[0].exc), "ValueError('selected Windows PowerShell runtime is missing')")
        publish = next(node for node in self.windows.body if isinstance(node, ast.For))
        self.assertLess(guard.lineno, publish.lineno)

    def test_manifest_tool_metadata_stays_distinct_from_the_complete_artifact_inventory(self):
        self.assertEqual(ast.literal_eval(_assigned(self.tools, "manifest_tools")), {})
        self.assertEqual(ast.literal_eval(_assigned(self.tools, "manifest_inputs")), [])
        gperf = _assigned(self.tools, "artifacts")
        self.assertEqual(ast.unparse(gperf), "[executable, *runtime]")
        self.assertLess(gperf.lineno, self.windows.lineno)
        metadata = next(node for node in ast.walk(self.outputs["manifest.json"]) if isinstance(node, ast.Dict))
        fields = {
            ast.literal_eval(key): ast.unparse(value) for key, value in zip(metadata.keys, metadata.values, strict=True)
        }
        # Guard the Windows role separation, not unrelated tool-family metadata.
        self.assertEqual(fields["artifacts"], "result['links']")
        self.assertEqual(fields["windows_manifest_tools"], "manifest_tools")
        # Only the Windows conditional may fill either manifest-tool collection.
        for statement in self.tools.body:
            if statement is self.windows:
                continue
            for node in ast.walk(statement):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
                    self.assertNotIn(
                        ast.unparse(node.func),
                        {"manifest_tools.update", "manifest_inputs.append", "manifest_inputs.extend"},
                    )
                if isinstance(node, ast.Assign):
                    self.assertFalse(
                        any(
                            isinstance(target, ast.Subscript) and ast.unparse(target.value) == "manifest_tools"
                            for target in node.targets
                        )
                    )


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
