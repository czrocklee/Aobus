"""Repository-owned projection, stable paths and prepared-mode contracts."""

import ast
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from ao.core import bazel_deps as deps
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
        adapter = deps._Adapter(spec, {"schema": 1, "patches": [workspace._manual_patch(spec, package, interface)]})
        adapter.prepare()
        if owned:
            adapter.header_files[package] = [header]
        output = self.root / (name + "-output")
        output.mkdir()
        plan = {}
        manifest = adapter.build(output, plan=plan)
        return output, plan, manifest

    def test_owned_headers_are_sparse_without_materializing_provider_payloads(self):
        output, plan, manifest = self.render()
        self.assertEqual(len(plan), 1)
        self.assertEqual(next(iter(plan.values()))[0].name, "fixture.h")
        self.assertFalse((output / "artifacts").exists())
        self.assertEqual(manifest["packages"][0]["declared_headers"], 1)
        self.assertFalse((output / "interfaces.bzl").exists())
        self.assertFalse((output / "MODULE.bazel").exists())

    def test_interface_paths_do_not_embed_the_provider_prefix(self):
        first, first_plan, _ = self.render("first")
        second, second_plan, _ = self.render("second")
        self.assertEqual(list(first_plan), list(second_plan))
        self.assertEqual((first / "BUILD.bazel").read_bytes(), (second / "BUILD.bazel").read_bytes())
        self.assertNotIn(str(self.root), (first / "BUILD.bazel").read_text())

    def test_prepared_modes_have_no_default_fallback(self):
        for config, mode in (("Debug", "dbg"), ("Release", "opt")):
            with self.subTest(config=config):
                output, _, _ = self.render(config.lower(), config=config)
                build = (output / "BUILD.bazel").read_text()
                self.assertIn(json.dumps(mode), build)
                self.assertIn("no_match_error", build)
                self.assertIn(f"Prepared {config} dependencies require -c {mode}", build)
                self.assertNotIn("//conditions:default", build)

    def test_unknown_or_unsatisfied_compile_requirements_are_not_silently_discarded(self):
        for index, flag in enumerate(("-pthread", "-fPIC", "-msse2")):
            with self.subTest(flag=flag), self.assertRaisesRegex(deps.BazelDependencyError, "unsatisfied compile"):
                self.render(str(index), copts=(flag,))

    def test_exact_pipewire_requirement_is_evidence_not_dead_wrapper_copts(self):
        output, _, manifest = self.render(package="pipewire", copts=("-fno-strict-aliasing", "-fno-strict-overflow"))
        self.assertEqual(
            manifest["packages"][0]["interface"]["copts"], ["-fno-strict-aliasing", "-fno-strict-overflow"]
        )
        self.assertIn("copts = []", (output / "BUILD.bazel").read_text())

    def test_gtkmm_accepts_only_empty_or_complete_linux_sse_requirements_without_wrapper_copts(self):
        for name, copts in (("portable", ()), ("x86", ("-mfpmath=sse", "-msse", "-msse2"))):
            with self.subTest(name=name):
                output, _, manifest = self.render(name, package="gtkmm", copts=copts)
                self.assertEqual(manifest["packages"][0]["interface"]["copts"], list(copts))
                self.assertIn("copts = []", (output / "BUILD.bazel").read_text())

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
        source = ast.parse((workspace.PROJECT_ROOT / "bazel/dependencies.bzl").read_text())
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
            with patch.object(workspace, "_INPUT_PROBE", observed.append):
                self.assertEqual(workspace._rapidyaml_version_header(include), present)
            self.assertEqual(set(observed), {present, absent})
            absent.write_text("new conflicting marker")
            with self.assertRaisesRegex(workspace.BazelWorkspaceError, "ambiguous"):
                workspace._rapidyaml_version_header(include)

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
            with patch.object(workspace, "_INPUT_PROBE", observed.append):
                self.assertEqual(workspace._native_path(spelling, root), root / "payload")
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
        self.inventory = {"watch_files": [], "watch_directories": [], "links": [], "files": {}}
        self.invocations = []

        def attribute(**kwargs):
            return kwargs

        self.namespace = {
            "attr": SimpleNamespace(string=attribute, label=attribute, label_list=attribute),
            "Label": str,
            "repository_rule": attribute,
            "module_extension": attribute,
            "json": SimpleNamespace(encode=json.dumps, decode=json.loads),
            "fail": self.fail,
        }
        source = (workspace.PROJECT_ROOT / "bazel/dependencies.bzl").read_text(encoding="utf-8")
        exec(compile(ast.parse(source), "dependencies.bzl", "exec"), self.namespace)
        attributes = self.namespace["_governed_repository"]["attrs"]
        self.policy = attributes["policy"]["default"]
        self.resolver = attributes["resolver"]["default"]
        self.ctx = SimpleNamespace(
            getenv={"AOBUS_BAZEL_CONFIGURATION": str(self.root / "request.json"), "AOBUS_BAZEL_PYTHON": "python"}.get,
            path=self.path,
            watch=lambda path: self.watches.add(str(path)),
            attr=SimpleNamespace(resolver=self.resolver, policy=self.policy, group="tools"),
            # Match the real filesystem separators used by _RepositoryPath.
            os=SimpleNamespace(
                name="windows" if sys.platform == "win32" else sys.platform,
                environ={"UNRELATED": "not forwarded"},
            ),
            file=self.file,
            execute=self.execute,
            read=lambda path: Path(str(path)).read_text(encoding="utf-8"),
        )

    def path(self, value):
        value = str(value)
        if value.startswith("//"):
            path = workspace.PROJECT_ROOT / value[2:].replace(":", "/").lstrip("/")
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
        for name in ("TEMP", "TMP", "TMPDIR"):
            self.assertEqual(options["environment"][name], scratch)
        self.assertIsNone(options["environment"]["UNRELATED"])
        self.assertTrue(Path(scratch).is_dir())

    def test_shared_policy_retains_only_dependency_and_sdk_inputs(self):
        self.assertEqual(
            set(self.policy),
            {
                "//:config/bazel/dependencies.json",
                "//:dependency-contract.json",
                "//:nixpkgs.json",
                "//:vcpkg.json",
                "//:vcpkg-configuration.json",
                "//:script/ao/macos-toolchain.json",
            },
        )
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
        core = workspace.PROJECT_ROOT / "script/ao/core"
        self.assertFalse({str(path) for path in core.glob("*.py")} & self.watches)
        self.assertNotIn(str(core), self.directory_reads)


class LaunchConfigurationTest(unittest.TestCase):
    def test_windows_runtime_profile_is_explicit_in_both_launcher_and_rule(self):
        runtime = {"USERPROFILE", "LOCALAPPDATA", "APPDATA", "HOMEDRIVE", "HOMEPATH", "SYSTEMROOT", "SYSTEMDRIVE"}
        tree_ast = ast.parse((workspace.PROJECT_ROOT / "bazel/dependencies.bzl").read_text())
        names = next(
            ast.literal_eval(node.value)
            for node in tree_ast.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "_ENVIRONMENT" for target in node.targets)
        )
        self.assertLessEqual(runtime, set(names))
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
                patch.object(workspace, "native_platform", return_value="windows"),
                patch.object(workspace, "vcpkg_prefix", return_value=root / "provider"),
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
            self.assertNotIn("TEMP", names)
            self.assertNotIn("TMP", names)
            self.assertNotIn("TMPDIR", names)

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
                        "nixpkgsRevision": workspace._json(workspace.PROJECT_ROOT / "nixpkgs.json")["rev"],
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
                patch.object(workspace, "_nix_root", return_value=provider),
                patch.object(Path, "is_dir", lambda path: path == Path("/nix/store/catch2") or original_is_dir(path)),
                patch.object(workspace, "_executable", return_value="/fixture/pkgconf"),
                patch.object(
                    workspace.subprocess, "run", return_value=workspace.subprocess.CompletedProcess([], 1, "", "")
                ),
            ):
                base, _, _ = workspace._linux_provider(policy, identity_path=root / "identity.json")
            self.assertEqual(base["pkg_config"]["pc_dirs"], [str(pcdir)])

    def test_preparation_writes_configuration_without_resolving_or_hashing_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-launch-") as temporary:
            tree = Path(temporary)
            with (
                patch.object(workspace, "native_platform", return_value="linux"),
                patch.object(workspace, "_executable", side_effect=AssertionError("outer compiler probe")),
                patch.object(workspace, "refresh_dependencies", side_effect=AssertionError("outer dependency refresh")),
                patch.object(workspace, "_toolchain_identity", side_effect=AssertionError("outer toolchain hash")),
                patch.object(
                    workspace, "linux_hardening_environment", side_effect=AssertionError("outer toolchain policy")
                ),
            ):
                prepared = workspace.prepare_workspace(tree, "debug")
                request = json.loads((prepared / "request.json").read_text())
                rc = (prepared / "aobus.bazelrc").read_text()
                stamp = (prepared / "aobus.bazelrc").stat().st_mtime_ns
                self.assertEqual(workspace.prepare_workspace(tree, "debug"), prepared)
                self.assertEqual((prepared / "aobus.bazelrc").stat().st_mtime_ns, stamp)
            self.assertEqual(request["config"], "debug")
            self.assertIsNone(request["prefix"])
            self.assertIn("--repo_env=AOBUS_BAZEL_CONFIGURATION=", rc)
            self.assertIn("--repo_env=AOBUS_BAZEL_PYTHON=", rc)
            self.assertNotIn("--repo_env=CC=", rc)
            self.assertNotIn("--repo_env=CXX=", rc)
            self.assertNotIn("--batch", rc)
            self.assertFalse((tree / "generated").exists())


if __name__ == "__main__":
    unittest.main()
