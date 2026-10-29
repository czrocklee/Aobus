"""Tests for ao.core.cmakecache — CMakeCache.txt tokenization and shared readers."""

import tempfile
import unittest
from pathlib import Path

from ao.core import cmakecache


class ParseCacheTest(unittest.TestCase):
    def test_entries_keep_file_order_names_without_type_and_raw_values(self):
        text = (
            "// a comment line\n"
            "# another comment\n"
            "\n"
            "no separator\n"
            "NO_TYPE_SUFFIX=value\n"
            "AOBUS_BUILD_TESTS:BOOL=ON\n"
            "CMAKE_CACHEFILE_DIR:INTERNAL=/tmp/build/aobus\n"
            "EMPTY:STRING=\n"
            "EQUALS:STRING=a=b=c\n"
        )

        # Untyped, separator-less, and comment lines are not cache entries;
        # INTERNAL entries are ordinary entries and empty values are kept.
        self.assertEqual(
            list(cmakecache.parse_cache(text)),
            [
                ("AOBUS_BUILD_TESTS", "ON"),
                ("CMAKE_CACHEFILE_DIR", "/tmp/build/aobus"),
                ("EMPTY", ""),
                ("EQUALS", "a=b=c"),
            ],
        )

    def test_duplicate_names_stay_in_first_to_last_file_order(self):
        # The file order is the only duplicate resolution this layer owns;
        # first-match consumers rely on the earliest entry staying first.
        self.assertEqual(
            list(cmakecache.parse_cache("KEY:STRING=first\nKEY:BOOL=second\n")),
            [("KEY", "first"), ("KEY", "second")],
        )


class CacheReadersTest(unittest.TestCase):
    def write_cache(self, data: bytes) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "CMakeCache.txt"
        path.write_bytes(data)
        return path

    def missing_cache(self) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return Path(directory.name) / "CMakeCache.txt"

    def test_read_cache_keeps_the_last_duplicate_and_raw_values(self):
        path = self.write_cache(b"SEPARATED:STRING=a=b\n// comment\nKEY:STRING=first\nKEY:BOOL=second=kept\n")

        cache = cmakecache.read_cache(path)

        self.assertEqual(cache, {"SEPARATED": "a=b", "KEY": "second=kept"})
        self.assertNotIn("ABSENT", cache)

    def test_read_cache_decodes_strictly(self):
        path = self.write_cache(b"CMAKE_BUILD_TYPE:STRING=Rel\xffease\n")

        with self.assertRaises(UnicodeDecodeError):
            cmakecache.read_cache(path)

    def test_read_cache_propagates_read_errors(self):
        with self.assertRaises(OSError):
            cmakecache.read_cache(self.missing_cache())

    def test_read_entries_preserves_file_order_and_replaces_invalid_bytes(self):
        path = self.write_cache(b"FIRST:STRING=one\nSECOND:STRING=tw\xffo\nFIRST:STRING=again\n")

        self.assertEqual(
            cmakecache.read_entries(path),
            [("FIRST", "one"), ("SECOND", "tw\ufffdo"), ("FIRST", "again")],
        )

    def test_read_entries_propagates_read_errors(self):
        with self.assertRaises(OSError):
            cmakecache.read_entries(self.missing_cache())


if __name__ == "__main__":
    unittest.main()
