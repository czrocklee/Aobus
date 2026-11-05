"""Consumer contracts for native host-tool facts and their fixed Starlark projection."""

import ast
import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import bazel_host_tools as tools
from ao.core import bazel_native as native
from ao.core import dependency_policy

_BAZEL = Path(__file__).resolve().parents[2] / "bazel"


class _Select:
    def __init__(self, conditions, no_match_error=None):
        self.conditions = conditions
        self.no_match_error = no_match_error


class _Repo:
    """Only repository_ctx file/symlink I/O is faked; projections run unchanged."""

    def __init__(self):
        self.files = {}
        self.links = {}

    def path(self, source):
        return SimpleNamespace(realpath=source)

    def symlink(self, source, destination):
        if destination in self.links:
            raise AssertionError("duplicate projection: " + destination)
        self.links[destination] = str(source)

    def file(self, name, content, *, executable=False):
        if executable:
            raise AssertionError("host tools must not write executable programs")
        self.files[name] = content


class HostToolsProjectionTest(unittest.TestCase):
    def setUp(self):
        self.repo = _Repo()
        self.rules = {}
        self.exports = []
        self.settings = {}
        namespace = {"json": SimpleNamespace(encode=json.dumps)}
        exec(
            compile((_BAZEL / "host_tools_repository.bzl").read_text(encoding="utf-8"), "repository.bzl", "exec"),
            namespace,
        )
        self.materialize = namespace["materialize_host_tools"]

    def data(self):
        # A deliberately small Windows-like consumer SDK, not a dependency inventory.
        return {
            "gperf": {
                "identity": {"provider": "fixture", "version": "3.3"},
                "executable": "C:/SDK/tools/gperf.exe",
                "runtime": ["C:/SDK/tools/helper.dll"],
                "evidence": ["C:/SDK/status"],
            },
            "catalog": {
                "tools": {
                    "python": "C:/Pythön/python.exe",
                    "genrb": "C:/SDK/genrb.exe",
                    "pkgdata": "C:/SDK/pkgdata.exe",
                },
                "artifacts": [
                    "C:/Pythön/python.exe",
                    "C:/Pythön/Lib/json/__init__.py",
                    "C:/resolution/catalog-sdk.json",
                ],
                "evidence": ["C:/SDK/unicode/uvernum.h"],
            },
            "manifest_tools": {"mt": "C:/Windows SDK/mt.exe", "powershell": "C:/Windows/powershell.exe"},
            "test_tools": {"source_root": "C:/Original checkout"},
            "app_version": "1.2.3",
        }

    def value(self, name, symbol):
        source = self.repo.files[name]
        tree = ast.parse(source)
        self.assertEqual(len(tree.body), 1)
        assignment = tree.body[0]
        self.assertIsInstance(assignment, ast.Assign)
        self.assertEqual(assignment.targets[0].id, symbol)
        return ast.literal_eval(assignment.value)

    def declare(self):
        def rule(kind):
            def record(**kwargs):
                self.assertNotIn(kwargs["name"], self.rules)
                self.rules[kwargs["name"]] = (kind, kwargs)

            return record

        def setting(**kwargs):
            self.settings[kwargs["name"]] = kwargs["values"]

        namespace = {
            "native": SimpleNamespace(
                exports_files=self.exports.extend,
                config_setting=setting,
                alias=rule("alias"),
                filegroup=rule("filegroup"),
            ),
            "select": _Select,
            "package": lambda **kwargs: self.assertEqual(kwargs["default_visibility"], ["//visibility:public"]),
        }

        def load(label, *symbols):
            if label == "@aobus//bazel:host_tools.bzl":
                source = (_BAZEL / "host_tools.bzl").read_text(encoding="utf-8")
            elif label == ":host_tools_data.bzl":
                source = self.repo.files["host_tools_data.bzl"]
            else:
                raise AssertionError("unexpected BUILD dependency: " + label)
            exec(compile(source, label, "exec"), namespace)
            for symbol in symbols:
                self.assertIn(symbol, namespace)

        namespace["load"] = load
        exec(compile(self.repo.files["BUILD.bazel"], "BUILD.bazel", "exec"), namespace)

    def sources(self, target):
        kind, rule = self.rules[target]
        self.assertEqual(kind, "filegroup")
        return [self.repo.links.get(path, path) for path in rule["srcs"]]

    def test_selected_native_locations_and_sparse_bytes_reach_existing_consumers(self):
        data = self.data()
        self.materialize(self.repo, data)
        self.declare()
        self.assertEqual(self.value("catalog_tools.bzl", "CATALOG_TOOLS"), data["catalog"]["tools"])
        self.assertEqual(self.value("test_tools.bzl", "TEST_TOOLS"), data["test_tools"])
        self.assertEqual(self.value("windows_manifest_tools.bzl", "WINDOWS_MANIFEST_TOOLS"), data["manifest_tools"])
        self.assertEqual(self.value("app_version.bzl", "APP_VERSION"), "1.2.3")
        self.assertEqual(self.repo.links[self.rules["gperf"][1]["actual"]], data["gperf"]["executable"])
        self.assertEqual(self.sources("gperf_runtime"), data["gperf"]["runtime"])
        self.assertEqual(self.sources("catalog_tools"), data["catalog"]["artifacts"])
        self.assertEqual(set(self.sources("windows_manifest_tools")), set(data["manifest_tools"].values()))
        # Evidence is watched upstream; only explicitly admitted action inputs are staged.
        self.assertNotIn(data["catalog"]["evidence"][0], self.repo.links.values())
        self.assertEqual(self.sources("dbus_daemon"), [])
        self.assertNotIn("glib_compile_resources", self.rules)
        self.assertIn("catalog_tools.bzl", self.exports)
        manifest = json.loads(self.repo.files["manifest.json"])
        self.assertEqual(manifest["tool"], data["gperf"]["identity"])
        self.assertEqual(manifest["catalog_tools"], data["catalog"]["tools"])
        self.assertEqual({item["destination"]: item["source"] for item in manifest["artifacts"]}, self.repo.links)

    def test_linux_resource_and_bus_runtime_evidence_remain_action_inputs(self):
        data = self.data()
        data["manifest_tools"] = {}
        data["resource"] = {
            "identity": {"provider": "fixture", "version": "2.0"},
            "executable": "/sdk/bin/glib-compile-resources",
            "runtime": ["/sdk/lib/libgio.so"],
            "evidence": ["/sdk/report.json"],
        }
        data["dbus"] = {
            "identity": {"provider": "fixture", "version": "1.0"},
            "artifacts": ["/sdk/bin/dbus-daemon", "/sdk/lib/libdbus.so", "/sdk/report.json"],
        }
        self.materialize(self.repo, data)
        self.declare()
        actual = self.rules["glib_compile_resources"][1]["actual"]
        self.assertEqual(self.repo.links[actual], data["resource"]["executable"])
        self.assertIn(actual, self.exports)
        self.assertEqual(
            self.sources("glib_compile_resources_runtime"), data["resource"]["runtime"] + data["resource"]["evidence"]
        )
        self.assertEqual(self.sources("dbus_daemon"), data["dbus"]["artifacts"] + ["test-tools.json"])
        self.assertEqual(json.loads(self.repo.files["test-tools.json"]), data["dbus"]["identity"])
        self.assertEqual(self.sources("windows_manifest_tools"), [])

    def test_catalog_icu_selects_the_consumers_mode_and_fastbuild_has_no_default(self):
        self.materialize(self.repo, self.data())
        self.declare()
        actual = self.rules["catalog_icu"][1]["actual"]
        self.assertEqual(
            {self.settings[condition[1:]]["compilation_mode"]: label for condition, label in actual.conditions.items()},
            {"dbg": "//catalog-debug:icu", "opt": "//catalog-release:icu"},
        )
        self.assertNotIn("//conditions:default", actual.conditions)
        self.assertIn("dbg or opt", actual.no_match_error)

    def test_repository_entrypoint_is_independent_of_provider_paths_and_versions(self):
        first = self.data()
        self.materialize(self.repo, first)
        build = self.repo.files["BUILD.bazel"]
        second = self.data()
        second["gperf"]["identity"]["version"] = "future"
        second["gperf"]["executable"] = "/different/gperf"
        other = _Repo()
        self.materialize(other, second)
        self.assertEqual(other.files["BUILD.bazel"], build)
        # The BUILD consumes values and the checked-in declaration macro, not serialized rules.
        self.assertNotIn("filegroup(", build)
        self.assertNotIn("future", build)
        self.value("host_tools_data.bzl", "HOST_TOOLS")


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
        self.file(
            "CMakeLists.txt", "#[[ project(ignored VERSION 9.9.9) ]]\nproject(aobus VERSION 1.2.3) # sole source\n"
        )
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
