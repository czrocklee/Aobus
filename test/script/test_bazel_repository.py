"""Repository-owned projection, stable paths and prepared-mode contracts."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
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


class ReadInventoryTest(unittest.TestCase):
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
                self.assertEqual(native.native_path(spelling), root / "payload")
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
                    "VSINSTALLDIR": "C:/selected-vs/",
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
                    "test": [{"name": "catch2", "nix": "catch2", "modules": ["catch2-with-main"], "vcpkg": ["catch2"]}],
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
