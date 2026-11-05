"""SDK header observations retain each package's topology and ownership boundary."""

import hashlib
import json
import os
import unittest
from types import SimpleNamespace

import test_bazel_deps as adapter_tests

from ao.core import bazel_deps as g
from ao.core import bazel_native as native


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

    def test_child_only_interface_does_not_gain_another_packages_parent_headers(self):
        fixture = self.fixture
        child = fixture.header.parent / "child"
        child.mkdir()
        (child / "child.h").write_bytes(b"child header")
        self.add_package("bar", [child])
        manifest = fixture.generate()
        self.assertEqual(
            fixture.package("bar")["headers"],
            [
                {
                    "root": str(child),
                    "index": 0,
                    "files": [{"source": str(child / "child.h"), "path": "child.h"}],
                }
            ],
        )
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [2, 1])

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

    def test_native_materialization_rejects_a_package_overwriting_an_internal_header_target(self):
        fixture = self.fixture
        # Target-name ownership moved to Starlark. Exercise its real declaration
        # guard, not an adapter-generated BUILD string or a second target inventory.
        self.add_package("_headers_foo_0", [fixture.header.parent])
        fixture.generate()
        namespace = {"fail": self.fail, "json": SimpleNamespace(encode=json.dumps)}
        source = native.PROJECT_ROOT / "bazel/import_repository.bzl"
        exec(compile(source.read_text(encoding="utf-8"), str(source), "exec"), namespace)
        ctx = SimpleNamespace(path=str, symlink=lambda *args: None, file=lambda *args, **kwargs: None)
        with self.assertRaisesRegex(AssertionError, "generated target name collision"):
            namespace["materialize_imports"](
                ctx, "", "release", fixture.resolution["packages"], fixture.resolution["manifest"]
            )

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


if __name__ == "__main__":
    unittest.main()
