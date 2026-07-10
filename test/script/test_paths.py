"""Tests for filesystem-safe path normalization."""

import re
import unittest
from pathlib import Path
from unittest import mock

from ao.command import analyze, tidy
from ao.core import paths


class AbsolutePathTest(unittest.TestCase):
    def test_windows_uses_lexical_abspath_when_filesystem_resolution_fails(self):
        candidate = Path("Y:/repo/script/ao/core/paths.py")
        lexical = r"Y:\repo\script\ao\core\paths.py"

        with mock.patch.object(paths.os.path, "abspath", return_value=lexical) as abspath:
            with mock.patch.object(Path, "resolve", side_effect=OSError(1005, "unrecognized volume")) as resolve:
                result = paths.absolute_path(candidate, os_name="nt")

        self.assertEqual(result, Path(lexical))
        abspath.assert_called_once_with(candidate)
        resolve.assert_not_called()

    def test_non_windows_preserves_filesystem_resolution(self):
        candidate = Path("repo/script/ao/core/paths.py")
        resolved = Path("/canonical/repo/script/ao/core/paths.py")

        with mock.patch.object(Path, "resolve", return_value=resolved) as resolve:
            result = paths.absolute_path(candidate, os_name="posix")

        self.assertEqual(result, resolved)
        resolve.assert_called_once_with()


class DriveRootFilterTest(unittest.TestCase):
    def test_project_filters_accept_both_separators_at_a_windows_drive_root(self):
        drive_root = mock.Mock()
        drive_root.as_posix.return_value = "Y:/"

        with mock.patch.object(tidy, "absolute_path", return_value=drive_root):
            tidy_filter = tidy.project_header_filter(("lib",))
        with mock.patch.object(analyze, "absolute_path", return_value=drive_root):
            analyze_filter = analyze.project_header_filter()

        for path in ("Y:/lib/Foo.cpp", r"Y:\lib\Foo.cpp"):
            self.assertIsNotNone(re.fullmatch(tidy_filter, path))
            self.assertIsNotNone(re.fullmatch(analyze_filter, path))


if __name__ == "__main__":
    unittest.main()
