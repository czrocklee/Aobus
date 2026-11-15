"""Linux GTK consumer SDK interfaces, provider provenance and resource-tool admission."""

import os
import subprocess
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_host_tools as host_tools
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers


class GtkSdkPolicyTest(unittest.TestCase):
    def test_gtk_and_process_imports_follow_their_actual_platform_consumers(self):
        policy = providers.load_policy()
        for host, expected in (
            ("linux", {"gtkmm", "boost_process"}),
            ("macos", {"boost_process"}),
            ("windows", {"boost_process"}),
        ):
            with self.subTest(host=host):
                names = {p["name"] for p in providers.package_groups(policy, host=host)["native"]}
                self.assertEqual(names & {"gtkmm", "boost_process"}, expected)

    def test_gtk_provider_keeps_transitive_source_identities_separate_from_gtkmm_outputs(self):
        with tempfile.TemporaryDirectory(prefix="aobus-gtk-inputs-") as temporary:
            root = Path(temporary).resolve()
            policy = providers.load_policy()
            gtkmm = next(p for p in policy["packages"]["native"] if p["name"] == "gtkmm")
            policy["packages"] = {"native": [gtkmm], "test": []}
            sources = [gtkmm["nix"], *gtkmm["nix_inputs"]]
            outputs = {name: root / name for name in sources}
            for output in outputs.values():
                (output / "lib/pkgconfig").mkdir(parents=True)
            report = root / "report.json"
            native.write_json(
                report,
                {
                    "schemaVersion": 1,
                    "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
                },
            )

            def dependency(report, source):
                return outputs[source], source + "-observed", "/nix/store/" + source + ".drv", [outputs[source]]

            def query(argv, **kwargs):
                self.assertEqual(argv[:2], ["/selected/pkg-config", "--variable=pcfiledir"])
                self.assertEqual(len(argv), 3)
                owners = {"gtkmm-4.0": "gtkmm", "cairo-ft": "cairo", "cairo-fc": "cairo"}
                self.assertIn(argv[-1], owners)
                return subprocess.CompletedProcess(argv, 0, str(outputs[owners[argv[-1]]] / "lib/pkgconfig"), "")

            with (
                patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
                patch.object(providers, "nix_dependency", side_effect=dependency),
                patch.object(
                    providers,
                    "nix_root",
                    side_effect=lambda path: next(output for output in outputs.values() if path.is_relative_to(output)),
                ),
                patch.object(native, "executable", side_effect=lambda name, **kwargs: "/selected/" + name),
                patch.object(subprocess, "run", side_effect=query),
            ):
                base, versions, expected = providers.linux_provider(policy, identity_path=root / "identity.json")
            identity = native.read_json(root / "identity.json")
            self.assertEqual(identity["packages"]["gtkmm"]["outputs"], [str(outputs["gtkmm"])])
            self.assertEqual(set(identity["inputs"]), set(sources) - {"gtkmm"})
            self.assertEqual(identity["inputs"]["gtk4"]["outputs"], [str(outputs["gtk4"])])
            self.assertEqual(identity["inputs"]["gtk4"]["deriver"], "/nix/store/gtk4.drv")
            self.assertEqual(versions, {"gtkmm": "gtkmm-observed"})
            self.assertEqual(set(expected["gtkmm"]), set(outputs.values()))
            self.assertLessEqual(set(map(str, outputs.values())), set(base["allowed_roots"]))


class GtkProjectionTest(unittest.TestCase):
    """Real refresh, metadata parsing and repository projection; only SDK I/O is fake."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-gtk-projection-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.gtkmm = self.root / "gtkmm"
        self.gtk = self.root / "gtk4"
        self.cairo = self.root / "cairo"
        self.fontconfig = self.root / "fontconfig"
        self.foreign = self.root / "unrelated-provider"
        for provider in (self.gtkmm, self.gtk, self.cairo, self.fontconfig, self.foreign):
            (provider / "include").mkdir(parents=True)
            (provider / "include/fixture.h").write_text("// SDK fixture\n")
            (provider / "lib/pkgconfig").mkdir(parents=True)
        self.pcdirs = [provider / "lib/pkgconfig" for provider in (self.gtkmm, self.gtk, self.cairo, self.fontconfig)]
        for provider, module, library in (
            (self.gtkmm, "gtkmm-4.0", "gtkmm-4.0"),
            (self.gtk, "gtk4", "gtk-4"),
            (self.cairo, "cairo-ft", "cairo"),
            (self.cairo, "cairo-fc", "cairo"),
            (self.fontconfig, "fontconfig", "fontconfig"),
        ):
            (provider / "lib/pkgconfig" / (module + ".pc")).write_text("Name: fixture\nVersion: fixture\n")
            (provider / "lib" / ("lib" + library + ".so")).write_bytes(b"shared SDK fixture")
        self.resolver = self.root / "resolver"
        self.resolver.write_bytes(b"fixture tool, not executed")
        self.resolver.chmod(0o755)
        policy = providers.load_policy()
        policy["packages"] = {"native": [p for p in policy["packages"]["native"] if p["name"] == "gtkmm"], "test": []}
        self.policy = self.root / "policy.json"
        native.write_json(self.policy, policy)
        self.observed = "4.99.1"
        self.metadata_version = self.observed
        self.cairo_observed = "1.99.2"
        self.cairo_metadata_version = self.cairo_observed
        self.flags = "-mfpmath=sse -msse -msse2"
        self.header_provider = self.gtk

    def provider(self, policy, *, identity_path):
        native.write_json(
            identity_path,
            {
                "packages": {"gtkmm": {"outputs": [str(self.gtkmm)]}},
                "inputs": {"cairo": {"version": self.cairo_observed}},
            },
        )
        return (
            {
                "provider": {"name": "nix", "version": "fixture", "evidence": []},
                "allowed_roots": list(map(str, (self.gtkmm, self.gtk, self.cairo, self.fontconfig, self.foreign))),
                "library_dirs": [
                    str(provider / "lib") for provider in (self.gtkmm, self.gtk, self.cairo, self.fontconfig)
                ],
                "pkg_config": {"executable": str(self.resolver), "pc_dirs": list(map(str, self.pcdirs))},
                "elf": {"readelf": str(self.resolver)},
            },
            {"gtkmm": self.observed},
            {"gtkmm": [self.gtkmm, self.gtk, self.cairo, self.fontconfig]},
        )

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.resolver))
        if argv[1] == "-d":
            self.assertEqual(len(argv), 3)
            answer = "(SONAME) [" + Path(argv[-1]).name + "]"
        elif argv[1] in {"--modversion", "--variable=pcfiledir", "--print-requires"}:
            self.assertEqual(len(argv), 3)
            owners = {
                "gtkmm-4.0": self.gtkmm,
                "gtk4": self.gtk,
                "cairo-ft": self.cairo,
                "cairo-fc": self.cairo,
                "fontconfig": self.fontconfig,
            }
            versions = {
                "gtkmm-4.0": self.metadata_version,
                "gtk4": "4.98.0",
                "cairo-ft": self.cairo_metadata_version,
                "cairo-fc": self.cairo_metadata_version,
                "fontconfig": "2.99.0",
            }
            self.assertIn(argv[-1], owners)
            if argv[1] == "--modversion":
                answer = versions[argv[-1]]
            elif argv[1] == "--variable=pcfiledir":
                answer = str(owners[argv[-1]] / "lib/pkgconfig")
            else:
                answer = {"gtkmm-4.0": "gtk4 >= 4.0", "cairo-fc": "fontconfig >= 2.13.0"}.get(argv[-1], "")
        elif argv == [str(self.resolver), "--cflags", "gtkmm-4.0", "cairo-ft", "cairo-fc"]:
            roots = (self.gtkmm, self.header_provider, self.cairo, self.fontconfig)
            answer = self.flags + " " + " ".join(f'-I"{root / "include"}"' for root in roots)
        elif argv == [str(self.resolver), "--libs", "gtkmm-4.0", "cairo-ft", "cairo-fc"]:
            answer = "-lgtkmm-4.0 -lgtk-4 -lcairo -lfontconfig"
        else:
            raise AssertionError(f"unexpected GTK SDK query: {argv}")
        return subprocess.CompletedProcess(argv, 0, answer, "")

    def refresh(self):
        def nix_root(path):
            return next(
                root
                for root in (self.gtkmm, self.gtk, self.cairo, self.fontconfig, self.foreign)
                if path.is_relative_to(root)
            )

        with (
            patch.object(providers, "POLICY", self.policy),
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(providers, "linux_provider", side_effect=self.provider),
            patch.object(providers, "nix_root", side_effect=nix_root),
            patch.object(subprocess, "run", side_effect=self.query),
        ):
            return dependencies.resolve_dependencies(self.root / "resolution", "debug", group="native")

    def test_metadata_projects_the_observed_gtkmm_version_and_rendering_closure(self):
        repository = self.refresh()
        manifest = repository["manifest"]
        package = manifest["packages"][0]
        self.assertEqual(manifest["modules"]["gtkmm-4.0"]["version"], self.observed)
        self.assertEqual(manifest["modules"]["cairo-ft"]["version"], self.cairo_observed)
        self.assertEqual(manifest["modules"]["cairo-fc"]["version"], self.cairo_observed)
        self.assertEqual(
            [item["name"] for item in package["resolved_libraries"]], ["gtkmm-4.0", "gtk-4", "cairo", "fontconfig"]
        )
        self.assertIn(str(self.gtk / "include"), package["interface"]["header_dirs"])
        self.assertIn(str(self.fontconfig / "include"), package["interface"]["header_dirs"])
        normalized = repository["packages"][0]
        self.assertNotIn("copts", normalized)
        self.assertEqual(
            [library["source"] for library in normalized["libraries"]],
            [
                str(self.gtkmm / "lib/libgtkmm-4.0.so"),
                str(self.gtk / "lib/libgtk-4.so"),
                str(self.cairo / "lib/libcairo.so"),
                str(self.fontconfig / "lib/libfontconfig.so"),
            ],
        )


class BoostProcessSdkTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-boost-process-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.provider = self.root / "boost"
        include = self.provider / "include"
        for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp", "detail/config.hpp"):
            path = include / "boost/process/v2" / marker
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// installed Process v2 API\n")
        (include / "boost/process/v2/detail/config.hpp").write_text(
            "#define BOOST_PROCESS_V2_DECL\n#define BOOST_PROCESS_DYN_LINK\nnamespace filesystem = boost::filesystem;\n"
        )
        self.header = include / "boost/version.hpp"
        self.header.write_text("#define BOOST_VERSION 109100\n")
        filesystem = include / "boost/filesystem/path.hpp"
        filesystem.parent.mkdir()
        filesystem.write_text("// compiled filesystem API\n")
        (self.provider / "lib/pkgconfig").mkdir(parents=True)
        (self.provider / "lib/pkgconfig/boost.pc").write_text("Name: boost\nVersion: 1.91.0\n")
        for name in ("boost_process", "boost_filesystem"):
            (self.provider / "lib" / ("lib" + name + ".so")).write_bytes(b"shared SDK fixture")
        self.readelf = self.root / "readelf"
        self.readelf.write_bytes(b"ELF query fixture, not executed")
        self.readelf.chmod(0o755)
        self.version = "1.91.0"
        self.library_version = self.version
        self.symbol = "boost::process::v2::process_environment::do_setup("
        self.spec = {
            "platform": "linux",
            "provider": {"name": "nix"},
            "linkage": "shared",
            "elf": {"readelf": str(self.readelf)},
        }

    def query(self, argv, **kwargs):
        self.assertEqual(argv[0], str(self.readelf))
        self.assertIn(argv[1:-1], (["--dynamic", "--dyn-syms", "--wide", "--demangle"], ["-d"]))
        name = Path(argv[-1]).name.removeprefix("lib").removesuffix(".so")
        self.assertIn(name, {"boost_process", "boost_filesystem"})
        symbol = self.symbol if name == "boost_process" else "boost::filesystem::path::codecvt()"
        answer = f"(SONAME) [lib{name}.so.{self.library_version}]\n 1: 000001 1 FUNC GLOBAL DEFAULT 12 {symbol}\n"
        return subprocess.CompletedProcess(argv, 0, answer, "")

    def interface(self):
        with patch.object(subprocess, "run", side_effect=self.query):
            return dependencies._linux_boost_process_interface(self.spec, [self.provider], self.version)

    def test_installed_process_v2_exports_its_shared_filesystem_consumer_abi(self):
        interface = self.interface()
        self.assertEqual([item["name"] for item in interface["libraries"]], ["boost_process", "boost_filesystem"])
        self.assertEqual(
            set(interface["defines"]),
            {"BOOST_PROCESS_NO_LIB", "BOOST_PROCESS_DYN_LINK", "BOOST_FILESYSTEM_NO_LIB", "BOOST_FILESYSTEM_DYN_LINK"},
        )
        self.assertEqual(interface["header_dirs"], [str(self.provider / "include")])
        # Future coherent provider observations pass without a Bazel version edit.
        self.version = self.library_version = "1.93.0"
        self.header.write_text("#define BOOST_VERSION 109300\n")
        self.interface()

    def test_refresh_does_not_treat_the_header_only_boost_pc_as_process_metadata(self):
        policy = providers.load_policy()
        policy["packages"] = {
            "native": [p for p in policy["packages"]["native"] if p["name"] == "boost_process"],
            "test": [],
        }
        policy_path = self.root / "policy.json"
        native.write_json(policy_path, policy)

        def provider(policy, *, identity_path):
            native.write_json(identity_path, {"packages": {"boost_process": {"outputs": [str(self.provider)]}}})
            return (
                {
                    "provider": {"name": "nix", "version": "fixture", "evidence": []},
                    "allowed_roots": [str(self.provider)],
                    "library_dirs": [str(self.provider / "lib")],
                    "pkg_config": {"executable": str(self.readelf), "pc_dirs": [str(self.provider / "lib/pkgconfig")]},
                    "elf": self.spec["elf"],
                },
                {"boost_process": self.version},
                {"boost_process": [self.provider]},
            )

        with (
            patch.object(providers, "POLICY", policy_path),
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(providers, "linux_provider", side_effect=provider),
            patch.object(providers, "nix_root", return_value=self.provider),
            patch.object(subprocess, "run", side_effect=self.query),
        ):
            resolution = dependencies.resolve_dependencies(self.root / "resolution", "debug", group="native")
        manifest = resolution["manifest"]
        self.assertEqual(manifest["packages"][0]["origin"], "manual_fallback")
        self.assertEqual(
            [item["name"] for item in manifest["packages"][0]["resolved_libraries"]],
            ["boost_process", "boost_filesystem"],
        )


class GResourceSdkTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-glib-resource-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.dev, self.out = self.root / "glib-dev", self.root / "glib-out"
        self.compiler = self.dev / "bin/glib-compile-resources"
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_bytes(b"resource compiler fixture, never executed")
        self.compiler.chmod(0o755)
        (self.out / "lib").mkdir(parents=True)
        self.runtime = []
        for name in ("gio", "glib", "gobject"):
            path = self.out / "lib" / f"lib{name}-2.0.so.0.9900.1"
            path.write_bytes(b"GLib runtime fixture")
            self.runtime.append(path)
        self.aliases = {Path("/nix/store/fixture-glib-dev"): self.dev, Path("/nix/store/fixture-glib-out"): self.out}
        self.report_path = self.root / "report.json"
        self.pin = native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"]
        self.report = {
            "schemaVersion": 1,
            "nixpkgsRevision": self.pin,
            "dependencies": {
                "glib": {
                    "version": "2.99.1",
                    "storePath": "/nix/store/fixture-glib-out",
                    "drvPath": "/nix/store/fixture-glib.drv",
                    "outputs": {"dev": "/nix/store/fixture-glib-dev", "out": "/nix/store/fixture-glib-out"},
                }
            },
        }
        self.tool_version = "2.99.1\n"
        self.tool_status = 0
        self.escape = None
        self.queries = []

    def query(self, argv, **kwargs):
        self.assertEqual(argv, [str(self.compiler), "--version"])
        self.assertNotIn("CMAKE_PREFIX_PATH", kwargs["env"])
        self.assertFalse(any(name.startswith("NIX_CFLAGS_") for name in kwargs["env"]))
        self.queries.append(argv)
        return subprocess.CompletedProcess(argv, self.tool_status, self.tool_version, "")

    @contextmanager
    def sdk(self):
        native.write_json(self.report_path, self.report)
        original_is_dir = Path.is_dir

        def nix_root(path):
            if path in self.aliases:
                return self.aliases[path]
            if path == self.escape:
                return self.root / "foreign-output"
            return next(output for output in self.aliases.values() if path.is_relative_to(output))

        with (
            patch.dict(
                os.environ,
                {
                    "AOBUS_NIX_DEPENDENCY_REPORT": str(self.report_path),
                    "CMAKE_PREFIX_PATH": "/ambient",
                    "NIX_CFLAGS_COMPILE": "-I/ambient",
                },
            ),
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(providers, "nix_root", side_effect=nix_root),
            patch.object(Path, "is_dir", lambda path: path in self.aliases or original_is_dir(path)),
            patch.object(subprocess, "run", side_effect=self.query),
        ):
            yield

    def test_report_owned_resource_compiler_declares_glib_runtime_and_provider_evidence(self):
        with self.sdk():
            identity, compiler, runtime, evidence = host_tools._linux_gresource()
        self.assertEqual(compiler, self.compiler)
        self.assertEqual(set(runtime), set(self.runtime))
        self.assertEqual(identity["version"], "2.99.1")
        self.assertEqual(identity["deriver"], "/nix/store/fixture-glib.drv")
        self.assertEqual(set(identity["outputs"]), {str(self.dev), str(self.out)})
        self.assertEqual(evidence, [self.report_path, native.PROJECT_ROOT / "nixpkgs.json"])

    def test_a_second_tool_or_missing_adjacent_runtime_never_uses_path_fallback(self):
        second = self.out / "bin/glib-compile-resources"
        second.parent.mkdir()
        second.write_bytes(b"ambiguous compiler fixture")
        with (
            self.sdk(),
            self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous glib-compile-resources"),
        ):
            host_tools._linux_gresource()
        second.unlink()
        self.runtime[0].unlink()
        with (
            self.sdk(),
            self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous resource compiler GLib runtime"),
        ):
            host_tools._linux_gresource()


if __name__ == "__main__":
    unittest.main()
