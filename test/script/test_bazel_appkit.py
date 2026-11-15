"""AppKit product graph and bundle-assembly contracts; native gates stay separate."""

import importlib.util
import json
import os
import plistlib
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from ao.core import bazel_lint
from ao.core.paths import PROJECT_ROOT

APPKIT_PACKAGE = PROJECT_ROOT / "app/macos-appkit"

_SPEC = importlib.util.spec_from_file_location("build_bundle", APPKIT_PACKAGE / "build_bundle.py")
assert _SPEC is not None and _SPEC.loader is not None
build_bundle = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(build_bundle)


class BazelAppkitGraphTest(unittest.TestCase):
    def test_standard_objc_library_owners_remain_lint_compile_owners(self):
        # The standard rule replaced the package-local consumer; the lint
        # owner filter must admit the actual BUILD kinds so no AppKit
        # translation unit loses its owner, and the removed local kind
        # must not linger as an admitted owner.
        self.assertIn("objc_library", bazel_lint._COMPILE_OWNER_KINDS)
        self.assertNotIn("appkit_objc_library", bazel_lint._COMPILE_OWNER_KINDS)

    def test_only_the_smoke_bundle_declares_the_supervisor_bridge(self):
        # The smoke bundle bridges through the standalone process supervisor;
        # the shipping bundle must stay a plain native exec without one.
        build = (APPKIT_PACKAGE / "BUILD.bazel").read_text(encoding="utf-8")
        smoke = re.search(r'aobus_appkit_bundle\(\s*name = "ao_appkit_smoke".*?\n\)', build, re.DOTALL)
        self.assertIsNotNone(smoke)
        self.assertIn('supervisor = "//:script/ao/core/appkitprocess.py"', smoke.group(0))
        shipping = re.search(r'aobus_appkit_bundle\(\s*name = "aobus-appkit".*?\n\)', build, re.DOTALL)
        self.assertIsNotNone(shipping)
        self.assertNotIn("supervisor", shipping.group(0))


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
        supervisor=None,
        python=None,
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
        if supervisor is not None:
            arguments.extend(["--supervisor", str(supervisor)])
        if python is not None:
            arguments.extend(["--python", str(python)])
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

    def stub_python(self, name="python-stub"):
        """Record the admitted interpreter's own arguments, one per line."""
        python = self.root / name
        python.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8", newline="\n")
        python.chmod(0o755)
        return python

    def test_supervisor_bundle_copies_the_real_supervisor_and_bridges_the_launcher(self):
        supervisor = PROJECT_ROOT / "script/ao/core/appkitprocess.py"
        python = self.stub_python("python's stub")
        app = self.root / "ao_appkit_smoke.app"
        launcher = self.root / "ao_appkit_smoke"
        forwarded = ["--scenario", "desktop", "--library", "/tmp/library", "--state-root", "/tmp/state"]
        build_bundle.run(
            self.options(
                executable_name="ao_appkit_smoke",
                app=app,
                launcher=launcher,
                supervisor=supervisor,
                python=python,
            )
        )
        copied = app / "Contents/Resources/appkitprocess.py"
        self.assertEqual(copied.read_bytes(), supervisor.read_bytes())
        text = launcher.read_text(encoding="utf-8")
        self.assertIn(shlex.quote(str(python)), text)
        self.assertIn("-I -S -B -X utf8", text)
        self.assertIn("ao_appkit_smoke.app/Contents/Resources/appkitprocess.py", text)
        self.assertIn("ao_appkit_smoke.app/Contents/MacOS/ao_appkit_smoke", text)
        if os.name == "posix":
            result = subprocess.run([str(launcher), *forwarded], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            # The bridge admits the interpreter with startup isolation, then
            # passes the copied supervisor, the bundled binary and the caller's
            # forwarded arguments; no execroot lookup survives in the tree.
            self.assertEqual(
                result.stdout.splitlines(),
                [
                    "-I",
                    "-S",
                    "-B",
                    "-X",
                    "utf8",
                    str(copied),
                    str(app / "Contents/MacOS/ao_appkit_smoke"),
                    *forwarded,
                ],
            )

    def test_bundle_without_supervisor_keeps_the_plain_native_launcher(self):
        build_bundle.run(self.options())
        app = self.root / "Aobus.app"
        self.assertEqual([], list((app / "Contents/Resources").glob("*.py")))
        text = (self.root / "aobus-appkit").read_text(encoding="utf-8")
        self.assertNotIn("python", text)
        self.assertNotIn("-I", text)

    def test_supervisor_without_the_admitted_interpreter_fails_without_writing_outputs(self):
        supervisor = self.root / "appkitprocess.py"
        supervisor.write_text("standalone supervisor fixture\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "supervisor requires the admitted python"):
            build_bundle.run(self.options(supervisor=supervisor))
        self.assertFalse((self.root / "Aobus.app").exists())
        self.assertFalse((self.root / "aobus-appkit").exists())

    def test_localizations_follow_the_tag_rule(self):
        for locales, expected in (
            (["root", "qps_Ploc"], ["en"]),
            (["root", "de", "zh_Hans", "fr_CA", "qps_Ploc"], ["en", "de", "fr-CA", "zh-Hans"]),
        ):
            with self.subTest(locales=locales):
                self.set_inventory(locales)
                self.assertEqual(build_bundle.bundle_localizations(self.inventory), expected)

    def test_unresolved_plist_placeholder_fails_without_writing_outputs(self):
        text = self.template.read_text(encoding="utf-8").replace("${PROJECT_VERSION}", "${UNRESOLVED_VERSION}")
        self.template.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Unresolved"):
            build_bundle.run(self.options())
        self.assertFalse((self.root / "Aobus.app").exists())
        self.assertFalse((self.root / "aobus-appkit").exists())

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
