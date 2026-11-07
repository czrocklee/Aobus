"""Repository-owned projection, stable paths and prepared-mode contracts."""

import ast
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ao.core import bazel_compiler_probe as compiler_probe
from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_deps as deps
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers
from ao.core import bazel_resolver
from ao.core import bazel_workspace as workspace
from ao.core.bazel_resolver import _audit_executable, _external_input


class RepositoryPlanTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-repository-plan-")
        self.addCleanup(temporary.cleanup)
        # Incidental Windows short TEMP aliases are not provider topology.
        self.root = Path(temporary.name).resolve()

    def render(self, name="first", *, config="Debug", copts=(), package="fixture", owned=True, platform="linux"):
        provider = self.root / name
        include = provider / "include"
        include.mkdir(parents=True)
        header = include / "fixture.h"
        header.write_text("declared header\n")
        (include / "unrelated.h").write_text("unrelated installed port\n")
        selected = {"name": package, "modules": []}
        spec = {
            "schema": 1,
            "platform": platform,
            "config": config,
            "linkage": "static",
            "provider": {"name": "fixture", "version": "reviewed"},
            "allowed_roots": [str(provider)],
            "packages": [selected],
        }
        interface = {"header_dirs": [str(include)], "copts": list(copts)}
        adapter = deps._Adapter(spec, {"schema": 1, "patches": [dependencies._manual_patch(spec, package, interface)]})
        adapter.prepare()
        if owned:
            adapter.header_files[package] = [header]
        return adapter.resolve()

    def test_owned_headers_are_sparse_without_materializing_provider_payloads(self):
        resolution = self.render()
        self.assertEqual(
            resolution["packages"][0]["headers"],
            [
                {
                    "root": str(self.root / "first/include"),
                    "index": 0,
                    "files": [{"source": str(self.root / "first/include/fixture.h"), "path": "fixture.h"}],
                }
            ],
        )
        self.assertEqual(resolution["manifest"]["packages"][0]["declared_headers"], 1)
        self.assertEqual(list(self.root.iterdir()), [self.root / "first"])

    def test_relocated_providers_retain_lexical_header_paths_and_search_topology(self):
        first = self.render("first")["packages"][0]
        second = self.render("second")["packages"][0]
        self.assertEqual(first["includes"], [{"tree": 0, "path": "."}])
        self.assertEqual(second["includes"], first["includes"])
        for name, package in (("first", first), ("second", second)):
            tree = package["headers"][0]
            self.assertEqual(tree["root"], str(self.root / name / "include"))
            self.assertEqual(
                tree["files"], [{"source": str(self.root / name / "include/fixture.h"), "path": "fixture.h"}]
            )

    def test_admission_retains_configuration_without_a_python_target_inventory(self):
        for config in ("Debug", "Release"):
            with self.subTest(config=config):
                resolution = self.render(config.lower(), config=config)
                self.assertEqual(resolution["manifest"]["configuration"]["config"], config)
                self.assertNotIn("targets", resolution["manifest"])
                self.assertEqual([package["name"] for package in resolution["packages"]], ["fixture"])

    def test_unknown_or_unsatisfied_compile_requirements_are_not_silently_discarded(self):
        for index, flag in enumerate(("-pthread", "-fPIC", "-msse2")):
            with self.subTest(flag=flag), self.assertRaisesRegex(deps.BazelDependencyError, "unsatisfied compile"):
                self.render(str(index), copts=(flag,))

    def test_exact_pipewire_requirement_is_evidence_not_dead_wrapper_copts(self):
        resolution = self.render(package="pipewire", copts=("-fno-strict-aliasing", "-fno-strict-overflow"))
        manifest = resolution["manifest"]
        self.assertEqual(
            manifest["packages"][0]["interface"]["copts"], ["-fno-strict-aliasing", "-fno-strict-overflow"]
        )
        self.assertNotIn("copts", resolution["packages"][0])

    def test_gtkmm_accepts_only_empty_or_complete_linux_sse_requirements_without_wrapper_copts(self):
        for name, copts in (("portable", ()), ("x86", ("-mfpmath=sse", "-msse", "-msse2"))):
            with self.subTest(name=name):
                resolution = self.render(name, package="gtkmm", copts=copts)
                self.assertEqual(resolution["manifest"]["packages"][0]["interface"]["copts"], list(copts))
                self.assertNotIn("copts", resolution["packages"][0])

    def test_gtkmm_partial_extra_unknown_and_nonlinux_compile_requirements_fail_closed(self):
        for index, (platform, copts, message) in enumerate(
            (
                ("linux", ("-msse2",), "unreviewed GTKmm"),
                ("linux", ("-mfpmath=sse", "-msse", "-msse2", "-pthread"), "unreviewed GTKmm"),
                ("linux", ("-march=native",), "unsupported compile flag"),
                ("macos", ("-mfpmath=sse", "-msse", "-msse2"), "unsatisfied compile"),
            )
        ):
            with (
                self.subTest(platform=platform, copts=copts),
                self.assertRaisesRegex(deps.BazelDependencyError, message),
            ):
                self.render(str(index), package="gtkmm", copts=copts, platform=platform)


class ReadInventoryTest(unittest.TestCase):
    def test_watch_path_normalization_preserves_posix_case_and_uses_windows_semantics(self):
        source = ast.parse((native.PROJECT_ROOT / "bazel/dependencies.bzl").read_text())
        function = next(
            node for node in source.body if isinstance(node, ast.FunctionDef) and node.name == "_watch_path"
        )
        namespace = {}
        exec(compile(ast.Module(body=[function], type_ignores=[]), "watch_path", "exec"), namespace)
        normalize = namespace["_watch_path"]
        posix = SimpleNamespace(os=SimpleNamespace(name="linux"))
        windows = SimpleNamespace(os=SimpleNamespace(name="windows 2025"))
        self.assertNotEqual(normalize(posix, "/tmp/Repository"), normalize(posix, "/tmp/repository"))
        self.assertEqual(normalize(windows, "C:\\Repository"), normalize(windows, "c:/repository"))

    def test_generated_repository_and_cwd_search_probes_are_not_external_inputs(self):
        with tempfile.TemporaryDirectory(prefix="aobus-own-repository-") as temporary:
            root = Path(temporary)
            repository = root / "generated"
            repository.mkdir()
            for path in (repository, repository / "_resolution/repository.json", repository / "missing-tool.exe"):
                with self.subTest(path=path):
                    self.assertIsNone(_external_input(str(path), repository))
            outside = root / "provider/header.h"
            self.assertEqual(_external_input(str(outside), repository), str(outside))
            self.assertIsNone(_external_input(3, repository))

    def test_windows_subprocess_audit_optional_executable_does_not_break_resolution(self):
        self.assertIsNone(_audit_executable(None))
        self.assertIsNone(_audit_executable(3))
        self.assertEqual(_audit_executable("C:/tools/pkgconf.exe"), "C:/tools/pkgconf.exe")
        self.assertEqual(_audit_executable(b"/tools/pkgconf"), "/tools/pkgconf")
        self.assertEqual(_audit_executable(Path("/tools/pkgconf")), str(Path("/tools/pkgconf")))

    def test_rapidyaml_records_both_present_and_absent_nested_markers(self):
        with tempfile.TemporaryDirectory(prefix="aobus-version-probes-") as temporary:
            include = Path(temporary)
            present = include / "c4/yml/version.hpp"
            absent = include / "ryml/c4/yml/version.hpp"
            present.parent.mkdir(parents=True)
            absent.parent.mkdir(parents=True)
            present.write_text("version marker")
            observed = []
            with native.capture_probes(observed.append):
                self.assertEqual(dependencies._rapidyaml_version_header(include), present)
            self.assertEqual(set(observed), {present, absent})
            absent.write_text("new conflicting marker")
            with self.assertRaisesRegex(native.BazelWorkspaceError, "ambiguous"):
                dependencies._rapidyaml_version_header(include)

    def test_adapter_retains_lexical_alias_watch_before_returning_canonical_payload(self):
        with tempfile.TemporaryDirectory(prefix="aobus-alias-probes-") as temporary:
            root = Path(temporary).resolve()
            payload = root / "payload.h"
            payload.write_text("payload")
            spelling = root / "alias.h"
            spec = {
                "schema": 1,
                "platform": "linux",
                "config": "Debug",
                "linkage": "static",
                "provider": {"name": "fixture", "version": "1"},
                "allowed_roots": [str(root)],
                "packages": [],
            }
            adapter = deps._Adapter(spec, {"schema": 1, "patches": []})
            with patch.object(deps, "_resolve", return_value=payload):
                self.assertEqual(adapter.path(str(spelling)), payload)
            self.assertEqual(adapter.aliases, {spelling})

    def test_adapter_records_missing_library_candidates(self):
        with tempfile.TemporaryDirectory(prefix="aobus-library-probes-") as temporary:
            root = Path(temporary).resolve()
            spec = {
                "schema": 1,
                "platform": "linux",
                "config": "Debug",
                "linkage": "static",
                "provider": {"name": "fixture", "version": "1"},
                "allowed_roots": [str(root)],
                "library_dirs": [str(root)],
                "packages": [],
            }
            adapter = deps._Adapter(spec, {"schema": 1, "patches": []})
            self.assertEqual(adapter.candidates("missing", "static"), [])
            self.assertIn(root / "libmissing.a", adapter.probes)

    def test_native_path_observes_lexical_input_before_canonicalization(self):
        with tempfile.TemporaryDirectory(prefix="aobus-path-probes-") as temporary:
            root = Path(temporary).resolve()
            spelling = root / "nested" / ".." / "payload"
            observed = []
            with native.capture_probes(observed.append):
                self.assertEqual(native.native_path(spelling, root), root / "payload")
            self.assertEqual(observed, [spelling.absolute()])


class _RepositoryPath:
    def __init__(self, path, directory_reads):
        self.path = Path(path)
        self.directory_reads = directory_reads

    def __str__(self):
        return str(self.path)

    @property
    def dirname(self):
        return _RepositoryPath(self.path.parent, self.directory_reads)

    @property
    def basename(self):
        return self.path.name

    @property
    def exists(self):
        return self.path.exists()

    @property
    def is_dir(self):
        return self.path.is_dir()

    def get_child(self, name):
        return _RepositoryPath(self.path / name, self.directory_reads)

    def readdir(self, *, watch):
        if watch != "yes":
            raise AssertionError("directory membership must be watched")
        self.directory_reads.add(str(self.path))
        return [_RepositoryPath(path, self.directory_reads) for path in self.path.iterdir()]


class RepositoryRuleTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-rule with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.watches = set()
        self.directory_reads = set()
        # Minimal valid tools observation: both-config imports keyed only by
        # configuration, exactly as the resolver publishes them.
        self.inventory = {
            "watch_files": [],
            "watch_directories": [],
            "imports": {
                "debug": {"packages": [], "manifest": {"configuration": "debug"}},
                "release": {"packages": [], "manifest": {"configuration": "release"}},
            },
        }
        self.invocations = []
        self.materialized_imports = []
        self.materialized_winui = []
        self.materialized_aliases = []
        self.materialized_host_tools = []
        self.materialized_toolchain = []
        self.public_interfaces = {"debug": ["fixture"], "release": ["fixture"]}

        def attribute(**kwargs):
            return kwargs

        self.environment_rules = []
        self.namespace = {
            "attr": SimpleNamespace(string=attribute, label=attribute, label_list=attribute),
            "Label": str,
            "repository_rule": attribute,
            "module_extension": attribute,
            "json": SimpleNamespace(encode=json.dumps, decode=json.loads),
            "fail": self.fail,
            "load": self.load,
        }
        source = (native.PROJECT_ROOT / "bazel/dependencies.bzl").read_text(encoding="utf-8")
        exec(compile(ast.parse(source), "dependencies.bzl", "exec"), self.namespace)
        attributes = self.namespace["_governed_repository"]["attrs"]
        self.policy = attributes["policy"]["default"]
        self.resolver = attributes["resolver"]["default"]
        host = "windows" if sys.platform == "win32" else "macos" if sys.platform == "darwin" else "linux"
        # Real minimal mode-neutral metadata: the shared environment json doubles
        # as the resolver request, exactly as the environment repository publishes it.
        self.environment = {
            "schema": 1,
            "host": host,
            "python": "python",
            "environment": {"PATH": "/prepared/tools/bin", "TEMP": "shared-environment-temporary"},
        }
        self.file("environment.json", json.dumps(self.environment), executable=False)
        self.ctx = SimpleNamespace(
            path=self.path,
            watch=lambda path: self.watches.add(str(path)),
            attr=SimpleNamespace(
                resolver=self.resolver, policy=self.policy, group="tools", _environment="environment.json"
            ),
            # Match the real filesystem separators used by _RepositoryPath.
            os=SimpleNamespace(
                name="windows" if sys.platform == "win32" else sys.platform,
                environ={"UNRELATED": "not forwarded"},
            ),
            file=self.file,
            execute=self.execute,
            read=lambda path: Path(str(path)).read_text(encoding="utf-8"),
        )

    def load(self, label, *names):
        """Fake environment and native materialization at their public boundaries."""
        if label == "//bazel:import_repository.bzl":

            def materialize_imports(ctx, subpackage, config, packages, manifest):
                self.assertIs(ctx, self.ctx)
                self.materialized_imports.append((subpackage, config, packages, manifest))
                return self.public_interfaces[config]

            def materialize_aliases(ctx, targets):
                self.assertIs(ctx, self.ctx)
                self.materialized_aliases.append(targets)

            self.namespace.update(materialize_imports=materialize_imports, materialize_aliases=materialize_aliases)
            self.assertEqual(set(names), {"materialize_imports", "materialize_aliases"})
            return
        if label == "//bazel:winui_repository.bzl":
            self.assertEqual(names, ("materialize_winui",))

            def materialize_winui(ctx, subpackage, facts):
                self.assertIs(ctx, self.ctx)
                self.materialized_winui.append((subpackage, facts))
                return self.public_interfaces[subpackage]

            self.namespace["materialize_winui"] = materialize_winui
            return
        if label in {"//bazel:host_tools_repository.bzl", "//bazel:toolchain_repository.bzl"}:
            name = "materialize_host_tools" if "host_tools" in label else "materialize_toolchain"
            self.assertEqual(names, (name,))
            records = self.materialized_host_tools if "host_tools" in label else self.materialized_toolchain

            def materialize_sdk(ctx, facts):
                self.assertIs(ctx, self.ctx)
                records.append(facts)

            self.namespace[name] = materialize_sdk
            return
        self.assertEqual(label, "//bazel:environment.bzl")

        def repository_environment(ctx):
            path = ctx.path(ctx.attr._environment)
            data = json.loads(ctx.read(path))
            if (
                not isinstance(data, dict)
                or data.get("schema") != 1
                or not isinstance(data.get("environment"), dict)
                or not isinstance(data.get("python"), str)
            ):
                self.fail("Invalid shared native environment")
            ctx.watch(path)
            return data, dict(data["environment"])

        self.namespace["repository_environment"] = repository_environment
        self.namespace["ENVIRONMENT_ATTRIBUTE"] = self.namespace["attr"].label(
            default="@aobus_environment//:environment.json", allow_single_file=True
        )
        self.namespace["environment_repository"] = lambda **kwargs: self.environment_rules.append(kwargs)
        for name in names:
            self.assertIn(name, self.namespace)

    def path(self, value):
        value = str(value)
        if value.startswith("//"):
            path = native.PROJECT_ROOT / value[2:].replace(":", "/").lstrip("/")
        else:
            path = self.root / value
        return _RepositoryPath(path, self.directory_reads)

    def file(self, name, content, *, executable):
        self.assertFalse(executable)
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def execute(self, argv, **kwargs):
        self.assertTrue((self.root / "_tmp/.keep").is_file())
        self.invocations.append((argv, kwargs))
        self.file("_resolution/repository.json", json.dumps(self.inventory), executable=False)
        return SimpleNamespace(return_code=0, stdout="", stderr="")

    def test_isolated_resolver_uses_existing_scratch_directory_as_cache_prefix(self):
        self.namespace["_governed_repository_impl"](self.ctx)
        argv, options = self.invocations[0]
        scratch = str(self.root / "_tmp")
        self.assertEqual(
            argv[:10],
            [
                "python",
                "-I",
                "-S",
                "-B",
                "-X",
                "utf8",
                "-X",
                "pycache_prefix=" + scratch,
                str(self.path(self.resolver)),
                "--request",
            ],
        )
        environment = options["environment"]
        for name in ("TEMP", "TMP", "TMPDIR"):
            self.assertEqual(environment[name], scratch)
        # Resolver scratch stays repository-owned even when the shared
        # environment publishes its own TEMP; client values stay closed.
        self.assertEqual(environment["PATH"], "/prepared/tools/bin")
        self.assertIsNone(environment["UNRELATED"])
        self.assertEqual(environment["PYTHONDONTWRITEBYTECODE"], "1")
        self.assertTrue(Path(scratch).is_dir())

    def test_shared_environment_json_is_the_watched_mode_neutral_request(self):
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertIn(str(self.root / "environment.json"), self.watches)
        argv, _ = self.invocations[0]
        scratch = str(self.root / "_tmp")
        forwarded = {**self.environment["environment"], "TEMP": scratch, "TMP": scratch, "TMPDIR": scratch}
        self.assertEqual(
            argv[10:-1],
            [
                str(self.root / "environment.json"),
                "--group",
                "tools",
                "--output",
                str(self.root / "_resolution"),
                "--environment-json",
            ],
        )
        self.assertEqual(json.loads(argv[-1]), forwarded)

    def test_shared_policy_retains_only_dependency_contract_inputs(self):
        # SDK and bootstrap inputs moved to the shared environment repository.
        self.assertEqual(set(self.policy), {"//:config/bazel/dependencies.json", "//:dependency-contract.json"})
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertLessEqual({str(self.path(label)) for label in self.policy}, self.watches)

    def test_inventory_sources_membership_and_dynamic_bytecode_remain_watched(self):
        outside = self.root.parent / (self.root.name + "-inputs")
        outside.mkdir()
        self.addCleanup(outside.rmdir)
        external_files = {str(outside / name) for name in ("module.py", "legacy.pyc", "missing.cache")}
        self.inventory["watch_files"] = [*external_files, str(self.root / "_tmp/probe.pyc")]
        self.inventory["watch_directories"] = [str(outside), str(outside / "missing"), str(self.root / "_tmp")]
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertLessEqual(external_files | {str(outside / "missing")}, self.watches)
        self.assertIn(str(outside), self.directory_reads)
        self.assertNotIn(str(self.root / "_tmp/probe.pyc"), self.watches)
        self.assertNotIn(str(self.root / "_tmp"), self.directory_reads)
        # Portal modules the resolver never imported are not repository inputs.
        core = native.PROJECT_ROOT / "script/ao/core"
        self.assertFalse({str(path) for path in core.glob("*.py")} & self.watches)
        self.assertNotIn(str(core), self.directory_reads)

    def test_matching_configuration_interfaces_are_delegated_to_native_mode_aliases(self):
        self.ctx.attr.group = "native"
        self.inventory = {
            "watch_files": [],
            "watch_directories": [],
            "imports": {
                config: {"packages": [], "manifest": {"configuration": config}} for config in ("debug", "release")
            },
        }
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(
            self.materialized_imports,
            [
                ("debug", "debug", [], {"configuration": "debug"}),
                ("release", "release", [], {"configuration": "release"}),
            ],
        )
        self.assertEqual(self.materialized_aliases, [["fixture"]])
        self.assertEqual(self.materialized_winui, [])
        self.assertEqual(self.materialized_host_tools, [])
        self.assertEqual(self.materialized_toolchain, [])

    def test_incomplete_extra_or_root_layout_import_observations_fail_before_materialization(self):
        complete = {
            "debug": {"packages": [], "manifest": {"configuration": "debug"}},
            "release": {"packages": [], "manifest": {"configuration": "release"}},
        }
        for group in ("native", "test", "tools"):
            for name, imports in (
                ("empty", {}),
                ("incomplete", {"debug": complete["debug"]}),
                ("extra", {**complete, "profile": complete["debug"]}),
                ("root-layout", {"": complete["debug"]}),
            ):
                with self.subTest(group=group, observation=name):
                    self.ctx.attr.group = group
                    self.inventory = {"watch_files": [], "watch_directories": [], "imports": imports}
                    with self.assertRaisesRegex(AssertionError, "require Debug and Release"):
                        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(self.materialized_imports, [])
        self.assertEqual(self.materialized_aliases, [])
        self.assertEqual(self.materialized_winui, [])

    def test_incompatible_debug_release_interfaces_fail_before_alias_materialization(self):
        self.public_interfaces["release"] = ["different_consumer"]
        for group in ("native", "winui"):
            with self.subTest(group=group):
                self.ctx.attr.group = group
                observation = (
                    {"winui": {"debug": {}, "release": {}}}
                    if group == "winui"
                    else {
                        "imports": {
                            "debug": {"packages": [], "manifest": {}},
                            "release": {"packages": [], "manifest": {}},
                        }
                    }
                )
                self.inventory = {"watch_files": [], "watch_directories": [], **observation}
                self.materialized_imports.clear()
                self.materialized_winui.clear()
                self.materialized_aliases.clear()
                with self.assertRaisesRegex(AssertionError, "different consumer interfaces"):
                    self.namespace["_governed_repository_impl"](self.ctx)
                self.assertEqual(self.materialized_aliases, [])

    def test_tools_catalog_imports_cover_both_configurations_without_root_aliases(self):
        # setUp publishes the tools group request with a complete observation.
        self.inventory["host_tools"] = {"python": "python"}
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(
            self.materialized_imports,
            [
                ("catalog-debug", "debug", [], {"configuration": "debug"}),
                ("catalog-release", "release", [], {"configuration": "release"}),
            ],
        )
        self.assertEqual(self.materialized_aliases, [])
        self.assertEqual(self.materialized_host_tools, [{"python": "python"}])
        self.assertEqual(self.materialized_winui, [])
        self.assertEqual(self.materialized_toolchain, [])

    def test_winui_observations_are_projected_before_matching_root_aliases(self):
        self.ctx.attr.group = "winui"
        self.inventory = {
            "watch_files": [],
            "watch_directories": [],
            "winui": {"debug": {"sdk": "first"}, "release": {"sdk": "second"}},
        }
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(self.materialized_winui, [("debug", {"sdk": "first"}), ("release", {"sdk": "second"})])
        self.assertEqual(self.materialized_aliases, [["fixture"]])
        self.assertEqual(self.materialized_imports, [])
        self.assertEqual(self.materialized_host_tools, [])
        self.assertEqual(self.materialized_toolchain, [])

    def test_winui_absent_sdk_facts_still_project_both_configurations(self):
        self.ctx.attr.group = "winui"
        self.inventory = {"watch_files": [], "watch_directories": [], "winui": {"debug": None, "release": None}}
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(self.materialized_winui, [("debug", None), ("release", None)])
        self.assertEqual(self.materialized_aliases, [["fixture"]])
        self.assertEqual(self.materialized_imports, [])

    def test_winui_observations_require_exact_debug_and_release_keys(self):
        self.ctx.attr.group = "winui"
        for name, observation in (
            ("empty", {}),
            ("incomplete", {"debug": None}),
            ("extra", {"debug": None, "release": None, "profile": None}),
            ("root-layout", {"": None, "debug": None, "release": None}),
        ):
            with self.subTest(observation=name):
                self.inventory = {"watch_files": [], "watch_directories": [], "winui": observation}
                with self.assertRaisesRegex(AssertionError, "WinUI SDK"):
                    self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(self.materialized_winui, [])
        self.assertEqual(self.materialized_aliases, [])
        self.assertEqual(self.materialized_imports, [])

    def test_toolchain_dispatch_materializes_only_the_compiler_observation(self):
        self.ctx.attr.group = "toolchain"
        self.inventory = {
            "watch_files": [],
            "watch_directories": [],
            "toolchain": {"identity": {"compiler": "fixture"}},
        }
        self.namespace["_governed_repository_impl"](self.ctx)
        self.assertEqual(self.materialized_toolchain, [{"identity": {"compiler": "fixture"}}])
        self.assertEqual(self.materialized_imports, [])
        self.assertEqual(self.materialized_winui, [])
        self.assertEqual(self.materialized_aliases, [])
        self.assertEqual(self.materialized_host_tools, [])


class ResolverRequestTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-resolver-request-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def resolve(self, request, group):
        """Run the real resolver entry point with the dependency boundary faked."""
        (self.root / "request.json").write_text(json.dumps(request), encoding="utf-8")
        output = self.root / "resolution"
        argv = [
            "resolver",
            "--request",
            str(self.root / "request.json"),
            "--group",
            group,
            "--output",
            str(output),
            "--environment-json",
            "{}",
        ]
        calls = []

        def projection(tree, config, prefix=None, group=None):
            calls.append((config, prefix, group))
            return {
                "packages": [{"name": "fixture"}],
                "manifest": {"configuration": config},
                "watch_files": [],
                "watch_directories": [],
            }

        with (
            patch.object(sys, "argv", argv),
            patch.dict(os.environ, {}, clear=True),
            patch.object(sys, "addaudithook"),
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(dependencies, "resolve_dependencies", side_effect=projection),
        ):
            status = bazel_resolver.main()
        report = json.loads((output / "repository.json").read_text(encoding="utf-8"))
        return status, report, calls

    def test_native_and_test_groups_resolve_both_configurations_and_key_imports_by_configuration(self):
        for group in ("native", "test"):
            with self.subTest(group=group):
                status, report, calls = self.resolve({"schema": 1, "host": "linux"}, group)
                self.assertEqual(status, 0)
                self.assertEqual(calls, [("debug", None, group), ("release", None, group)])
                self.assertEqual(list(report["imports"]), ["debug", "release"])
                for config in ("debug", "release"):
                    self.assertEqual(set(report["imports"][config]), {"packages", "manifest"})
                    self.assertEqual(report["imports"][config]["manifest"], {"configuration": config})
                self.assertNotIn("winui", report)
                self.assertNotIn("error", report)
                # The resolver's own source stays an explicit repository input.
                self.assertIn(str(Path(bazel_resolver.__file__).resolve()), report["watch_files"])

    def test_top_level_request_config_is_rejected_mode_neutrally(self):
        errors = set()
        for config in ("debug", "release", "fastbuild"):
            with self.subTest(config=config):
                status, report, calls = self.resolve({"schema": 1, "host": "linux", "config": config}, "native")
                self.assertEqual(status, 1)
                self.assertEqual(report["imports"], {})
                self.assertEqual(calls, [])
                self.assertRegex(report["error"], "mode-neutral")
                errors.add(report["error"])
        self.assertEqual(len(errors), 1)

    def test_winui_group_observes_absent_sdk_facts_without_dependency_resolution(self):
        status, report, calls = self.resolve({"schema": 1, "host": "linux"}, "winui")
        self.assertEqual(status, 0)
        self.assertEqual(report["winui"], {"debug": None, "release": None})
        self.assertEqual(report["imports"], {})
        self.assertEqual(calls, [])
        self.assertNotIn("host_tools", report)
        self.assertNotIn("toolchain", report)
        self.assertNotIn("error", report)


class LaunchConfigurationTest(unittest.TestCase):
    def test_windows_runtime_profile_is_explicit_in_the_generated_launcher(self):
        # The repository rule no longer selects the runtime profile; the shared
        # environment repository consumes these launcher repo_env selectors.
        runtime = {"USERPROFILE", "LOCALAPPDATA", "APPDATA", "HOMEDRIVE", "HOMEPATH", "SYSTEMROOT", "SYSTEMDRIVE"}
        with tempfile.TemporaryDirectory(prefix="aobus-native-runtime-") as temporary:
            root = Path(temporary)
            environment = {name: str(root / name.lower()) for name in runtime}
            environment.update(
                {
                    "WINDOWSSDKDIR": str(root / "sdk"),
                    "WINDOWSSDKVERSION": "10.0.26100.0",
                    "VCTOOLSVERSION": "14.51.36231",
                    "VCTOOLSINSTALLDIR": str(root / "vc"),
                    "TEMP": "volatile-nix-shell-temporary-directory",
                    "TMP": "another-volatile-temporary-directory",
                    "UNRELATED_CLIENT_VALUE": "must-not-be-forwarded",
                }
            )
            with (
                patch.dict(workspace.os.environ, environment, clear=True),
                patch.object(native, "native_platform", return_value="windows"),
                patch.object(providers, "vcpkg_prefix", return_value=root / "provider"),
                patch.object(workspace.builddir, "windows_state_root", return_value=root / "state"),
            ):
                prepared = workspace.prepare_workspace(root / "build", "debug", prefix=str(root / "provider"))
            rc = (prepared / "aobus.bazelrc").read_text()
            for name in runtime:
                self.assertIn("--repo_env=" + name + "=", rc)
            for name in ("WindowsSdkDir", "WindowsSDKVersion", "VCToolsVersion", "VCToolsInstallDir"):
                self.assertIn("--repo_env=" + name + "=", rc)
            self.assertNotIn("UNRELATED_CLIENT_VALUE", rc)
            self.assertNotIn("volatile-", rc)

    def test_nix_share_metadata_is_declared_without_ambient_pkgconfig_paths(self):
        with tempfile.TemporaryDirectory(prefix="aobus-nix-metadata-") as temporary:
            root = Path(temporary)
            provider = root / "catch2"
            pcdir = provider / "share/pkgconfig"
            pcdir.mkdir(parents=True)
            (pcdir / "catch2-with-main.pc").write_text("Name: fixture\nVersion: 1\n")
            report = root / "report.json"
            report.write_text(
                json.dumps(
                    {
                        "schemaVersion": 1,
                        "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
                        "dependencies": {
                            "catch2": {
                                "version": "1",
                                "storePath": "/nix/store/catch2",
                                "drvPath": "/nix/store/catch2.drv",
                                "outputs": {"out": "/nix/store/catch2"},
                            }
                        },
                    }
                )
            )
            policy = {
                "packages": {
                    "native": [],
                    "test": [{"name": "catch2", "nix": "catch2", "modules": ["catch2-with-main"]}],
                }
            }
            original_is_dir = Path.is_dir
            with (
                patch.dict(workspace.os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}, clear=True),
                patch.object(providers, "nix_root", return_value=provider),
                patch.object(Path, "is_dir", lambda path: path == Path("/nix/store/catch2") or original_is_dir(path)),
                patch.object(native, "executable", return_value="/fixture/pkgconf"),
                patch.object(
                    providers.subprocess, "run", return_value=providers.subprocess.CompletedProcess([], 1, "", "")
                ),
            ):
                base, _, _ = providers.linux_provider(policy, identity_path=root / "identity.json")
            self.assertEqual(base["pkg_config"]["pc_dirs"], [str(pcdir)])

    def test_preparation_writes_configuration_without_resolving_or_hashing_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-launch-") as temporary:
            tree = Path(temporary)
            with (
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(native, "executable", side_effect=AssertionError("outer compiler probe")),
                patch.object(
                    dependencies, "resolve_dependencies", side_effect=AssertionError("outer dependency resolution")
                ),
                patch.object(compiler_probe, "probe_compiler", side_effect=AssertionError("outer compiler probe")),
                patch.object(compiler_probe, "_compiler_identity", side_effect=AssertionError("outer compiler hash")),
                patch.object(compiler_probe, "nix_cc_wrapper", side_effect=AssertionError("outer compiler report")),
            ):
                prepared = workspace.prepare_workspace(tree, "debug")
                request = json.loads((prepared / "request.json").read_text())
                rc = (prepared / "aobus.bazelrc").read_text()
                stamp = (prepared / "aobus.bazelrc").stat().st_mtime_ns
                self.assertEqual(workspace.prepare_workspace(tree, "debug"), prepared)
                self.assertEqual((prepared / "aobus.bazelrc").stat().st_mtime_ns, stamp)
            # The portal request is mode-neutral; the generated rc selects the profile.
            self.assertNotIn("config", request)
            self.assertIsNone(request["prefix"])
            self.assertIn("--repo_env=AOBUS_BAZEL_CONFIGURATION=", rc)
            self.assertIn("--repo_env=AOBUS_BAZEL_PYTHON=", rc)
            self.assertIn('build "--config=debug"', rc)
            self.assertNotIn("--repo_env=CC=", rc)
            self.assertNotIn("--repo_env=CXX=", rc)
            self.assertNotIn("--batch", rc)
            self.assertFalse((tree / "generated").exists())


if __name__ == "__main__":
    unittest.main()
