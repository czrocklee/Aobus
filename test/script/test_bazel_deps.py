"""Contracts for governed metadata and unmaterialized Bazel repository plans."""

import copy
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import bazel_dependencies as dependencies
from ao.portal import bazel_deps as g


class BazelDependencyTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.store = self.root / "nix/store"
        self.provider = self.store / f"{'0' * 32}-fixture-dev"
        for directory in ("include", "lib/pkgconfig", "bin", "share"):
            (self.provider / directory).mkdir(parents=True, exist_ok=True)
        self.header = self.provider / "include/foo.h"
        self.header.write_bytes(b"#define FOO 1\n")
        self.lib = self.provider / "lib/libfoo.a"
        self.lib.write_bytes(b"archive-v1")
        self.pcfile = self.provider / "lib/pkgconfig/foo.pc"
        self.pc = self.provider / "bin/pkg-config-fixture"
        self.pc.write_bytes(b"resolver fixture, never executed")
        self.pc.chmod(0o755)
        self.readelf = self.provider / "bin/readelf-fixture"
        self.readelf.write_bytes(b"ELF inspector fixture, never executed")
        self.readelf.chmod(0o755)
        self.soname = "libfoo.so.42"
        self.dynamic = ""
        self.nix_runtime_dirs = [f"/nix/store/{digit * 32}-fixture-runtime-{digit}/lib" for digit in ("1", "2")]
        self.metadata = {"foo": {"version": "1.2.3", "requires": [], "private": []}}
        self.cflags = f'-I"{self.header.parent}" -DFOO_API=1'
        self.libs = f'-L"{self.lib.parent}" -lfoo'
        self.write_pc()
        self.spec = {
            "schema": 1,
            "platform": "linux",
            "config": "Release",
            "linkage": "static",
            "provider": {"name": "fixture", "version": "contract-v1"},
            "allowed_roots": [str(self.provider)],
            "library_dirs": [str(self.lib.parent)],
            "system_libraries": ["m"],
            "pkg_config": {"executable": str(self.pc), "pc_dirs": [str(self.pcfile.parent)]},
            "packages": [{"name": "foo", "modules": ["foo"], "expected_versions": {"foo": "1.2.3"}}],
        }
        self.patches = {"schema": 1, "patches": []}
        self.out = self.root / "out"
        patcher = mock.patch.object(g.subprocess, "run", side_effect=self.query)
        self.process = patcher.start()
        self.addCleanup(patcher.stop)

    def write_pc(self):
        self.pcfile.write_text(
            f"prefix={self.provider.as_posix()}\nName: foo\nDescription: fixture\nVersion: 1.2.3\n"
            f"Cflags: {self.cflags}\nLibs: {self.libs}\n",
            encoding="utf-8",
        )

    def query(self, argv, **kwargs):
        executable, *args = argv
        if executable == str(self.readelf):
            output = f"0x000 (SONAME) Library soname: [{self.soname}]" if self.soname else "no soname"
            output += "\n" + self.dynamic
        else:
            self.assertEqual(executable, str(self.pc))
            args = [a for a in args if a != "--static"]
            query, module = args[:2]
            if query == "--cflags":
                output = self.cflags
            elif query == "--libs":
                output = self.libs
            elif query == "--variable=pcfiledir":
                output = str(self.pcfile.parent)
            else:
                metadata = self.metadata[module]
                if query == "--modversion":
                    output = metadata["version"]
                elif query == "--print-requires":
                    output = "\n".join(metadata["requires"])
                elif query == "--print-requires-private":
                    output = "\n".join(metadata["private"])
                else:
                    self.fail(f"unexpected query: {argv}")
        return subprocess.CompletedProcess(argv, 0, output, "")

    def generate(self):
        adapter = g._Adapter(self.spec, self.patches)
        adapter.prepare()
        self.resolution = adapter.resolve()
        return self.resolution["manifest"]

    def package(self, name="foo"):
        return next(package for package in self.resolution["packages"] if package["name"] == name)

    def artifact(self, path):
        for package in self.resolution["packages"]:
            for tree in package["headers"]:
                for file in tree["files"]:
                    if Path(tree["root"]) / file["path"] == path:
                        return Path(file["source"])
        raise AssertionError(f"Header observation missing: {path}")

    def patch(self, add, *, missing=None, present=None, requirements=None, remove=None):
        requirements = requirements or []
        patch = {
            "id": "fixture-fix",
            "when": {
                "package": "foo",
                "platform": self.spec["platform"],
                "config": self.spec["config"],
                "provider": "fixture",
                "provider_version": "contract-v1",
            },
            "expect": {"missing": missing or {}, "present": present or {}, "unmet_requirements": requirements},
            "add": add,
            "satisfy": requirements,
        }
        if remove is not None:
            patch["remove"] = remove
        self.patches["patches"].append(patch)
        return patch

    def manual(self, interface=None):
        self.spec["packages"][0] = {"name": "foo", "modules": []}
        self.spec.pop("pkg_config", None)
        self.patch(
            interface
            if interface is not None
            else {
                "header_dirs": [str(self.header.parent)],
                "libraries": [{"name": "foo", "kind": self.spec["linkage"], "file": str(self.lib)}],
            },
            requirements=["manual_interface"],
        )

    def shared(self):
        self.spec["linkage"] = "shared"
        self.lib = self.lib.with_name("libfoo.so.42")
        self.lib.write_bytes(b"fake ELF")
        self.spec["elf"] = {"readelf": str(self.readelf)}

    def generate_with_runtime_directories(self, *directories, resolutions=None):
        # Fake only native runtime-directory I/O; fixtures never write into /nix/store.
        existing = {Path(value) for value in directories}
        resolutions = resolutions or {}
        original_resolve, original_is_dir = g._resolve, Path.is_dir

        def resolve(path, *, strict=True):
            value = path.as_posix()
            if value.startswith("/nix/store/"):
                return Path(resolutions.get(value, value))
            return original_resolve(path, strict=strict)

        def is_dir(path):
            return path in existing if path.as_posix().startswith("/nix/store/") else original_is_dir(path)

        with mock.patch.object(g, "_resolve", side_effect=resolve), mock.patch.object(Path, "is_dir", is_dir):
            return self.generate()

    def metadata_file(self, name, *, requires=(), private=()):
        path = self.pcfile.with_name(name + ".pc")
        path.write_text(f"Name: {name}\nDescription: dependency\nVersion: 2\n", encoding="utf-8")
        self.metadata[name] = {"version": "2", "requires": list(requires), "private": list(private)}
        return path

    def include_root(self, package="foo", index=0):
        observation = self.package(package)
        include = observation["includes"][index]
        return Path(observation["headers"][include["tree"]]["root"]) / include["path"]

    def snapshot(self):
        return {
            p.relative_to(self.root).as_posix(): p.read_bytes()
            for p in self.root.rglob("*")
            if p.is_file() and not p.is_relative_to(self.provider)
        }

    def symlink(self, source, target, *, directory=False):
        try:
            source.symlink_to(target, target_is_directory=directory)
        except OSError as exc:
            self.skipTest(f"host lacks symlink privileges: {exc}")

    def test_allowed_root_membership_includes_file_roots_and_excludes_siblings(self):
        sibling = self.lib.parent.with_name(self.lib.parent.name + "-sibling")
        sibling.mkdir()
        outside = sibling / "outside.h"
        outside.write_bytes(b"outside")
        self.spec["allowed_roots"] = [str(self.header), str(self.lib.parent)]
        self.spec.pop("pkg_config")
        adapter = g._Adapter(self.spec, self.patches)
        self.assertEqual(adapter.path(str(self.header)), self.header)
        self.assertEqual(adapter.path(str(self.lib)), self.lib)
        with self.assertRaisesRegex(g.BazelDependencyError, "outside allowed provider roots"):
            adapter.path(str(outside))

    def test_pc_projection_declares_headers_imports_macros_and_no_broad_flags(self):
        manifest = self.generate()
        self.assertEqual(manifest["counts"], {"automatic": 1, "manual_fallback": 0})
        self.assertEqual(manifest["modules"]["foo"]["version"], "1.2.3")
        self.assertEqual(manifest["materialization"], "repository")
        self.assertFalse(self.out.exists())
        self.assertNotIn("targets", manifest)
        self.assertEqual(
            self.package(),
            {
                "name": "foo",
                "headers": [
                    {
                        "root": str(self.header.parent),
                        "index": 0,
                        "files": [{"source": str(self.header), "path": "foo.h"}],
                    }
                ],
                "includes": [{"tree": 0, "path": "."}],
                "defines": ["FOO_API=1"],
                "linkopts": [],
                "libraries": [
                    {"index": 0, "kind": "static", "source": str(self.lib), "basename": "libfoo.a", "runtime": None}
                ],
                "runtime": [],
            },
        )
        self.assertNotIn("tools", manifest["packages"][0]["interface"])
        self.assertEqual(
            {role for entry in manifest["inputs"] for role in entry["roles"]},
            {"resolver_tool", "pkg_config_metadata", "library", "header"},
        )

    def test_group_can_mix_shared_metadata_and_complete_static_fallback(self):
        self.shared()
        static = self.provider / "lib/libryml.a"
        static.write_bytes(b"static YAML archive")
        self.spec["packages"].append({"name": "rapidyaml", "modules": [], "linkage": "static"})
        patch = self.patch(
            {
                "header_dirs": [str(self.header.parent)],
                "libraries": [{"name": "ryml", "kind": "static", "file": str(static)}],
            },
            requirements=["manual_interface"],
        )
        patch["when"]["package"] = "rapidyaml"
        manifest = self.generate()
        self.assertEqual(manifest["counts"], {"automatic": 1, "manual_fallback": 1})
        self.assertEqual([p["linkage"] for p in manifest["packages"]], ["shared", "static"])
        self.assertEqual(self.package()["libraries"][0]["source"], str(self.lib))
        self.assertEqual(self.package("rapidyaml")["libraries"][0]["source"], str(static))
        self.assertEqual(self.package("rapidyaml")["libraries"][0]["kind"], "static")
        self.spec["packages"][1]["linkage"] = "unknown"
        with self.assertRaisesRegex(g.BazelDependencyError, "unsupported package linkage"):
            self.generate()

    def test_duplicate_metadata_in_isolated_group_fails_instead_of_search_order_selection(self):
        other = self.provider / "other-pc"
        other.mkdir()
        (other / "foo.pc").write_bytes(self.pcfile.read_bytes())
        self.spec["pkg_config"]["pc_dirs"].append(str(other))
        with self.assertRaisesRegex(g.BazelDependencyError, "ambiguous pkg-config metadata"):
            self.generate()

    def test_planning_rehashes_same_path_changed_header_and_library(self):
        first = self.generate()
        self.header.write_bytes(b"#define FOO 2\n")
        self.lib.write_bytes(b"archive-v2")
        changed = self.generate()
        before = {x["path"]: x["sha256"] for x in first["inputs"]}
        after = {x["path"]: x["sha256"] for x in changed["inputs"]}
        for source in (self.header, self.lib):
            self.assertNotEqual(before[str(source)], after[str(source)])
            self.assertEqual(after[str(source)], hashlib.sha256(source.read_bytes()).hexdigest())
        self.assertEqual(self.artifact(self.include_root() / "foo.h"), self.header)
        self.assertEqual(self.package()["libraries"][0]["source"], str(self.lib))

    def test_nested_include_roots_preserve_relative_sibling_headers_and_search_order(self):
        include = self.header.parent
        nested = include / "ryml"
        nested.mkdir()
        sibling = include / "c4/yml/std/std.hpp"
        sibling.parent.mkdir(parents=True)
        sibling.write_text("// sibling header\n", encoding="utf-8")
        (nested / "ryml_std.hpp").write_text('#include "../c4/yml/std/std.hpp"\n', encoding="utf-8")
        for nested_first in (False, True):
            with self.subTest(nested_first=nested_first):
                roots = [nested, include] if nested_first else [include, nested]
                self.cflags = " ".join(f'-I"{root}"' for root in roots)
                self.write_pc()
                manifest = self.generate()
                self.assertEqual(
                    self.package()["includes"],
                    [{"tree": 0, "path": "ryml"}, {"tree": 0, "path": "."}]
                    if nested_first
                    else [{"tree": 0, "path": "."}, {"tree": 0, "path": "ryml"}],
                )
                self.assertEqual(self.package()["headers"][0]["index"], 1 if nested_first else 0)
                self.assertEqual(self.package()["headers"][0]["root"], str(include))
                self.assertEqual(self.artifact(include / "c4/yml/std/std.hpp"), sibling)
                self.assertEqual(self.artifact(include / "ryml/ryml_std.hpp"), nested / "ryml_std.hpp")
                self.assertEqual(manifest["packages"][0]["declared_headers"], 3)

    def test_added_and_removed_headers_invalidate_output_membership(self):
        self.generate()
        extra = self.header.with_name("extra.h")
        extra.write_bytes(b"// extra\n")
        self.generate()
        self.assertEqual(self.artifact(self.include_root() / "extra.h"), extra)
        extra.unlink()
        self.generate()
        self.assertEqual(self.package()["headers"][0]["files"], [{"source": str(self.header), "path": "foo.h"}])

    def test_unknown_and_misclassified_flags_are_rejected(self):
        for cflags, libs in (
            ("-funknown", ""),
            ("", "-Wl,-rpath,/ambient"),
            ("-L/tmp", ""),
            ("", "-I/tmp"),
            ("", "-Wl,--start-group"),
            ("", "-lunknown_system"),
            ("-I", ""),
            ('-D"unterminated', ""),
        ):
            old_cflags, old_libs = self.cflags, self.libs
            self.cflags += " " + cflags
            self.libs += " " + libs
            with self.subTest(cflags=cflags, libs=libs), self.assertRaises(g.BazelDependencyError):
                self.generate()
            self.cflags, self.libs = old_cflags, old_libs

    def test_split_flags_are_parsed_but_unsatisfied_compile_options_fail_closed(self):
        self.cflags = f'-isystem "{self.header.parent}" -D FOO_API=1 -mfpmath=sse -msse -msse2 -pthread -fPIC'
        self.libs = f'-L "{self.lib.parent}" -l foo'
        adapter = g._Adapter(self.spec, self.patches)
        adapter.prepare()
        interface = adapter.packages[0][1]
        self.assertEqual(interface.header_dirs, [str(self.header.parent)])
        self.assertEqual(interface.defines, ["FOO_API=1"])
        self.assertEqual(interface.copts, ["-mfpmath=sse", "-msse", "-msse2", "-pthread", "-fPIC"])
        with self.assertRaisesRegex(g.BazelDependencyError, "unsatisfied compile requirements"):
            adapter.resolve()

    def test_pipewire_spa_requirements_are_recorded_for_explicit_consumer_application(self):
        self.metadata_file("libspa-0.2")
        self.metadata["foo"]["requires"] = ["libspa-0.2"]
        self.cflags += " -D_REENTRANT -fno-strict-aliasing -fno-strict-overflow"
        self.spec["packages"][0]["name"] = "pipewire"
        result = self.generate()
        self.assertIn("libspa-0.2", result["modules"])
        self.assertIn("_REENTRANT", result["packages"][0]["interface"]["defines"])
        self.assertEqual(result["packages"][0]["interface"]["copts"], ["-fno-strict-aliasing", "-fno-strict-overflow"])
        self.assertNotIn("copts", self.package("pipewire"))
        for flag in ("-fstrict-aliasing", "-fno-unknown"):
            self.cflags += " " + flag
            with self.subTest(flag=flag), self.assertRaisesRegex(g.BazelDependencyError, "unsupported compile flag"):
                self.generate()
            self.cflags = self.cflags.removesuffix(" " + flag)

    def test_static_private_metadata_is_recorded(self):
        dep = self.metadata_file("private")
        self.metadata["foo"]["private"] = ["private >= 2"]
        manifest = self.generate()
        self.assertEqual(manifest["modules"]["private"]["version"], "2")
        self.assertIn(str(dep), [i["path"] for i in manifest["inputs"]])
        self.assertTrue(
            any("--static" in call.args[0] and "--libs" in call.args[0] for call in self.process.call_args_list)
        )

    def test_shared_private_cflags_metadata_and_unreachable_inventory_are_recorded(self):
        self.shared()
        private = self.metadata_file("private", private=["missing-static-only"])
        unused = self.metadata_file("unused")
        self.metadata["foo"]["private"] = ["private >= 2", "missing-static-only"]
        self.cflags += " -DPRIVATE_API=1"
        first = self.generate()
        self.assertIn("PRIVATE_API=1", first["packages"][0]["interface"]["defines"])
        recorded = {i["path"]: i["sha256"] for i in first["inputs"]}
        self.assertIn(str(private), recorded)
        self.assertIn(str(unused), recorded)
        self.assertNotIn("missing-static-only", first["modules"])
        self.assertFalse(any("--static" in call.args[0] for call in self.process.call_args_list))
        private.write_bytes(b"changed private compile metadata")
        second = self.generate()
        self.assertNotEqual(
            recorded[str(private)], next(i["sha256"] for i in second["inputs"] if i["path"] == str(private))
        )

    def test_shared_resolver_rejection_fails_without_writing_repository_files(self):
        self.shared()
        self.generate()
        before = self.snapshot()
        with self.pcfile.open("a", encoding="utf-8") as stream:
            stream.write("Requires.private: missing-static-only\n")
        original = self.query

        def query(argv, **kwargs):
            if "--cflags" in argv:
                return subprocess.CompletedProcess(argv, 1, "", "missing-static-only not found")
            return original(argv, **kwargs)

        self.process.side_effect = query
        with self.assertRaisesRegex(g.BazelDependencyError, "external query failed"):
            self.generate()
        self.assertEqual(before, self.snapshot())

    def test_private_requirement_variables_are_bounded_without_static_queries(self):
        self.shared()
        with self.pcfile.open("a", encoding="utf-8") as stream:
            stream.write("api=2.0\nprivate=missing-${api}\nRequires.private: ${private} >= 1, \\\n other\n")
        self.generate()
        self.assertFalse(any("--print-requires-private" in call.args[0] for call in self.process.call_args_list))
        for variables in ("private=/outside/private.pc", "private=${private}", "private=${unknown}"):
            self.write_pc()
            with self.pcfile.open("a", encoding="utf-8") as stream:
                stream.write(variables + "\nRequires.private: ${private}\n")
            with (
                self.subTest(variables=variables),
                self.assertRaisesRegex(g.BazelDependencyError, "unsupported pkg-config requirement"),
            ):
                self.generate()

    def test_resolver_environment_is_isolated_and_never_uses_shell(self):
        with mock.patch.dict(
            os.environ,
            {
                "PATH": "/ambient-tools",
                "PKG_CONFIG_PATH": "/ambient",
                "PKG_CONFIG_LIBDIR": "/ambient",
                "CFLAGS": "-bad",
            },
        ):
            self.generate()
        for call in self.process.call_args_list:
            self.assertEqual(call.kwargs["env"]["PATH"], os.defpath)
            self.assertEqual(call.kwargs["env"]["PKG_CONFIG_PATH"], "")
            self.assertEqual(call.kwargs["env"]["PKG_CONFIG_LIBDIR"], str(self.pcfile.parent))
            self.assertNotIn("CFLAGS", call.kwargs["env"])
            self.assertNotIn("shell", call.kwargs)
            self.assertEqual(call.kwargs["timeout"], 60)

    def test_raw_metadata_and_adapter_share_installed_only_environment_baseline(self):
        self.generate()
        strict = self.process.call_args_list[0].kwargs["env"]
        self.assertEqual(strict["PKG_CONFIG_DISABLE_UNINSTALLED"], "1")
        self.process.reset_mock()
        self.assertEqual(
            dependencies._metadata_flags(self.spec, "--cflags"),
            [f"-I{self.header.parent}", "-DFOO_API=1"],
        )
        raw_call = self.process.call_args
        self.assertEqual(raw_call.args, ([str(self.pc), "--static", "--cflags", "foo"],))
        self.assertEqual(
            {key: value for key, value in raw_call.kwargs.items() if key != "env"},
            {"capture_output": True, "text": True, "check": True},
        )
        raw = raw_call.kwargs["env"]
        # The raw repair observation and the strict adapter share one
        # installed-only baseline; no consumer keeps an extra independent
        # uninstalled-modules guard.
        self.assertEqual(raw["PKG_CONFIG_DISABLE_UNINSTALLED"], "1")
        self.assertEqual(raw, strict)

    def test_explicit_resolver_symlink_preserves_invocation_path_and_hashes_target(self):
        target = self.pc
        alias = target.with_name("pkgconf-alias")
        try:
            alias.symlink_to(target)
        except OSError:
            if os.name != "nt":
                raise
            # Windows may lack symlink privileges; emulate only resolution while
            # retaining the real file/executable checks and mocked subprocess.
            original_resolve = Path.resolve

            def resolve(path, *args, **kwargs):
                if path == alias:
                    return target
                return original_resolve(path, *args, **kwargs)

            patcher = mock.patch.object(Path, "resolve", resolve)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.pc = alias
        self.spec["pkg_config"]["executable"] = str(alias)
        manifest = self.generate()
        self.assertTrue(self.process.call_args_list)
        for call in self.process.call_args_list:
            self.assertEqual(call.args[0][0], str(alias))
        tools = [entry for entry in manifest["inputs"] if "resolver_tool" in entry["roles"]]
        self.assertEqual([entry["path"] for entry in tools], [str(target)])
        self.assertEqual(tools[0]["sha256"], hashlib.sha256(target.read_bytes()).hexdigest())

    def test_external_query_failure_and_timeout_are_public_errors(self):
        for outcome in (
            subprocess.CompletedProcess([], 1, "", "bad metadata"),
            subprocess.TimeoutExpired("fixture", 60),
            FileNotFoundError("resolver vanished"),
        ):
            if isinstance(outcome, Exception):
                self.process.side_effect = outcome
            else:
                self.process.side_effect = None
                self.process.return_value = outcome
            with self.subTest(outcome=outcome), self.assertRaises(g.BazelDependencyError):
                self.generate()

    def test_system_only_linking_is_explicit_and_provider_ambiguity_fails(self):
        self.libs = "-lm"
        manifest = self.generate()
        self.assertEqual(manifest["packages"][0]["resolved_libraries"], [{"name": "m", "kind": "system"}])
        self.assertEqual(self.package()["libraries"], [])
        self.assertEqual(self.package()["linkopts"], ["-lm"])
        (self.lib.parent / "libm.a").write_bytes(b"local m")
        with self.assertRaisesRegex(g.BazelDependencyError, "system/provider library ambiguity"):
            self.generate()

    def test_library_ambiguity_missing_and_explicit_selection(self):
        second = self.provider / "other"
        second.mkdir()
        other = second / "libfoo.a"
        other.write_bytes(b"different")
        self.spec["library_dirs"].append(str(second))
        with self.assertRaisesRegex(g.BazelDependencyError, "library resolution missing/ambiguous"):
            self.generate()
        self.manual()
        self.generate()
        self.lib.unlink()
        with self.assertRaisesRegex(g.BazelDependencyError, "missing file"):
            self.generate()
        self.assertTrue(other.is_file())

    def test_shared_versioned_candidates_are_ambiguous_until_explicitly_selected(self):
        self.shared()
        another = self.lib.with_name("libfoo.so.43")
        another.write_bytes(b"another shared version")
        with self.assertRaisesRegex(g.BazelDependencyError, "library resolution missing/ambiguous"):
            self.generate()
        self.manual()
        result = self.generate()
        self.assertEqual(result["packages"][0]["resolved_libraries"][0]["file"], str(self.lib))

    def test_alias_library_symlinks_to_one_file_are_not_ambiguous(self):
        second = self.provider / "other"
        second.mkdir()
        self.symlink(second / self.lib.name, self.lib)
        self.spec["library_dirs"].append(str(second))
        result = self.generate()
        self.assertEqual(result["packages"][0]["resolved_libraries"][0]["file"], str(self.lib))

    def test_artifact_roots_keep_common_headers_but_reject_library_search_in_them(self):
        self.spec["library_roots"] = [str(self.lib.parent)]
        self.spec["runtime_roots"] = [str(self.provider / "bin")]
        good = self.generate()
        self.assertEqual(good["packages"][0]["declared_headers"], 1)
        before = self.snapshot()
        (self.header.parent / "libfoo.a").write_bytes(b"not a permitted library location")
        self.libs = f'-L"{self.header.parent}" -lfoo'
        with self.assertRaisesRegex(g.BazelDependencyError, "outside configured library roots"):
            self.generate()
        self.assertEqual(self.snapshot(), before)

    def test_missing_library_fails_without_using_ambient_search(self):
        self.lib.unlink()
        with self.assertRaisesRegex(g.BazelDependencyError, "library resolution missing/ambiguous"):
            self.generate()

    def test_complete_manual_fallback_and_missing_manual_patch(self):
        self.manual()
        manifest = self.generate()
        self.assertEqual(manifest["counts"], {"automatic": 0, "manual_fallback": 1})
        self.assertEqual(manifest["applied_patches"], ["fixture-fix"])
        self.assertEqual(manifest["modules"], {})
        self.process.assert_not_called()
        self.patches["patches"].clear()
        with self.assertRaisesRegex(g.BazelDependencyError, "unmet requirements"):
            self.generate()

    def test_generated_compile_repair_guards_each_added_value(self):
        patch = dependencies._compile_interface_patch(self.spec, "foo", {"defines": ["EXTRA"]})
        self.assertIsNotNone(patch)
        self.assertNotIn("remove", patch)
        self.patches["patches"] = [patch]
        manifest = self.generate()
        self.assertEqual(self.package()["defines"], ["FOO_API=1", "EXTRA"])
        self.assertEqual(manifest["applied_patches"], ["foo-compile-interface"])
        self.cflags += " -DEXTRA"
        with self.assertRaisesRegex(g.BazelDependencyError, "stale patch foo-compile-interface: expected missing"):
            self.generate()

    def test_generated_manual_repairs_preserve_the_catch_fallback_guard_exception(self):
        interface = {"libraries": [{"name": "foo", "kind": "static", "file": str(self.lib)}]}
        for name, patch, missing in (
            ("foo", dependencies._manual_patch(self.spec, "foo", interface), interface),
            (
                "catch2",
                dependencies._catch_fallback(self.spec, {"linux": [self.lib.name]}, self.provider),
                {},
            ),
        ):
            with self.subTest(package=name):
                self.spec["packages"] = [{"name": name, "modules": []}]
                self.patches["patches"] = [patch]
                self.assertEqual(
                    patch["expect"],
                    {"present": {}, "missing": missing, "unmet_requirements": ["manual_interface"]},
                )
                self.assertEqual(patch["satisfy"], ["manual_interface"])
                self.assertNotIn("remove", patch)
                manifest = self.generate()
                self.assertEqual(manifest["counts"], {"automatic": 0, "manual_fallback": 1})
                self.assertEqual(manifest["applied_patches"], [patch["id"]])
                self.assertEqual(self.package(name)["libraries"][0]["source"], str(self.lib))

    def test_generated_windows_link_repair_guards_retained_archives_and_sdk_replacement(self):
        self.spec.update(platform="windows")
        self.spec["provider"]["name"] = "vcpkg"
        self.spec["packages"][0]["name"] = "opus"
        self.lib = self.provider / "lib/foo.lib"
        self.lib.write_bytes(b"static archive")
        self.libs += " -lm"
        patch = dependencies._windows_static_link_patch(self.spec, "opus", ["foo"], ["m"])
        self.patches["patches"] = [patch]
        manifest = self.generate()
        self.assertEqual(self.package("opus")["linkopts"], ["m.lib"])
        self.assertEqual([item["source"] for item in self.package("opus")["libraries"]], [str(self.lib)])
        self.assertEqual(manifest["applied_patches"], ["windows-opus-system-links"])
        self.libs = "-lm"
        with self.assertRaisesRegex(g.BazelDependencyError, "stale patch windows-opus-system-links: expected present"):
            self.generate()

    def test_generated_runtime_removal_guards_the_removed_library(self):
        self.spec["platform"] = "macos"
        self.libs += " -lm"
        patch = dependencies._macos_icu_runtime_patch(self.spec, {"libraries": ["m"]}, package="foo")
        self.patches["patches"] = [patch]
        manifest = self.generate()
        self.assertEqual(self.package()["linkopts"], [])
        self.assertEqual(
            manifest["packages"][0]["resolved_libraries"], [{"name": "foo", "kind": "static", "file": str(self.lib)}]
        )
        self.assertEqual(manifest["applied_patches"], ["macos-foo-implicit-runtime"])
        self.libs = self.libs.removesuffix(" -lm")
        with self.assertRaisesRegex(g.BazelDependencyError, "stale patch macos-foo-implicit-runtime: expected present"):
            self.generate()

    def test_stale_nonapplicable_patch_predicates_are_errors(self):
        for key, value in (
            ("provider_version", "obsolete"),
            ("config", "Debug"),
            ("platform", "windows"),
            ("package", "other"),
            ("provider", "other"),
        ):
            self.patches["patches"] = []
            patch = self.patch({"defines": ["EXTRA"]}, missing={"defines": ["EXTRA"]})
            patch["when"][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(g.BazelDependencyError, "stale/nonapplicable"):
                self.generate()

    def test_patch_removal_and_readdition_is_not_a_change(self):
        same = {"defines": ["FOO_API=1"]}
        self.patch(same, present=same, remove=same)
        with self.assertRaisesRegex(g.BazelDependencyError, "redundant remove/re-add"):
            self.generate()

    def test_runtime_and_guarded_header_patch_are_planned(self):
        extra = self.provider / "extra"
        extra.mkdir()
        (extra / "extra.h").write_bytes(b"// fixture\n")
        data = self.provider / "share/resource.dat"
        data.write_bytes(b"data")
        additions = {
            "header_dirs": [str(extra)],
            "runtime": [str(data)],
        }
        self.patch(additions, missing=copy.deepcopy(additions))
        manifest = self.generate()
        self.assertEqual(self.artifact(self.include_root(index=1) / "extra.h"), extra / "extra.h")
        self.assertEqual(self.package()["runtime"], [str(data)])
        self.assertTrue({"header", "runtime"} <= {r for i in manifest["inputs"] for r in i["roles"]})

    def test_windows_shared_import_requires_guarded_dll_and_thread_flag_translation(self):
        self.spec.update(platform="windows", config="Debug", linkage="shared")
        self.lib = self.provider / "lib/foo.lib"
        self.lib.write_bytes(b"import")
        dll = self.provider / "bin/foo.dll"
        dll.write_bytes(b"dll")
        self.cflags += " -pthread"
        self.libs += " -pthread"
        old = {"name": "foo", "kind": "shared"}
        new = dict(old, runtime=str(dll))
        present = {"libraries": [old], "copts": ["-pthread"], "linkopts": ["-pthread"]}
        self.patch({"libraries": [new]}, missing={"libraries": [new]}, present=present, remove=copy.deepcopy(present))
        manifest = self.generate()
        self.assertEqual(
            self.package()["libraries"],
            [{"index": 0, "kind": "shared", "source": str(self.lib), "basename": "foo.lib", "runtime": str(dll)}],
        )
        self.assertEqual(self.package()["linkopts"], [])
        self.assertNotIn("-pthread", self.package()["defines"])
        self.assertEqual(manifest["packages"][0]["resolved_libraries"][0]["runtime"], str(dll))

    def test_windows_sdk_link_options_are_transitive_and_limited_to_declared_names(self):
        self.spec.update(platform="windows", system_libraries=[])
        self.lib = self.provider / "lib/foo.lib"
        self.lib.write_bytes(b"static import fixture")
        self.manual()
        interface = self.patches["patches"][0]["add"]
        interface["linkopts"] = ["Shell32.lib"]
        with self.assertRaisesRegex(g.BazelDependencyError, "unsupported link flag"):
            self.generate()
        self.spec["system_libraries"] = ["Shell32"]
        self.generate()
        self.assertEqual(self.package()["linkopts"], ["Shell32.lib"])
        self.assertEqual([library["source"] for library in self.package()["libraries"]], [str(self.lib)])
        for option in ("Other.lib", "../Shell32.lib", "C:/SDK/Shell32.lib", "-lShell32"):
            with self.subTest(option=option):
                interface["linkopts"] = [option]
                with self.assertRaisesRegex(g.BazelDependencyError, "unsupported link flag"):
                    self.generate()
        interface["linkopts"] = ["Shell32.lib"]
        for host in ("linux", "macos"):
            with self.subTest(host=host):
                self.spec["platform"] = host
                self.patches["patches"][0]["when"]["platform"] = host
                with self.assertRaisesRegex(g.BazelDependencyError, "unsupported link flag"):
                    self.generate()
        self.spec["platform"] = "windows"
        self.patches["patches"][0]["when"]["platform"] = "windows"
        interface["libraries"].append({"name": "Shell32", "kind": "system"})
        with self.assertRaisesRegex(g.BazelDependencyError, "static provider/system library link order"):
            self.generate()

    def test_windows_overlapping_interfaces_retain_payload_identity_and_narrow_headers(self):
        original_spec = copy.deepcopy(self.spec)
        self.lib = self.provider / "lib/foo.lib"
        self.lib.write_bytes(b"import library")
        dll = self.provider / "bin/foo.dll"
        dll.write_bytes(b"selected DLL")
        other_lib = self.provider / "lib/other/foo.lib"
        other_lib.parent.mkdir()
        other_lib.write_bytes(b"another import library")
        other_dll = self.provider / "bin/other/foo.dll"
        other_dll.parent.mkdir()
        other_dll.write_bytes(b"another DLL")
        child = self.header.parent / "child"
        child.mkdir()
        (child / "bar.h").write_text("#define BAR 1\n", encoding="utf-8")
        for difference in (None, "file", "runtime", "kind"):
            with self.subTest(difference=difference):
                self.spec = copy.deepcopy(original_spec)
                self.spec.update(platform="windows", config="Debug", linkage="shared")
                self.patches = {"schema": 1, "patches": []}
                library = {"name": "foo", "kind": "shared", "file": str(self.lib), "runtime": str(dll)}
                self.manual({"header_dirs": [str(self.header.parent)], "libraries": [library]})
                package = {"name": "bar", "modules": []}
                second = copy.deepcopy(self.patches["patches"][0])
                second["id"] = "bar-interface"
                second["when"]["package"] = "bar"
                second["add"]["header_dirs"] = [str(child)]
                second["add"]["defines"] = ["BAR_API=1"]
                imported = second["add"]["libraries"][0]
                imported["name"] = "alias"
                if difference == "file":
                    imported["file"] = str(other_lib)
                elif difference == "runtime":
                    imported["runtime"] = str(other_dll)
                elif difference == "kind":
                    imported["kind"] = "static"
                    imported.pop("runtime")
                    package["linkage"] = "static"
                self.spec["packages"].append(package)
                self.patches["patches"].append(second)
                manifest = self.generate()
                first, second = self.package(), self.package("bar")
                self.assertEqual(second["defines"], ["BAR_API=1"])
                self.assertEqual(first["defines"], [])
                self.assertEqual(
                    second["headers"],
                    [
                        {
                            "root": str(child),
                            "index": 0,
                            "files": [{"source": str(child / "bar.h"), "path": "bar.h"}],
                        }
                    ],
                )
                self.assertEqual(second["runtime"], [])
                self.assertEqual(
                    second["libraries"],
                    [
                        {
                            "index": 0,
                            "kind": imported["kind"],
                            "source": imported["file"],
                            "basename": "foo.lib",
                            "runtime": imported.get("runtime"),
                        }
                    ],
                )
                self.assertEqual(first["libraries"] == second["libraries"], difference is None)
                self.assertEqual(len(manifest["packages"]), 2)
                self.assertEqual(manifest["packages"][1]["resolved_libraries"][0], imported)

    def test_windows_shared_codec_private_runtime_is_planned_without_a_private_link_library(self):
        self.spec.update(platform="windows", linkage="shared")
        self.spec["library_roots"] = [str(self.provider / "lib")]
        self.spec["runtime_roots"] = [str(self.provider / "bin")]
        self.lib = self.provider / "lib/foo.lib"
        self.lib.write_bytes(b"codec import archive")
        runtime, private = self.provider / "bin/foo.dll", self.provider / "bin/private.dll"
        runtime.write_bytes(b"codec DLL fixture")
        private.write_bytes(b"private runtime v1")
        present = [{"name": "foo", "kind": "shared"}]
        interface = {
            "libraries": [{"name": "foo", "kind": "shared", "file": str(self.lib), "runtime": str(runtime)}],
            "runtime": [str(private)],
        }
        self.patch(interface, missing=interface, present={"libraries": present}, remove={"libraries": present})
        first = self.generate()
        package = first["packages"][0]
        self.assertEqual([item["name"] for item in package["resolved_libraries"]], ["foo"])
        self.assertEqual(package["interface"]["runtime"], [str(private)])
        self.assertEqual(self.package()["runtime"], [str(private)])
        private.write_bytes(b"private runtime v2")
        changed = self.generate()
        self.assertNotEqual(
            next(item["sha256"] for item in first["inputs"] if item["path"] == str(private)),
            next(item["sha256"] for item in changed["inputs"] if item["path"] == str(private)),
        )
        self.assertEqual(self.package()["runtime"], [str(private)])
        before = self.snapshot()
        private.unlink()
        with self.assertRaisesRegex(g.BazelDependencyError, "missing file"):
            self.generate()
        self.assertEqual(self.snapshot(), before)

    def test_windows_extra_runtime_is_separate_from_the_imported_dll(self):
        self.spec.update(platform="windows", linkage="shared")
        self.spec["library_roots"] = [str(self.provider / "lib")]
        self.spec["runtime_roots"] = [str(self.provider / "bin")]
        self.lib = self.provider / "lib/foo.lib"
        self.lib.write_bytes(b"codec import archive")
        codec, ogg, other = (self.provider / "bin" / name for name in ("foo.dll", "ogg.dll", "other.dll"))
        codec.write_bytes(b"codec DLL fixture")
        ogg.write_bytes(b"ogg runtime fixture")
        other.write_bytes(b"second runtime fixture")
        present = [{"name": "foo", "kind": "shared"}]
        interface = {
            "libraries": [{"name": "foo", "kind": "shared", "file": str(self.lib), "runtime": str(codec)}],
            "runtime": [str(ogg), str(other)],
        }
        self.patch(interface, missing=interface, present={"libraries": present}, remove={"libraries": present})
        package = self.generate()["packages"][0]
        self.assertEqual([item["name"] for item in package["resolved_libraries"]], ["foo"])
        self.assertEqual(package["interface"]["runtime"], [str(ogg), str(other)])
        self.assertEqual(self.package()["runtime"], [str(ogg), str(other)])
        self.assertEqual(
            self.package()["libraries"],
            [
                {
                    "index": 0,
                    "kind": "shared",
                    "source": str(self.lib),
                    "basename": "foo.lib",
                    "runtime": str(codec),
                }
            ],
        )

    def test_windows_static_imports_and_configuration_are_exact(self):
        self.spec.update(platform="windows", config="Debug")
        self.lib = self.provider / "lib/food.lib"
        self.lib.write_bytes(b"static")
        self.manual()
        self.generate()
        self.assertEqual(
            self.package()["libraries"],
            [
                {
                    "index": 0,
                    "kind": "static",
                    "source": str(self.lib),
                    "basename": "food.lib",
                    "runtime": None,
                }
            ],
        )
        self.assertEqual(self.resolution["manifest"]["configuration"]["config"], "Debug")
        library = self.patches["patches"][0]["add"]["libraries"][0]
        library["runtime"] = str(self.provider / "bin/foo.dll")
        with self.assertRaisesRegex(g.BazelDependencyError, "only valid for shared"):
            self.generate()

    def test_macos_static_and_shared_imports_are_planned(self):
        self.spec.update(platform="macos", config="Debug")
        self.manual()
        self.generate()
        self.spec["linkage"] = "shared"
        self.lib = self.provider / "lib/libfoo.dylib"
        self.lib.write_bytes(b"dylib")
        self.patches["patches"] = []
        self.manual(
            {
                "libraries": [{"name": "foo", "kind": "shared", "file": str(self.lib)}],
            }
        )
        self.generate()
        self.assertEqual(
            self.package()["libraries"],
            [
                {
                    "index": 0,
                    "kind": "shared",
                    "source": str(self.lib),
                    "basename": "libfoo.dylib",
                    "runtime": None,
                }
            ],
        )

    def test_linux_shared_uses_soname_and_requires_one_valid_name(self):
        self.shared()
        result = self.generate()
        self.assertEqual(result["packages"][0]["resolved_libraries"][0]["soname"], "libfoo.so.42")
        self.assertEqual(self.package()["libraries"][0]["basename"], "libfoo.so.42")
        for soname in ("", "../evil.so", "not-a-soname"):
            self.soname = soname
            with (
                self.subTest(soname=soname),
                self.assertRaisesRegex(g.BazelDependencyError, "missing/invalid ELF SONAME"),
            ):
                self.generate()

    def test_nix_shared_runtime_paths_are_ordered_deduplicated_linker_only_hints(self):
        self.shared()
        self.spec["provider"]["name"] = "nix"
        first, second = self.nix_runtime_dirs
        self.dynamic = f"0x000 (RUNPATH) Library runpath: [{second}:{first}:{second}]"
        self.libs += " -pthread"
        manifest = self.generate_with_runtime_directories(first, second)
        fields = self.package()
        self.assertEqual(fields["linkopts"], ["-pthread", "-Wl,-rpath-link," + second, "-Wl,-rpath-link," + first])
        self.assertNotIn("copts", fields)
        self.assertEqual(fields["includes"], [{"tree": 0, "path": "."}])
        package = manifest["packages"][0]
        self.assertEqual(package["interface"]["linkopts"], ["-pthread"])
        self.assertEqual(set(package["resolved_libraries"][0]), {"name", "kind", "file", "soname"})
        self.assertEqual(self.package()["libraries"][0]["basename"], "libfoo.so.42")

    def test_nix_runtime_search_uses_runpath_before_legacy_rpath(self):
        self.shared()
        self.spec["provider"]["name"] = "nix"
        first, second = self.nix_runtime_dirs
        for dynamic, expected in (
            (f"0x000 (RPATH) Library rpath: [{first}]", [first]),
            (
                f"0x000 (RPATH) Library rpath: [{first}]\n0x000 (RUNPATH) Library runpath: [{second}]",
                [second],
            ),
            ("", []),
        ):
            with self.subTest(dynamic=dynamic):
                self.dynamic = dynamic
                self.generate_with_runtime_directories(first, second)
                self.assertEqual(self.package()["linkopts"], ["-Wl,-rpath-link," + path for path in expected])

    def test_non_nix_shared_and_nix_static_imports_do_not_gain_runtime_search_hints(self):
        self.shared()
        self.dynamic = "0x000 (RUNPATH) Library runpath: [/usr/lib]"
        self.generate()
        self.assertEqual(self.package()["linkopts"], [])
        self.spec["provider"]["name"] = "nix"
        self.spec["linkage"] = "static"
        self.lib = self.provider / "lib/libfoo.a"
        self.process.reset_mock()
        self.generate()
        self.assertEqual(self.package()["linkopts"], [])
        self.assertFalse(any(call.args[0][0] == str(self.readelf) for call in self.process.call_args_list))

    def test_header_symlink_directories_are_enumerated_and_cycles_fail(self):
        extra = self.provider / "actual"
        extra.mkdir()
        (extra / "actual.h").write_bytes(b"// real header\n")
        self.symlink(self.header.parent / "linked", extra, directory=True)
        self.generate()
        self.assertEqual(self.artifact(self.include_root() / "linked/actual.h"), extra / "actual.h")
        self.symlink(extra / "cycle", self.header.parent, directory=True)
        with self.assertRaisesRegex(g.BazelDependencyError, "symlink cycle"):
            self.generate()

    def resolve_nix_headers(self):
        self.spec["provider"]["name"] = "nix"
        with mock.patch.object(g, "_NIX_STORE", self.store):
            adapter = g._Adapter(self.spec, self.patches)
            adapter.prepare()
            self.resolution = adapter.resolve()
        return adapter, self.resolution["manifest"]

    def test_is_nix_store_directory_validates_nix32_store_paths_and_rejects_traversal(self):
        valid_hash = "0123456789abcdfghijklmnpqrsvwxyz"
        for path in (
            Path(f"/nix/store/{valid_hash}-foo"),
            Path(f"/nix/store/{'0' * 32}-libfoo+1.2_3?a=b-dev/include/nested"),
        ):
            with self.subTest(valid=path):
                self.assertTrue(g._is_nix_store_directory(path))
        for path in (
            Path(f"/nix/store/{'e' * 32}-foo"),
            Path(f"/nix/store/{'o' * 32}-foo"),
            Path(f"/nix/store/{'t' * 32}-foo"),
            Path(f"/nix/store/{'u' * 32}-foo"),
            Path(f"/nix/store/{'A' * 32}-foo"),
            Path(f"/nix/store/{'0' * 31}-foo"),
            Path(f"/nix/store/{'0' * 33}-foo"),
            Path(f"/nix/store/{'0' * 32}"),
            Path(f"/nix/store/{'0' * 32}-"),
            Path(f"/tmp/nix/store/{valid_hash}-foo"),
            Path("/tmp/provider/include"),
            Path(f"/nix/store/{valid_hash}-foo/.."),
            Path(f"/nix/store/{valid_hash}-foo/include/../other"),
        ):
            with self.subTest(invalid=path):
                self.assertFalse(g._is_nix_store_directory(path))
        self.shared()
        self.spec["provider"]["name"] = "nix"
        for bad_dir in (f"/nix/store/{valid_hash}-foo/.", f"/nix/store/{valid_hash}-foo/..", "/tmp/outside"):
            self.dynamic = f"0x000 (RUNPATH) Library runpath: [{bad_dir}]"
            with (
                self.subTest(runpath=bad_dir),
                self.assertRaisesRegex(g.BazelDependencyError, "invalid Nix ELF runtime search directory"),
            ):
                self.generate_with_runtime_directories(bad_dir)

    def test_nix_header_records_use_component_order_and_retain_hash_and_other_input_roles(self):
        include = self.header.parent
        (include / "a").mkdir()
        for relative in ("a-extra.h", "a.inl", "a/x (β)@+,.h"):
            (include / relative).write_bytes(relative.encode())
        digest = hashlib.sha256(self.header.read_bytes()).hexdigest()
        self.spec["provider"]["evidence"] = [{"path": str(self.header), "sha256": digest}]
        _adapter, manifest = self.resolve_nix_headers()
        paths = ["a/x (β)@+,.h", "a-extra.h", "a.inl", "foo.h"]
        self.assertEqual(
            self.package()["headers"],
            [{"root": str(include), "index": 0, "files": [{"source": str(include / p), "path": p} for p in paths]}],
        )
        self.assertEqual(manifest["packages"][0]["declared_headers"], len(paths))
        self.assertIn(
            {"path": str(self.header), "roles": ["header", "provider_evidence"], "sha256": digest}, manifest["inputs"]
        )

    def test_nix_nested_roots_preserve_order_and_reject_empty_nested(self):
        include = self.header.parent
        nested = include / "ryml"
        nested.mkdir()
        (include / "c4").mkdir()
        (include / "c4/dep.h").write_bytes(b"// dep\n")
        (nested / "ryml.hpp").write_bytes(b"// ryml\n")
        empty_nested = include / "empty_sub"
        empty_nested.mkdir()
        linked_empty = include / "linked_empty"
        self.symlink(linked_empty, Path("empty_sub"), directory=True)
        for nested_first in (False, True):
            with self.subTest(nested_first=nested_first):
                roots = [nested, include] if nested_first else [include, nested]
                self.cflags = " ".join(f'-I"{root}"' for root in roots)
                self.write_pc()
                _adapter, manifest = self.resolve_nix_headers()
                self.assertEqual(
                    self.package()["includes"],
                    [{"tree": 0, "path": "ryml"}, {"tree": 0, "path": "."}]
                    if nested_first
                    else [{"tree": 0, "path": "."}, {"tree": 0, "path": "ryml"}],
                )
                self.assertEqual(self.package()["headers"][0]["index"], roots.index(include))
                self.assertEqual(
                    [f["path"] for f in self.package()["headers"][0]["files"]], ["c4/dep.h", "foo.h", "ryml/ryml.hpp"]
                )
                self.assertEqual(manifest["packages"][0]["declared_headers"], 3)
        for empty_root in (empty_nested, linked_empty):
            with self.subTest(empty_root=empty_root):
                self.cflags = f'-I"{include}" -I"{empty_root}"'
                self.write_pc()
                with self.assertRaisesRegex(g.BazelDependencyError, "empty header directory"):
                    self.resolve_nix_headers()

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux byte filenames")
    def test_nix_rejects_unrepresentable_emitted_names_including_ancestors_and_links(self):
        include = self.header.parent
        empty = include / "empty:name"
        empty.mkdir()
        self.resolve_nix_headers()  # Empty directories aren't projected.
        for name, kind in (
            ("bad:name", "ancestor"),
            ("bad\\name", "file"),
            ("bad\x01name", "symlink"),
            ("bad\x7fname", "file"),
            (os.fsdecode(b"bad\xffname"), "file"),
        ):
            with self.subTest(name=name, kind=kind):
                path = include / name
                if kind == "ancestor":
                    path.mkdir()
                    (path / "valid.h").write_bytes(b"nested")
                elif kind == "symlink":
                    self.symlink(path, self.header)
                else:
                    path.write_bytes(b"header")
                with self.assertRaisesRegex(g.BazelDependencyError, "invalid Bazel header path"):
                    self.resolve_nix_headers()
                if kind == "ancestor":
                    shutil.rmtree(path)
                else:
                    path.unlink()

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux byte filenames")
    def test_nix_header_alias_requires_a_json_representable_source_not_a_label(self):
        alias = self.header.parent / "alias.h"
        for basename in ("actual:name", os.fsdecode(b"actual\xffname")):
            with self.subTest(basename=basename):
                source = self.provider / "share" / basename
                source.write_bytes(b"// header\n")
                self.symlink(alias, source)
                if basename == "actual:name":
                    self.resolve_nix_headers()
                    self.assertIn({"source": str(source), "path": "alias.h"}, self.package()["headers"][0]["files"])
                else:
                    with self.assertRaisesRegex(g.BazelDependencyError, "non-UTF-8 header source"):
                        self.resolve_nix_headers()
                alias.unlink()

    def test_nix_rejects_bazel_boundary_files_and_symlinks(self):
        include = self.header.parent
        nested = include / "nested"
        nested.mkdir()
        (nested / "nested.h").write_bytes(b"// nested\n")
        for boundary in ("BUILD", "BUILD.bazel"):
            for location, creator in (
                (include / boundary, lambda path: path.write_bytes(b"# boundary\n")),
                (nested / boundary, lambda path: path.write_bytes(b"# nested boundary\n")),
                (include / boundary, lambda path: self.symlink(path, self.header)),
            ):
                with self.subTest(boundary=boundary, location=location):
                    creator(location)
                    try:
                        with self.assertRaisesRegex(g.BazelDependencyError, "Bazel package boundary file"):
                            self.resolve_nix_headers()
                    finally:
                        location.unlink()

    def test_nix_accepts_non_boundary_names_in_sparse_file_labels(self):
        include = self.header.parent
        self.header.unlink()
        (include / "BUILD").mkdir()
        (include / "BUILD.bazel").mkdir()
        paths = ["BUILD/WORKSPACE", "BUILD.bazel/WORKSPACE.bazel", "MODULE.bazel", "REPO.bazel"]
        for path in paths:
            (include / path).write_bytes(b"header")
        self.resolve_nix_headers()
        self.assertEqual([f["path"] for f in self.package()["headers"][0]["files"]], paths)

    def test_nix_rejects_empty_dangling_escaping_and_cyclic_entries(self):
        include = self.header.parent
        outside_file = self.root / "outside.h"
        outside_file.write_bytes(b"// outside\n")
        outside_dir = self.root / "outside_dir"
        outside_dir.mkdir()
        (outside_dir / "escape.h").write_bytes(b"// escape\n")
        for label, setup, pattern in (
            ("dangling", lambda: self.symlink(include / "dangling.h", include / "missing.h"), "missing file"),
            ("escape-file", lambda: self.symlink(include / "escape.h", outside_file), "outside allowed provider roots"),
            (
                "escape-dir",
                lambda: self.symlink(include / "escape_dir", outside_dir, directory=True),
                "outside allowed provider roots",
            ),
            (
                "cycle",
                lambda: (
                    (include / "sub").mkdir(),
                    self.symlink(include / "sub/loop", Path(".."), directory=True),
                ),
                "symlink cycle",
            ),
            (
                "mutual-cycle",
                lambda: (
                    self.symlink(include / "a", include / "b", directory=True),
                    self.symlink(include / "b", include / "a", directory=True),
                ),
                "symlink cycle",
            ),
        ):
            with self.subTest(label=label):
                setup()
                try:
                    with self.assertRaisesRegex(g.BazelDependencyError, pattern):
                        self.resolve_nix_headers()
                finally:
                    for item in list(include.iterdir()):
                        if item != self.header:
                            if item.is_symlink() or item.is_file():
                                item.unlink()
                            else:
                                shutil.rmtree(item)
        self.header.unlink()
        with self.assertRaisesRegex(g.BazelDependencyError, "empty header directory"):
            self.resolve_nix_headers()

    def test_nix_rejects_symlinks_to_admitted_mutable_evidence(self):
        evidence = self.root / "provider.json"
        evidence.write_bytes(b"{}\n")
        self.spec["allowed_roots"].append(str(evidence))
        self.spec["provider"]["evidence"] = [{"path": str(evidence), "sha256": hashlib.sha256(b"{}\n").hexdigest()}]
        self.symlink(self.header.parent / "evidence.h", evidence)
        with self.assertRaisesRegex(g.BazelDependencyError, "leaves admitted Nix outputs"):
            self.resolve_nix_headers()

    @unittest.skipIf(os.name == "nt", "requires POSIX symlink and parent traversal")
    def test_nix_accepts_parent_segments_in_pkg_config_includes_and_relative_link_targets(self):
        include = self.header.parent
        self.cflags = f'-I"{self.pcfile.parent}/../../include"'
        self.symlink(self.provider / "pcdir", Path("lib/pkgconfig"), directory=True)
        # '..' follows the symlink's actual target, not the lexical location of pcdir.
        self.symlink(include / "alias.h", Path("../pcdir/../../include/foo.h"))
        _adapter, manifest = self.resolve_nix_headers()
        self.assertEqual(self.include_root(), include)
        self.assertEqual(
            self.package()["headers"][0]["files"],
            [
                {"source": str(self.header), "path": "alias.h"},
                {"source": str(self.header), "path": "foo.h"},
            ],
        )
        self.assertEqual(manifest["packages"][0]["declared_headers"], 2)

    @unittest.skipIf(os.name == "nt", "Nix uses POSIX absolute symlink targets")
    def test_nix_rejects_links_through_mutable_intermediates(self):
        host_alias = self.root / "host-include"
        self.symlink(host_alias, self.header.parent, directory=True)
        # The bridge is outside the scanned include tree; it is only reached
        # as a directory component of another link's target.
        bridge = self.provider / "share/bridge"
        self.symlink(bridge, host_alias, directory=True)
        header = self.header.parent / "linked.h"
        # Lexically collapsing bridge/.. would conceal the disallowed hop.
        self.symlink(bridge.parent / "include", self.header.parent, directory=True)
        for target in (host_alias / self.header.name, bridge / self.header.name, bridge / "../include/foo.h"):
            with self.subTest(target=target):
                self.symlink(header, target)
                try:
                    self.assertEqual(header.resolve(strict=True), self.header)
                    with self.assertRaisesRegex(g.BazelDependencyError, "leaves admitted Nix outputs"):
                        self.resolve_nix_headers()
                finally:
                    header.unlink()
        # An include root alias must not bypass the hop checks by being
        # canonicalized during --cflags processing before the tree is scanned.
        for candidate in (bridge, bridge / "../include"):
            with self.subTest(candidate=candidate):
                self.assertEqual(candidate.resolve(strict=True), self.header.parent)
                self.cflags = f'-I"{candidate}"'
                with self.assertRaisesRegex(g.BazelDependencyError, "leaves admitted Nix outputs"):
                    self.resolve_nix_headers()

    @unittest.skipUnless(hasattr(os, "mkfifo"), "requires os.mkfifo")
    def test_nix_rejects_non_regular_files_and_symlinks_to_them(self):
        include = self.header.parent
        fifo = include / "pipe.h"
        os.mkfifo(fifo)
        with self.assertRaisesRegex(g.BazelDependencyError, "unsupported non-file header entry"):
            self.resolve_nix_headers()
        fifo.unlink()
        outside_fifo = self.provider / "share/pipe.h"
        os.mkfifo(outside_fifo)
        self.symlink(include / "pipe_link.h", outside_fifo)
        with self.assertRaisesRegex(g.BazelDependencyError, "unsupported non-file header entry"):
            self.resolve_nix_headers()

    def test_nix_internal_symlinks_count_lexical_entries_and_record_aliases(self):
        include = self.header.parent
        self.header.unlink()
        ft2 = include / "freetype2"
        ft = ft2 / "freetype"
        ft.mkdir(parents=True)
        (ft2 / "ft2build.h").write_bytes(b"// ft2build\n")
        (ft / "freetype.h").write_bytes(b"// freetype\n")
        ft2build_alias = include / "ft2build.h"
        ft_alias = include / "freetype"
        self.symlink(ft2build_alias, Path("freetype2/ft2build.h"))
        self.symlink(ft_alias, Path("freetype2/freetype"), directory=True)
        for flags, expected_paths in (
            (f'-I"{include}"', ["."]),
            (f'-I"{include}" -I"{ft_alias}"', [".", "freetype2/freetype"]),
        ):
            with self.subTest(flags=flags):
                self.cflags = flags
                self.write_pc()
                adapter, manifest = self.resolve_nix_headers()
                self.assertEqual(manifest["packages"][0]["declared_headers"], 4)
                self.assertTrue({ft2build_alias, ft_alias, ft_alias / "freetype.h"} <= adapter.aliases)
                self.assertEqual(
                    self.package()["headers"][0]["files"],
                    [
                        {"source": str(ft / "freetype.h"), "path": "freetype/freetype.h"},
                        {"source": str(ft / "freetype.h"), "path": "freetype2/freetype/freetype.h"},
                        {"source": str(ft2 / "ft2build.h"), "path": "freetype2/ft2build.h"},
                        {"source": str(ft2 / "ft2build.h"), "path": "ft2build.h"},
                    ],
                )
                self.assertEqual(
                    self.package()["includes"],
                    [{"tree": 0, "path": path} for path in expected_paths],
                )

    @unittest.skipIf(os.name == "nt", "Nix uses POSIX absolute symlink targets")
    def test_nix_accepts_cross_output_links_and_safe_chains(self):
        include = self.header.parent
        other = self.store / f"{'1' * 32}-other-dev"
        target = other / "extra headers/other header.h"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"// other header\n")
        self.spec["allowed_roots"].append(str(other))
        self.symlink(include / "direct.h", target)
        self.symlink(other / "header alias", Path("extra headers"), directory=True)
        self.symlink(include / "chain.h", Path("../..") / other.name / "header alias" / target.name)
        _adapter, manifest = self.resolve_nix_headers()
        self.assertEqual(manifest["packages"][0]["declared_headers"], 3)
        self.assertEqual(
            self.package()["headers"][0]["files"],
            [
                {"source": str(target), "path": "chain.h"},
                {"source": str(target), "path": "direct.h"},
                {"source": str(self.header), "path": "foo.h"},
            ],
        )

    def test_owned_windows_headers_exclude_unlisted_siblings(self):
        self.spec["platform"] = "windows"
        (self.lib.parent / "foo.lib").write_bytes(b"import library")
        self.header.with_name("unowned.h").write_bytes(b"unowned")
        adapter = g._Adapter(self.spec, self.patches)
        adapter.prepare()
        adapter.header_files["foo"] = [self.header]
        self.resolution = adapter.resolve()
        self.assertEqual(self.package()["headers"][0]["files"], [{"source": str(self.header), "path": "foo.h"}])
        inputs = self.resolution["manifest"]["inputs"]
        self.assertEqual([item["path"] for item in inputs if "header" in item["roles"]], [str(self.header)])

    def test_resolution_never_writes_metadata_or_materializes_provider_links(self):
        with (
            mock.patch.object(Path, "write_text", side_effect=AssertionError("unexpected metadata write")),
            mock.patch.object(Path, "write_bytes", side_effect=AssertionError("unexpected artifact copy")),
            mock.patch.object(Path, "mkdir", side_effect=AssertionError("unexpected output directory")),
            mock.patch.object(Path, "symlink_to", side_effect=AssertionError("unexpected materialization")),
        ):
            self.generate()
        self.assertEqual(self.package()["headers"][0]["files"], [{"source": str(self.header), "path": "foo.h"}])
        self.assertFalse(self.out.exists())

    def native_pkg_config(self):
        # An ambient Perl .bat wrapper is not a standalone native resolver: its
        # interpreter is deliberately unavailable in the adapter's isolated PATH.
        suffix = ".exe" if os.name == "nt" else ""
        executable = shutil.which("pkg-config" + suffix) or shutil.which("pkgconf" + suffix)
        if executable is None:
            self.skipTest("optional native pkg-config executable unavailable; mocked contracts still execute")
        return str(Path(executable).absolute())

    def test_real_pkg_config_fixture_when_available(self):
        self.spec["pkg_config"]["executable"] = self.native_pkg_config()
        self.write_pc()
        with mock.patch.object(g.subprocess, "run", wraps=self.real_run):
            manifest = self.generate()
        self.assertEqual(manifest["modules"]["foo"]["version"], "1.2.3")
        self.assertEqual(self.package()["libraries"][0]["source"], str(self.lib))

    def test_real_paired_uninstalled_metadata_selects_the_installed_interface(self):
        executable = self.native_pkg_config()
        self.spec["pkg_config"]["executable"] = executable
        # POSIX-spelled quoted paths keep the fixture readable by every real
        # native resolver, including a Windows one fed backslash-free data.
        self.cflags = f'-I"{self.header.parent.as_posix()}" -DFOO_API=1'
        self.libs = f'-L"{self.lib.parent.as_posix()}" -lfoo'
        self.write_pc()
        uninstalled = self.pcfile.with_name("foo-uninstalled.pc")
        uninstalled.write_text(
            f"prefix={self.provider.as_posix()}\nName: foo\nDescription: fixture\nVersion: 1.2.3\n"
            f'Cflags: -I"{self.header.parent.as_posix()}" -DUNINSTALLED_ONLY=1\n'
            f'Libs: -L"{self.lib.parent.as_posix()}" -lfoo\n',
            encoding="utf-8",
        )
        # Control: with the presence-based filter absent, the real resolver
        # prefers the paired uninstalled metadata over the installed module.
        control = dict(g.pkg_config_environment(self.spec["pkg_config"]["pc_dirs"]))
        control.pop("PKG_CONFIG_DISABLE_UNINSTALLED", None)
        preferred = self.real_run(
            [executable, "--cflags", "foo"], env=control, capture_output=True, text=True, check=True
        )
        self.assertIn("-DUNINSTALLED_ONLY=1", preferred.stdout)
        self.assertNotIn("FOO_API", preferred.stdout)
        with mock.patch.object(g.subprocess, "run", wraps=self.real_run):
            repair = dependencies._compile_interface_patch(self.spec, "foo", {"defines": ["FOO_API=1"]})
            if repair is not None:
                self.patches["patches"].append(repair)
            manifest = self.generate()
        self.assertIsNone(repair)
        defines = manifest["packages"][0]["interface"]["defines"]
        self.assertIn("FOO_API=1", defines)
        self.assertNotIn("UNINSTALLED_ONLY=1", defines)
        self.assertEqual(manifest["modules"]["foo"]["version"], "1.2.3")
        self.assertEqual(manifest["modules"]["foo"]["metadata"], str(self.pcfile))
        # Installed-only selection does not weaken the bounded metadata inventory.
        self.assertIn(str(uninstalled), [entry["path"] for entry in manifest["inputs"]])

    def test_real_shared_metadata_respects_native_missing_private_dependency_behavior(self):
        executable = self.native_pkg_config()
        self.shared()
        self.spec["pkg_config"]["executable"] = executable
        private = self.metadata_file("private")
        with private.open("a", encoding="utf-8") as stream:
            stream.write("Cflags: -DPRIVATE_API=1\n")
        self.write_pc()
        with self.pcfile.open("a", encoding="utf-8") as stream:
            stream.write("Requires.private: private\n")
        native_queries = []

        def query(argv, **kwargs):
            if argv[0] == str(self.readelf):
                return self.query(argv, **kwargs)
            native_queries.append((argv.copy(), kwargs.copy()))
            return self.real_run(argv, **kwargs)

        with mock.patch.object(g.subprocess, "run", side_effect=query):
            self.generate()
            before = self.snapshot()
            self.assertTrue(native_queries)
            self.pcfile.write_text(
                self.pcfile.read_text(encoding="utf-8").replace(
                    "Requires.private: private\n", "Requires.private: private, missing-static-only\n"
                ),
                encoding="utf-8",
            )
            # Probe the exact successful native invocations and isolated pc_dirs.
            # Some resolvers reject missing private deps even for shared cflags.
            probes = [self.real_run(argv, **kwargs) for argv, kwargs in native_queries]
            failures = [result for result in probes if result.returncode != 0]
            if failures:
                self.assertIn("missing-static-only", "\n".join(result.stderr for result in failures))
                with self.assertRaisesRegex(g.BazelDependencyError, "external query failed") as failure:
                    self.generate()
                self.assertIn("missing-static-only", str(failure.exception))
                self.assertEqual(before, self.snapshot())
                self.assertFalse(self.out.exists())
            else:
                manifest = self.generate()
                self.assertIn("FOO_API=1", manifest["packages"][0]["interface"]["defines"])
                self.assertIn(str(private), [entry["path"] for entry in manifest["inputs"]])
                self.assertNotIn("missing-static-only", manifest["modules"])

    real_run = staticmethod(subprocess.run)


if __name__ == "__main__":
    unittest.main()
