"""Tests for ao.core.builddir — flavor/preset/build-tree mapping."""

import unittest
from pathlib import Path
from unittest import mock

from ao.core import builddir


class BuildDirTest(unittest.TestCase):
    def test_presets_cover_all_flavors(self):
        self.assertEqual(
            builddir.PRESETS,
            {
                "debug": "linux-debug",
                "release": "linux-release",
                "pgo1": "linux-pgo-profile",
                "pgo2": "linux-pgo-optimize",
                "profile": "profile",
            },
        )

    def test_plain_flavors_map_to_their_own_tree(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            import os

            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.build_dir("debug"), Path("/tmp/build/debug"))
            self.assertEqual(builddir.build_dir("release"), Path("/tmp/build/release"))
            self.assertEqual(builddir.build_dir("profile"), Path("/tmp/build/profile"))

    def test_suffixes_combine_in_fixed_order(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            import os

            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.build_dir("debug", clang=True), Path("/tmp/build/debug-clang"))
            self.assertEqual(builddir.build_dir("debug", asan=True), Path("/tmp/build/debug-asan"))
            self.assertEqual(builddir.build_dir("debug", tsan=True), Path("/tmp/build/debug-tsan"))
            self.assertEqual(builddir.build_dir("debug", clang=True, asan=True), Path("/tmp/build/debug-clang-asan"))

    def test_pgo_steps_share_one_tree(self):
        with mock.patch.dict("os.environ", {}, clear=False):
            import os

            os.environ.pop("BUILD_DIR", None)
            self.assertEqual(builddir.build_dir("pgo1"), builddir.build_dir("pgo2"))
            self.assertEqual(builddir.build_dir("pgo1", clang=True), Path("/tmp/build/pgo-clang"))

    def test_build_dir_env_always_wins(self):
        # The fleet oracle sandbox redirects builds via BUILD_DIR (see Engine.cpp).
        with mock.patch.dict("os.environ", {"BUILD_DIR": "/host/oracle-build"}):
            self.assertEqual(builddir.build_dir("debug", asan=True), Path("/host/oracle-build"))


if __name__ == "__main__":
    unittest.main()
