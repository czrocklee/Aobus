"""Core Bazel workspace, provider and host-tool behavior contracts."""

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import contextmanager
from pathlib import Path, PureWindowsPath
from unittest.mock import Mock, patch

from test_bazel_windows_linkage import static_archive

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_host_tools as host_tools
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers
from ao.core import bazel_workspace as workspace


class NixDependencyMetadataTests(unittest.TestCase):
    def setUp(self):
        self.bin = Path("/nix/store/package-bin")
        self.out = Path("/nix/store/package-out")
        self.dev = Path("/nix/store/package-dev")
        self.entry = {
            "version": "1.2.3",
            "storePath": self.bin.as_posix(),
            "drvPath": "/nix/store/package.drv",
            "outputs": {
                "out": self.out.as_posix(),
                "bin": self.bin.as_posix(),
                "dev": self.dev.as_posix(),
                "doc": "/nix/store/package-doc",
            },
        }
        self.report = {"dependencies": {"fixture": self.entry}}
        self.installed = {self.bin, self.out, self.dev}
        for patcher in (
            patch.object(Path, "is_dir", lambda path: path in self.installed),
            patch.object(providers, "nix_root", side_effect=lambda path: path),
            patch.object(native, "executable", side_effect=AssertionError("no Nix CLI discovery")),
            patch.object(providers.subprocess, "run", side_effect=AssertionError("no Nix CLI query")),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_reported_selected_bin_and_sibling_outputs_do_not_require_derivation_file(self):
        with patch.object(Path, "is_file", side_effect=AssertionError("drv file presence is not authority")):
            root, version, derivation, outputs = providers.nix_dependency(self.report, "fixture")
        self.assertEqual(root, self.bin)
        self.assertEqual(version, "1.2.3")
        self.assertEqual(derivation, "/nix/store/package.drv")
        self.assertEqual(outputs, [self.bin, self.dev, self.out])

    def test_canonical_output_aliases_are_deduplicated_and_selected_root_is_retained(self):
        alias = Path("/nix/store/package-alias")
        self.installed.add(alias)
        self.entry["storePath"] = alias.as_posix()
        self.entry["outputs"]["alias"] = alias.as_posix()
        with patch.object(providers, "nix_root", side_effect=lambda path: self.bin if path == alias else path):
            root, _, _, outputs = providers.nix_dependency(self.report, "fixture")
        self.assertEqual(root, self.bin)
        self.assertEqual(outputs, [self.bin, self.dev, self.out])


class BazelWorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.small_policy = native.read_json(providers.POLICY)
        self.small_policy["packages"]["native"] = [self.small_policy["packages"]["native"][0]]
        self.small_policy["packages"]["test"] = [self.small_policy["packages"]["test"][0]]
        self.small_policy_path = self.root / "policy.json"
        self.write_json(self.small_policy_path, self.small_policy)

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def test_checkout_paths_and_existing_cmake_trees_are_rejected(self):
        for path in (native.PROJECT_ROOT, native.PROJECT_ROOT / "build/bazel", native.PROJECT_ROOT.parent):
            with self.subTest(path=path), self.assertRaisesRegex(native.BazelWorkspaceError, "outside"):
                workspace.workspace_path(str(path), "debug")
        (self.root / "CMakeCache.txt").touch()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "CMake tree"):
            workspace.workspace_path(str(self.root), "debug")

    @unittest.skipIf(os.name == "nt", "Native Windows aliases are covered by the canonical-anchor regression")
    def test_workspace_path_rejects_checkout_paths_via_source_symlink_alias(self):
        source = self.root / "source"
        source.mkdir()
        alias = self.root / "source-alias"
        alias.symlink_to(source, target_is_directory=True)
        with patch.object(native, "PROJECT_ROOT", alias):
            for selected in (source, source / "build", source.parent):
                with self.subTest(selected=selected), self.assertRaises(native.BazelWorkspaceError):
                    workspace.workspace_path(str(selected), "debug")

    def test_workspace_path_compares_mapped_and_unc_source_anchors_canonically(self):
        canonical = self.root / "canonical-source"
        original_resolve = Path.resolve
        for alias in (Path("Y:/checkout"), Path("//server/share/checkout")):

            def resolve(path, *args, source_alias=alias, **kwargs):
                return canonical if path == source_alias else original_resolve(path, *args, **kwargs)

            with (
                self.subTest(alias=alias),
                patch.object(native, "PROJECT_ROOT", alias),
                patch.object(Path, "resolve", resolve),
            ):
                for selected in (canonical, canonical / "build", canonical.parent):
                    with self.subTest(selected=selected), self.assertRaises(native.BazelWorkspaceError):
                        workspace.workspace_path(str(selected), "debug")

    def test_build_dir_override_is_exact_and_external(self):
        with patch.dict(os.environ, {"BUILD_DIR": str(self.root)}):
            self.assertEqual(workspace.workspace_path(None, "release"), self.root)

    def test_child_environment_drops_all_target_role_flags_not_compiler_identity(self):
        source = {
            "NIX_CFLAGS_COMPILE": "bad",
            "NIX_CFLAGS_COMPILE_FOR_TARGET": "bad",
            "NIX_LDFLAGS_FOR_BUILD": "bad",
            "CPLUS_INCLUDE_PATH": "bad",
            "CCACHE_DIR": "bad",
            "CXXFLAGS": "bad",
            "LIBRARY_PATH": "bad",
            "CXX": "/nix/store/clang++",
            "PATH": "/tools",
        }
        self.assertEqual(native.child_environment(source), {"CXX": "/nix/store/clang++", "PATH": "/tools"})
        self.assertEqual(source["NIX_CFLAGS_COMPILE_FOR_TARGET"], "bad")

    def test_native_setup_rejects_download_hash_before_install(self):
        executable = self.root / "managed/bazel"
        lock = {
            "schemaVersion": 1,
            "version": "9.2.0",
            "artifacts": {
                "darwin-arm64": {"url": "https://example.invalid/bazel", "sha256": "0" * 64},
            },
        }

        def download(url, path):
            Path(path).write_bytes(b"wrong")

        with (
            patch.object(native, "read_json", return_value=lock),
            patch.object(native, "native_platform", return_value="macos"),
            patch.object(workspace, "_artifact_key", return_value="darwin-arm64"),
            patch.object(workspace, "_managed_tool", return_value=executable),
            patch.object(workspace.urllib.request, "urlretrieve", side_effect=download),
            patch.dict(os.environ, {}, clear=True),
        ):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "SHA-256"):
                workspace.bazel_executable(setup=True)
        self.assertFalse(executable.exists())

    def test_native_setup_installs_only_verified_bytes_and_checks_version(self):
        executable = self.root / "managed/bazel"
        payload = b"verified executable"
        lock = {
            "schemaVersion": 1,
            "version": "9.2.0",
            "artifacts": {
                "darwin-arm64": {"url": "https://example.invalid/bazel", "sha256": hashlib.sha256(payload).hexdigest()},
            },
        }
        with (
            patch.object(native, "read_json", return_value=lock),
            patch.object(native, "native_platform", return_value="macos"),
            patch.object(workspace, "_artifact_key", return_value="darwin-arm64"),
            patch.object(workspace, "_managed_tool", return_value=executable),
            patch.object(
                workspace.urllib.request, "urlretrieve", side_effect=lambda url, path: Path(path).write_bytes(payload)
            ),
            patch.object(workspace, "_tool_version") as version,
            patch.object(workspace.os, "replace", wraps=os.replace) as replace,
            patch.dict(os.environ, {}, clear=True),
        ):
            self.assertEqual(workspace.bazel_executable(setup=True), executable)
            self.assertEqual(workspace.bazel_executable(), executable)
        self.assertEqual(executable.read_bytes(), payload)
        self.assertEqual(version.call_count, 2)
        replace.assert_called_once()
        staged, destination = replace.call_args.args
        self.assertEqual(destination, executable)
        self.assertEqual(staged.parent, executable.parent)
        self.assertNotEqual(staged, executable)
        self.assertFalse(staged.exists())

    def test_tool_publication_bad_checksum_preserves_old_or_missing_target(self):
        download = self.root / "download"
        download.write_bytes(b"bad download")
        for existing in (False, True):
            executable = self.root / f"bazel-{existing}"
            if existing:
                executable.write_bytes(b"old binary")
            with patch.object(workspace.os, "replace") as replace:
                with self.assertRaisesRegex(native.BazelWorkspaceError, "SHA-256"):
                    workspace._publish_tool(download, executable, "0" * 64)
            replace.assert_not_called()
            if existing:
                self.assertEqual(executable.read_bytes(), b"old binary")
            else:
                self.assertFalse(executable.exists())
        self.assertEqual(list(self.root.glob(".bazel-stage-*")), [])

    def test_tool_publication_reuses_matching_concurrent_winner(self):
        payload = b"verified binary"
        download, executable = self.root / "download", self.root / "bazel"
        download.write_bytes(payload)

        def competing_publish(staged, destination):
            destination.write_bytes(payload)
            raise PermissionError("concurrent setup started the executable")

        with patch.object(workspace.os, "replace", side_effect=competing_publish):
            workspace._publish_tool(download, executable, hashlib.sha256(payload).hexdigest())
        self.assertEqual(executable.read_bytes(), payload)
        with patch.object(workspace.os, "replace") as replace:
            workspace._publish_tool(download, executable, hashlib.sha256(payload).hexdigest())
        replace.assert_not_called()
        self.assertEqual(list(self.root.glob(".bazel-stage-*")), [])

    def test_write_json_preserves_content_and_does_not_rewrite_equivalent_values(self):
        path = self.root / "nested/target.json"
        payload = {"zeta": "café \u2713\n\x01", "alpha": {"b": 2, "a": [1, 2]}}
        native.write_json(path, payload)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), payload)
        first = path.read_bytes()
        old = 1_000_000_000_000_000_000
        os.utime(path, ns=(old, old))
        sentinel = path.stat().st_mtime_ns
        equivalent = {"alpha": {"a": [1, 2], "b": 2}, "zeta": payload["zeta"]}
        native.write_json(path, equivalent)
        self.assertEqual(path.read_bytes(), first)
        self.assertEqual(path.stat().st_mtime_ns, sentinel)

    def test_write_json_rewrites_changed_values_with_new_bytes_and_timestamp(self):
        path = self.root / "target.json"
        native.write_json(path, {"value": 1})
        first = path.read_bytes()
        old = 1_000_000_000_000_000_000
        os.utime(path, ns=(old, old))
        sentinel = path.stat().st_mtime_ns
        native.write_json(path, {"value": 2})
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"value": 2})
        self.assertNotEqual(path.read_bytes(), first)
        self.assertNotEqual(path.stat().st_mtime_ns, sentinel)

    def provider(self):
        prefix = self.root / "installed/x64-windows"
        (prefix / "include").mkdir(parents=True)
        for config in ("debug", "release"):
            selected = prefix / ("debug" if config == "debug" else "")
            (selected / "lib/pkgconfig").mkdir(parents=True)
            (selected / "bin").mkdir()
        (prefix.parent / "vcpkg").mkdir()
        (prefix.parent / "vcpkg/status").write_text(
            "Package: icu\nVersion: 78.3#1\nArchitecture: x64-windows\nStatus: install ok installed\n\n"
            "Package: icu\nFeature: tools\nArchitecture: x64-windows\nStatus: install ok installed\n\n"
            "Package: catch2\nVersion: 3.99.1\nArchitecture: x64-windows\nStatus: install ok installed\n\n"
            "Package: icu\nVersion: 1.0\nArchitecture: x64-linux\nStatus: install ok installed\n",
            encoding="utf-8",
        )
        host = prefix.parent / "x64-windows"
        tool = host / "tools/pkgconf/pkgconf.exe"
        tool.parent.mkdir(parents=True)
        tool.touch()
        tool.chmod(0o755)
        tool.with_name("pkgconf-8.dll").write_bytes(b"host pkgconf runtime")
        status = prefix.parent / "vcpkg/status"
        with status.open("a", encoding="utf-8") as stream:
            stream.write(
                "\n\nPackage: pkgconf\nVersion: 3.0.3\nArchitecture: x64-windows\nStatus: install ok installed\n"
            )
        info = prefix.parent / "vcpkg/info"
        info.mkdir()
        (info / "pkgconf_3.0.3_x64-windows.list").write_text(
            "x64-windows/tools/pkgconf/pkgconf.exe\nx64-windows/tools/pkgconf/pkgconf-8.dll\n", encoding="utf-8"
        )
        for relative, text in {
            "unicode/uvernum.h": "#define U_ICU_VERSION_MAJOR_NUM 78\n#define U_ICU_VERSION_MINOR_NUM 3\n",
            "unicode/platform.h": "#if defined(U_STATIC_IMPLEMENTATION)\n#elif 1\n#   define U_EXPORT\n#endif\n",
            "unicode/utypes.h": (
                "#if defined(U_COMBINED_IMPLEMENTATION)\n#define U_DATA_API U_EXPORT\n#elif 1\n"
                "#define U_DATA_API\n#define U_COMMON_API\n#define U_I18N_API\n#define U_LAYOUT_API\n"
                "#define U_LAYOUTEX_API\n#define U_IO_API\n#define U_TOOLUTIL_API\n"
                "#elif defined(U_COMMON_IMPLEMENTATION)\n#endif\n"
            ),
            "catch2/catch_version_macros.hpp": (
                "#define CATCH_VERSION_MAJOR 3\n#define CATCH_VERSION_MINOR 99\n#define CATCH_VERSION_PATCH 1\n"
            ),
        }.items():
            marker = prefix / "include" / relative
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text(text)
        return prefix

    def nix_entry(self, root, version, outputs):
        return {
            "version": version,
            "storePath": "/nix/store/" + root.name,
            "drvPath": "/nix/store/" + root.name + ".drv",
            "outputs": {name: "/nix/store/" + path.name for name, path in outputs.items()},
        }

    @contextmanager
    def nix_paths(self, roots):
        paths = {Path("/nix/store") / root.name: root for root in roots}
        original_is_dir = Path.is_dir

        def is_dir(path):
            if path.parent == Path("/nix/store"):
                return path in paths and original_is_dir(paths[path])
            return original_is_dir(path)

        def store_root(path):
            resolved = paths.get(path, path).resolve()
            return next(root for root in roots if resolved.is_relative_to(root))

        with patch.object(Path, "is_dir", is_dir), patch.object(providers, "nix_root", side_effect=store_root):
            yield

    def linux_provider(self, *, wrong_metadata=False, split_library_output=False):
        icu, dev, catch, other = (
            self.root / name for name in ("icu-out", "icu-dev", "catch2", "other-icu-same-version")
        )
        report_path = self.root / "nix-report.json"
        report = {
            "schemaVersion": 1,
            "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
            "dependencies": {
                "icu": self.nix_entry(icu, "78.3", {"out": icu, "dev": dev}),
                "catch2": self.nix_entry(catch, "3.16.0", {"out": catch}),
            },
        }
        self.write_json(report_path, report)
        roots = [icu, dev, catch, other]
        for root in roots:
            (root / "lib").mkdir(parents=True, exist_ok=True)
        for root in (dev, catch, other):
            (root / "lib/pkgconfig").mkdir(exist_ok=True)

        def query(argv, **kwargs):
            self.assertEqual(argv[0], "/tools/pkg-config")
            variable, module = argv[1].removeprefix("--variable="), argv[2]
            if module == "catch2-with-main":
                root = catch
            elif variable == "pcfiledir":
                root = catch if wrong_metadata == "catch2" else other if wrong_metadata else dev
            elif variable == "libdir":
                root = dev if split_library_output else icu
            else:
                root = dev
            suffix = {"pcfiledir": "lib/pkgconfig", "libdir": "lib", "includedir": "include"}[variable]
            return subprocess.CompletedProcess(argv, 0, str(root / suffix))

        with (
            patch.dict(
                os.environ,
                {
                    "AOBUS_NIX_DEPENDENCY_REPORT": str(report_path),
                    "PKG_CONFIG_PATH_FOR_TARGET": str(other / "lib/pkgconfig"),
                },
            ),
            patch.object(
                native,
                "executable",
                side_effect=lambda name, **kwargs: {"pkg-config": "/tools/pkg-config", "readelf": "/tools/readelf"}[
                    name
                ],
            ),
            self.nix_paths(roots),
            patch.object(providers.subprocess, "run", side_effect=query),
        ):
            return providers.linux_provider(self.small_policy, identity_path=self.root / "identity.json")

    def test_linux_provider_whitelists_only_package_report_outputs_and_metadata(self):
        base, versions, expected = self.linux_provider()
        self.assertEqual(versions["icu"], "78.3")
        self.assertEqual(expected["icu"], [self.root / "icu-dev", self.root / "icu-out"])
        self.assertNotIn(str(self.root / "other-icu-same-version"), base["allowed_roots"])
        self.assertEqual(
            base["pkg_config"]["pc_dirs"],
            sorted(
                [
                    str(self.root / "icu-dev/lib/pkgconfig"),
                    str(self.root / "catch2/lib/pkgconfig"),
                ]
            ),
        )
        identity = json.loads((self.root / "identity.json").read_text())
        self.assertEqual(identity["packages"]["icu"]["deriver"], "/nix/store/icu-out.drv")
        self.assertEqual(identity["packages"]["icu"]["outputs"], [str(path) for path in expected["icu"]])
        self.assertEqual(identity["packages"]["icu"]["storePath"], str(self.root / "icu-out"))

    def test_linux_provider_accepts_split_dev_library_directory_from_reported_outputs(self):
        base, _, expected = self.linux_provider(split_library_output=True)
        self.assertIn(str(self.root / "icu-dev/lib"), base["library_dirs"])
        self.assertEqual(expected["icu"], [self.root / "icu-dev", self.root / "icu-out"])

    def test_linux_resolution_metadata_uses_only_selected_group_output_union(self):
        root, transitive, other_group = (self.root / name for name in ("icu", "lmdb", "catch2"))
        manifest = {"packages": [], "modules": {"transitive": {"metadata": str(transitive / "lib/pkgconfig/lmdb.pc")}}}
        with self.nix_paths([root, transitive, other_group]):
            providers.verify_linux_resolution(manifest, {"icu": [root], "lmdb": [transitive]})
            manifest["modules"]["transitive"]["metadata"] = str(other_group / "lib/pkgconfig/catch2.pc")
            with self.assertRaisesRegex(native.BazelWorkspaceError, "resolved metadata"):
                providers.verify_linux_resolution(manifest, {"icu": [root], "lmdb": [transitive]})

    @unittest.skipUnless(sys.platform == "linux", "Nix multicall dispatch is a Linux provider boundary")
    def test_executable_preserves_pkg_config_multicall_symlink_invocation_name(self):
        executable = self.root / "tools/nix"
        executable.parent.mkdir()
        executable.write_text(
            f"#!{sys.executable}\nimport pathlib, sys\n"
            "assert pathlib.Path(sys.argv[0]).name == 'pkg-config'\n"
            "assert sys.argv[1:] == ['--version']\n"
            "print('1.0')\n",
            encoding="utf-8",
        )
        executable.chmod(0o755)
        alias = executable.with_name("pkg-config")
        alias.symlink_to(executable.name)
        with patch.object(native.shutil, "which", return_value=str(alias)):
            selected = native.executable("pkg-config")
        self.assertEqual(selected, str(alias))
        result = subprocess.run([selected, "--version"], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1.0\n")

    def test_tree_selects_its_provisioned_triplet_and_never_cmake_metadata(self):
        prefix = self.provider()
        cmake = self.root / "cmake-debug"
        cmake.mkdir()
        (cmake / "CMakeCache.txt").write_text(
            f"VCPKG_INSTALLED_DIR:PATH={prefix.parent}\nVCPKG_TARGET_TRIPLET:STRING={prefix.name}\n", encoding="utf-8"
        )
        tree = self.root / "bazel-debug"
        # The adjacent CMake tree is never consulted: the Bazel tree selects
        # its own provisioned triplet and reads no CMake metadata.
        with patch.object(native, "native_platform", return_value="windows"):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "run `ao deps provision`"):
                providers.vcpkg_prefix(None, tree)
            tree.mkdir()
            prefix.parent.rename(tree / "vcpkg_installed")
            self.assertEqual(providers.vcpkg_prefix(None, tree), (tree / "vcpkg_installed/x64-windows").resolve())

    def test_vcpkg_triplets_are_owned_by_the_native_provider_selection(self):
        with patch.object(native, "native_platform", return_value="windows"):
            self.assertEqual(providers.vcpkg_triplets(), ("x64-windows", "x64-windows"))
        with patch.object(native, "native_platform", return_value="macos"):
            with patch.dict(providers.os.environ, {"AOBUS_VCPKG_TRIPLET": "arm64-aobus-osx"}):
                self.assertEqual(providers.vcpkg_triplets(), ("arm64-aobus-osx", "arm64-aobus-osx"))
            with patch.dict(providers.os.environ, clear=True):
                with self.assertRaisesRegex(native.BazelWorkspaceError, "AOBUS_VCPKG_TRIPLET"):
                    providers.vcpkg_triplets()
        with patch.object(native, "native_platform", return_value="linux"):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "Nix"):
                providers.vcpkg_triplets()

    def test_native_provider_filters_host_triplet_and_selects_debug_paths(self):
        prefix = self.provider()
        base, versions = providers.native_provider(
            prefix, "debug", "windows", identity_path=self.root / "provider-identity.json", policy=self.small_policy
        )
        self.assertEqual(versions, {"icu": "78.3", "catch2": "3.99.1"})
        self.assertEqual(base["library_dirs"], [str(prefix / "debug/lib")])
        self.assertEqual(base["library_roots"], [str(prefix / "debug/lib")])
        self.assertEqual(base["runtime_roots"], [str(prefix / "debug/bin")])
        self.assertNotIn(str(prefix), base["allowed_roots"])
        self.assertIn(str(prefix / "include"), base["allowed_roots"])
        self.assertNotIn(str(prefix / "lib"), base["allowed_roots"])
        self.assertEqual(base["pkg_config"]["executable"], str(prefix.parent / "x64-windows/tools/pkgconf/pkgconf.exe"))
        self.assertTrue(base["provider"]["version"].startswith(f"{prefix}:"))
        identity = json.loads((self.root / "provider-identity.json").read_text())
        self.assertEqual(identity["packages"]["icu"]["features"], ["core", "tools"])
        self.assertEqual(identity["prefix"], str(prefix))

    def test_macos_runtime_patch_removes_only_reviewed_trailing_implicit_libraries(self):
        policy = native.read_json(providers.POLICY)["macos_icu_runtime_remove"]
        spec = {"platform": "macos", "config": "Release", "provider": {"name": "vcpkg", "version": "prefix:status"}}
        with patch.object(dependencies, "_metadata_flags", return_value=["-licuuc", "-lc++", "-lpthread", "-lm"]):
            result = dependencies._macos_icu_runtime_patch(spec, policy)
        self.assertEqual(result["when"]["provider_version"], "prefix:status")
        self.assertEqual(
            result["remove"]["libraries"],
            [
                {"name": "c++", "kind": "system"},
                {"name": "pthread", "kind": "system"},
                {"name": "m", "kind": "system"},
            ],
        )
        self.assertEqual(result["expect"]["present"], result["remove"])
        with patch.object(dependencies, "_metadata_flags", return_value=["-licuuc", "-lunknown"]):
            self.assertIsNone(dependencies._macos_icu_runtime_patch(spec, policy))
        with patch.object(dependencies, "_metadata_flags", return_value=["-lc++", "-licuuc"]):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "interleaving"):
                dependencies._macos_icu_runtime_patch(spec, policy)

    def test_refresh_rechecks_header_membership_against_current_ownership_inventory(self):
        prefix = self.provider()
        for name in ("manual-link/Catch2Maind.lib", "Catch2d.lib"):
            path = prefix / "debug/lib" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(static_archive("debug"))
        info = prefix.parent / "vcpkg/info"
        info.mkdir(exist_ok=True)
        inventory = info / f"catch2_3.99.1_{prefix.name}.list"
        inventory.write_text(
            f"{prefix.name}/include/catch2/catch_version_macros.hpp\n{prefix.name}/debug/lib/manual-link/Catch2Maind.lib\n{prefix.name}/debug/lib/Catch2d.lib\n",
            encoding="utf-8",
        )
        tree = self.root / "bazel"
        observed = []
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(providers, "POLICY", self.small_policy_path),
            native.capture_probes(observed.append),
        ):
            first = dependencies.resolve_dependencies(tree, "debug", prefix=str(prefix), group="test")
            added = prefix / "include/new-header.h"
            added.touch()
            unchanged = dependencies.resolve_dependencies(tree, "debug", prefix=str(prefix), group="test")
            self.assertEqual(first["manifest"], unchanged["manifest"])
            self.assertEqual(first["packages"], unchanged["packages"])
            with inventory.open("a", encoding="utf-8") as stream:
                stream.write(f"{prefix.name}/include/new-header.h\n")
            resolution = dependencies.resolve_dependencies(tree, "debug", prefix=str(prefix), group="test")
        manifest = resolution["manifest"]
        self.assertEqual(manifest["packages"][0]["declared_headers"], 2)
        self.assertEqual(manifest["applied_patches"], ["catch2-static-fallback"])
        self.assertEqual(
            manifest["packages"][0]["resolved_libraries"][0]["file"],
            str(prefix / "debug/lib/manual-link/Catch2Maind.lib"),
        )
        self.assertIn(
            str(added),
            [
                file["source"]
                for package in resolution["packages"]
                for header in package["headers"]
                for file in header["files"]
            ],
        )
        self.assertIn(str(inventory), resolution["watch_files"])
        self.assertIn(prefix / "debug/lib/pkgconfig/catch2-with-main.pc", observed)
        self.assertIn(str(prefix / "debug/lib"), resolution["watch_directories"])
        self.assertFalse((tree / "generated").exists())

    def installed_artifacts(self, root, names, *, shared=True):
        (root / "include").mkdir(parents=True, exist_ok=True)
        for config in ("debug", "release"):
            selected = root / ("debug" if config == "debug" else "")
            for name in names:
                artifacts = [f"lib/{name}.lib", f"bin/{name}.dll"] if shared else [f"lib/{name}.lib"]
                for relative in artifacts:
                    path = selected / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()

    def test_windows_shared_fallback_selects_exact_config_before_binary_pair_admission(self):
        root = self.root / "triplet"
        self.installed_artifacts(root, ["lmdb"], shared=False)
        (root / "include/lmdb.h").touch()
        policy = native.read_json(providers.POLICY)
        for config in ("debug", "release"):
            interface = dependencies._library_interface({"name": "lmdb"}, [root], config, "windows", policy)
            selected = root / ("debug" if config == "debug" else "")
            self.assertEqual(
                interface["libraries"], [{"name": "lmdb", "file": str(selected / "lib/lmdb.lib"), "kind": "shared"}]
            )
            runtime = selected / "bin/lmdb.dll"
            runtime.parent.mkdir(parents=True, exist_ok=True)
            runtime.touch()
            # Names alone do not admit this DLL; the later COFF/PE/ownership join does.
            with self.subTest(config=config):
                self.assertEqual(
                    dependencies._library_interface({"name": "lmdb"}, [root], config, "windows", policy), interface
                )
            runtime.unlink()
        (root / "lib/lmdb.lib").unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
            dependencies._library_interface({"name": "lmdb"}, [root], "release", "windows", policy)

    def test_windows_rapidyaml_fallback_uses_reviewed_static_c4core_without_cmake(self):
        root = self.root / "triplet"
        self.installed_artifacts(root, ["ryml", "c4core"], shared=False)
        (root / "include/ryml").mkdir()
        (root / "include/ryml/ryml.hpp").touch()
        policy = native.read_json(providers.POLICY)
        for config in ("debug", "release"):
            with (
                self.subTest(config=config),
                patch.object(
                    Path, "read_text", side_effect=AssertionError("artifact fallback must not read CMake exports")
                ),
            ):
                interface = dependencies._library_interface({"name": "rapidyaml"}, [root], config, "windows", policy)
            selected = root / ("debug" if config == "debug" else "")
            self.assertEqual(interface["header_dirs"], [str(root / "include"), str(root / "include/ryml")])
            self.assertEqual(interface["defines"], ["C4_CPP=17"])
            self.assertEqual(
                interface["libraries"],
                [
                    {"name": name, "kind": "static", "file": str(selected / "lib" / f"{name}.lib")}
                    for name in ("ryml", "c4core")
                ],
            )

    def gperf_report(self):
        root = self.root / "nix-gperf"
        executable = root / "bin/gperf"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"gperf bytes")
        executable.chmod(0o755)
        report = self.root / "nix-report.json"
        self.write_json(
            report,
            {
                "schemaVersion": 1,
                "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
                "dependencies": {"gperf": self.nix_entry(root, "3.3", {"out": root})},
            },
        )
        return root, executable, report

    def test_linux_gperf_uses_report_root_and_deriver_without_ambient_lookup(self):
        root, executable, report = self.gperf_report()
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.nix_paths([root]),
            patch.object(native.shutil, "which", side_effect=AssertionError("no ambient gperf lookup")),
        ):
            identity, selected, runtime, evidence = host_tools._linux_gperf()
            self.assertEqual(selected, executable)
            self.assertEqual(runtime, [])
            self.assertEqual(identity["deriver"], "/nix/store/nix-gperf.drv")
            self.assertEqual(identity["version"], "3.3")
            self.assertIn(report, evidence)
            executable.unlink()
            with self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the Nix report"):
                host_tools._linux_gperf()
        with patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(native.BazelWorkspaceError, "report"):
            host_tools._linux_gperf()
        value = native.read_json(report)
        value["dependencies"].clear()
        self.write_json(report, value)
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.assertRaisesRegex(native.BazelWorkspaceError, "gperf output identity.*re-enter"),
        ):
            host_tools._linux_gperf()

    def native_gperf(self, host="windows"):
        installed = self.root / "installed"
        target = installed / ("x64-windows" if host == "windows" else "arm64-target")
        triplet = "x64-windows" if host == "windows" else "x64-aobus-osx"
        directory = installed / triplet / "tools/gperf"
        directory.mkdir(parents=True)
        executable = directory / ("gperf.exe" if host == "windows" else "gperf")
        executable.write_bytes(b"host gperf")
        executable.chmod(0o755)
        status = installed / "vcpkg/status"
        status.parent.mkdir()
        status.write_text(
            f"Package: gperf\nVersion: 3.3#1\nArchitecture: {triplet}\nStatus: install ok installed\n\n"
            "Package: gperf\nVersion: 3.1\nArchitecture: unused-triplet\nStatus: deinstall ok not-installed\n",
            encoding="utf-8",
        )
        info = installed / "vcpkg/info"
        info.mkdir()
        (info / f"gperf_3.3_{triplet}.list").write_text(f"{triplet}/tools/gperf/{executable.name}\n", encoding="utf-8")
        return target, executable, status

    def test_native_gperf_uses_unique_host_port_status_not_target_triplet_or_path(self):
        for host in ("windows", "macos"):
            with self.subTest(host=host):
                target, executable, status = self.native_gperf(host)
                runtime = executable.with_name("adjacent.dll")
                if host == "windows":
                    runtime.write_bytes(b"required runtime")
                    ownership = status.parent / "info/gperf_3.3_x64-windows.list"
                    with ownership.open("a", encoding="utf-8") as stream:
                        stream.write("x64-windows/tools/gperf/adjacent.dll\n")
                unrelated = executable.parents[2] / "bin/ambient.dll"
                unrelated.parent.mkdir()
                unrelated.touch()
                with (
                    patch.object(native.shutil, "which", side_effect=AssertionError("no ambient fallback")),
                    patch.object(native, "host_machine", return_value="AMD64"),
                    patch.object(native.platform, "machine", side_effect=AssertionError("no uname/WMI query")),
                ):
                    identity, selected, dlls, evidence = host_tools._native_gperf(target, host)
                self.assertEqual(identity["host_machine"], "AMD64")
                self.assertEqual(selected, executable)
                if host == "windows":
                    self.assertEqual(identity["triplet"], target.name)
                else:
                    self.assertNotEqual(identity["triplet"], target.name)
                self.assertEqual(identity["version"], "3.3")
                self.assertEqual(identity["status_version"], "3.3#1")
                self.assertIn("unverified", identity["provenance"])
                self.assertEqual(dlls, [runtime] if host == "windows" else [])
                self.assertIn(status, evidence)
                shutil.rmtree(self.root / "installed")

    def resolve_gperf(self, executable, runtime, evidence):
        request = self.root / "request.json"
        self.write_json(request, {"schema": 1, "host": "linux"})
        output = self.root / "repository/_resolution"
        fixtures = self.root / "tool-inputs"
        for name in (
            "bin/genrb",
            "bin/pkgdata",
            "bin/dbus-daemon",
            "bin/glib-compile-resources",
            "lib/libglib-2.0.so",
            "glib-report.json",
            "lib/libdbus-1.so",
            "stdlib.py",
            "dbus-report.json",
            "catalog-sdk.json",
            "debug/libicuuc.a",
            "debug/status",
            "release/libicuuc.a",
            "release/status",
        ):
            path = fixtures / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"independent tool SDK fixture\n")
        for config in ("debug", "release"):
            (fixtures / config / "pkgconfig").mkdir(exist_ok=True)
        # Keep the resolver's audit hook isolated from the unittest process.
        # Each new consumer has an authored subplan/SDK selection, independent
        # of gperf. Production provider guards are not changed for this fixture.
        controller = textwrap.dedent(
            """\
            import json
            import sys
            from pathlib import Path
            from unittest.mock import patch
            sys.path.insert(0, sys.argv.pop(1))
            from ao.core import bazel_resolver
            from ao.core import bazel_native as native
            from ao.core import bazel_dependencies as dependencies
            from ao.core import bazel_host_tools as host_tools
            selection = ({'version': '3.3'}, Path(sys.argv.pop(1)),
                         [Path(p) for p in sys.argv.pop(1).split('|') if p], [Path(sys.argv.pop(1))])
            fixtures = Path(sys.argv.pop(1))

            def catalog(tree, config, *, prefix, group):
                assert prefix is None and group == 'catalog'
                return {
                    'packages': [{'name': 'icu', 'headers': [], 'includes': [], 'defines': [],
                                  'linkopts': [], 'runtime': [], 'libraries': [
                                      {'index': 0, 'kind': 'static', 'source': str(fixtures / config / 'libicuuc.a'),
                                       'basename': 'libicuuc.a', 'runtime': None}]}],
                    'manifest': {'config': config, 'package': 'icu'},
                    'watch_files': [str(fixtures / config / 'status')],
                    'watch_directories': [str(fixtures / config / 'pkgconfig')],
                }

            sdk = (
                {'python': sys.executable, 'genrb': str(fixtures / 'bin/genrb'),
                 'pkgdata': str(fixtures / 'bin/pkgdata')},
                [Path(sys.executable), fixtures / 'stdlib.py', fixtures / 'bin/genrb',
                 fixtures / 'bin/pkgdata', fixtures / 'catalog-sdk.json'],
                [fixtures / 'catalog-sdk.json'],
            )
            dbus = ({'provider': 'fixture', 'outputs': [str(fixtures)]}, fixtures / 'bin/dbus-daemon',
                    [fixtures / 'lib/libdbus-1.so'], [fixtures / 'dbus-report.json'])
            resource = ({'provider': 'fixture', 'outputs': [str(fixtures)]}, fixtures / 'bin/glib-compile-resources',
                        [fixtures / 'lib/libglib-2.0.so'], [fixtures / 'glib-report.json'])
            with (
                patch.object(native, 'native_platform', return_value='linux'),
                patch.object(host_tools, '_linux_gperf', return_value=selection),
                patch.object(dependencies, 'resolve_dependencies', side_effect=catalog) as imports,
                patch.object(host_tools, '_catalog_sdk', return_value=sdk),
                patch.object(host_tools, '_linux_dbus', return_value=dbus),
                patch.object(host_tools, '_linux_gresource', return_value=resource),
                patch.object(host_tools, '_app_version', return_value='1.2.3'),
            ):
                status = bazel_resolver.main()
                if status == 0:
                    assert [call.args[1] for call in imports.call_args_list] == ['debug', 'release']
                raise SystemExit(status)
            """
        )
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                "-B",
                "-c",
                controller,
                str(native.PROJECT_ROOT / "script"),
                str(executable),
                "|".join(map(str, runtime)),
                str(evidence),
                str(fixtures),
                "--request",
                str(request),
                "--group",
                "tools",
                "--output",
                str(output),
                "--environment-json",
                json.dumps({name: os.environ[name] for name in ("PATH", "SYSTEMROOT", "WINDIR") if name in os.environ}),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.stderr, "")
        return result.returncode, native.read_json(output / "repository.json")

    @unittest.skipIf(os.name == "nt", "POSIX executable fixture; native Windows tools need native validation")
    def test_gperf_repository_plan_declares_runtime_and_evidence_watches_without_copying(self):
        _, executable, report = self.gperf_report()
        executable.write_text(
            f"#!{sys.executable}\nimport sys\nassert sys.argv[1:] == ['--version']\nprint('GNU gperf 3.3')\n"
        )
        runtime = executable.with_name("dependency.dll")
        runtime.write_bytes(b"runtime one")
        status, plan = self.resolve_gperf(executable, [runtime], report)
        self.assertEqual(status, 0, plan.get("error"))
        facts = plan["host_tools"]
        self.assertEqual(facts["gperf"]["executable"], str(executable))
        self.assertEqual(facts["gperf"]["runtime"], [str(runtime)])
        self.assertEqual(facts["gperf"]["evidence"], [str(report)])
        self.assertEqual(facts["gperf"]["identity"], {"version": "3.3"})
        self.assertNotIn("links", plan)
        self.assertNotIn("files", plan)
        self.assertLessEqual({str(executable), str(runtime), str(report)}, set(plan["watch_files"]))
        self.assertIn(str(executable.parent), plan["watch_directories"])
        fixtures = self.root / "tool-inputs"
        for config in ("debug", "release"):
            admitted = plan["imports"][config]
            self.assertNotIn("config", admitted)
            self.assertEqual(admitted["packages"][0]["libraries"][0]["source"], str(fixtures / config / "libicuuc.a"))
            self.assertIn(str(fixtures / config / "status"), plan["watch_files"])
            self.assertIn(str(fixtures / config / "pkgconfig"), plan["watch_directories"])
            self.assertEqual(
                admitted["manifest"],
                {"config": config, "package": "icu"},
            )
        self.assertLessEqual(
            {str(fixtures / "stdlib.py"), str(fixtures / "catalog-sdk.json")},
            set(facts["catalog"]["artifacts"]),
        )
        self.assertEqual(facts["catalog"]["evidence"], [str(fixtures / "catalog-sdk.json")])
        self.assertEqual(
            set(facts["dbus"]["artifacts"]),
            {str(fixtures / name) for name in ("bin/dbus-daemon", "lib/libdbus-1.so", "dbus-report.json")},
        )
        self.assertEqual(facts["app_version"], "1.2.3")
        self.assertEqual(
            facts["test_tools"],
            {
                "dbus_daemon": str(fixtures / "bin/dbus-daemon"),
                "dbus_daemon_directory": str(fixtures / "bin"),
            },
        )
        self.assertFalse((self.root / "repository/bin").exists())
        status, plan = self.resolve_gperf(executable, [], report)
        self.assertEqual(status, 0, plan.get("error"))
        self.assertEqual(plan["host_tools"]["gperf"]["executable"], str(executable))
        self.assertEqual(plan["host_tools"]["gperf"]["runtime"], [])
        self.assertNotIn("links", plan)
        self.assertNotIn("files", plan)
        self.assertNotIn(str(runtime), plan["watch_files"])

    def test_windows_execute_rejects_network_sources_before_launch_or_log_truncation(self):
        log = self.root / "build.log"
        cases = (
            (r"\\server\share\checkout", r"\\server\share\checkout"),
            (r"Y:\checkout", r"\\server\share\checkout"),
            (r"Y:\checkout", r"\\?\UNC\server\share\checkout"),
        )
        for declared, canonical in cases:
            source = Mock(spec=Path, name=declared)
            source.resolve.return_value = PureWindowsPath(canonical)
            log.write_bytes(b"preserved failure evidence")
            with (
                self.subTest(declared=declared, canonical=canonical),
                patch.object(native, "native_platform", return_value="windows"),
                patch.object(native, "PROJECT_ROOT", source),
                patch.object(workspace.subprocess, "run") as run,
                self.assertRaisesRegex(native.BazelWorkspaceError, "require a local source checkout"),
            ):
                workspace.execute(Path("bazel.exe"), self.root, self.root / "no-generated-inputs", "build", "debug", [])
            run.assert_not_called()
            self.assertEqual(log.read_bytes(), b"preserved failure evidence")

    def test_windows_execute_allows_normal_and_extended_length_local_source_paths(self):
        generated = self.root / "configuration"
        for canonical in (r"C:\checkout", r"\\?\C:\checkout"):
            source = Mock(spec=Path)
            source.resolve.return_value = PureWindowsPath(canonical)
            with (
                self.subTest(canonical=canonical),
                patch.object(native, "native_platform", return_value="windows"),
                patch.object(native, "PROJECT_ROOT", source),
                patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
            ):
                self.assertEqual(
                    workspace.execute(Path("bazel.exe"), self.root, generated, "build", "debug", ["//lib:lib"]), 0
                )
            run.assert_called_once()

    def test_windows_execute_only_launches_bazel_and_forwards_its_status_and_log(self):
        generated = self.root / "configuration"
        # Stale peer outputs must not cause the launcher to inspect any executable.
        binary_dir = self.root / "bazel-bin/test"
        binary_dir.mkdir(parents=True)
        (binary_dir / "utility_test.exe").write_bytes(b"stale peer")
        for status in (0, 7):

            def launch(argv, status=status, **kwargs):
                kwargs["stdout"].write(b"Bazel owns manifest validation\n")
                return subprocess.CompletedProcess(argv, status)

            with (
                self.subTest(status=status),
                patch.object(native, "native_platform", return_value="windows"),
                patch.object(native, "PROJECT_ROOT", self.root),
                patch.object(workspace.subprocess, "run", side_effect=launch) as run,
                patch("sys.stdout", new_callable=io.StringIO) as output,
            ):
                self.assertEqual(
                    workspace.execute(Path("bazel.exe"), self.root, generated, "build", "debug", ["//custom:test"]),
                    status,
                )
            run.assert_called_once()
            self.assertEqual(run.call_args.args[0][0], "bazel.exe")
            self.assertEqual(run.call_args.args[0][-1], "//custom:test")
            self.assertEqual(run.call_args.kwargs["cwd"], self.root)
            self.assertEqual(run.call_args.kwargs["stderr"], subprocess.STDOUT)
            self.assertEqual((self.root / "build.log").read_bytes(), b"Bazel owns manifest validation\n")
            self.assertEqual("Bazel owns manifest validation" in output.getvalue(), bool(status))
        self.assertEqual((binary_dir / "utility_test.exe").read_bytes(), b"stale peer")

    def test_execute_uses_prepared_rc_and_clean_environment_without_global_toolchain_action_env(self):
        generated = self.root / "configuration"
        executable = Path("/tools/bazel")
        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(
                native,
                "executable",
                side_effect=AssertionError("execution must use prepared toolchain configuration"),
            ),
            patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)) as run,
            patch.dict(os.environ, {"NIX_CFLAGS_COMPILE_FOR_TARGET": "bad"}),
        ):
            status = workspace.execute(executable, self.root, generated, "build", "release", ["//lib:lib"])
        self.assertEqual(status, 7)
        argv = run.call_args.args[0]
        self.assertEqual(argv[:4], [str(executable), f"--bazelrc={generated / 'aobus.bazelrc'}", "build", "--jobs=2"])
        self.assertEqual(argv[-1], "//lib:lib")
        self.assertNotIn("--batch", argv)
        self.assertFalse(
            any(arg.startswith(("--repo_env=", "--action_env=", "--config=", "--output_user_root=")) for arg in argv)
        )
        self.assertFalse(any(arg.startswith("--remote_cache") for arg in argv))
        self.assertNotIn("NIX_CFLAGS_COMPILE_FOR_TARGET", run.call_args.kwargs["env"])

    def test_execute_preserves_forwarded_options_and_remote_cache(self):
        prepared = self.root / "configuration"
        arguments = ["--test_filter=Decoder*", "--noremote_upload_local_results"]
        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
            patch("sys.stdout"),
        ):
            self.assertEqual(
                workspace.execute(
                    Path("bazel"),
                    self.root,
                    prepared,
                    "build",
                    "debug",
                    arguments,
                    jobs=3,
                    remote_cache="grpcs://cache.example",
                ),
                0,
            )
        argv = run.call_args.args[0]
        self.assertIn("--jobs=3", argv)
        self.assertIn("--remote_cache=grpcs://cache.example", argv)
        self.assertEqual(argv[-2:], arguments)
        self.assertEqual(arguments, ["--test_filter=Decoder*", "--noremote_upload_local_results"])

    def test_execute_admits_only_the_live_build_and_clean_consumers(self):
        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(workspace.subprocess, "run") as run,
        ):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "requires explicit targets"):
                workspace.execute(Path("bazel"), self.root, self.root / "configuration", "build", "debug", [])
            with self.assertRaisesRegex(native.BazelWorkspaceError, "does not accept expunge"):
                workspace.execute(
                    Path("bazel"), self.root, self.root / "configuration", "clean", "debug", ["--expunge"]
                )
            with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported portal build action"):
                workspace.execute(Path("bazel"), self.root, self.root / "configuration", "query", "debug", ["//..."])
        run.assert_not_called()

    @unittest.skipIf(os.name == "nt", "POSIX executable process boundary; Windows needs native Bazel validation")
    def test_real_child_receives_clean_environment_and_failure_is_logged(self):
        generated = self.root / "configuration"
        executable = self.root / "fake-bazel"
        executable.write_text(
            f"#!{sys.executable}\nimport os, sys\n"
            "assert 'NIX_CFLAGS_COMPILE_FOR_TARGET' not in os.environ\n"
            "print('actual child ran')\nsys.exit(9)\n",
            encoding="utf-8",
        )
        executable.chmod(0o755)
        with patch.dict(os.environ, {"NIX_CFLAGS_COMPILE_FOR_TARGET": "bad"}):
            self.assertEqual(workspace.execute(executable, self.root, generated, "build", "debug", ["//lib:lib"]), 9)
        self.assertEqual((self.root / "build.log").read_text(), "actual child ran\n")


if __name__ == "__main__":
    unittest.main()
