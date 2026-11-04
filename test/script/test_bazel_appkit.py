"""AppKit product graph and bundle-assembly contracts; native gates stay separate."""

import ast
import importlib.util
import json
import os
import plistlib
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_bazel_gtk import string_literals
from test_bazel_uimodel import fields_for

from ao.core.paths import PROJECT_ROOT

APPKIT_PACKAGE = PROJECT_ROOT / "app/macos-appkit"

_SPEC = importlib.util.spec_from_file_location("build_bundle", APPKIT_PACKAGE / "build_bundle.py")
assert _SPEC is not None and _SPEC.loader is not None
build_bundle = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(build_bundle)


def cmake_appkit_units():
    """Derive ao_appkit's compilation units and the bundle entry from CMake."""
    text = (APPKIT_PACKAGE / "CMakeLists.txt").read_text(encoding="utf-8")
    library = re.search(r"add_library\(ao_appkit OBJECT([^)]*)\)", text, re.DOTALL).group(1)
    executable = re.search(r"add_executable\(aobus-appkit MACOSX_BUNDLE([^)]*)\)", text, re.DOTALL).group(1)
    units = set(re.findall(r"[\w.-]+\.(?:mm|cpp|h)\b", library))
    entries = re.findall(r"[\w.-]+\.mm\b", executable)
    assert len(entries) == 1, "expected exactly one CMake bundle entry unit"
    return units, entries[0]


def cmake_appkit_smoke_units():
    """Derive the CMake AppKit smoke harness units from their real owner."""
    text = (PROJECT_ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
    executable = re.search(r"add_executable\(ao_appkit_smoke MACOSX_BUNDLE([^)]*)\)", text, re.DOTALL)
    assert executable is not None, "CMake AppKit smoke declaration was not found"
    return set(re.findall(r"[\w./-]+\.(?:mm|cpp|h)\b", executable.group(1)))


def source_path(label):
    """Repository-relative path of a same-repository Bazel source label."""
    if label.startswith("//"):
        package, _, target = label[2:].partition(":")
        return f"{package}/{target}" if package else target
    return label


def rule_attributes(path):
    """The attrs dictionary of the single rule() declaration in a .bzl file."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    (rule_call,) = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "rule"
    ]
    attrs = next(keyword.value for keyword in rule_call.keywords if keyword.arg == "attrs")
    return {key.value: value for key, value in zip(attrs.keys, attrs.values, strict=True)}


def attribute_default(attribute):
    return ast.literal_eval(next(keyword.value for keyword in attribute.keywords if keyword.arg == "default"))


def exports_fields(path):
    """File lists and visibility of every exports_files call in a BUILD file."""
    entries = []
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "exports_files":
            files = [literal for literal in string_literals(node.args[0])] if node.args else []
            visibility = next((keyword.value for keyword in node.keywords if keyword.arg == "visibility"), None)
            entries.append((files, visibility))
    return entries


class BazelAppkitGraphTest(unittest.TestCase):
    def test_objc_and_cpp_libraries_own_the_complete_cmake_object_membership(self):
        units, entry = cmake_appkit_units()
        appkit = fields_for("app/macos-appkit", "appkit_objc_library", "appkit")
        appkit_cpp = fields_for("app/macos-appkit", "cc_library", "appkit_cpp")
        main = fields_for("app/macos-appkit", "appkit_objc_library", "main")
        objc = {name for name in units if name.endswith(".mm")}
        cpp = {name for name in units if name.endswith(".cpp")}
        headers = {name for name in units if name.endswith(".h")}
        # Every CMake unit lands in its real owner; the bundle entry stays out
        # of the product library, exactly like the separate CMake executable.
        self.assertEqual(set(string_literals(appkit.get("srcs"))), objc)
        self.assertEqual(set(string_literals(appkit_cpp.get("srcs"))), cpp)
        self.assertEqual(set(string_literals(main.get("srcs"))), {entry})
        self.assertNotIn(entry, objc)
        # CMake registers headers only loosely; the two libraries must cover
        # every CMake-listed header, and the plain C++ units must declare the
        # adapter headers they actually include even where CMake lists none.
        declared_headers = set(string_literals(appkit.get("hdrs"))) | set(string_literals(appkit_cpp.get("hdrs")))
        self.assertLessEqual(headers, declared_headers)
        for header in ("MediaPlayerAdapter.h", "PlaybackSeekTarget.h"):
            self.assertIn(header, string_literals(appkit_cpp.get("hdrs")))

    def test_appkit_links_the_cmake_closure_and_frameworks(self):
        appkit = fields_for("app/macos-appkit", "appkit_objc_library", "appkit")
        deps = set(string_literals(appkit["deps"]))
        for label in (
            ":appkit_cpp",
            "//app/desktop:desktop_launch",
            "//app/i18n:completion",
            "//app/i18n:i18n",
            "//app/i18n:ordering",
            "//app/runtime:runtime",
            "//app/uimodel:uimodel",
            "//lib/audio:audio",
        ):
            self.assertIn(label, deps)
        # The CMake AppKit product links no allocator replacement.
        for dep in deps:
            self.assertNotIn("mimalloc", dep)
        # Each framework stays a `-framework NAME` pair; the system frameworks
        # carry no link-order contract, so assert the pairing and the required
        # consumer set rather than one incidental argv order.
        linkopts = string_literals(appkit["linkopts"])
        self.assertEqual(len(linkopts) % 2, 0, linkopts)
        self.assertEqual(set(linkopts[0::2]), {"-framework"})
        cmake = (APPKIT_PACKAGE / "CMakeLists.txt").read_text(encoding="utf-8")
        links = re.search(r"target_link_libraries\(ao_appkit\s+PUBLIC([^)]*)\)", cmake, re.DOTALL).group(1)
        frameworks = re.findall(r'"-framework ([\w]+)"', links)
        self.assertTrue(frameworks, "CMake must declare the AppKit system frameworks")
        self.assertCountEqual(linkopts[1::2], frameworks)

    def test_bundle_binary_links_main_and_appkit_without_extra_sources(self):
        main = fields_for("app/macos-appkit", "appkit_objc_library", "main")
        self.assertEqual(set(string_literals(main["deps"])), {":appkit"})
        binary = fields_for("app/macos-appkit", "cc_binary", "aobus-appkit-bin")
        self.assertEqual(string_literals(binary.get("srcs")), [])
        self.assertIs(ast.literal_eval(binary["linkstatic"]), True)
        self.assertEqual(set(string_literals(binary["deps"])), {":appkit", ":main"})

    def test_appkit_smoke_reuses_cmake_scenario_units_and_owns_its_own_binary(self):
        cmake_mm = {name for name in cmake_appkit_smoke_units() if name.endswith(".mm")}
        smoke = fields_for("app/macos-appkit", "appkit_objc_library", "appkit_smoke")
        smoke_srcs = string_literals(smoke["srcs"])
        self.assertEqual(
            {source_path(label).removeprefix("test/") for label in smoke_srcs if label.endswith(".mm")},
            cmake_mm,
        )
        self.assertIn("//test:macos-appkit/AppKitSmokeMain.mm", smoke_srcs)
        self.assertNotIn("//test:media/wav/TestWav.cpp", smoke_srcs)
        appkit = fields_for("app/macos-appkit", "appkit_objc_library", "appkit")
        self.assertNotIn("//test:macos-appkit/AppKitSmokeMain.mm", string_literals(appkit["srcs"]))
        deps = set(string_literals(smoke["deps"]))
        for label in (
            ":appkit",
            "//test:appkit_smoke_headers",
            "//test:coreaudio_test_support",
            "//test:media_test_support",
        ):
            self.assertIn(label, deps)
        self.assertIs(ast.literal_eval(smoke["testonly"]), True)
        self.assertEqual(ast.literal_eval(smoke["target_compatible_with"]), ["@platforms//os:macos"])

        binary = fields_for("app/macos-appkit", "cc_binary", "ao_appkit_smoke_bin")
        self.assertEqual(string_literals(binary.get("srcs")), [])
        self.assertEqual(set(string_literals(binary["deps"])), {":appkit", ":appkit_smoke"})
        self.assertIs(ast.literal_eval(binary["linkstatic"]), True)
        self.assertIs(ast.literal_eval(binary["testonly"]), True)
        self.assertEqual(ast.literal_eval(binary["target_compatible_with"]), ["@platforms//os:macos"])

        bundle = fields_for("app/macos-appkit", "aobus_appkit_bundle", "ao_appkit_smoke")
        shipping_bundle = fields_for("app/macos-appkit", "aobus_appkit_bundle", "aobus-appkit")
        self.assertEqual(ast.literal_eval(bundle["exe"]), ":ao_appkit_smoke_bin")
        self.assertEqual(ast.literal_eval(bundle["tree_name"]), "ao_appkit_smoke.app")
        self.assertEqual(ast.literal_eval(bundle["executable_name"]), "ao_appkit_smoke")
        self.assertEqual(ast.literal_eval(bundle["bundle_identifier"]), "org.aobus.desktop.smoke")
        self.assertEqual(ast.literal_eval(bundle["display_name"]), "Aobus Native Tests")
        for shared in ("plist", "icon", "license", "inventory", "macos_toolchain"):
            self.assertEqual(ast.literal_eval(bundle[shared]), ast.literal_eval(shipping_bundle[shared]))
        self.assertIs(ast.literal_eval(bundle["testonly"]), True)

    def test_bundle_rule_defaults_to_the_shipping_tree_and_keeps_optional_identity_overrides(self):
        attrs = rule_attributes(APPKIT_PACKAGE / "bundle.bzl")
        self.assertEqual(attribute_default(attrs["tree_name"]), "Aobus.app")
        self.assertEqual(attribute_default(attrs["executable_name"]), "aobus-appkit")
        self.assertEqual(attribute_default(attrs["bundle_identifier"]), "")
        self.assertEqual(attribute_default(attrs["display_name"]), "")
        shipping = fields_for("app/macos-appkit", "aobus_appkit_bundle", "aobus-appkit")
        for optional in ("tree_name", "bundle_identifier", "display_name"):
            self.assertNotIn(optional, shipping)

    def test_product_targets_stay_macos_only(self):
        for kind, name in (
            ("appkit_objc_library", "appkit"),
            ("appkit_objc_library", "main"),
            ("appkit_objc_library", "appkit_smoke"),
            ("cc_library", "appkit_cpp"),
            ("cc_binary", "aobus-appkit-bin"),
            ("cc_binary", "ao_appkit_smoke_bin"),
            ("aobus_appkit_bundle", "aobus-appkit"),
            ("aobus_appkit_bundle", "ao_appkit_smoke"),
        ):
            with self.subTest(target=name):
                fields = fields_for("app/macos-appkit", kind, name)
                self.assertEqual(ast.literal_eval(fields["target_compatible_with"]), ["@platforms//os:macos"])

    def test_appkit_cpp_keeps_whole_cmake_object_membership_alive(self):
        fields = fields_for("app/macos-appkit", "cc_library", "appkit_cpp")
        # CMake owns these units through an OBJECT library; an ordinary archive
        # needs alwayslink so static-archive dead stripping cannot drop them.
        self.assertIs(ast.literal_eval(fields["alwayslink"]), True)
        self.assertIs(ast.literal_eval(fields["linkstatic"]), True)
        self.assertEqual(ast.literal_eval(fields["includes"]), ["."])

    def test_bundle_rule_declares_every_authoritative_input(self):
        bundle = fields_for("app/macos-appkit", "aobus_appkit_bundle", "aobus-appkit")
        self.assertEqual(ast.literal_eval(bundle["exe"]), ":aobus-appkit-bin")
        self.assertEqual(ast.literal_eval(bundle["plist"]), "DesktopInfo.plist.in")
        self.assertEqual(ast.literal_eval(bundle["icon"]), "//asset:brand/Aobus.icns")
        self.assertEqual(ast.literal_eval(bundle["license"]), "//asset:brand/LICENSE.txt")
        self.assertEqual(ast.literal_eval(bundle["inventory"]), "//app/i18n:catalog/package.lst")
        self.assertEqual(ast.literal_eval(bundle["macos_toolchain"]), "//:script/ao/macos-toolchain.json")
        self.assertIn("//visibility:public", ast.literal_eval(bundle["visibility"]))

    def test_bundle_rule_uses_the_declared_python_sdk_and_version_authority(self):
        tree = ast.parse((APPKIT_PACKAGE / "bundle.bzl").read_text(encoding="utf-8"))
        loaded = [
            literal
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "load"
            for literal in string_literals(node)
        ]
        self.assertIn("@aobus_native_tools//:catalog_tools.bzl", loaded)
        self.assertIn("@aobus_native_tools//:app_version.bzl", loaded)
        labels = [
            literal
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "Label"
            for literal in string_literals(node)
        ]
        # The helper stays package-local; the interpreter runtime comes from
        # the already declared catalog tools.
        self.assertIn("@aobus_native_tools//:catalog_tools", labels)
        declared_trees = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "declare_directory"
        ]
        self.assertEqual(len(declared_trees), 1, "expected one declared bundle tree output")
        # The tree name follows the rule attribute default, not a literal in
        # the action implementation.
        self.assertEqual(ast.unparse(declared_trees[0].args[0]), "ctx.attr.tree_name")
        executable_defaults = [
            keyword.value
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "DefaultInfo"
            for keyword in node.keywords
            if keyword.arg == "executable"
        ]
        self.assertTrue(executable_defaults, "the bundle rule must return a runnable launcher executable")

    def test_bundle_rule_declares_runnable_output_roles_and_a_tree_path_argument(self):
        tree = ast.parse((APPKIT_PACKAGE / "bundle.bzl").read_text(encoding="utf-8"))
        rule_calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "rule"
        ]
        self.assertEqual(len(rule_calls), 1)
        rule_keywords = {keyword.arg: keyword.value for keyword in rule_calls[0].keywords}
        self.assertIs(ast.literal_eval(rule_keywords["executable"]), True)
        default_info = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "DefaultInfo"
        ]
        self.assertEqual(len(default_info), 1)
        keywords = {keyword.arg: keyword.value for keyword in default_info[0].keywords}
        # Keep the required launcher/output/runfiles roles without forbidding
        # other legal DefaultInfo keywords. Native gates validate consumption.
        self.assertLessEqual({"executable", "files", "runfiles"}, set(keywords))
        declared_trees = [
            target.id
            for node in ast.walk(tree)
            if isinstance(node, ast.Assign)
            and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Attribute)
            and node.value.func.attr == "declare_directory"
            for target in node.targets
            if isinstance(target, ast.Name)
        ]
        self.assertEqual(len(declared_trees), 1, "expected one declared bundle tree output")
        bundle_tree = declared_trees[0]
        # A tree artifact is one output-directory argument, not Args directory
        # expansion into its eventual members.
        app_arguments = [
            node.args[1]
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "add"
            and len(node.args) == 2
            and ast.literal_eval(node.args[0]) == "--app"
        ]
        self.assertEqual(len(app_arguments), 1, "expected one --app argument")
        (argument,) = app_arguments
        self.assertIsInstance(argument, ast.Attribute)
        self.assertEqual(argument.attr, "path")
        self.assertIsInstance(argument.value, ast.Name)
        self.assertEqual(argument.value.id, bundle_tree)

    def test_asset_exports_only_the_two_declared_brand_files_to_appkit(self):
        ((files, visibility),) = [
            entry for entry in exports_fields(PROJECT_ROOT / "asset/BUILD.bazel") if "brand/Aobus.icns" in entry[0]
        ]
        self.assertCountEqual(files, ["brand/Aobus.icns", "brand/LICENSE.txt"])
        self.assertEqual(ast.literal_eval(visibility), ["//app/macos-appkit:__pkg__"])

    def test_i18n_exports_the_inventory_only_to_the_bundle_owner(self):
        ((files, visibility),) = [
            entry
            for entry in exports_fields(PROJECT_ROOT / "app/i18n/BUILD.bazel")
            if "catalog/package.lst" in entry[0]
        ]
        self.assertEqual(files, ["catalog/package.lst"])
        self.assertEqual(ast.literal_eval(visibility), ["//app/macos-appkit:__pkg__"])

    def test_desktop_launch_builds_on_every_shipping_desktop_host(self):
        fields = fields_for("app/desktop", "cc_library", "desktop_launch")
        compatibility = ast.literal_eval(fields["target_compatible_with"].args[0])
        for host in ("@platforms//os:linux", "@platforms//os:macos", "@platforms//os:windows"):
            with self.subTest(host=host):
                self.assertEqual(compatibility[host], [])
        self.assertIn("//app/macos-appkit:__pkg__", ast.literal_eval(fields["visibility"]))
        # Boost::process stays the private mechanism dependency like in CMake.
        self.assertEqual(ast.literal_eval(fields["implementation_deps"]), ["@aobus_native_deps//:boost_process"])


class BazelAppkitBundleHelperTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus appkit bundle-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.icon = self.root / "Aobus.icns"
        self.icon.write_bytes(b"fake icns payload")
        self.license = self.root / "LICENSE.txt"
        self.license.write_text("brand license", encoding="utf-8")
        self.template = self.root / "DesktopInfo.plist.in"
        self.template.write_text(
            (APPKIT_PACKAGE / "DesktopInfo.plist.in").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        self.inventory = self.root / "package.lst"
        self.set_inventory(["root", "de", "zh_Hans", "fr_CA", "qps_Ploc"])
        self.macos_toolchain = self.root / "macos-toolchain.json"
        self.macos_toolchain.write_text(
            json.dumps({"schemaVersion": 1, "deploymentTarget": "15.0"}),
            encoding="utf-8",
        )
        self.exe = self.root / "aobus-appkit-bin"
        self.exe.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\"\n", encoding="utf-8", newline="\n")
        self.exe.chmod(0o755)

    def set_inventory(self, locales):
        self.inventory.write_text("".join(locale + ".res\n" for locale in locales), encoding="utf-8")

    def options(
        self,
        *,
        executable_name="aobus-appkit",
        app=None,
        launcher=None,
        bundle_identifier=None,
        display_name=None,
    ):
        arguments = [
            "--exe",
            str(self.exe),
            "--plist",
            str(self.template),
            "--icon",
            str(self.icon),
            "--license",
            str(self.license),
            "--inventory",
            str(self.inventory),
            "--macos-toolchain",
            str(self.macos_toolchain),
            "--app",
            str(app if app is not None else self.root / "Aobus.app"),
            "--launcher",
            str(launcher if launcher is not None else self.root / "aobus-appkit"),
            "--version",
            "1.2.3",
            "--executable-name",
            executable_name,
        ]
        if bundle_identifier is not None:
            arguments.extend(["--bundle-identifier", bundle_identifier])
        if display_name is not None:
            arguments.extend(["--display-name", display_name])
        return build_bundle.parse_args(arguments)

    def read_plist(self, app):
        with (app / "Contents/Info.plist").open("rb") as source:
            return plistlib.load(source)

    def test_bundle_tree_carries_the_declared_inputs_and_plist_fields(self):
        build_bundle.run(self.options())
        app = self.root / "Aobus.app"
        executable = app / "Contents/MacOS/aobus-appkit"
        self.assertEqual(executable.read_bytes(), self.exe.read_bytes())
        self.assertTrue(os.access(executable, os.X_OK))
        plist = self.read_plist(app)
        self.assertEqual(plist["CFBundleExecutable"], "aobus-appkit")
        self.assertEqual(plist["CFBundleShortVersionString"], "1.2.3")
        self.assertEqual(plist["LSMinimumSystemVersion"], "15.0")
        self.assertEqual(plist["CFBundleLocalizations"], ["en", "de", "fr-CA", "zh-Hans"])
        self.assertEqual((app / "Contents/Resources/Aobus.icns").read_bytes(), self.icon.read_bytes())
        self.assertEqual((app / "Contents/Resources/Brand/LICENSE.txt").read_text(encoding="utf-8"), "brand license")

    def test_second_consumer_identity_changes_only_plist_roles_and_launcher_target(self):
        build_bundle.run(self.options())
        shipping = self.read_plist(self.root / "Aobus.app")
        self.assertEqual(shipping["CFBundleIdentifier"], "org.aobus.desktop")
        self.assertEqual(shipping["CFBundleName"], "Aobus")

        app = self.root / "ao_appkit_smoke.app"
        launcher = self.root / "ao_appkit_smoke"
        build_bundle.run(
            self.options(
                executable_name="ao_appkit_smoke",
                app=app,
                launcher=launcher,
                bundle_identifier="org.aobus.desktop.smoke",
                display_name="Aobus Native Tests",
            )
        )
        plist = self.read_plist(app)
        self.assertEqual(plist["CFBundleExecutable"], "ao_appkit_smoke")
        self.assertEqual(plist["CFBundleIdentifier"], "org.aobus.desktop.smoke")
        self.assertEqual(plist["CFBundleName"], "Aobus Native Tests")
        self.assertEqual(plist["CFBundlePackageType"], shipping["CFBundlePackageType"])
        self.assertEqual(plist["LSMinimumSystemVersion"], shipping["LSMinimumSystemVersion"])
        self.assertEqual(plist["CFBundleLocalizations"], shipping["CFBundleLocalizations"])
        self.assertEqual((app / "Contents/MacOS/ao_appkit_smoke").read_bytes(), self.exe.read_bytes())
        self.assertIn("ao_appkit_smoke.app/Contents/MacOS/ao_appkit_smoke", launcher.read_text(encoding="utf-8"))
        if os.name == "posix":
            result = subprocess.run([str(launcher), "--probe"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "--probe")

    def test_empty_identity_overrides_are_rejected_before_writing(self):
        for field in ("bundle_identifier", "display_name"):
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, "must not be empty"):
                    build_bundle.run(self.options(**{field: ""}))
                self.assertFalse((self.root / "Aobus.app").exists())

    @unittest.skipUnless(os.name == "posix", "the generated bundle launcher is a POSIX shell script")
    def test_launcher_execs_the_bundle_binary_without_added_arguments(self):
        build_bundle.run(self.options())
        launcher = self.root / "aobus-appkit"
        self.assertTrue(os.access(launcher, os.X_OK))
        result = subprocess.run(
            [str(launcher), "--state-root", "/tmp/state", "--library", "/tmp/library"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "--state-root /tmp/state --library /tmp/library")

    def test_localizations_follow_the_cmake_tag_rule(self):
        for locales, expected in (
            (["root", "qps_Ploc"], ["en"]),
            (["root", "de", "zh_Hans", "fr_CA", "qps_Ploc"], ["en", "de", "fr-CA", "zh-Hans"]),
        ):
            with self.subTest(locales=locales):
                self.set_inventory(locales)
                self.assertEqual(build_bundle.bundle_localizations(self.inventory), expected)

    def test_localizations_reject_unsafe_duplicate_and_rootless_inventories(self):
        for locales in (
            [],
            ["de"],
            ["root", "en"],
            ["root", "de", "de"],
            ["root", "../evil"],
            ["root", "x"],
            ["root", "de.res"],
        ):
            with self.subTest(locales=locales):
                self.set_inventory(locales)
                with self.assertRaises(ValueError):
                    build_bundle.bundle_localizations(self.inventory)

    def test_rejects_unsafe_bundle_names_before_writing(self):
        for name in ("../evil", "sub/dir", "a b", "a;touch", "$(x)", "-", ""):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "Unsafe bundle name"):
                    build_bundle.run(self.options(executable_name=name))
                self.assertFalse((self.root / "Aobus.app").exists())
                self.assertFalse((self.root / "aobus-appkit").exists())

    def test_missing_payload_inputs_fail_without_writing_outputs(self):
        for attribute in ("icon", "license", "exe"):
            with self.subTest(missing=attribute):
                path = getattr(self, attribute)
                payload = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ValueError, "Missing declared bundle input"):
                    build_bundle.run(self.options())
                path.write_bytes(payload)
                if attribute == "exe":
                    path.chmod(0o755)
                self.assertFalse((self.root / "Aobus.app").exists())

    def test_overlapping_outputs_and_inputs_fail_without_writing_outputs(self):
        # The launcher cannot live inside the bundle tree it launches.
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            build_bundle.run(self.options(launcher=self.root / "Aobus.app" / "inside"))
        # The tree cannot cover a declared input file.
        nested = self.root / "nested"
        nested.mkdir()
        (nested / "LICENSE.txt").write_text("brand license", encoding="utf-8")
        original_license = self.license
        self.license = nested / "LICENSE.txt"
        try:
            with self.assertRaisesRegex(ValueError, "Overlapping"):
                build_bundle.run(self.options(app=nested))
        finally:
            self.license = original_license
        self.assertFalse((self.root / "Aobus.app").exists())

    def test_unresolved_plist_placeholder_fails_without_writing_outputs(self):
        text = self.template.read_text(encoding="utf-8").replace("${PROJECT_VERSION}", "${UNRESOLVED_VERSION}")
        self.template.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Unresolved"):
            build_bundle.run(self.options())
        self.assertFalse((self.root / "Aobus.app").exists())
        self.assertFalse((self.root / "aobus-appkit").exists())

    def test_plist_requires_the_declared_bundle_fields(self):
        text = self.template.read_text(encoding="utf-8")
        for key in ("CFBundleExecutable", "CFBundleLocalizations", "LSMinimumSystemVersion"):
            with self.subTest(key=key):
                self.template.write_text(
                    "\n".join(line for line in text.splitlines() if f"<key>{key}</key>" not in line) + "\n",
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(ValueError, "misses required keys"):
                    build_bundle.run(self.options())
                self.assertFalse((self.root / "Aobus.app").exists())

    def test_deployment_target_comes_from_the_single_macos_authority(self):
        for document, pattern in (
            ({"schemaVersion": 2, "deploymentTarget": "15.0"}, "schema"),
            ({"schemaVersion": 1}, "no deployment"),
            ({"schemaVersion": 1, "deploymentTarget": ""}, "no deployment"),
            ({"schemaVersion": 1, "deploymentTarget": 15}, "no deployment"),
        ):
            with self.subTest(document=document):
                self.macos_toolchain.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, pattern):
                    build_bundle.deployment_target(self.macos_toolchain)

    def test_icon_must_match_the_plist_icon_reference(self):
        self.icon = self.root / "Other.icns"
        self.icon.write_bytes(b"other icon")
        with self.assertRaisesRegex(ValueError, "icon"):
            build_bundle.run(self.options())
        self.assertFalse((self.root / "Aobus.app").exists())

    def test_bundle_from_the_real_repository_inputs(self):
        deployment = json.loads((PROJECT_ROOT / "script/ao/macos-toolchain.json").read_text(encoding="utf-8"))
        options = build_bundle.parse_args(
            [
                "--exe",
                str(self.exe),
                "--plist",
                str(APPKIT_PACKAGE / "DesktopInfo.plist.in"),
                "--icon",
                str(self.icon),
                "--license",
                str(self.license),
                "--inventory",
                str(PROJECT_ROOT / "app/i18n/catalog/package.lst"),
                "--macos-toolchain",
                str(PROJECT_ROOT / "script/ao/macos-toolchain.json"),
                "--app",
                str(self.root / "Aobus.app"),
                "--launcher",
                str(self.root / "aobus-appkit"),
                "--version",
                "0.0.0-test",
                "--executable-name",
                "aobus-appkit",
            ]
        )
        build_bundle.run(options)
        plist = self.read_plist(self.root / "Aobus.app")
        self.assertEqual(plist["CFBundleExecutable"], "aobus-appkit")
        self.assertEqual(plist["CFBundleShortVersionString"], "0.0.0-test")
        self.assertEqual(plist["LSMinimumSystemVersion"], deployment["deploymentTarget"])
        tags = plist["CFBundleLocalizations"]
        entries = set((PROJECT_ROOT / "app/i18n/catalog/package.lst").read_text(encoding="utf-8").splitlines())
        # Membership is derived from the inventory: English first, pseudo
        # localization absent, every other tag backed by an authored resource.
        self.assertEqual(tags[0], "en")
        self.assertNotIn("qps-ploc", tags)
        self.assertEqual(tags[1:], sorted(tags[1:]))
        self.assertTrue({tag.replace("-", "_") + ".res" for tag in tags[1:]} <= entries)


if __name__ == "__main__":
    unittest.main()
