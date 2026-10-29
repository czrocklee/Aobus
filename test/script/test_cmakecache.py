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
        # Preserve duplicates in file order so consumers can resolve them by
        # keeping the last entry, matching CMake's own cache loading.
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

    def test_read_entries_preserves_file_order_raw_values_and_replaces_invalid_bytes(self):
        path = self.write_cache(b"FIRST:STRING=one\nSEPARATED:STRING=a=b\nSECOND:STRING=tw\xffo\nFIRST:STRING=again\n")

        self.assertEqual(
            cmakecache.read_entries(path),
            [("FIRST", "one"), ("SEPARATED", "a=b"), ("SECOND", "tw\ufffdo"), ("FIRST", "again")],
        )

    def test_read_entries_propagates_read_errors(self):
        with self.assertRaises(OSError):
            cmakecache.read_entries(self.missing_cache())


if __name__ == "__main__":
    unittest.main()
