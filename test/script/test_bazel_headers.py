"""Selected vcpkg ownership is complete, bounded, and topology-preserving."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core.bazel_headers import HeaderInventory, HeaderInventoryError, vcpkg_header_inventory


class BazelHeaderInventoryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-vcpkg-headers-")
        self.addCleanup(temporary.cleanup)
        self.installed = Path(temporary.name).resolve() / "installed"
        self.info = self.installed / "vcpkg/info"
        self.info.mkdir(parents=True)
        self.triplet("x64-windows")

    def triplet(self, name):
        self.prefix = self.installed / name
        self.include = self.prefix / "include"
        self.include.mkdir(parents=True)

    def source(self, relative, content=b"header fixture\n"):
        path = self.prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def inventory(self, port="owned", version="1.2.3", entries=("include/owned.hpp",)):
        path = self.info / f"{port}_{version}_{self.prefix.name}.list"
        path.write_text("\n".join(f"{self.prefix.name}/{entry}" for entry in entries) + "\n", encoding="utf-8")
        return path

    def collect(self, ports=None, roots=None):
        return vcpkg_header_inventory(
            self.prefix, {"owned": "1.2.3"} if ports is None else ports, [self.include] if roots is None else roots
        )

    def symlink(self, source, target, *, directory=False):
        if os.name == "nt":
            self.skipTest("Windows symlink privileges are outside this fixture contract")
        source.symlink_to(target, target_is_directory=directory)

    def test_exact_upstream_filenames_support_hyphenated_ports_versions_and_triplets(self):
        for triplet in ("x64-aobus-osx", "arm64-osx", "arm64-windows-static-md"):
            with self.subTest(triplet=triplet):
                self.triplet(triplet)
                header = self.source("include/alac/ALACDecoder.h")
                manifest = self.inventory("alac", "2017-11-03-c38887c5", ["include/alac/ALACDecoder.h"])
                boost = self.source("include/boost/algorithm.hpp")
                boost_manifest = self.inventory("boost-algorithm", "1.91.0", ["include/boost/algorithm.hpp"])
                self.assertEqual(
                    self.collect({"boost-algorithm": "1.91.0", "alac": "2017-11-03-c38887c5"}),
                    HeaderInventory((header, boost), (manifest, boost_manifest), (self.info,)),
                )

    def test_port_revision_is_not_part_of_the_inventory_filename(self):
        header = self.source("include/ogg/ogg.h")
        manifest = self.inventory("libogg", "1.3.6", ["include/ogg/ogg.h"])
        # The caller has already accepted status version 1.3.6#1.
        self.assertEqual(self.collect({"libogg": "1.3.6"}).files, (header,))
        manifest.rename(manifest.with_name(manifest.name.replace("1.3.6_", "1.3.6#1_")))
        with self.assertRaisesRegex(HeaderInventoryError, "missing, stale, or ambiguous"):
            self.collect({"libogg": "1.3.6"})

    def test_inventory_selection_does_not_choose_a_stale_version_or_scan_physical_headers(self):
        self.source("include/owned.hpp")
        self.inventory(version="1.2.2")
        with self.assertRaisesRegex(HeaderInventoryError, "expected.*owned_1.2.3_x64-windows.list"):
            self.collect()

    def test_missing_selected_port_inventory_fails_even_when_another_port_owns_headers(self):
        self.source("include/owned.hpp")
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "missing, stale, or ambiguous inventory for extra"):
            self.collect({"owned": "1.2.3", "extra": "2"})

    def test_competing_version_inventory_is_ambiguous_even_when_exact_inventory_exists(self):
        self.source("include/owned.hpp")
        self.inventory()
        self.inventory(version="1.2.2")
        with self.assertRaisesRegex(HeaderInventoryError, "ambiguous inventory for owned"):
            self.collect()

    def test_other_triplet_and_unselected_port_inventories_do_not_change_selection(self):
        header = self.source("include/owned.hpp")
        manifest = self.inventory()
        (self.info / "owned_9_arm64-osx.list").write_bytes(b"not read\n")
        self.inventory("unselected", "1", ["include/unselected.hpp"])
        self.assertEqual(self.collect(), HeaderInventory((header,), (manifest,), (self.info,)))

    def test_case_variant_inventory_name_is_rejected_on_every_host(self):
        self.source("include/owned.hpp")
        manifest = self.inventory()
        manifest.rename(manifest.with_name("Owned_1.2.3_x64-windows.list"))
        with self.assertRaisesRegex(HeaderInventoryError, "missing, stale, or ambiguous"):
            self.collect()

    def test_ports_with_no_headers_still_contribute_metadata_watches(self):
        header = self.source("include/boost/config.hpp")
        config = self.inventory("boost-config", "1.91.0", ["include/boost/config.hpp"])
        cmake = self.inventory("boost-cmake", "1.91.0", ["share/boost-cmake/copyright"])
        self.assertEqual(
            self.collect({"boost-config": "1.91.0", "boost-cmake": "1.91.0"}),
            HeaderInventory((header,), (cmake, config), (self.info,)),
        )

    def test_only_listed_files_beneath_selected_roots_are_imported_without_suffix_filtering(self):
        entries = ["include/owned.hpp", "include/detail/fragment", "include/detail/values.inc"]
        files = tuple(sorted(self.source(entry) for entry in entries))
        self.source("include/unlisted.hpp")
        self.source("lib/owned.lib")
        self.inventory(entries=[*entries, "lib/owned.lib", "share/owned/missing-copyright"])
        self.assertEqual(self.collect().files, files)

    def test_inventory_read_never_enumerates_the_physical_include_tree(self):
        header = self.source("include/owned.hpp")
        self.inventory()
        original = Path.iterdir

        def metadata_only(path):
            self.assertEqual(path, self.info)
            return original(path)

        with (
            patch.object(Path, "iterdir", metadata_only),
            patch.object(Path, "glob", side_effect=AssertionError("no glob enumeration")),
            patch.object(Path, "rglob", side_effect=AssertionError("no recursive enumeration")),
        ):
            self.assertEqual(self.collect().files, (header,))

    def test_directory_entries_are_not_header_files_and_crlf_is_accepted(self):
        header = self.source("include/owned.hpp")
        manifest = self.inventory()
        manifest.write_bytes(
            b"x64-windows/\r\nx64-windows/include/\r\nx64-windows/include/owned.hpp\r\nx64-windows/share/owned/\r\n"
        )
        self.assertEqual(self.collect().files, (header,))

    def test_inventory_with_only_directories_does_not_admit_unlisted_physical_headers(self):
        self.source("include/owned.hpp")
        self.inventory(entries=["", "include/"])
        with self.assertRaisesRegex(HeaderInventoryError, "no selected owned files"):
            self.collect()

    def test_missing_selected_header_is_an_error(self):
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "cannot resolve inventory path.*owned.hpp"):
            self.collect()

    def test_directory_without_directory_marker_is_not_a_file(self):
        (self.include / "owned.hpp").mkdir()
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "listed header source is not a file"):
            self.collect()

    def test_empty_or_undecodable_inventory_is_an_error(self):
        self.source("include/owned.hpp")
        manifest = self.inventory()
        for content, message in ((b"", "empty inventory"), (b"\xff\n", "cannot read vcpkg header inventory")):
            with self.subTest(content=content):
                manifest.write_bytes(content)
                with self.assertRaisesRegex(HeaderInventoryError, message):
                    self.collect()

    def test_unsafe_paths_are_rejected_even_outside_selected_header_roots(self):
        self.source("include/owned.hpp")
        manifest = self.inventory()
        for entry in (
            "/x64-windows/include/owned.hpp",
            "C:/x64-windows/include/owned.hpp",
            "C:x64-windows/include/owned.hpp",
            r"\\server\include\owned.hpp",
            r"x64-windows\include\owned.hpp",
            "//server/include/owned.hpp",
            "../x64-windows/include/owned.hpp",
            "x64-windows/include/../include/owned.hpp",
            "x64-windows/share/../share/copyright",
            "x64-windows/include/./owned.hpp",
            "x64-windows/include//owned.hpp",
            "x64-windows/include/owned.hpp/../",
            "x64-windows/include/owned.hpp:stream",
            "x64-windows/include/owned.hpp.",
            "x64-windows/include/owned.hpp ",
            "x64-windows/include/\x00.hpp",
            "x64-windows/include/\t.hpp",
            "",
        ):
            with self.subTest(entry=entry):
                manifest.write_text(f"x64-windows/include/owned.hpp\n{entry}\n", encoding="utf-8")
                with self.assertRaisesRegex(HeaderInventoryError, "unsafe inventory entry"):
                    self.collect()

    def test_wrong_triplet_entries_are_rejected_including_nonheaders_and_directories(self):
        self.source("include/owned.hpp")
        manifest = self.inventory()
        for entry in ("arm64-windows/include/owned.hpp", "arm64-windows/share/copyright", "arm64-windows/"):
            with self.subTest(entry=entry):
                manifest.write_text(entry + "\n", encoding="utf-8")
                with self.assertRaisesRegex(HeaderInventoryError, "wrong triplet"):
                    self.collect()

    def test_invalid_port_selections_are_rejected_before_metadata_selection(self):
        for ports in (
            {},
            [],
            {"owned[feature]": "1"},
            {"owned_feature": "1"},
            {"Owned": "1"},
            {"../owned": "1"},
            {"-owned": "1"},
            {1: "1"},
            {"owned": 1},
            {"owned": ""},
            {"owned": "1.2.3#4"},
            {"owned": "../1"},
            {"owned": "1/2"},
            {"owned": "1\\2"},
            {"owned": "1:2"},
            {"owned": "1\n2"},
        ):
            with self.subTest(ports=ports), self.assertRaises(HeaderInventoryError):
                self.collect(ports)

    def test_safe_nonsemver_upstream_version_is_not_required_to_be_numeric(self):
        header = self.source("include/owned.hpp")
        self.inventory(version="release_2026-01+fixture.1")
        self.assertEqual(self.collect({"owned": "release_2026-01+fixture.1"}).files, (header,))

    def test_invalid_or_missing_triplet_prefix_is_an_error(self):
        self.triplet("x64_invalid")
        with self.assertRaisesRegex(HeaderInventoryError, "invalid installed triplet prefix"):
            self.collect()
        with self.assertRaisesRegex(HeaderInventoryError, "cannot resolve inventory path"):
            vcpkg_header_inventory(self.installed / "missing", {"owned": "1"}, [self.include])

    def test_empty_missing_nondirectory_or_outside_include_roots_are_errors(self):
        header = self.source("include/owned.hpp")
        self.inventory()
        outside = self.installed / "outside"
        outside.mkdir()
        for roots, message in (
            ([], "include_roots must not be empty"),
            ([self.include, self.include / "missing"], "cannot resolve inventory path"),
            ([header], "not a directory"),
            ([outside], "outside prefix/include"),
            ([self.prefix], "outside prefix/include"),
            ([self.include / ".." / "include"], "outside prefix/include"),
        ):
            with self.subTest(roots=roots), self.assertRaisesRegex(HeaderInventoryError, message):
                self.collect(roots=roots)

    def test_each_existing_requested_root_must_have_selected_ownership(self):
        self.source("include/owned.hpp")
        unowned = self.source("include/extra/unowned.hpp").parent
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "no selected owned files.*extra"):
            self.collect(roots=[self.include, unowned])

    def test_overlapping_port_ownership_and_duplicate_rows_deduplicate_the_same_file(self):
        header = self.source("include/owned.hpp")
        owned = self.inventory(entries=["include/owned.hpp", "include/owned.hpp"])
        shared = self.inventory("shared", "2", ["include/owned.hpp"])
        self.assertEqual(
            self.collect({"shared": "2", "owned": "1.2.3"}),
            HeaderInventory((header,), (owned, shared), (self.info,)),
        )

    def test_casefold_destination_conflicts_fail_even_when_file_bytes_match(self):
        self.source("include/Owned.hpp")
        self.source("include/owned.hpp")
        self.inventory(entries=["include/Owned.hpp", "include/owned.hpp"])
        with self.assertRaisesRegex(HeaderInventoryError, "casefold header destination conflict"):
            self.collect()

    def test_parent_and_child_roots_keep_sibling_relative_include_topology(self):
        wrapper = self.source("include/ryml/wrapper.hpp", b'#include "../c4/dependency.hpp"\n')
        dependency = self.source("include/c4/dependency.hpp")
        self.source("include/not-selected.hpp")
        self.inventory("ryml", "0.16.0", ["include/ryml/wrapper.hpp"])
        self.inventory("c4core", "0.6.0", ["include/c4/dependency.hpp"])
        for roots in ([self.include, wrapper.parent], [wrapper.parent, self.include]):
            with self.subTest(roots=roots):
                result = self.collect({"ryml": "0.16.0", "c4core": "0.6.0"}, roots)
                self.assertEqual(result.files, (dependency, wrapper))
                self.assertEqual(wrapper.read_bytes(), b'#include "../c4/dependency.hpp"\n')
                self.assertEqual((wrapper.parent / "../c4/dependency.hpp").resolve(), dependency)
                self.assertEqual(
                    [path.relative_to(self.include).as_posix() for path in result.files],
                    ["c4/dependency.hpp", "ryml/wrapper.hpp"],
                )

    def test_fakeit_child_interface_does_not_admit_other_frameworks_or_catch2(self):
        selected = self.source("include/fakeit/single_header/catch/fakeit.hpp")
        self.source("include/fakeit/single_header/boost/fakeit.hpp")
        self.source("include/catch2/catch_all.hpp")
        manifest = self.inventory(
            "fakeit",
            "2.5.0",
            ["include/fakeit/single_header/catch/fakeit.hpp", "include/fakeit/single_header/boost/fakeit.hpp"],
        )
        self.assertEqual(
            self.collect({"fakeit": "2.5.0"}, [selected.parent]),
            HeaderInventory((selected,), (manifest,), (self.info,)),
        )

    def test_safe_header_symlink_keeps_its_provider_spelling(self):
        target = self.source("include/detail/owned.hpp")
        alias = self.include / "owned.hpp"
        self.symlink(alias, target)
        self.inventory()
        self.assertEqual(self.collect().files, (alias,))
        self.assertNotEqual(alias, target)

    def test_header_symlink_outside_prefix_is_rejected(self):
        target = self.installed / "outside.hpp"
        target.write_bytes(b"not allowed\n")
        self.symlink(self.include / "owned.hpp", target)
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "escapes selected header roots"):
            self.collect()

    def test_header_symlink_outside_selected_child_root_is_rejected(self):
        target = self.source("include/other/owned.hpp")
        child = self.include / "selected"
        child.mkdir()
        self.symlink(child / "owned.hpp", target)
        self.inventory(entries=["include/selected/owned.hpp"])
        with self.assertRaisesRegex(HeaderInventoryError, "escapes selected header roots"):
            self.collect(roots=[child])

    def test_header_symlink_into_nonheader_prefix_directory_is_rejected(self):
        target = self.source("lib/owned.hpp")
        self.symlink(self.include / "owned.hpp", target)
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "escapes selected header roots"):
            self.collect()

    def test_dangling_header_symlink_is_rejected(self):
        self.symlink(self.include / "owned.hpp", self.include / "missing.hpp")
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "cannot resolve inventory path"):
            self.collect()

    def test_header_symlink_cycle_is_rejected(self):
        self.symlink(self.include / "owned.hpp", self.include / "owned.hpp")
        self.inventory()
        with self.assertRaisesRegex(HeaderInventoryError, "cannot resolve inventory path"):
            self.collect()

    def test_selected_root_symlink_outside_header_area_is_rejected(self):
        outside = self.installed / "outside"
        outside.mkdir()
        child = self.include / "selected"
        self.symlink(child, outside, directory=True)
        with self.assertRaisesRegex(HeaderInventoryError, "include root escapes"):
            self.collect(roots=[child])

    def test_internal_directory_symlink_preserves_listed_topology(self):
        target = self.source("include/detail/owned.hpp")
        child = self.include / "selected"
        self.symlink(child, target.parent, directory=True)
        self.inventory(entries=["include/selected/owned.hpp"])
        self.assertEqual(self.collect(roots=[child]).files, (child / "owned.hpp",))

    def test_prefix_include_symlink_outside_prefix_is_rejected(self):
        outside = self.installed / "outside"
        outside.mkdir()
        self.include.rmdir()
        self.symlink(self.include, outside, directory=True)
        with self.assertRaisesRegex(HeaderInventoryError, "header directory escapes"):
            self.collect()

    def test_inventory_source_must_be_a_file_inside_info(self):
        self.source("include/owned.hpp")
        manifest = self.inventory()
        manifest.unlink()
        manifest.mkdir()
        with self.assertRaisesRegex(HeaderInventoryError, "inventory source escapes info or is not a file"):
            self.collect()

    def test_inventory_source_symlink_cannot_read_outside_info(self):
        self.source("include/owned.hpp")
        manifest = self.inventory()
        outside = self.installed / "outside.list"
        manifest.rename(outside)
        self.symlink(manifest, outside)
        with self.assertRaisesRegex(HeaderInventoryError, "inventory source escapes info"):
            self.collect()

    def test_missing_inventory_directory_is_not_a_whole_prefix_fallback(self):
        self.source("include/owned.hpp")
        self.info.rmdir()
        with self.assertRaisesRegex(HeaderInventoryError, "cannot resolve inventory path.*info"):
            self.collect()

    def test_inventory_directory_symlink_outside_vcpkg_metadata_is_rejected(self):
        outside = self.installed / "outside"
        outside.mkdir()
        self.info.rmdir()
        self.symlink(self.info, outside, directory=True)
        with self.assertRaisesRegex(HeaderInventoryError, "inventory directory escapes vcpkg metadata"):
            self.collect()


if __name__ == "__main__":
    unittest.main()
