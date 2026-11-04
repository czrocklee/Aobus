"""Catalog inventory/action contracts; native ICU and full-link gates are separate."""

import ast
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from test_bazel_query import rule_fields

from ao.core.paths import PROJECT_ROOT

_SPEC = importlib.util.spec_from_file_location("build_catalog", PROJECT_ROOT / "tool/catalog/build_catalog.py")
assert _SPEC is not None and _SPEC.loader is not None
catalog = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(catalog)


class BazelCatalogGenerationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus catalog with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.authored = self.root / "authored"
        self.authored.mkdir()
        self.inventory = self.authored / "package.lst"
        self.template = self.root / "CatalogLocaleTags.h.in"
        self.template.write_text("tags {\n@AOBUS_CATALOG_LOCALE_TAG_LITERALS@}\n", encoding="utf-8")
        self.sources = []
        self.set_inventory(["root", "zh_Hant", "de", "fr_CA", "qps_Ploc"])
        self.tools = self.root / "native tools"
        self.tools.mkdir()
        for name in ("compiler", "genrb", "pkgdata"):
            (self.tools / name).touch()
        output = self.root / "output"
        self.options = catalog.parse_args(
            [
                "--inventory",
                str(self.inventory),
                "--template",
                str(self.template),
                "--compiler",
                str(self.tools / "compiler"),
                "--genrb",
                str(self.tools / "genrb"),
                "--pkgdata",
                str(self.tools / "pkgdata"),
                "--private",
                str(output / "private"),
                "--cpp",
                str(output / "EmbeddedCatalogData.cpp"),
                "--header",
                str(output / "CatalogLocaleTags.h"),
                "--dat",
                str(output / "aobus_messages.dat"),
                "--systemroot",
                str(self.root / "Windows"),
                *[argument for source in self.sources for argument in ("--source", str(source))],
            ]
        )

    def set_inventory(self, locales):
        self.inventory.write_text("".join(locale + ".res\n" for locale in locales), encoding="utf-8")
        self.sources = [self.authored / (locale + ".txt") for locale in locales if locale != "qps_Ploc"]
        for source in self.sources:
            source.write_text("fixture", encoding="utf-8")

    def test_inventory_owns_authored_members_and_regional_tags(self):
        self.assertEqual(
            catalog.read_inventory(self.inventory, self.sources),
            [
                ("root", "en"),
                ("zh_Hant", "zh-Hant"),
                ("de", "de"),
                ("fr_CA", "fr-CA"),
                ("qps_Ploc", "qps-ploc"),
            ],
        )
        output = self.root / "tags.h"
        catalog.write_locale_header(self.template, output, catalog.read_inventory(self.inventory, self.sources))
        self.assertEqual(
            output.read_text(encoding="utf-8"), 'tags {\n    "en",\n    "de",\n    "fr-CA",\n    "zh-Hant",\n}\n'
        )

    def test_inventory_rejects_unsafe_duplicate_missing_and_projection_collisions(self):
        invalid = [
            "root.res\nqps_Ploc.res\n../de.res\n",
            "root.res\nqps_Ploc.res\n/de.res\n",
            "root.res\nqps_Ploc.res\nde\\DE.res\n",
            "root.res\nqps_Ploc.res\n de.res\n",
            "root.res\nqps_Ploc.res\nde.res\nde.res\n",
            "root.res\nqps_Ploc.res\nen.res\n",
            "root.res\nqps_Ploc.res\nqps_ploc.res\n",
            "root.res\nqps_Ploc.res\ncon.res\n",
            "root.res\nqps_Ploc.res\n\n",
            "root.res\n",
            "qps_Ploc.res\n",
            "",
        ]
        for text in invalid:
            with self.subTest(text=text):
                self.inventory.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    catalog.read_inventory(self.inventory, self.sources)

    def test_authored_inputs_must_be_exact_declared_existing_members(self):
        for sources in (self.sources[:-1], [*self.sources, self.sources[0]], [*self.sources, self.template]):
            with self.subTest(sources=sources), self.assertRaises(ValueError):
                catalog.read_inventory(self.inventory, sources)
        self.sources[0].unlink()
        with self.assertRaisesRegex(ValueError, "Missing authored"):
            catalog.read_inventory(self.inventory, self.sources)

    def test_template_rejects_missing_duplicate_and_unresolved_substitutions(self):
        locales = catalog.read_inventory(self.inventory, self.sources)
        for text in (
            "missing",
            "@AOBUS_CATALOG_LOCALE_TAG_LITERALS@" * 2,
            "@AOBUS_CATALOG_LOCALE_TAG_LITERALS@ @OTHER@",
        ):
            with self.subTest(text=text):
                self.template.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    catalog.write_locale_header(self.template, self.root / "tags.h", locales)

    def fake_native(self, arguments, **kwargs):
        self.assertEqual(kwargs["cwd"], self.options.private)
        self.assertIs(kwargs["check"], True)
        self.assertNotIn("shell", kwargs)
        self.assertNotIn("PATH", kwargs["env"])
        for name in ("TEMP", "TMP", "TMPDIR", "PYTHONPYCACHEPREFIX"):
            self.assertTrue(Path(kwargs["env"][name]).is_relative_to(self.options.private))
            self.assertTrue(Path(kwargs["env"][name]).is_dir())
        self.assertEqual(kwargs["env"]["SYSTEMROOT"], self.options.systemroot)
        executable = Path(arguments[0]).name
        resources = self.options.private / "res"
        if executable == "genrb":
            self.assertEqual(arguments[1:4], ["--strict", "--encoding", "UTF-8"])
            for name in arguments[8:]:
                (resources / (Path(name).stem + ".res")).write_bytes(b"resource")
        elif executable == "compiler":
            for index, argument in enumerate(arguments):
                if argument in {"--pseudo-source", "--en-resw", "--pseudo-resw"}:
                    output = Path(arguments[index + 1])
                    self.assertTrue(output.parent.is_dir())
                    output.write_text("generated", encoding="utf-8")
                elif argument == "--translation":
                    self.assertTrue(Path(arguments[index + 1]).is_file())
                    output = Path(arguments[index + 2])
                    self.assertTrue(output.parent.is_dir())
                    output.write_text("translation", encoding="utf-8")
        elif executable == "pkgdata":
            for locale in ("root", "zh_Hant", "de", "fr_CA", "qps_Ploc"):
                self.assertTrue((resources / (locale + ".res")).is_file())
            (resources / "aobus_messages.dat").write_bytes(bytes(range(19)))
        else:
            self.fail(f"Unexpected native tool: {executable}")
        return subprocess.CompletedProcess(arguments, 0)

    def test_pipeline_runs_real_tool_contracts_in_order_and_publishes_fixed_outputs(self):
        with (
            mock.patch.dict(os.environ, {"PATH": "must not leak", "TEMP": "must not leak"}),
            mock.patch.object(catalog.subprocess, "run", side_effect=self.fake_native) as execute,
        ):
            catalog.run(self.options)
        calls = [call.args[0] for call in execute.call_args_list]
        self.assertEqual([Path(args[0]).name for args in calls], ["genrb", "compiler", "genrb", "pkgdata"])
        self.assertEqual(calls[0][8:], ["root.txt", "zh_Hant.txt", "de.txt", "fr_CA.txt"])
        self.assertEqual(calls[2][8:], ["qps_Ploc.txt"])
        self.assertEqual(
            calls[3][1:8], ["--quiet", "--force-prefix", "--name", "aobus_messages", "--mode", "common", "--sourcedir"]
        )
        self.assertEqual(calls[3][-1], str(self.inventory))
        for args in calls:
            self.assertTrue(Path(args[0]).is_absolute())
        self.assertEqual(self.options.dat.read_bytes(), bytes(range(19)))
        cpp = self.options.cpp.read_text(encoding="utf-8")
        self.assertIn('#include "app/i18n/EmbeddedCatalogData.h"', cpp)
        self.assertIn("alignas(16) constexpr std::byte kEmbeddedCatalogData[]", cpp)
        self.assertIn("namespace ao::i18n::detail", cpp)
        self.assertIn("std::span<std::byte const> embeddedCatalogData() noexcept", cpp)
        self.assertEqual(cpp.count("std::byte{0x"), 19)
        self.assertIn("std::byte{0x12},", cpp)
        self.assertNotIn("qps-ploc", self.options.header.read_text(encoding="utf-8"))

    def test_native_failure_is_foreground_and_stops_later_stages(self):
        for failed_stage in range(4):
            calls = []

            def fail(arguments, *, calls=calls, failed_stage=failed_stage, **kwargs):
                calls.append(arguments)
                if len(calls) == failed_stage + 1:
                    raise subprocess.CalledProcessError(17, arguments)
                return self.fake_native(arguments, **kwargs)

            with self.subTest(stage=failed_stage), mock.patch.object(catalog.subprocess, "run", side_effect=fail):
                with self.assertRaises(subprocess.CalledProcessError):
                    catalog.run(self.options)
            self.assertEqual(len(calls), failed_stage + 1)
            self.assertFalse(self.options.cpp.exists())

    def test_empty_data_and_overlapping_outputs_fail_closed(self):
        empty = self.root / "empty.dat"
        empty.touch()
        with self.assertRaisesRegex(ValueError, "empty"):
            catalog.embed_data(empty, self.options.cpp)
        self.options.private = self.options.cpp.parent
        with mock.patch.object(catalog.subprocess, "run") as execute:
            with self.assertRaisesRegex(ValueError, "Overlapping"):
                catalog.run(self.options)
            execute.assert_not_called()


class BazelCatalogDeclarationsTest(unittest.TestCase):
    def fields(self, directory, kind, name):
        return next(
            fields
            for fields in rule_fields(PROJECT_ROOT / directory / "BUILD.bazel", kind)
            if ast.literal_eval(fields["name"]) == name
        )

    def test_tool_uses_narrow_path_and_exec_icu_without_full_utility(self):
        compiler = self.fields("tool/catalog", "cc_binary", "ao_catalog_compiler")
        dependencies = ast.literal_eval(compiler["deps"])
        self.assertIn("//lib/utility:path", dependencies)
        self.assertIn("//app/include:catalog_headers", dependencies)
        self.assertNotIn("//app/include:i18n_headers", dependencies)
        self.assertNotIn("//lib/utility:utility", dependencies)
        self.assertEqual(ast.unparse(compiler["linkopts"]), "WINDOWS_UTF8_LINKOPTS")
        self.assertEqual(ast.unparse(compiler["additional_linker_inputs"]), "WINDOWS_UTF8_LINKER_INPUTS")
        for name, dependency in (
            ("catalog_pattern", "@aobus_native_deps//:icu"),
            ("catalog_compiler_pattern", "@aobus_native_tools//:catalog_icu"),
        ):
            fields = self.fields("app/i18n", "cc_library", name)
            self.assertEqual(ast.literal_eval(fields["implementation_deps"]), [dependency])
            self.assertEqual(ast.literal_eval(fields["include_prefix"]), "i18n")
            self.assertEqual(
                ast.literal_eval(fields["deps"]), ["//app/include:catalog_headers", "//include:utility_headers"]
            )

    def test_catalog_header_capsule_keeps_exec_tools_out_of_runtime_imports(self):
        headers = self.fields("app/include", "cc_library", "catalog_headers")
        self.assertCountEqual(
            ast.literal_eval(headers["hdrs"]), ["ao/i18n/MessageCatalog.h", "ao/i18n/MessageInventory.def"]
        )
        self.assertEqual(ast.literal_eval(headers["deps"]), ["//include:utility_headers"])
        path = self.fields("lib/utility", "cc_library", "path")
        self.assertEqual(ast.literal_eval(path["srcs"]), ["Path.cpp"])
        self.assertEqual(ast.literal_eval(path["deps"]), ["//include:utility_headers"])
        contract = self.fields("include", "cc_library", "utility_headers")
        self.assertIn("//bazel:compile_contract", ast.literal_eval(contract["deps"]))

    def test_interactive_policy_labels_keep_the_full_public_header_contract(self):
        for name, source in (("ordering", "IcuTextOrdering.cpp"), ("completion", "IcuCompletionAliases.cpp")):
            with self.subTest(target=name):
                fields = self.fields("app/i18n", "cc_library", name)
                self.assertIn(source, ast.literal_eval(fields["srcs"]))
                self.assertIn("//app/include:i18n_headers", ast.literal_eval(fields["deps"]))

    def test_action_declares_compiler_runtime_tools_isolated_python_and_private_side_outputs(self):
        tree = ast.parse((PROJECT_ROOT / "bazel/catalog.bzl").read_text(encoding="utf-8"))
        declaration = next(
            node.value
            for node in tree.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "catalog" for target in node.targets)
        )
        attributes = next(keyword.value for keyword in declaration.keywords if keyword.arg == "attrs")
        attrs = {ast.literal_eval(key): value for key, value in zip(attributes.keys, attributes.values, strict=True)}
        tools = {keyword.arg: keyword.value for keyword in attrs["tools"].keywords}
        self.assertEqual(ast.unparse(attrs["tools"].func), "attr.label_list")
        self.assertEqual(ast.literal_eval(tools["cfg"]), "exec")
        self.assertEqual(ast.unparse(tools["default"]), "[Label('//tool/catalog:ao_catalog_compiler')]")
        native = {keyword.arg: keyword.value for keyword in attrs["_native_tools"].keywords}
        self.assertEqual(ast.literal_eval(native["cfg"]), "exec")
        self.assertEqual(ast.unparse(native["default"]), "Label('@aobus_native_tools//:catalog_tools')")
        action = next(
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and ast.unparse(node.func) == "ctx.actions.run"
        )
        options = {keyword.arg: keyword.value for keyword in action.keywords}
        self.assertEqual(ast.unparse(options["executable"]), "CATALOG_TOOLS['python']")
        self.assertEqual(ast.unparse(options["tools"]), "[compiler] + ctx.files._native_tools")
        self.assertEqual(ast.unparse(options["outputs"]), "[cpp, header, data, private]")
        self.assertIs(ast.literal_eval(options["use_default_shell_env"]), False)
        source = ast.unparse(tree)
        self.assertIn("ctx.attr.tools[0][DefaultInfo].files_to_run", source)
        self.assertIn("env['SYSTEMROOT'] = CATALOG_TOOLS['SystemRoot']", source)
        self.assertIn("arguments.add('--systemroot', CATALOG_TOOLS['SystemRoot'])", source)
        self.assertIn("arguments.add('--private', private.path)", source)
        self.assertNotIn("CATALOG_TOOLS['systemroot']", source)
        interpreter = next(
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and ast.unparse(node.func) == "arguments.add_all"
        )
        flags = interpreter.args[0].elts
        self.assertEqual([ast.literal_eval(flag) for flag in flags[:6]], ["-I", "-S", "-B", "-X", "utf8", "-X"])
        self.assertEqual(ast.unparse(flags[6]), "'pycache_prefix=' + private.path + '/pycache'")
        groups = next(
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and ast.unparse(node.func) == "OutputGroupInfo"
        )
        self.assertEqual(
            {keyword.arg: ast.unparse(keyword.value) for keyword in groups.keywords},
            {
                "cpp": "depset([cpp])",
                "header": "depset([header])",
                "dat": "depset([data])",
                "intermediates": "depset([private])",
            },
        )
        codegen = self.fields("app/i18n", "catalog", "catalog_codegen")
        self.assertEqual(ast.literal_eval(codegen["inventory"]), "catalog/package.lst")
        self.assertEqual(ast.unparse(codegen["srcs"]), "glob(['catalog/*.txt'])")
        data = self.fields("app/i18n", "cc_library", "catalog_data")
        self.assertEqual(ast.literal_eval(data["srcs"]), [":catalog_cpp"])
        tags = self.fields("app/i18n", "cc_library", "catalog_locale_tags")
        self.assertEqual(ast.literal_eval(tags["strip_include_prefix"]), "catalog_codegen")
        self.assertEqual(ast.literal_eval(tags["visibility"]), ["//visibility:private"])


if __name__ == "__main__":
    unittest.main()
