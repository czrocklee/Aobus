"""SDK header observations retain each package's topology and ownership boundary."""

import hashlib
import os
import unittest
from unittest import mock

import test_bazel_deps as adapter_tests

from ao.portal import bazel_deps as g


class BazelSharedHeadersTest(unittest.TestCase):
    def setUp(self):
        # Compose the adapter fixture without inheriting/rerunning its cases.
        self.fixture = adapter_tests.BazelDependencyTest()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()

    def add_package(self, name, directories, **interface):
        fixture = self.fixture
        fixture.spec["packages"].append({"name": name, "modules": []})
        change = fixture.patch(
            {"header_dirs": [str(path) for path in directories], **interface}, requirements=["manual_interface"]
        )
        change["id"] = f"complete-{name}"
        change["when"]["package"] = name
        change["when"]["provider"] = fixture.spec["provider"]["name"]

    def headers(self, name):
        return {file["source"] for tree in self.fixture.package(name)["headers"] for file in tree["files"]}

    def test_equal_roots_keep_package_interfaces_and_sparse_header_ownership_distinct(self):
        fixture = self.fixture
        archive = fixture.lib.with_name("libbar.a")
        archive.write_bytes(b"bar archive")
        self.add_package(
            "bar",
            [fixture.header.parent],
            defines=["BAR_API=2"],
            libraries=[{"name": "bar", "kind": "static", "file": str(archive)}],
        )
        self.add_package("baz", [fixture.header.parent], defines=["BAZ_API=3"])
        manifest = fixture.generate()
        for name, defines in (("foo", ["FOO_API=1"]), ("bar", ["BAR_API=2"]), ("baz", ["BAZ_API=3"])):
            self.assertEqual(fixture.package(name)["defines"], defines)
            self.assertEqual(self.headers(name), {str(fixture.header)})
        self.assertEqual(fixture.package("foo")["libraries"][0]["source"], str(fixture.lib))
        self.assertEqual(fixture.package("bar")["libraries"][0]["source"], str(archive))
        self.assertEqual(fixture.package("baz")["libraries"], [])
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [1, 1, 1])
        self.assertEqual(manifest["counts"], {"automatic": 1, "manual_fallback": 2})
        self.assertFalse(fixture.out.exists())

    def test_shared_roots_preserve_each_packages_search_order(self):
        fixture = self.fixture
        other = fixture.provider / "other-include"
        other.mkdir()
        (other / "foo.h").write_bytes(b"other implementation")
        fixture.cflags += f' -I"{other}"'
        fixture.write_pc()
        self.add_package("bar", [other, fixture.header.parent])
        fixture.generate()
        for name, expected in (("foo", [fixture.header.parent, other]), ("bar", [other, fixture.header.parent])):
            self.assertEqual([fixture.include_root(name, index) for index in range(2)], expected)
        self.assertEqual(self.headers("foo"), self.headers("bar"))

    def test_distinct_roots_do_not_merge_just_because_their_bytes_match(self):
        fixture = self.fixture
        other = fixture.provider / "other-include"
        other.mkdir()
        other_header = other / "foo.h"
        other_header.write_bytes(fixture.header.read_bytes())
        self.add_package("bar", [other])
        fixture.generate()
        self.assertNotEqual(fixture.package("foo")["headers"], fixture.package("bar")["headers"])
        self.assertEqual(fixture.include_root("bar"), other)
        self.assertEqual(self.headers("bar"), {str(other_header)})

    def check_child_only_interface(self, provider):
        fixture = self.fixture
        fixture.spec["provider"]["name"] = provider
        self.enterContext(mock.patch.object(g, "_NIX_STORE", fixture.store))
        root = fixture.header.parent
        child = root / "child"
        child.mkdir()
        header = child / "child.h"
        header.write_bytes(b"child header")
        self.add_package("bar", [child])
        manifest = fixture.generate()
        self.assertEqual(fixture.include_root("foo"), root)
        self.assertEqual(fixture.include_root("bar"), child)
        self.assertEqual(
            fixture.package("bar")["headers"],
            [{"root": str(child), "index": 0, "files": [{"source": str(header), "path": "child.h"}]}],
        )
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [2, 1])

    def test_generic_child_only_interface_stays_narrow(self):
        self.check_child_only_interface("fixture")

    def test_nix_child_only_interface_stays_narrow(self):
        self.check_child_only_interface("nix")

    def test_nested_root_topology_and_order_preserve_sibling_relative_includes(self):
        fixture = self.fixture
        root = fixture.header.parent
        child = root / "ryml"
        (root / "c4").mkdir()
        child.mkdir()
        dependency = root / "c4/dependency.h"
        wrapper = child / "wrapper.h"
        dependency.write_bytes(b"nested dependency")
        wrapper.write_bytes(b'#include "../c4/dependency.h"\n')
        for directories in ([root, child], [child, root]):
            with self.subTest(directories=directories):
                fixture.patches["patches"].clear()
                fixture.spec["packages"] = [{"name": "foo", "modules": ["foo"], "expected_versions": {"foo": "1.2.3"}}]
                self.add_package("bar", directories)
                manifest = fixture.generate()
                parent = fixture.include_root("bar", directories.index(root))
                nested = fixture.include_root("bar", directories.index(child))
                self.assertEqual(nested, parent / "ryml")
                self.assertEqual(fixture.artifact(nested / "wrapper.h"), wrapper)
                self.assertEqual(fixture.artifact(parent / "c4/dependency.h"), dependency)
                self.assertEqual(fixture.package("bar")["headers"][0]["index"], directories.index(root))
                self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [3, 3])
                self.assertFalse(fixture.out.exists())

    @unittest.skipIf(os.name == "nt", "Windows symlink creation is outside this fixture contract")
    def test_canonical_alias_of_the_same_root_keeps_package_observations_narrow(self):
        fixture = self.fixture
        alias = fixture.provider / "include-alias"
        alias.symlink_to(fixture.header.parent, target_is_directory=True)
        self.add_package("bar", [alias])
        manifest = fixture.generate()
        self.assertEqual(fixture.package("bar")["name"], "bar")
        self.assertEqual(self.headers("bar"), {str(fixture.header)})
        self.assertEqual(fixture.include_root("bar"), fixture.header.parent)
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [1, 1])

    def test_resolution_rehashes_current_header_bytes_and_membership_without_timestamp_signals(self):
        fixture = self.fixture
        self.add_package("bar", [fixture.header.parent])
        first = fixture.generate()
        source_time = fixture.header.stat().st_mtime_ns
        fixture.header.write_bytes(b"changed bytes without a timestamp signal")
        os.utime(fixture.header, ns=(source_time, source_time))
        changed = fixture.generate()
        first_hash = next(item["sha256"] for item in first["inputs"] if item["path"] == str(fixture.header))
        changed_hash = next(item["sha256"] for item in changed["inputs"] if item["path"] == str(fixture.header))
        self.assertNotEqual(first_hash, changed_hash)
        self.assertEqual(changed_hash, hashlib.sha256(fixture.header.read_bytes()).hexdigest())
        extra = fixture.header.with_name("added.h")
        extra.write_bytes(b"new declared member")
        manifest = fixture.generate()
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [2, 2])
        fixture.header.unlink()
        manifest = fixture.generate()
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [1, 1])
        self.assertEqual(self.headers("foo"), {str(extra)})
        self.assertEqual(self.headers("bar"), {str(extra)})

    def test_empty_or_out_of_root_package_cannot_produce_valid_observations(self):
        fixture = self.fixture
        self.add_package("bar", [fixture.header.parent])
        empty = fixture.provider / "empty"
        empty.mkdir()
        self.add_package("empty", [empty])
        with self.assertRaisesRegex(g.BazelDependencyError, "empty header directory"):
            fixture.generate()
        fixture.spec["packages"].pop()
        fixture.patches["patches"].pop()
        outside = fixture.root / "outside-include"
        outside.mkdir()
        (outside / "untrusted.h").write_bytes(b"not allowed")
        self.add_package("outside", [outside])
        with self.assertRaisesRegex(g.BazelDependencyError, "outside allowed provider roots"):
            fixture.generate()
        self.assertFalse(fixture.out.exists())

    def test_nix_shared_anchors_are_scanned_once_and_keep_ordered_sparse_interfaces(self):
        fixture = self.fixture
        fixture.spec["provider"]["name"] = "nix"
        nested = fixture.header.parent / "nested"
        nested.mkdir()
        (nested / "nested.h").write_bytes(b"nested header")
        other = fixture.provider / "other-include"
        other.mkdir()
        (other / "other.h").write_bytes(b"other header")
        fixture.cflags = f'-I"{fixture.header.parent}" -I"{other}" -DFOO_API=1'
        fixture.write_pc()
        archive = fixture.lib.with_name("libbar.a")
        archive.write_bytes(b"bar archive")
        self.add_package(
            "bar",
            [other, fixture.header.parent],
            defines=["BAR_API=2"],
            libraries=[{"name": "bar", "kind": "static", "file": str(archive)}],
        )
        self.enterContext(mock.patch.object(g, "_NIX_STORE", fixture.store))
        adapter = g._Adapter(fixture.spec, fixture.patches)
        adapter.prepare()
        with mock.patch.object(g.os, "scandir", wraps=os.scandir) as scandir:
            fixture.resolution = adapter.resolve()
        manifest = fixture.resolution["manifest"]
        self.assertCountEqual([call.args[0] for call in scandir.call_args_list], [fixture.header.parent, nested, other])
        primary_files = [
            {"source": str(fixture.header), "path": "foo.h"},
            {"source": str(nested / "nested.h"), "path": "nested/nested.h"},
        ]
        other_files = [{"source": str(other / "other.h"), "path": "other.h"}]
        for name, expected in (("foo", [primary_files, other_files]), ("bar", [other_files, primary_files])):
            trees = fixture.package(name)["headers"]
            self.assertEqual([tree["files"] for tree in trees], expected)
            self.assertEqual([tree["index"] for tree in trees], [0, 1])
        self.assertEqual([fixture.include_root("foo", index) for index in range(2)], [fixture.header.parent, other])
        self.assertEqual([fixture.include_root("bar", index) for index in range(2)], [other, fixture.header.parent])
        self.assertEqual(fixture.package("foo")["defines"], ["FOO_API=1"])
        self.assertEqual(fixture.package("bar")["defines"], ["BAR_API=2"])
        self.assertEqual(fixture.package("foo")["libraries"][0]["source"], str(fixture.lib))
        self.assertEqual(fixture.package("bar")["libraries"][0]["source"], str(archive))
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [3, 3])
        self.assertEqual(
            {role for entry in manifest["inputs"] for role in entry["roles"]},
            {"resolver_tool", "pkg_config_metadata", "library", "header"},
        )
        foo_files = fixture.package("foo")["headers"][0]["files"]
        foo_files[0]["path"] = "modified.h"
        foo_files.append({"source": "extra", "path": "extra.h"})
        self.assertEqual(fixture.package("bar")["headers"][1]["files"], primary_files)


if __name__ == "__main__":
    unittest.main()
