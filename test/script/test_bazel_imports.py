"""Mechanism contracts of the native SDK Starlark import boundary."""

import ast
import json
import unittest
from pathlib import Path
from types import SimpleNamespace

_BAZEL = Path(__file__).resolve().parents[2] / "bazel"


class _StarlarkFailure(Exception):
    """Sentinel for the Starlark helpers' fail() calls; carries the message."""


class _Select:
    """A recorded select() payload: its conditions and no-match failure text."""

    def __init__(self, conditions, no_match_error=None):
        self.conditions = conditions
        self.no_match_error = no_match_error


class _StarlarkHarness(unittest.TestCase):
    """Executes the real import helpers with repository_ctx and native faked at their boundary."""

    def setUp(self):
        self.links = {}
        self.files = {}
        self.rules = {}
        self.config_settings = {}
        self.exports = []

        def fail(message):
            raise _StarlarkFailure(message)

        self.namespace = {"json": SimpleNamespace(encode=json.dumps, decode=json.loads), "fail": fail}
        source = (_BAZEL / "import_repository.bzl").read_text(encoding="utf-8")
        exec(compile(ast.parse(source), "import_repository.bzl", "exec"), self.namespace)
        self.imports_namespace = {"fail": fail, "select": _Select, "native": self._native(), "load": self._load}
        source = (_BAZEL / "imports.bzl").read_text(encoding="utf-8")
        exec(compile(ast.parse(source), "imports.bzl", "exec"), self.imports_namespace)
        self.materialize_imports = self.namespace["materialize_imports"]
        self.materialize_aliases = self.namespace["materialize_aliases"]
        self.sdk_imports = self.imports_namespace["sdk_imports"]
        self.configured_aliases = self.imports_namespace["configured_aliases"]

    def _rule(self, kind):
        def declare(**kwargs):
            self.assertNotIn(kwargs["name"], self.rules, "duplicate generated rule: " + kwargs["name"])
            self.rules[kwargs["name"]] = (kind, kwargs)

        return declare

    def _native(self):
        def config_setting(**kwargs):
            self.assertNotIn(kwargs["name"], self.config_settings, "duplicate config setting: " + kwargs["name"])
            self.config_settings[kwargs["name"]] = kwargs["values"]

        return SimpleNamespace(
            exports_files=lambda names: self.exports.extend(names),
            config_setting=config_setting,
            filegroup=self._rule("filegroup"),
            alias=self._rule("alias"),
        )

    def _load(self, label, *names):
        """Bridge Starlark loads: rules_cc is faked at its boundary; repository helpers stay real."""
        if label == "@rules_cc//cc:defs.bzl":
            self.assertEqual(names, ("cc_import", "cc_library"))
            self.imports_namespace["cc_import"] = self._rule("cc_import")
            self.imports_namespace["cc_library"] = self._rule("cc_library")
        elif label == ":import_repository.bzl":
            for name in names:
                self.assertIn(name, self.namespace, "missing repository helper: " + name)
                self.imports_namespace[name] = self.namespace[name]
        else:
            raise AssertionError("unexpected load: " + label)

    def context(self):
        """A repository_ctx fake: links and generated files are recorded, never written to disk."""
        return SimpleNamespace(path=lambda source: source, symlink=self._symlink, file=self._file)

    def _symlink(self, target, destination):
        self.assertNotIn(destination, self.links, "duplicate artifact link: " + destination)
        self.links[destination] = target

    def _file(self, name, content, *, executable=False):
        self.assertFalse(executable)
        self.files[name] = content

    def tree(self, index, root, *paths):
        """One resolved header anchor: sparse declared files carrying lexical paths."""
        return {
            "root": root,
            "index": index,
            "files": [{"source": root + "/" + path, "path": path} for path in paths],
        }

    def library(self, index, source, basename, *, kind="static", runtime=None):
        return {"index": index, "kind": kind, "source": source, "basename": basename, "runtime": runtime}

    def package(self, name="fixture", *, headers=(), includes=(), libraries=(), runtime=(), defines=(), linkopts=()):
        """The adapter's normalized package schema; tree and library indices are owned upstream."""
        return {
            "name": name,
            "headers": list(headers),
            "includes": list(includes),
            "defines": list(defines),
            "linkopts": list(linkopts),
            "libraries": list(libraries),
            "runtime": list(runtime),
        }

    def manifest(self, platform="linux"):
        return {"configuration": {"schema": 1, "platform": platform, "config": "Debug", "linkage": "static"}}

    def nested(self):
        """Two header trees with a nested shared subtree, in the adapter's resolved order."""
        return self.package(
            headers=[
                self.tree(0, "/provider/include/A", "a.h", "sub/s.h"),
                self.tree(1, "/provider/include/B", "b.h"),
            ],
            includes=[{"tree": 0, "path": "."}, {"tree": 1, "path": "."}, {"tree": 0, "path": "sub"}],
            libraries=[self.library(0, "/provider/lib/libfixture.a", "libfixture.a")],
            defines=["FIXTURE=1"],
        )

    def materialized(self, packages, *, platform="linux", config="debug", subpackage=None):
        """Project packages into a subpackage; return the public targets and generated graph data."""
        subpackage = config if subpackage is None else subpackage
        targets = self.materialize_imports(self.context(), subpackage, config, packages, self.manifest(platform))
        return targets, self.projected(subpackage)[1]

    def projected(self, subpackage):
        """Evaluate the generated data-only module the way the subpackage BUILD load does."""
        namespace = {}
        exec(self.files[subpackage + "/sdk_data.bzl"], namespace)
        return namespace["CONFIG"], namespace["PACKAGES"]


class MaterializeImportsTest(_StarlarkHarness):
    """The repository helper's projection: sparse files, staged artifacts, and fail-closed paths."""

    def test_sparse_header_files_and_nested_include_topology_project_in_declared_order(self):
        targets, data = self.materialized([self.nested()])
        self.assertEqual(targets, ["fixture", "fixture_runtime", "fixture_runtime_extra", "manifest.json"])
        # Only declared files are linked; the provider header tree is never mirrored.
        self.assertEqual(
            self.links,
            {
                "debug/artifacts/_headers/fixture_0/a.h": "/provider/include/A/a.h",
                "debug/artifacts/_headers/fixture_0/sub/s.h": "/provider/include/A/sub/s.h",
                "debug/artifacts/_headers/fixture_1/b.h": "/provider/include/B/b.h",
                "debug/artifacts/fixture/lib/0/libfixture.a": "/provider/lib/libfixture.a",
            },
        )
        self.assertEqual(
            data[0]["headers"],
            [
                {
                    "name": "_headers_fixture_0",
                    "files": ["artifacts/_headers/fixture_0/a.h", "artifacts/_headers/fixture_0/sub/s.h"],
                },
                {"name": "_headers_fixture_1", "files": ["artifacts/_headers/fixture_1/b.h"]},
            ],
        )
        # The global include order A, B, A/sub survives with the nested tree remapped.
        self.assertEqual(
            data[0]["includes"],
            ["artifacts/_headers/fixture_0", "artifacts/_headers/fixture_1", "artifacts/_headers/fixture_0/sub"],
        )
        build = self.files["debug/BUILD.bazel"]
        self.assertIn('load("@aobus//bazel:imports.bzl", "sdk_imports")', build)
        self.assertIn('load(":sdk_data.bzl", "CONFIG", "PACKAGES")', build)

    def test_windows_shared_source_and_runtime_deduplicate_to_one_import_target(self):
        shared = self.library(0, "/vcpkg/fixture.lib", "fixture.lib", kind="shared", runtime="/vcpkg/fixture.dll")
        packages = [
            self.package("first", headers=[self.tree(0, "/vcpkg/first/include", "first.h")], libraries=[shared]),
            self.package(
                "second", headers=[self.tree(0, "/vcpkg/second/include", "second.h")], libraries=[dict(shared)]
            ),
        ]
        _, data = self.materialized(packages, platform="windows", config="release", subpackage="release")
        # One import library and one DLL are linked once and shared by both packages;
        # each package's header scope is still linked separately.
        self.assertEqual(
            self.links,
            {
                "release/artifacts/_headers/first_0/first.h": "/vcpkg/first/include/first.h",
                "release/artifacts/_headers/second_0/second.h": "/vcpkg/second/include/second.h",
                "release/artifacts/first/lib/0/fixture.lib": "/vcpkg/fixture.lib",
                "release/artifacts/first/lib/0/fixture.dll": "/vcpkg/fixture.dll",
            },
        )
        self.assertEqual(data[1]["libraries"], data[0]["libraries"])
        # Header scopes stay per-package even when the import target is shared.
        self.assertEqual([package["headers"][0]["name"] for package in data], ["_headers_first_0", "_headers_second_0"])
        self.sdk_imports("release", data)
        imports = sorted(name for name, (kind, _) in self.rules.items() if kind == "cc_import")
        self.assertEqual(imports, ["first_import_0"])
        _, kwargs = self.rules["first_import_0"]
        self.assertEqual(
            kwargs["interface_library"].conditions, {":_prepared_mode": "artifacts/first/lib/0/fixture.lib"}
        )
        # The runtime DLL is mode-independent and never wrapped in the prepared select.
        self.assertEqual(kwargs["shared_library"], "artifacts/first/lib/0/fixture.dll")
        self.assertEqual(self.rules["first"][1]["hdrs"], [":_headers_first_0"])
        self.assertEqual(self.rules["second"][1]["hdrs"], [":_headers_second_0"])
        self.assertEqual(self.rules["second"][1]["deps"].conditions, {":_prepared_mode": [":first_import_0"]})

    def test_shared_import_deduplication_is_scoped_to_windows_source_and_runtime_pairs(self):
        shared = self.library(0, "/provider/lib/libfixture.so", "libfixture.so", kind="shared")
        packages = [
            self.package("first", headers=[self.tree(0, "/provider/first", "first.h")], libraries=[shared]),
            self.package("second", headers=[self.tree(0, "/provider/second", "second.h")], libraries=[dict(shared)]),
        ]
        _, data = self.materialized(packages)
        self.assertEqual([package["libraries"][0]["name"] for package in data], ["first_import_0", "second_import_0"])
        self.sdk_imports("debug", data)
        imports = sorted(name for name, (kind, _) in self.rules.items() if kind == "cc_import")
        self.assertEqual(imports, ["first_import_0", "second_import_0"])

    def test_library_indices_retain_source_positions_after_system_links_become_linkopts(self):
        packages = [
            self.package(
                headers=[self.tree(0, "/provider/include", "fixture.h")],
                # Index 1 was a system library; it is admitted as a linkopt, not an import.
                libraries=[
                    self.library(0, "/provider/lib/core.lib", "core.lib"),
                    self.library(2, "/provider/bin/net.lib", "net.lib", kind="shared", runtime="/provider/bin/net.dll"),
                ],
                linkopts=["ws2_32.lib"],
            )
        ]
        _, data = self.materialized(packages, platform="windows")
        self.assertEqual(
            data[0]["libraries"],
            [
                {
                    "name": "fixture_import_0",
                    "kind": "static",
                    "file": "artifacts/fixture/lib/0/core.lib",
                    "runtime": None,
                },
                {
                    "name": "fixture_import_2",
                    "kind": "shared",
                    "file": "artifacts/fixture/lib/2/net.lib",
                    "runtime": "artifacts/fixture/lib/2/net.dll",
                },
            ],
        )
        self.assertEqual(data[0]["linkopts"], ["ws2_32.lib"])
        self.assertIn("debug/artifacts/fixture/lib/2/net.dll", self.links)

    def test_library_runtime_and_extra_runtime_stage_distinct_artifact_sets(self):
        packages = [
            self.package(
                headers=[self.tree(0, "/provider/include", "fixture.h")],
                libraries=[
                    self.library(
                        0,
                        "/provider/bin/fixture.lib",
                        "fixture.lib",
                        kind="shared",
                        runtime="/provider/bin/fixture.dll",
                    )
                ],
                runtime=["/provider/bin/plugin.dll"],
            )
        ]
        _, data = self.materialized(packages, platform="windows")
        self.assertEqual(
            data[0]["runtime"], ["artifacts/fixture/lib/0/fixture.dll", "artifacts/fixture/runtime/0/plugin.dll"]
        )
        self.assertEqual(data[0]["runtime_extra"], ["artifacts/fixture/runtime/0/plugin.dll"])
        self.sdk_imports("debug", data)
        self.assertEqual(self.rules["fixture_runtime"][1]["srcs"].conditions, {":_prepared_mode": data[0]["runtime"]})
        self.assertEqual(
            self.rules["fixture_runtime_extra"][1]["srcs"].conditions,
            {":_prepared_mode": ["artifacts/fixture/runtime/0/plugin.dll"]},
        )
        _, library = self.rules["fixture"]
        # The DLL is staged as plain data for the linking consumer, never a prepared-only artifact.
        self.assertEqual(library["data"], data[0]["runtime"])
        self.assertEqual(library["deps"].conditions, {":_prepared_mode": [":fixture_import_0"]})

    def test_debug_and_release_projections_expose_equal_public_interfaces(self):
        packages = [self.nested()]
        debug = self.materialize_imports(self.context(), "debug", "debug", packages, self.manifest())
        release = self.materialize_imports(self.context(), "release", "release", packages, self.manifest())
        self.assertEqual(debug, release)
        self.assertEqual(debug, ["fixture", "fixture_runtime", "fixture_runtime_extra", "manifest.json"])
        debug_config, debug_data = self.projected("debug")
        release_config, release_data = self.projected("release")
        self.assertEqual((debug_config, debug_data), ("debug", release_data))
        self.assertEqual(release_config, "release")
        # Both configurations stage the same artifacts from the same sources.
        self.assertEqual(
            {path.removeprefix("debug/"): source for path, source in self.links.items() if path.startswith("debug/")},
            {
                path.removeprefix("release/"): source
                for path, source in self.links.items()
                if path.startswith("release/")
            },
        )

    def test_projected_graph_data_excludes_absolute_provider_paths_except_admitted_linkopts(self):
        packages = [
            self.package(
                headers=[self.tree(0, "/provider/include", "a.h")],
                libraries=[self.library(0, "/provider/lib/libfixture.a", "libfixture.a")],
                runtime=["/provider/bin/plugin.so"],
                linkopts=["ws2_32.lib"],
                defines=["FIXTURE=1"],
            )
        ]
        manifest = {
            "configuration": {
                "schema": 1,
                "platform": "windows",
                "config": "Debug",
                "provider": {"name": "fixture", "version": "reviewed"},
            }
        }
        self.materialize_imports(self.context(), "catalog-debug", "debug", packages, manifest)
        data = self.files["catalog-debug/sdk_data.bzl"]
        self.assertNotIn("/provider", data)
        self.assertNotIn("root", data)
        self.assertIn("ws2_32.lib", data)
        self.assertIn("FIXTURE=1", data)
        # Preserve native audit facts and publish the actual Starlark surface,
        # rather than letting Python maintain a second target inventory.
        published = json.loads(self.files["catalog-debug/manifest.json"])
        self.assertEqual(
            published.pop("targets"), ["fixture", "fixture_runtime", "fixture_runtime_extra", "manifest.json"]
        )
        self.assertEqual(published, manifest)
        self.assertNotIn("targets", manifest)

    def test_projection_fails_closed_on_unsupported_configuration_or_subpackage(self):
        for subpackage, config, message in (
            ("debug", "fastbuild", "unsupported SDK configuration: fastbuild"),
            ("profile", "debug", "unsupported SDK subpackage: profile"),
        ):
            with self.subTest(config=config, subpackage=subpackage):
                self.links = {}
                self.files = {}
                with self.assertRaisesRegex(_StarlarkFailure, message):
                    self.materialize_imports(self.context(), subpackage, config, [], self.manifest())
                self.assertEqual(self.files, {})

    def test_projection_fails_closed_on_traversal_absolute_and_colliding_paths(self):
        headers = [self.tree(0, "/provider/include", "a.h")]
        for name, packages, message in (
            (
                "header traversal",
                [self.package(headers=[self.tree(0, "/provider/include", "../escape.h")])],
                "invalid artifact-relative path",
            ),
            (
                "header absolute path",
                [self.package(headers=[self.tree(0, "/provider/include", "/abs/a.h")])],
                "invalid artifact-relative path",
            ),
            (
                "include traversal",
                [self.package(headers=headers, includes=[{"tree": 0, "path": ".."}])],
                "invalid artifact-relative path",
            ),
            (
                "library basename traversal",
                [self.package(headers=headers, libraries=[self.library(0, "/p/lib/x.lib", "sub/../x.lib")])],
                "invalid artifact-relative path",
            ),
            (
                "artifact path collision",
                [
                    self.package(
                        headers=headers,
                        libraries=[self.library(0, "/p/one.lib", "x.lib"), self.library(0, "/p/two.lib", "x.lib")],
                    )
                ],
                "generated artifact path collision",
            ),
            (
                "conflicting shared import",
                [
                    self.package(
                        headers=headers,
                        libraries=[self.library(0, "/p/one.lib", "one.lib"), self.library(0, "/p/two.lib", "two.lib")],
                    )
                ],
                "conflicting shared import target",
            ),
        ):
            with self.subTest(name=name):
                self.links = {}
                self.files = {}
                with self.assertRaisesRegex(_StarlarkFailure, message):
                    self.materialize_imports(self.context(), "debug", "debug", packages, self.manifest())
                # A failed projection never materializes the repository entrypoint.
                self.assertEqual(self.files, {})

    def case_colliding(self):
        """Two distinct admitted files whose lexical paths differ only by case."""
        return self.package(
            headers=[
                {
                    "root": "/provider/include",
                    "index": 0,
                    "files": [
                        {"source": "/provider/include/Widget.h", "path": "Widget.h"},
                        {"source": "/provider/legacy/widget.h", "path": "widget.h"},
                    ],
                }
            ]
        )

    def test_windows_headers_colliding_only_by_case_fail_closed(self):
        # Distinct files that differ only by case share one case-insensitive
        # destination on Windows; projection must fail closed.
        with self.assertRaisesRegex(_StarlarkFailure, "generated artifact path collision"):
            self.materialize_imports(
                self.context(), "debug", "debug", [self.case_colliding()], self.manifest("windows")
            )
        self.assertEqual(self.files, {})

    def test_posix_headers_distinct_only_by_case_project_separately(self):
        # Case-sensitive POSIX destinations keep both files as distinct artifacts.
        _, data = self.materialized([self.case_colliding()])
        self.assertEqual(
            self.links,
            {
                "debug/artifacts/_headers/fixture_0/Widget.h": "/provider/include/Widget.h",
                "debug/artifacts/_headers/fixture_0/widget.h": "/provider/legacy/widget.h",
            },
        )
        self.assertEqual(
            data[0]["headers"][0]["files"],
            ["artifacts/_headers/fixture_0/Widget.h", "artifacts/_headers/fixture_0/widget.h"],
        )

    def test_windows_source_identity_folding_dedups_alias_spellings_to_one_link(self):
        # The same physical header observed under two spellings is one identity
        # on Windows: one link, no spurious collision, both entries retained.
        alias = {
            "root": "/provider/include",
            "index": 0,
            "files": [
                {"source": "C:/Provider/Include/fixture.h", "path": "fixture.h"},
                {"source": "c:\\provider\\include\\FIXTURE.H", "path": "fixture.h"},
            ],
        }
        targets, data = self.materialized([self.package(headers=[alias])], platform="windows")
        self.assertEqual(targets, ["fixture", "fixture_runtime", "fixture_runtime_extra", "manifest.json"])
        self.assertEqual(self.links, {"debug/artifacts/_headers/fixture_0/fixture.h": "C:/Provider/Include/fixture.h"})
        self.assertEqual(
            data[0]["headers"][0]["files"],
            ["artifacts/_headers/fixture_0/fixture.h", "artifacts/_headers/fixture_0/fixture.h"],
        )


class SdkImportsTest(_StarlarkHarness):
    """The graph macros' BUILD-level mechanisms: prepared selects and target admission."""

    def test_prepared_artifacts_select_only_the_matching_compilation_mode(self):
        _, packages = self.materialized([self.nested()])
        for config, mode in (("debug", "dbg"), ("release", "opt")):
            with self.subTest(config=config):
                self.rules = {}
                self.config_settings = {}
                self.exports = []
                self.sdk_imports(config, packages)
                self.assertEqual(self.config_settings, {"_prepared_mode": {"compilation_mode": mode}})
                self.assertEqual(self.exports, ["manifest.json"])
                _, kwargs = self.rules["fixture_import_0"]
                self.assertEqual(kwargs["visibility"], ["//visibility:private"])
                prepared = kwargs["static_library"]
                self.assertEqual(prepared.conditions, {":_prepared_mode": "artifacts/fixture/lib/0/libfixture.a"})
                self.assertNotIn("//conditions:default", prepared.conditions)
                self.assertIn("-c " + mode, prepared.no_match_error)
                self.assertIn(config.capitalize(), prepared.no_match_error)
                _, library = self.rules["fixture"]
                self.assertEqual(library["hdrs"], [":_headers_fixture_0", ":_headers_fixture_1"])
                self.assertEqual(library["includes"], packages[0]["includes"])
                self.assertEqual(library["deps"].conditions, {":_prepared_mode": [":fixture_import_0"]})
                self.assertEqual(library["data"], [])
                # Header filegroups are plain; only prepared artifacts carry the mode guard.
                self.assertEqual(
                    self.rules["_headers_fixture_0"][1]["srcs"],
                    ["artifacts/_headers/fixture_0/a.h", "artifacts/_headers/fixture_0/sub/s.h"],
                )

    def test_sdk_imports_rejects_fastbuild_and_other_unsupported_configurations(self):
        for config in ("fastbuild", "profile"):
            with self.subTest(config=config):
                with self.assertRaisesRegex(_StarlarkFailure, "unsupported SDK configuration: " + config):
                    self.sdk_imports(config, [])
                self.assertEqual(self.rules, {})
                self.assertEqual(self.exports, [])
                self.assertEqual(self.config_settings, {})

    def test_generated_target_collisions_fail_closed_before_any_rule_declaration(self):
        def projected(name):
            return {
                "name": name,
                "headers": [],
                "includes": [],
                "defines": [],
                "linkopts": [],
                "libraries": [],
                "runtime": [],
                "runtime_extra": [],
            }

        for name, packages, message in (
            ("duplicate package", [projected("fixture"), projected("fixture")], "name collision: fixture"),
            ("reserved export", [projected("manifest.json")], "name collision: manifest.json"),
            ("reserved mode setting", [projected("_prepared_mode")], "name collision: _prepared_mode"),
        ):
            with self.subTest(name=name):
                with self.assertRaisesRegex(_StarlarkFailure, message):
                    self.sdk_imports("debug", packages)
                self.assertEqual(self.rules, {})
                self.assertEqual(self.exports, [])
                self.assertEqual(self.config_settings, {})


class ConfiguredAliasesTest(_StarlarkHarness):
    """The root alias entrypoint: both configured subpackages map without a fastbuild default."""

    def test_root_aliases_map_materialized_targets_to_both_configurations(self):
        targets, _ = self.materialized([self.nested()])
        self.materialize_aliases(self.context(), targets)
        # Only target-name data is written at the root; graph data stays in the subpackages.
        self.assertEqual([name for name in sorted(self.files) if "/" not in name], ["BUILD.bazel", "sdk_targets.bzl"])
        self.assertEqual(
            self.files["sdk_targets.bzl"],
            "TARGETS = ['fixture', 'fixture_runtime', 'fixture_runtime_extra', 'manifest.json']\n",
        )
        build = self.files["BUILD.bazel"]
        self.assertIn('load("@aobus//bazel:imports.bzl", "configured_aliases")', build)
        self.assertIn('load(":sdk_targets.bzl", "TARGETS")', build)
        self.configured_aliases(targets)
        self.assertEqual(
            self.config_settings,
            {"_debug": {"compilation_mode": "dbg"}, "_release": {"compilation_mode": "opt"}},
        )
        for target in targets:
            with self.subTest(target=target):
                kind, kwargs = self.rules[target]
                self.assertEqual(kind, "alias")
                actual = kwargs["actual"]
                self.assertEqual(
                    actual.conditions, {":_debug": "//debug:" + target, ":_release": "//release:" + target}
                )
                self.assertNotIn("//conditions:default", actual.conditions)
                self.assertIn("fastbuild", actual.no_match_error)

    def test_alias_names_are_sorted_and_reserved_or_duplicate_names_fail_closed(self):
        self.materialize_aliases(self.context(), ["zeta", "alpha"])
        self.assertEqual(self.files["sdk_targets.bzl"], "TARGETS = ['alpha', 'zeta']\n")
        for targets, message in (
            (["fixture", "fixture"], "name collision: fixture"),
            (["_debug"], "name collision: _debug"),
            (["BUILD.bazel"], "name collision: BUILD.bazel"),
            (["sdk_targets.bzl"], "name collision: sdk_targets.bzl"),
        ):
            with self.subTest(targets=targets):
                with self.assertRaisesRegex(_StarlarkFailure, message):
                    self.configured_aliases(targets)
                self.assertEqual(self.rules, {})
                self.assertEqual(self.config_settings, {})


if __name__ == "__main__":
    unittest.main()
