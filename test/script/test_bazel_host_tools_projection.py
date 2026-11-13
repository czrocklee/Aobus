"""Consumer contracts for native host-tool facts and their fixed Starlark projection."""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_host_tools as tools
from ao.core import bazel_native as native
from ao.core import dependency_policy


class HostToolsFactsTest(unittest.TestCase):
    """Exercise real native admission; fake only tool execution and the selected Python SDK."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-host-tools-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "installed/arm64-aobus-osx"
        self.gperf = self.file("installed/arm64-aobus-osx/tools/gperf/gperf")
        self.genrb = self.file("installed/arm64-aobus-osx/tools/icu/bin/genrb")
        self.file("installed/arm64-aobus-osx/tools/icu/bin/pkgdata")
        self.file("vcpkg.json", '{"dependencies": [{"name": "gperf", "host": true}]}')
        self.file("vcpkg-configuration.json", "{}")
        self.file(
            "installed/vcpkg/status",
            "Package: gperf\nVersion: 3.3\nArchitecture: arm64-aobus-osx\nStatus: install ok installed\n\n"
            "Package: icu\nVersion: 78.3\nArchitecture: arm64-aobus-osx\nStatus: install ok installed\n\n"
            "Package: icu\nFeature: tools\nArchitecture: arm64-aobus-osx\nStatus: install ok installed\n",
        )
        self.file(
            "installed/arm64-aobus-osx/include/unicode/uvernum.h",
            "#define U_ICU_VERSION_MAJOR_NUM 78\n#define U_ICU_VERSION_MINOR_NUM 3\n",
        )
        self.file("VERSION.txt", "1.2.3\n")
        # A legacy bare VERSION and a stale CMake declaration must not be
        # admitted as the version.
        self.file("VERSION", "9.9.9\n")
        self.file("CMakeLists.txt", "project(aobus VERSION 9.9.9)\n")
        contract = copy.deepcopy(dependency_policy.load_contract())
        contract["dependencies"]["icu"]["policy"] = {"kind": "exact", "version": "78.3"}
        contract["exceptions"] = [entry for entry in contract["exceptions"] if entry["dependency"] != "icu"]
        self.file("dependency-contract.json", json.dumps(contract))
        self.python = self.file("python/bin/selected-python")
        self.stdlib = self.file("python/Lib/json/__init__.py").parents[1]
        self.file("python/Lib/site-packages/ambient.py")
        self.extension = self.file("python/lib-dynload/_struct.so")
        self.framework = self.file("python/Python")
        self.version_report = "3.3"
        for patcher in (
            mock.patch.object(native, "PROJECT_ROOT", self.root),
            mock.patch.object(dependency_policy, "load_contract", return_value=contract),
            mock.patch.object(sys, "executable", str(self.python)),
            mock.patch.object(sys, "base_prefix", str(self.root / "python")),
            mock.patch.object(tools.sysconfig, "get_path", return_value=str(self.stdlib)),
            mock.patch.object(tools.sysconfig, "get_config_var", side_effect=self.python_config),
            mock.patch.object(subprocess, "run", side_effect=self.execute),
            mock.patch.dict(os.environ, {"NIX_CFLAGS_SET": "must not leak", "CMAKE_PREFIX_PATH": "must not leak"}),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def file(self, name, text="fixture"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def python_config(self, name):
        return {"DESTSHARED": str(self.extension.parent), "PYTHONFRAMEWORK": "Python"}.get(name)

    def execute(self, argv, **kwargs):
        self.assertNotIn("CMAKE_PREFIX_PATH", kwargs["env"])
        self.assertNotIn("NIX_CFLAGS_SET", kwargs["env"])
        if argv == [str(self.gperf), "--version"]:
            self.assertIs(kwargs["check"], True)
            return subprocess.CompletedProcess(argv, 0, f"GNU gperf {self.version_report}\n", "")
        if argv == [str(self.genrb), "-V"]:
            self.assertIs(kwargs["check"], False)
            return subprocess.CompletedProcess(argv, 1, "", "genrb (ICU version 78.3)")
        raise AssertionError("unexpected native query: " + repr(argv))

    def resolve(self, **kwargs):
        return tools.resolve_host_tools(self.root / "resolution", self.prefix, "macos", **kwargs)

    def test_native_sdk_returns_data_and_selected_runtime_inventory_without_program_generation(self):
        result = self.resolve(source=str(self.root))
        self.assertEqual(result["app_version"], "1.2.3")
        self.assertIn(str(self.root / "VERSION.txt"), result["watch_files"])
        self.assertNotIn(str(self.root / "VERSION"), result["watch_files"])
        self.assertNotIn(str(self.root / "CMakeLists.txt"), result["watch_files"])
        self.assertEqual(result["test_tools"]["source_root"], str(self.root))
        self.assertEqual(result["catalog"]["tools"]["python"], str(self.python))
        self.assertIn(str(self.extension), result["catalog"]["artifacts"])
        self.assertIn(str(self.framework), result["catalog"]["artifacts"])
        self.assertIn(str(self.stdlib / "json/__init__.py"), result["watch_files"])
        self.assertIn(str(self.stdlib / "json"), result["watch_directories"])
        self.assertIn(str(self.gperf.parent), result["watch_directories"])
        self.assertNotIn(str(self.stdlib / "site-packages/ambient.py"), result["watch_files"])
        self.assertNotIn("resource", result)
        self.assertNotIn("dbus", result)
        self.assertNotIn("source_root", self.resolve()["test_tools"])
        self.assertEqual([path.name for path in (self.root / "resolution").iterdir()], ["catalog-sdk.json"])
        diagnostic = json.loads((self.root / "resolution/catalog-sdk.json").read_text(encoding="utf-8"))
        self.assertEqual(diagnostic["tools"], result["catalog"]["tools"])
        self.assertIn(str(self.root / "resolution/catalog-sdk.json"), result["catalog"]["artifacts"])
        json.dumps(result)  # The repository subprocess publishes facts without Path objects.

    def test_gperf_version_query_rejects_a_different_provider_version(self):
        self.version_report = "3.4"
        with self.assertRaisesRegex(ValueError, "gperf executable version differs"):
            self.resolve()

    def test_managed_source_requires_an_existing_absolute_string_or_none(self):
        for source in ({"source_root": str(self.root)}, "", "relative", str(self.root / "missing")):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, "prepared request source"):
                self.resolve(source=source)


if __name__ == "__main__":
    unittest.main()
