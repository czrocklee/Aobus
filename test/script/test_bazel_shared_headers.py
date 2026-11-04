"""Repository header plans retain each package's topology and ownership boundary."""

import ast
import hashlib
import os
import unittest
from pathlib import Path

import test_bazel_deps as adapter_tests

from ao.core import bazel_deps as g


def rules(output):
    result = {}
    tree = ast.parse((output / "BUILD.bazel").read_text(encoding="utf-8"))
    for node in tree.body:
        if not isinstance(node, ast.Expr) or not isinstance(node.value, ast.Call):
            continue
        call = node.value
        if not isinstance(call.func, ast.Name) or call.func.id not in {"filegroup", "cc_library", "cc_import"}:
            continue
        fields = {
            keyword.arg: ast.literal_eval(keyword.value.args[0])[":_prepared_mode"]
            if isinstance(keyword.value, ast.Call)
            else ast.literal_eval(keyword.value)
            for keyword in call.keywords
        }
        result[fields.pop("name")] = {"kind": call.func.id, **fields}
    return result


def expanded_headers(target, generated):
    fields = generated[target]
    entries = fields.get("hdrs", fields.get("srcs", []))
    result = set()
    for entry in entries:
        if entry.startswith(":"):
            result.update(expanded_headers(entry[1:], generated))
        else:
            result.add(entry)
    return result


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

    def test_equal_roots_keep_package_interfaces_and_sparse_header_namespaces_distinct(self):
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
        generated = rules(fixture.out)
        header_paths = [path for path, (source, _) in fixture.links.items() if source == fixture.header]
        self.assertEqual(len(header_paths), 3)
        self.assertFalse((fixture.out / "artifacts").exists())
        self.assertNotEqual(generated["foo"]["hdrs"], generated["bar"]["hdrs"])
        for package, defines in (("foo", ["FOO_API=1"]), ("bar", ["BAR_API=2"]), ("baz", ["BAZ_API=3"])):
            self.assertEqual(generated[package]["defines"], defines)
            self.assertEqual(generated[package]["copts"], [])
            self.assertEqual(
                {fixture.artifact(fixture.out / path) for path in expanded_headers(package, generated)},
                {fixture.header},
            )
        self.assertNotEqual(generated["foo"]["deps"], generated["bar"]["deps"])
        self.assertEqual(generated["baz"]["deps"], [])
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [1, 1, 1])
        self.assertEqual(manifest["counts"], {"automatic": 1, "manual_fallback": 2})

    def test_shared_roots_preserve_each_packages_search_order(self):
        fixture = self.fixture
        other = fixture.provider / "other-include"
        other.mkdir()
        (other / "foo.h").write_bytes(b"other implementation")
        fixture.cflags += f' -I"{other}"'
        fixture.write_pc()
        self.add_package("bar", [other, fixture.header.parent])
        fixture.generate()
        generated = rules(fixture.out)
        for package, expected in (("foo", [fixture.header.parent, other]), ("bar", [other, fixture.header.parent])):
            self.assertEqual(
                [fixture.artifact(fixture.out / root / "foo.h").parent for root in generated[package]["includes"]],
                expected,
            )
        self.assertEqual(
            {fixture.artifact(fixture.out / path) for path in expanded_headers("foo", generated)},
            {fixture.artifact(fixture.out / path) for path in expanded_headers("bar", generated)},
        )

    def test_distinct_roots_do_not_merge_just_because_their_bytes_match(self):
        fixture = self.fixture
        other = fixture.provider / "other-include"
        other.mkdir()
        other_header = other / "foo.h"
        other_header.write_bytes(fixture.header.read_bytes())
        self.add_package("bar", [other])
        fixture.generate()
        generated = rules(fixture.out)
        self.assertNotEqual(generated["foo"]["hdrs"], generated["bar"]["hdrs"])
        self.assertNotEqual(generated["foo"]["includes"], generated["bar"]["includes"])
        self.assertEqual(
            {fixture.artifact(fixture.out / path) for path in expanded_headers("bar", generated)}, {other_header}
        )

    def test_child_only_interface_does_not_gain_another_packages_parent_headers(self):
        fixture = self.fixture
        child = fixture.header.parent / "child"
        child.mkdir()
        (child / "child.h").write_bytes(b"child header")
        self.add_package("bar", [child])
        manifest = fixture.generate()
        declared = expanded_headers("bar", rules(fixture.out))
        self.assertEqual({Path(path).name for path in declared}, {"child.h"})
        self.assertNotIn("foo.h", {Path(path).name for path in declared})
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
                generated = rules(fixture.out)
                includes = generated["bar"]["includes"]
                parent = fixture.out / includes[directories.index(root)]
                nested = fixture.out / includes[directories.index(child)]
                self.assertEqual(nested, parent / "ryml")
                self.assertEqual(fixture.artifact(nested / "wrapper.h"), wrapper)
                self.assertEqual(fixture.artifact(parent / "c4/dependency.h"), dependency)
                self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [3, 3])
                self.assertFalse((fixture.out / "artifacts").exists())

    @unittest.skipIf(os.name == "nt", "Windows symlink creation is outside this fixture contract")
    def test_canonical_alias_of_the_same_root_keeps_package_projection_narrow(self):
        fixture = self.fixture
        alias = fixture.provider / "include-alias"
        alias.symlink_to(fixture.header.parent, target_is_directory=True)
        self.add_package("bar", [alias])
        manifest = fixture.generate()
        generated = rules(fixture.out)
        self.assertNotEqual(generated["foo"]["hdrs"], generated["bar"]["hdrs"])
        self.assertEqual(
            {fixture.artifact(fixture.out / path) for path in expanded_headers("bar", generated)}, {fixture.header}
        )
        self.assertEqual([package["declared_headers"] for package in manifest["packages"]], [1, 1])

    def test_planning_rehashes_current_header_bytes_and_membership_without_timestamp_signals(self):
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
        self.assertNotIn(fixture.header, [source for source, _ in fixture.links.values()])
        self.assertEqual({source for source, role in fixture.links.values() if role == "header"}, {extra})

    def test_package_cannot_overwrite_an_internal_header_target(self):
        fixture = self.fixture
        fixture.generate()
        label = rules(fixture.out)["foo"]["hdrs"][0]
        self.assertTrue(label.startswith(":"))
        self.add_package(label[1:], [fixture.header.parent])
        with self.assertRaisesRegex(g.BazelDependencyError, "generated target name collision"):
            fixture.generate()

    def test_empty_or_out_of_root_package_cannot_produce_a_valid_plan(self):
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
        self.assertFalse((fixture.out / "artifacts").exists())


if __name__ == "__main__":
    unittest.main()
